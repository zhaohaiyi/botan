/*
* (C) 2014,2015,2017,2019 Jack Lloyd
* (C) 2016 Matthias Gierlings
* (C) 2023 René Meusel, Rohde & Schwarz Cybersecurity
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include "cli.h"

#if defined(BOTAN_HAS_TLS) && defined(BOTAN_HAS_BOOST_ASIO) && defined(BOTAN_TARGET_OS_HAS_FILESYSTEM)

   #include <atomic>
   #include <fstream>
   #include <iomanip>
   #include <iostream>
   #include <memory>
   #include <string>
   #include <thread>
   #include <utility>
   #include <vector>

   #include <botan/internal/os_utils.h>
   #include <boost/asio.hpp>
   #include <boost/bind.hpp>

   #include <botan/hex.h>
   #include <botan/pkcs8.h>
   #include <botan/rng.h>
   #include <botan/tls_messages.h>
   #include <botan/tls_server.h>
   #include <botan/tls_session_manager_memory.h>
   #include <botan/version.h>
   #include <botan/x509cert.h>

   #if defined(BOTAN_HAS_TLS_SQLITE3_SESSION_MANAGER)
      #include <botan/tls_session_manager_sqlite.h>
   #endif

   #include "tls_helpers.h"

namespace Botan_CLI {

namespace {

using boost::asio::ip::tcp;

template <typename T>
boost::asio::io_context& get_io_service(T& s) {
   #if BOOST_VERSION >= 107000
   return static_cast<boost::asio::io_context&>((s).get_executor().context());
   #else
   return s.get_io_service();
   #endif
}

inline void log_error(const char* msg) {
   std::cout << msg << std::endl;
}

inline void log_exception(const char* where, const std::exception& e) {
   std::cout << where << ' ' << e.what() << std::endl;
}

class ServerStatus {
   public:
      ServerStatus(size_t max_clients) : m_max_clients(max_clients), m_clients_serviced(0) {}

      bool should_exit() const {
         if(m_max_clients == 0) {
            return false;
         }

         return clients_serviced() >= m_max_clients;
      }

      void client_serviced() { m_clients_serviced++; }

      size_t clients_serviced() const { return m_clients_serviced.load(); }

   private:
      size_t m_max_clients;
      std::atomic<size_t> m_clients_serviced;
};

/*
* This is an incomplete and highly buggy HTTP request parser. It is just
* barely sufficient to handle a GET request sent by a browser.
*/
class HTTP_Parser final {
   public:
      class Request {
         public:
            const std::string& verb() const { return m_verb; }

            const std::string& location() const { return m_location; }

            const std::map<std::string, std::string>& headers() const { return m_headers; }

            Request(const std::string& verb,
                    const std::string& location,
                    const std::map<std::string, std::string>& headers) :
                  m_verb(verb), m_location(location), m_headers(headers) {}

         private:
            std::string m_verb;
            std::string m_location;
            std::map<std::string, std::string> m_headers;
      };

      class Callbacks {
         public:
            virtual void handle_http_request(const Request& request) = 0;

            virtual ~Callbacks() = default;

            Callbacks() = default;

            Callbacks(const Callbacks& other) = delete;
            Callbacks(Callbacks&& other) = delete;
            Callbacks& operator=(const Callbacks& other) = delete;
            Callbacks& operator=(Callbacks&&) = delete;
      };

      HTTP_Parser(Callbacks& cb) : m_cb(cb) {}

      void consume_input(std::span<const uint8_t> buf) {
         m_req_buf.append(reinterpret_cast<const char*>(buf.data()), buf.size());

         std::istringstream strm(m_req_buf);

         std::string http_version;
         std::string verb;
         std::string location;
         std::map<std::string, std::string> headers;

         strm >> verb >> location >> http_version;

         if(verb.empty() || location.empty()) {
            return;
         }

         while(true) {
            std::string header_line;
            std::getline(strm, header_line);

            if(header_line == "\r") {
               continue;
            }

            auto delim = header_line.find(": ");
            if(delim == std::string::npos) {
               break;
            }

            const std::string hdr_name = header_line.substr(0, delim);
            const std::string hdr_val = header_line.substr(delim + 2, std::string::npos);

            headers[hdr_name] = hdr_val;

            if(headers.size() > 1024) {
               throw Botan::Invalid_Argument("Too many HTTP headers sent in request");
            }
         }

         if(!verb.empty() && !location.empty()) {
            Request req(verb, location, headers);
            m_cb.handle_http_request(req);
            m_req_buf.clear();
         }
      }

   private:
      Callbacks& m_cb;
      std::string m_req_buf;
};

const size_t READBUF_SIZE = 4096;

class TLS_Asio_HTTP_Session final : public std::enable_shared_from_this<TLS_Asio_HTTP_Session>,
                                    public Botan::TLS::Callbacks,
                                    public HTTP_Parser::Callbacks {
   public:
      typedef std::shared_ptr<TLS_Asio_HTTP_Session> pointer;

      static pointer create(boost::asio::io_service& io,
                            const std::shared_ptr<Botan::TLS::Session_Manager>& session_manager,
                            const std::shared_ptr<Botan::Credentials_Manager>& credentials,
                            const std::shared_ptr<Botan::TLS::Policy>& policy) {
         auto session = std::make_shared<TLS_Asio_HTTP_Session>(io);

         // Defer the setup of the TLS server to make use of
         // shared_from_this() which wouldn't work in the c'tor.
         session->setup(session_manager, credentials, policy);

         return session;
      }

      tcp::socket& client_socket() { return m_client_socket; }

      void start() {
         m_c2s.resize(READBUF_SIZE);
         client_read(boost::system::error_code(), 0);  // start read loop
      }

      void stop() {
         if(!m_tls) {
            // Server is already closed
            return;
         }

         m_tls->close();

         // Need to explicitly destroy the TLS::Server object to break the
         // circular ownership of shared_from_this() and the shared_ptr of
         // this kept inside the TLS::Channel.
         m_tls.reset();
      }

      TLS_Asio_HTTP_Session(boost::asio::io_service& io) : m_strand(io), m_client_socket(io), m_rng(cli_make_rng()) {}

   private:
      void setup(const std::shared_ptr<Botan::TLS::Session_Manager>& session_manager,
                 const std::shared_ptr<Botan::Credentials_Manager>& credentials,
                 const std::shared_ptr<Botan::TLS::Policy>& policy) {
         m_tls = std::make_unique<Botan::TLS::Server>(shared_from_this(), session_manager, credentials, policy, m_rng);
      }

      void client_read(const boost::system::error_code& error, size_t bytes_transferred) {
         if(error) {
            return stop();
         }

         if(!m_tls) {
            log_error("Received client data after close");
            return;
         }

         try {
            m_tls->received_data(&m_c2s[0], bytes_transferred);
         } catch(Botan::Exception& e) {
            log_exception("TLS connection failed", e);
            return stop();
         }
         if(m_tls->is_closed_for_reading()) {
            return stop();
         }

         m_client_socket.async_read_some(boost::asio::buffer(&m_c2s[0], m_c2s.size()),
                                         m_strand.wrap(boost::bind(&TLS_Asio_HTTP_Session::client_read,
                                                                   shared_from_this(),
                                                                   boost::asio::placeholders::error,
                                                                   boost::asio::placeholders::bytes_transferred)));
      }

      void handle_client_write_completion(const boost::system::error_code& error) {
         if(error) {
            return stop();
         }

         m_s2c.clear();

         if(m_s2c_pending.empty() && (!m_tls || m_tls->is_closed_for_writing())) {
            m_client_socket.close();
         }
         tls_emit_data({});  // initiate another write if needed
      }

      std::string tls_server_choose_app_protocol(const std::vector<std::string>& /*client_protos*/) override {
         return "http/1.1";
      }

      void tls_record_received(uint64_t /*rec_no*/, std::span<const uint8_t> buf) override {
         if(!m_http_parser) {
            m_http_parser = std::make_unique<HTTP_Parser>(*this);
         }

         m_http_parser->consume_input(buf);
      }

      std::string summarize_request(const HTTP_Parser::Request& request) {
         std::ostringstream strm;

         strm << "Client " << client_socket().remote_endpoint().address().to_string() << " requested " << request.verb()
              << " " << request.location() << "\n";

         if(request.headers().empty() == false) {
            strm << "Client HTTP headers:\n";
            for(const auto& kv : request.headers()) {
               strm << " " << kv.first << ": " << kv.second << "\n";
            }
         }

         return strm.str();
      }

      void handle_http_request(const HTTP_Parser::Request& request) override {
         if(!m_tls) {
            log_error("Received client data after close");
            return;
         }
         std::ostringstream response;
         if(request.verb() == "GET") {
            if(request.location() == "/" || request.location() == "/status") {
               const std::string http_summary = summarize_request(request);

               const std::string report = m_connection_summary + m_session_summary + m_chello_summary + http_summary;

               response << "HTTP/1.0 200 OK\r\n";
               response << "Server: " << Botan::version_string() << "\r\n";
               response << "Content-Type: text/plain\r\n";
               response << "Content-Length: " << report.size() << "\r\n";
               response << "\r\n";

               response << report;
            } else {
               response << "HTTP/1.0 404 Not Found\r\n\r\n";
            }
         } else {
            response << "HTTP/1.0 405 Method Not Allowed\r\n\r\n";
         }

         const std::string response_str = response.str();
         m_tls->send(response_str);
         m_tls->close();
      }

      void tls_emit_data(std::span<const uint8_t> buf) override {
         if(!buf.empty()) {
            m_s2c_pending.insert(m_s2c_pending.end(), buf.begin(), buf.end());
         }

         // no write now active and we still have output pending
         if(m_s2c.empty() && !m_s2c_pending.empty()) {
            std::swap(m_s2c_pending, m_s2c);

            boost::asio::async_write(m_client_socket,
                                     boost::asio::buffer(&m_s2c[0], m_s2c.size()),
                                     m_strand.wrap(boost::bind(&TLS_Asio_HTTP_Session::handle_client_write_completion,
                                                               shared_from_this(),
                                                               boost::asio::placeholders::error)));
         }
      }

      void tls_session_activated() override {
         std::ostringstream strm;

         strm << "TLS negotiation with " << Botan::version_string() << " test server\n\n";

         m_connection_summary = strm.str();
      }

      void tls_session_established(const Botan::TLS::Session_Summary& session) override {
         std::ostringstream strm;

         strm << "Version: " << session.version().to_string() << "\n";
         strm << "Ciphersuite: " << session.ciphersuite().to_string() << "\n";
         if(const auto& session_id = session.session_id(); !session_id.empty()) {
            strm << "SessionID: " << Botan::hex_encode(session_id.get()) << "\n";
         }
         if(!session.server_info().hostname().empty()) {
            strm << "SNI: " << session.server_info().hostname() << "\n";
         }

         m_session_summary = strm.str();
      }

      void tls_inspect_handshake_msg(const Botan::TLS::Handshake_Message& message) override {
         if(message.type() == Botan::TLS::Handshake_Type::ClientHello) {
            const Botan::TLS::Client_Hello& client_hello = dynamic_cast<const Botan::TLS::Client_Hello&>(message);

            std::ostringstream strm;

            strm << "Client random: " << Botan::hex_encode(client_hello.random()) << "\n";

            strm << "Client offered following ciphersuites:\n";
            for(uint16_t suite_id : client_hello.ciphersuites()) {
               const auto ciphersuite = Botan::TLS::Ciphersuite::by_id(suite_id);

               strm << " - 0x" << std::hex << std::setfill('0') << std::setw(4) << suite_id << std::dec
                    << std::setfill(' ') << std::setw(0) << " ";

               if(ciphersuite && ciphersuite->valid()) {
                  strm << ciphersuite->to_string() << "\n";
               } else if(suite_id == 0x00FF) {
                  strm << "Renegotiation SCSV\n";
               } else {
                  strm << "Unknown ciphersuite\n";
               }
            }

            m_chello_summary = strm.str();
         }
      }

      void tls_alert(Botan::TLS::Alert alert) override {
         if(!m_tls) {
            log_error("Received client data after close");
            return;
         }
         if(alert.type() == Botan::TLS::Alert::CloseNotify) {
            m_tls->close();
         } else {
            std::cout << "Alert " << alert.type_string() << std::endl;
         }
      }

      boost::asio::io_service::strand m_strand;

      tcp::socket m_client_socket;

      std::shared_ptr<Botan::RandomNumberGenerator> m_rng;
      std::unique_ptr<Botan::TLS::Server> m_tls;
      std::string m_chello_summary;
      std::string m_connection_summary;
      std::string m_session_summary;
      std::unique_ptr<HTTP_Parser> m_http_parser;

      std::vector<uint8_t> m_c2s;
      std::vector<uint8_t> m_s2c;
      std::vector<uint8_t> m_s2c_pending;
};

class TLS_Asio_HTTP_Server final {
   public:
      typedef TLS_Asio_HTTP_Session session;

      TLS_Asio_HTTP_Server(boost::asio::io_service& io,
                           unsigned short port,
                           std::shared_ptr<Botan::Credentials_Manager> creds,
                           std::shared_ptr<Botan::TLS::Policy> policy,
                           std::shared_ptr<Botan::TLS::Session_Manager> session_mgr,
                           size_t max_clients) :
            m_acceptor(io, tcp::endpoint(tcp::v4(), port)),
            m_creds(std::move(creds)),
            m_policy(std::move(policy)),
            m_session_manager(std::move(session_mgr)),
            m_status(max_clients) {
         serve_one_session();
      }

   private:
      void serve_one_session() {
         auto new_session = session::create(get_io_service(m_acceptor), m_session_manager, m_creds, m_policy);

         m_acceptor.async_accept(
            new_session->client_socket(),
            boost::bind(&TLS_Asio_HTTP_Server::handle_accept, this, new_session, boost::asio::placeholders::error));
      }

      void handle_accept(const session::pointer& new_session, const boost::system::error_code& error) {
         if(!error) {
            new_session->start();
            m_status.client_serviced();

            if(!m_status.should_exit()) {
               serve_one_session();
            }
         }
      }

      tcp::acceptor m_acceptor;

      std::shared_ptr<Botan::Credentials_Manager> m_creds;
      std::shared_ptr<Botan::TLS::Policy> m_policy;
      std::shared_ptr<Botan::TLS::Session_Manager> m_session_manager;
      ServerStatus m_status;
};

}  // namespace

class TLS_HTTP_Server final : public Command {
   public:
      TLS_HTTP_Server() :
            Command(
               "tls_http_server server_cert server_key "
               "--port=443 --policy=default --threads=0 --max-clients=0 "
               "--session-db= --session-db-pass=") {}

      std::string group() const override { return "tls"; }

      std::string description() const override { return "Provides a simple HTTP server"; }

      size_t thread_count() const {
         if(size_t t = get_arg_sz("threads")) {
            return t;
         }
         if(size_t t = Botan::OS::get_cpu_available()) {
            return t;
         }
         return 2;
      }

      void go() override {
         const uint16_t listen_port = get_arg_u16("port");

         const std::string server_crt = get_arg("server_cert");
         const std::string server_key = get_arg("server_key");

         const size_t num_threads = thread_count();
         const size_t max_clients = get_arg_sz("max-clients");

         auto creds = std::make_shared<Basic_Credentials_Manager>(server_crt, server_key);

         auto policy = load_tls_policy(get_arg("policy"));

         std::shared_ptr<Botan::TLS::Session_Manager> session_mgr;

         const std::string sessions_db = get_arg("session-db");

         if(!sessions_db.empty()) {
   #if defined(BOTAN_HAS_TLS_SQLITE3_SESSION_MANAGER)
            const std::string sessions_passphrase = get_passphrase_arg("Session DB passphrase", "session-db-pass");
            session_mgr.reset(
               new Botan::TLS::Session_Manager_SQLite(sessions_passphrase, rng_as_shared(), sessions_db));
   #else
            throw CLI_Error_Unsupported("Sqlite3 support not available");
   #endif
         }

         if(!session_mgr) {
            session_mgr.reset(new Botan::TLS::Session_Manager_In_Memory(rng_as_shared()));
         }

         boost::asio::io_service io;

         TLS_Asio_HTTP_Server server(io, listen_port, creds, policy, session_mgr, max_clients);

         std::vector<std::shared_ptr<std::thread>> threads;

         // run forever... first thread is main calling io.run below
         for(size_t i = 2; i <= num_threads; ++i) {
            threads.push_back(std::make_shared<std::thread>([&io]() { io.run(); }));
         }

         io.run();

         for(size_t i = 0; i < threads.size(); ++i) {
            threads[i]->join();
         }
      }
};

BOTAN_REGISTER_COMMAND("tls_http_server", TLS_HTTP_Server);

}  // namespace Botan_CLI

#endif
