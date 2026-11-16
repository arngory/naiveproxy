// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_GRPC_SUPPORT_INCLUDE_DATAGRAM_STREAM_C_H_
#define COMPONENTS_GRPC_SUPPORT_INCLUDE_DATAGRAM_STREAM_C_H_

#include <stdbool.h>
#include <stdint.h>

#include "bidirectional_stream_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Datagram Stream API.
 *
 * A datagram stream is a connect-udp request (RFC 9298) over HTTP/3: an
 * extended CONNECT whose HTTP Datagrams (RFC 9297) carry the payloads given to
 * datagram_stream_send(). The library adds Context ID 2 to every datagram it
 * sends and strips it from those it receives, dropping datagrams with another
 * Context ID. A payload that does not fit in a QUIC datagram goes in a DATAGRAM
 * capsule on the request stream, if the caller allows it.
 *
 * The capsules received are parsed by QUICHE: DATAGRAM capsules, and the
 * legacy datagram capsule types of earlier drafts (0xff37a0, 0xff37a5), carry
 * datagrams; capsules of unknown types are skipped, and so are the address,
 * route and compression capsules. A CLOSE_WEBTRANSPORT_SESSION or
 * DRAIN_WEBTRANSPORT_SESSION capsule, or one that does not parse, resets the
 * stream (on_failed()).
 */

/* Opaque object representing a datagram stream. */
typedef struct datagram_stream {
  void* obj;
  void* annotation;
} datagram_stream;

/* Set of callbacks of a datagram stream. They are invoked on the network
 * thread of the engine and must not block it; data passed to them is valid
 * only for the duration of the call. After on_closed(), on_failed() or
 * on_canceled() no callback is invoked.
 */
typedef struct datagram_stream_callback {
  /* Invoked with the response, whatever its status. Sending does not wait for
   * it: until then every datagram goes in a capsule. A status other than 200 is
   * followed by on_closed(). Contents of |headers| is valid for the duration of
   * the call.
   */
  void (*on_ready)(datagram_stream* stream,
                   int status,
                   const bidirectional_stream_header_array* headers);

  /* Invoked with one datagram: the payload after the Context ID, whether it
   * came in a QUIC datagram or in a capsule.
   */
  void (*on_datagram_received)(datagram_stream* stream,
                               const char* data,
                               int length);

  /* Invoked when the payload that fits in one QUIC datagram changes. */
  void (*on_max_payload_changed)(datagram_stream* stream, int max_payload);

  /* Invoked at a clean end: the server closed the stream, or the status was
   * not 200.
   */
  void (*on_closed)(datagram_stream* stream);

  /* Invoked if the stream failed: it was reset, or the connection was lost.
   * ERR_METHOD_NOT_SUPPORTED (-322) means the server announced no HTTP/3
   * datagrams (SETTINGS_H3_DATAGRAM) or no extended CONNECT: the request was
   * not sent.
   */
  void (*on_failed)(datagram_stream* stream, int net_error);

  /* Invoked after datagram_stream_cancel(). */
  void (*on_canceled)(datagram_stream* stream);
} datagram_stream_callback;

/* Results of datagram_stream_send(). */
#define DATAGRAM_STREAM_OK 0
/* The send queue is full: more than 64 datagrams wait for the network thread
 * or for the stream. */
#define DATAGRAM_STREAM_QUEUE_FULL -1
/* The payload does not fit in a QUIC datagram and capsules are not allowed,
 * or it is longer than any capsule may be. Until on_ready() with status 200
 * nothing fits in a QUIC datagram. Also for a negative |length|, or a null
 * |data| with a |length| above 0. */
#define DATAGRAM_STREAM_TOO_LARGE -2
/* The stream has not started, has ended or been canceled, or the server
 * stopped reading it (STOP_SENDING). */
#define DATAGRAM_STREAM_CLOSED -3
/* The payload needs a capsule while the oldest capsule not yet sent, in the
 * send buffer of the stream or queued for it, has waited longer than 100 ms. */
#define DATAGRAM_STREAM_CAPSULE_TIMEOUT -4

/* Flag of datagram_stream_send(): a payload that does not fit in a QUIC
 * datagram goes in a capsule. */
#define DATAGRAM_STREAM_SEND_ALLOW_CAPSULE 1

/* Creates a new datagram stream that uses |engine| and |callback|, as
 * bidirectional_stream_create() does. The returned |datagram_stream*| is owned
 * by the caller, and must be destroyed using datagram_stream_destroy(). Both
 * |callback| and |engine| must remain valid until the stream is destroyed.
 */
GRPC_SUPPORT_EXPORT
datagram_stream* datagram_stream_create(
    stream_engine* engine,
    void* annotation,
    const datagram_stream_callback* callback);

/* Starts the stream by sending an extended CONNECT request with :protocol
 * connect-udp to |url| with |headers|. The library adds capsule-protocol: ?1.
 * Returns 0 if the request is valid and started, -1 if |url| is not valid, or
 * the position of the first invalid header in |headers|.
 */
GRPC_SUPPORT_EXPORT
int datagram_stream_start(datagram_stream* stream,
                          const char* url,
                          const bidirectional_stream_header_array* headers);

/* Sends |length| bytes of |data| as one datagram. Can be called from any
 * thread: copies |data|, queues it for the network thread and returns at once,
 * with one of the results above. A capsule is written only while the send
 * buffer of the stream is empty; one still queued 100 ms after the stream
 * could have taken it is dropped and counted
 * (datagram_stream_dropped_capsules()). |flags| is 0 or
 * DATAGRAM_STREAM_SEND_ALLOW_CAPSULE.
 */
GRPC_SUPPORT_EXPORT
int datagram_stream_send(datagram_stream* stream,
                         const char* data,
                         int length,
                         int flags);

/* Returns the largest payload that fits in one QUIC datagram, after the
 * Quarter Stream ID and the Context ID. 0 before on_ready() with status 200:
 * capsules only. Can be called from any thread.
 */
GRPC_SUPPORT_EXPORT
int datagram_stream_max_payload(datagram_stream* stream);

/* Returns the number of datagrams dropped after datagram_stream_send() took
 * them: capsules that waited 100 ms for the stream, capsules over a full queue
 * or behind a write side the server stopped reading, and datagrams that no
 * longer fit in a QUIC datagram when capsules were not allowed. Can be called
 * from any thread.
 */
GRPC_SUPPORT_EXPORT
uint64_t datagram_stream_dropped_capsules(datagram_stream* stream);

/* Cancels the stream. Once datagram_stream_start() has returned 0,
 * on_canceled() follows, also when the cancel comes before the request is
 * sent, unless the stream has already ended. A cancel before
 * datagram_stream_start() makes it return -1, with no callback.
 */
GRPC_SUPPORT_EXPORT
void datagram_stream_cancel(datagram_stream* stream);

/* Destroys the stream. Can be called from any thread, including the network
 * thread, but is posted, so |stream| is valid until the calling task is
 * complete. Returns 0.
 */
GRPC_SUPPORT_EXPORT
int datagram_stream_destroy(datagram_stream* stream);

#ifdef __cplusplus
}
#endif

#endif  // COMPONENTS_GRPC_SUPPORT_INCLUDE_DATAGRAM_STREAM_C_H_
