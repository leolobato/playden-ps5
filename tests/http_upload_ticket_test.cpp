// SPDX-License-Identifier: GPL-3.0-or-later
// #50: an upload whose write blocks while a TLS 1.3 server sends a NewSessionTicket must reach the server once.
// Mbed TLS flushes the pending record while it handles the ticket; curl 8.22 then encrypted the same bytes a second
// time, and the server stored one 16 KiB record twice (same length, wrong bytes: Steam Cloud saves on Azure storage).
// The server here is Mbed TLS, stepped by hand: it stops just before sending its ticket, lets the client's upload
// fill the socket buffers, sends the ticket, then reads the body and answers with its SHA-1.
#include "steam/backend/http_client.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha1.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ticket.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "CHECK failed: %s\n", message);
    std::exit(1);
  }
}

std::string Sha1Hex(const std::vector<std::uint8_t>& data) {
  unsigned char digest[20];
  mbedtls_sha1(data.data(), data.size(), digest);
  char hex[41];
  for (int i = 0; i < 20; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return hex;
}

int Send(void* context, const unsigned char* data, size_t size) {
  const auto sent = ::send(*static_cast<int*>(context), data, size, 0);
  return sent < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : static_cast<int>(sent);
}
int Receive(void* context, unsigned char* data, size_t size) {
  const auto read = ::recv(*static_cast<int*>(context), data, size, 0);
  return read < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : static_cast<int>(read);
}

// Serves `uploads` requests, one per connection, each answered with the SHA-1 of the body it received.
struct Server {
  int listener = -1;
  int port = 0;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_x509_crt certificate;
  mbedtls_pk_context key;
  mbedtls_ssl_ticket_context tickets;
  mbedtls_ssl_config config;

  explicit Server(const std::string& fixtures) {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&key);
    mbedtls_ssl_ticket_init(&tickets);
    mbedtls_ssl_config_init(&config);
    Check(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0) == 0, "drbg");
    Check(mbedtls_x509_crt_parse_file(&certificate, (fixtures + "/server.pem").c_str()) == 0, "server certificate");
    Check(mbedtls_pk_parse_keyfile(&key, (fixtures + "/server-key.pem").c_str(), nullptr, mbedtls_ctr_drbg_random, &drbg) == 0,
          "server key");
    Check(mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT) == 0, "config");
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_3);
    Check(mbedtls_ssl_conf_own_cert(&config, &certificate, &key) == 0, "own certificate");
    Check(mbedtls_ssl_ticket_setup(&tickets, mbedtls_ctr_drbg_random, &drbg, MBEDTLS_CIPHER_AES_256_GCM, 3600) == 0, "tickets");
    mbedtls_ssl_conf_session_tickets_cb(&config, mbedtls_ssl_ticket_write, mbedtls_ssl_ticket_parse, &tickets);
    mbedtls_ssl_conf_new_session_tickets(&config, 1);

    listener = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1, small = 8192;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    ::setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);  // inherited: the client's writes block
    int actual = 0;
    socklen_t actual_size = sizeof actual;
    Check(::getsockopt(listener, SOL_SOCKET, SO_RCVBUF, &actual, &actual_size) == 0 && actual <= 64 * 1024,
          "small receive buffer");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0, "bind");
    socklen_t length = sizeof address;
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    port = ntohs(address.sin_port);
    Check(::listen(listener, 4) == 0, "listen");
  }

  void ServeOne() {
    int client = ::accept(listener, nullptr, nullptr);
    Check(client >= 0, "accept");
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    Check(mbedtls_ssl_setup(&ssl, &config) == 0, "ssl setup");
    mbedtls_ssl_set_bio(&ssl, &client, Send, Receive, nullptr);
    // Up to the ticket: from the client's side the handshake is done and it starts its upload. (The ticket is a
    // state past MBEDTLS_SSL_HANDSHAKE_OVER, so compare states rather than mbedtls_ssl_is_handshake_over().)
    while (ssl.MBEDTLS_PRIVATE(state) != MBEDTLS_SSL_TLS1_3_NEW_SESSION_TICKET)
      Check(mbedtls_ssl_handshake_step(&ssl) == 0, "server handshake step");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // nothing read: the client's write blocks
    // The ticket must arrive while the upload waits: its records are already queued here, unread.
    int unread = 0;
    Check(::ioctl(client, FIONREAD, &unread) == 0 && unread >= 4096, "the upload is waiting when the ticket goes out");
    while (ssl.MBEDTLS_PRIVATE(state) != MBEDTLS_SSL_HANDSHAKE_OVER)
      Check(mbedtls_ssl_handshake_step(&ssl) == 0, "ticket sent");

    std::vector<std::uint8_t> received;
    std::vector<unsigned char> buffer(64 * 1024);
    std::size_t header_end = std::string::npos, content_length = 0;
    while (header_end == std::string::npos || received.size() - header_end < content_length) {
      const int read = mbedtls_ssl_read(&ssl, buffer.data(), buffer.size());
      if (read == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET || read == MBEDTLS_ERR_SSL_WANT_READ) continue;
      Check(read > 0, "server read");
      received.insert(received.end(), buffer.begin(), buffer.begin() + read);
      if (header_end == std::string::npos) {
        const std::string text(received.begin(), received.end());
        const auto end = text.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        header_end = end + 4;
        const auto field = text.find("Content-Length: ");
        Check(field != std::string::npos && field < end, "content length");
        content_length = std::strtoul(text.c_str() + field + 16, nullptr, 10);
      }
    }
    const std::vector<std::uint8_t> body(received.begin() + static_cast<std::ptrdiff_t>(header_end),
                                         received.begin() + static_cast<std::ptrdiff_t>(header_end + content_length));
    const std::string answer = "HTTP/1.1 200 OK\r\nContent-Length: 40\r\nConnection: close\r\n\r\n" + Sha1Hex(body);
    std::size_t written = 0;
    while (written < answer.size()) {
      const int sent = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(answer.data()) + written,
                                         answer.size() - written);
      Check(sent > 0, "server write");
      written += static_cast<std::size_t>(sent);
    }
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    ::close(client);
  }
};

}  // namespace

int main(int argc, char** argv) {
  Check(argc == 2, "usage: http_upload_ticket_test <tests/fixtures/http>");
  const std::string fixtures = argv[1];
  Check(psa_crypto_init() == PSA_SUCCESS, "psa");
  Server server(fixtures);
  const std::vector<std::size_t> sizes = {600 * 1024, 1897451, 3 * 1024 * 1024};
  std::thread serving([&] {
    for (std::size_t i = 0; i < sizes.size(); ++i) server.ServeOne();
  });

  playden::steam::HttpClient http(fixtures + "/ca.pem");
  const std::atomic<bool> cancel{false};
  std::uint32_t seed = 50;
  int damaged = 0;
  for (const auto size : sizes) {
    playden::steam::HttpRequest request;
    request.method = playden::steam::HttpMethod::kPut;
    request.url = "https://localhost:" + std::to_string(server.port) + "/upload";
    request.timeout_ms = 60'000;
    request.max_response_bytes = 64;
    request.body.resize(size);
    for (auto& byte : request.body) {
      seed = seed * 1103515245u + 12345u;
      byte = static_cast<std::uint8_t>(seed >> 16);
    }
    const auto response = http.Perform(request, cancel);
    Check(response.ok() && response.status_code == 200, "upload answered");
    const std::string got(response.body.begin(), response.body.end());
    if (got != Sha1Hex(request.body)) {
      std::fprintf(stderr, "upload of %zu bytes arrived damaged\n", size);
      ++damaged;
    }
  }
  serving.join();
  Check(damaged == 0, "every upload arrived as sent");
  std::puts("http upload ticket test passed");
}
