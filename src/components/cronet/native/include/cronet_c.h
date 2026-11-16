// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_CRONET_NATIVE_INCLUDE_CRONET_C_H_
#define COMPONENTS_CRONET_NATIVE_INCLUDE_CRONET_C_H_

#include <stddef.h>
#include <stdint.h>

#include "cronet_export.h"

// Cronet public C API is generated from cronet.idl
#include "cronet.idl_c.h"

#ifdef __cplusplus
extern "C" {
#endif

// Stream Engine used by Bidirectional Stream C API for GRPC.
typedef struct stream_engine stream_engine;

// Additional Cronet C API not generated from cronet.idl.

// Sets net::CertVerifier* raw_mock_cert_verifier for testing of Cronet_Engine.
// Must be called before Cronet_Engine_InitWithParams().
CRONET_EXPORT void Cronet_Engine_SetMockCertVerifierForTesting(
    Cronet_EnginePtr engine,
    /* net::CertVerifier* */ void* raw_mock_cert_verifier);

// Returns "stream_engine" interface for bidirectionsl stream support for GRPC.
// Returned stream engine is owned by Cronet Engine and is only valid until
// Cronet_Engine_Shutdown().
CRONET_EXPORT stream_engine* Cronet_Engine_GetStreamEngine(
    Cronet_EnginePtr engine);

// Creates a CertVerifier that uses custom root certificates for validation.
// pem_root_certs: PEM-formatted root certificates (can contain multiple certs).
// Returns a pointer to the created net::CertVerifier.
// The caller is responsible for passing it to
// Cronet_Engine_SetMockCertVerifierForTesting() which takes ownership.
// Returns nullptr if the PEM data is invalid or no valid certificates found.
CRONET_EXPORT void* Cronet_CreateCertVerifierWithRootCerts(
    const char* pem_root_certs);

// Exact server certificates: a list of EngineParams, like quic_hints and
// public_key_pins, kept apart from cronet.idl because its entries are binary.
//
// An engine started with a non-empty list trusts a server certificate if and
// only if the leaf the server presents is, byte for byte, one of the DER
// certificates of the list. Nothing else is checked: no chain is built, no
// host name or IP address is matched, no validity period, root store
// (system or Chrome Root Store), revocation or Certificate Transparency is
// consulted. The TLS handshake still proves that the server holds the
// certificate's private key. Use it only with certificates that the
// application distributes itself, such as the self-signed certificate of a
// server reached by IP address.
//
// It applies to TLS over TCP (HTTP/1.1, HTTP/2) and to QUIC (HTTP/3) alike;
// QUIC accepts such a certificate although it chains to no known root. A
// match verifies with cert status 0 and as not issued by a known root; any
// other certificate fails with ERR_CERT_AUTHORITY_INVALID (over QUIC, the
// handshake fails).
//
// An empty list (the default) leaves certificate verification as it is.
//
// Cronet_Engine_StartWithParams() fails, and changes nothing, when the list
// is not empty and
// - one entry is not a DER X.509 certificate: Cronet_RESULT_ILLEGAL_ARGUMENT;
// - public_key_pins is not empty: Cronet_RESULT_ILLEGAL_ARGUMENT. A pin can
//   add nothing to an exact certificate, which fixes the key, and pins are
//   only kept for host names, while these certificates are meant for
//   addresses; with the default bypass for local trust anchors the pins
//   would also be skipped silently;
// - the engine has a certificate verifier of
//   Cronet_Engine_SetMockCertVerifierForTesting():
//   Cronet_RESULT_ILLEGAL_STATE. Only one of them can decide.

// Appends a copy of the |der_len| bytes at |der|, the DER encoding of an X.509
// certificate, to the exact server certificates of |self|. A null |der| adds
// an empty entry, which Cronet_Engine_StartWithParams() refuses.
CRONET_EXPORT void Cronet_EngineParams_exact_server_certificates_add(
    Cronet_EngineParamsPtr self,
    const uint8_t* der,
    size_t der_len);

// Returns the number of exact server certificates of |self|.
CRONET_EXPORT uint32_t Cronet_EngineParams_exact_server_certificates_size(
    const Cronet_EngineParamsPtr self);

// Returns the DER of the exact server certificate at |index| and writes its
// length to |der_len| if not null. The bytes belong to |self| and are valid
// until it is changed or destroyed. Out of range: null, and a length of 0.
CRONET_EXPORT const uint8_t* Cronet_EngineParams_exact_server_certificates_at(
    const Cronet_EngineParamsPtr self,
    uint32_t index,
    size_t* der_len);

// Removes all exact server certificates of |self|.
CRONET_EXPORT void Cronet_EngineParams_exact_server_certificates_clear(
    Cronet_EngineParamsPtr self);

// Dialer callback type for custom TCP connection establishment.
// context: User-provided context pointer passed to Cronet_Engine_SetDialer.
// address: IP address string (e.g. "1.2.3.4" or "::1").
// port: Port number.
// Returns: connected socket fd on success, negative net error code on failure.
// Common error codes:
//   -102: ERR_CONNECTION_REFUSED
//   -104: ERR_CONNECTION_FAILED
//   -109: ERR_ADDRESS_UNREACHABLE
//   -118: ERR_CONNECTION_TIMED_OUT
typedef intptr_t (*Cronet_DialerFunc)(void* context,
                                      const char* address,
                                      uint16_t port);

// Sets a custom dialer for TCP connections.
// When set, the engine will use this callback to establish TCP connections
// instead of the default system socket API.
// Must be called before Cronet_Engine_StartWithParams().
// dialer: The callback function to use for TCP connections, or nullptr to
//         disable custom dialing.
// context: User-provided context pointer that will be passed to the dialer.
CRONET_EXPORT void Cronet_Engine_SetDialer(Cronet_EnginePtr engine,
                                           Cronet_DialerFunc dialer,
                                           void* context);

// UDP Dialer callback type for custom UDP socket creation.
// context: User-provided context pointer passed to Cronet_Engine_SetUdpDialer.
// address: IP address string (e.g. "1.2.3.4" or "::1").
// port: Port number.
// out_local_address: Output buffer for local IP address (caller provides
// buffer,
//                    should be at least 46 bytes for INET6_ADDRSTRLEN).
// out_local_port: Output pointer for local port number.
// out_socket_id: Opaque identifier passed to on_close; zero skips notification.
// Returns: socket fd on success, negative net error code on failure.
// The returned socket can be:
//   - AF_INET/AF_INET6 SOCK_DGRAM: Standard UDP socket
//   - AF_UNIX SOCK_DGRAM: Unix domain datagram socket (Unix/macOS/Linux)
//   - AF_UNIX SOCK_STREAM: Unix domain stream socket (Windows, with framing)
typedef intptr_t (*Cronet_UdpDialerFunc)(void* context,
                                         const char* address,
                                         uint16_t port,
                                         char* out_local_address,
                                         uint16_t* out_local_port,
                                         uint64_t* out_socket_id);

typedef void (*Cronet_UdpSocketCloseFunc)(uint64_t socket_id);

// Sets a custom dialer for UDP sockets.
// When set, the engine will use this callback to create UDP sockets
// instead of the default system socket API.
// Must be called before Cronet_Engine_StartWithParams().
// dialer: The callback function to use for UDP sockets, or nullptr to
//         disable custom UDP dialing.
// context: User-provided context pointer that will be passed to the dialer.
CRONET_EXPORT void Cronet_Engine_SetUdpDialer(
    Cronet_EnginePtr engine,
    Cronet_UdpDialerFunc dialer,
    void* context,
    Cronet_UdpSocketCloseFunc on_close);

// Closes all connections managed by the engine's network session.
// This includes socket pools, HTTP stream pool, SPDY session pool,
// and QUIC session pool. Useful for releasing connection-related memory
// or speeding up engine shutdown.
// Must be called after Cronet_Engine_StartWithParams().
// Calls from Cronet's network thread schedule the close asynchronously.
CRONET_EXPORT void Cronet_Engine_CloseAllConnections(Cronet_EnginePtr engine);

#ifdef __cplusplus
}
#endif

#endif  // COMPONENTS_CRONET_NATIVE_INCLUDE_CRONET_C_H_
