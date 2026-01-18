#pragma once

// TLS 1.3 Implementation for Netpipe
//
// This module provides TLS 1.3 encryption as a layer on top of existing
// netpipe transports (TCP, IPC, etc.). TLS is NOT a transport itself -
// it's a security layer that encrypts/decrypts data flowing through a transport.
//
// Usage:
//
//   // Server side
//   TcpStream tcp;
//   tcp.listen({"0.0.0.0", 8443});
//   auto client_tcp = tcp.accept();
//
//   tls::SessionConfig config;
//   config.certificate = load_cert("server.crt");
//   config.private_key = load_key("server.key");
//
//   tls::Session session(config);
//   session.handshake_server(*client_tcp);
//
//   // Now send/receive encrypted data
//   session.send(*client_tcp, data);
//   auto response = session.recv(*client_tcp);
//
//   // Client side
//   TcpStream tcp;
//   tcp.connect({"server.com", 8443});
//
//   tls::SessionConfig config;
//   config.server_name = "server.com";
//
//   tls::Session session(config);
//   session.handshake_client(tcp);
//
//   auto msg = session.recv(tcp);
//   session.send(tcp, response);
//
// Supported features:
// - TLS 1.3 (RFC 8446)
// - ChaCha20-Poly1305 cipher suite
// - X25519 key exchange
// - Ed25519 signatures
// - X.509 certificates
//
// Dependencies:
// - keylock library (for cryptographic primitives)

#include <netpipe/security/tls/alert.hpp>
#include <netpipe/security/tls/extensions.hpp>
#include <netpipe/security/tls/handshake.hpp>
#include <netpipe/security/tls/key_schedule.hpp>
#include <netpipe/security/tls/messages.hpp>
#include <netpipe/security/tls/record.hpp>
#include <netpipe/security/tls/session.hpp>
