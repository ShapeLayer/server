/* session.h */

/**
 * Each TCP Connection:
 * - rx stream => packets
 * - packets   => tx stream
 * 
 * - The session is intended not to touch the poller directly.
 *   It reports what it needs(session_wants_write) and the server registers interest.
 * - fd must already be set to non-blocking mode. Socket options (TCP_NODELAY, ...)
 *   are the caller's working range.
 * - Callbacks run inside session_on_readable. They may call session_send and
 *   session_mark_closing, but must NOT call session_destroy.
 */

#pragma once
#ifndef SESSION_H
#define SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "core/ringbuf.h"
#include "core/packet.h"

/**
 * Session RX/TX Buffer Size
 * - RX: 16KB, TX: 64KB
 * - Capacities must be 2^n to support ring buffer's bitmasking.
 *   (because RB_MASK uses pos & (capacity - 1) to wrap around)
 * - rx buffer must be large enough to hold the largest packet.
 */
#define SESSION_RX_CAPACITY 16384u  // 16KB
#define SESSION_TX_CAPACITY 65536u  // 64KB

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(SESSION_RX_CAPACITY >= PKT_MAX_SIZE, "rx must hold the largest packet");
#endif

typedef struct session session_t;

typedef enum {
  SESS_REASON_NONE = 0,
  SESS_REASON_EOF,      /* peer sent FIN (recv returned 0) */
  SESS_REASON_IO_ERROR, /* ECONNRESET, EPIPE, ... (errno at that time is kept) */
  SESS_REASON_INVALID,  /* malformed packet header */
  SESS_REASON_LOCAL,    /* session_mark_closing was called */
} session_reason_t;

typedef struct {
  void (*on_packet)(session_t *s, uint16_t id, const uint8_t *payload, size_t len, void *ctx);
  void (*on_close)(session_t *s, session_reason_t reason, void *ctx);
} session_handler_t;

typedef enum { SESS_OK = 0, SESS_CLOSE = 1} session_status_t;

session_t *session_create(int fd, const session_handler_t *handler, void *ctx);
void session_destroy(session_t *s, int abortive);  // net_abort_close if abortive is true

// This handler works like a state machine:
// - It returns SESS_OK if the session is still active
// - or SESS_CLOSE if the session should be closed.
// intended to be called from poller when fd is readable/writable
session_status_t session_on_readable(session_t *s);
// - session_on_writable returns SESS_OK if the session is still active
// - or SESS_CLOSE if the session should be closed.
session_status_t session_on_writable(session_t *s);
int session_send(session_t *s, uint16_t id, const uint8_t *payload, size_t len);

int session_fd(const session_t *s);
int session_wants_write(const session_t *s);  // POLL_OUT when tx buffer is not empty
void session_mark_closing(session_t *s);
int session_is_closing(const session_t *s);
session_reason_t session_close_reason(const session_t *s);

struct session {
  int fd;
  int closing;  // true if session is closing (no more send)
  session_reason_t close_reason;
  ringbuf_t rx;  // receive buffer
  ringbuf_t tx;  // transmit buffer
  const session_handler_t *handler;
  void *ctx;  // user context
};

#endif  // SESSION_H
