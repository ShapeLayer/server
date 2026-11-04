#include "core/session.h"

#include "core/net.h"
#include "core/platform.h"
#include "core/ringbuf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static session_status_t fail(session_t *s, session_reason_t reason) {
  if (s->close_reason == SESS_REASON_NONE) s->close_reason = reason;
  s->closing = 1;
  return SESS_CLOSE;
}

/**
 * Creates a new session object with the specified file descriptor, handler, and context.
 * 
 * `session_create` receives a ownership of the fd, and tries to allocate rx and tx buffers.
 */
session_t *session_create(int fd, const session_handler_t *handler, void *ctx) {
  if (fd < 0 || handler == NULL) {
    errno = EINVAL;
    return NULL;
  }

  // If the runtime is running on macOS, set the SO_NOSIGPIPE option
  // to prevent SIGPIPE signals from being sent when writing to a closed socket.
  if (net_set_nosigpipe(fd) < 0) return NULL;

  session_t *s = calloc(1, sizeof *s);
  if (!s) return NULL;

  // Fill fields
  if (rb_init(&s->rx, SESSION_RX_CAPACITY) < 0) {
    free(s);
    return NULL;
  }
  if (rb_init(&s->tx, SESSION_TX_CAPACITY) < 0) {
    rb_free(&s->rx);
    free(s);
    return NULL;
  }

  s->fd = fd;
  s->handler = handler;
  s->ctx = ctx;

  s->closing = 0;
  s->close_reason = SESS_REASON_NONE;
  
  return s;
}

void session_destroy(session_t *s, int abortive) {
  if (!s) return;
  if (s->handler && s->handler->on_close) {
    s->handler->on_close(s, s->close_reason, s->ctx);
  }
  if (abortive)
    net_abort_close(s->fd);
  else
    close(s->fd);
  
  rb_free(&s->rx);
  rb_free(&s->tx);
  free(s);
}

static session_status_t dispatch(session_t *s)
{
  uint8_t payload[PKT_MAX_SIZE - PKT_HEADER_SIZE];

  for (;;) {
    if (s->closing) // If the session is closing, stop processing further packets
      return fail(s, SESS_REASON_LOCAL);
    
    pkt_header_t hdr;
    switch (pkt_extract(&s->rx, &hdr, payload, sizeof payload)) {
      case PKT_OK:
        if (s->handler && s->handler->on_packet)
          s->handler->on_packet(
            s, hdr.id, payload, hdr.size - PKT_HEADER_SIZE, s->ctx
          );
        break;
      case PKT_NEED_MORE:
        return SESS_OK;  // Wait for more data
      case PKT_INVALID:
        return fail(s, SESS_REASON_INVALID);
    }
  }
}

session_status_t session_on_readable(session_t *s) {
  if (s->closing) return SESS_CLOSE;

  // Read data from the socket into the receive buffer until EAGAIN(error
  // when no more data is available now) or EWOULDBLOCK(error when the socket
  // is non-blocking and no data is available)
  for (;;) {
    struct iovec iov[2];
    int cnt = rb_write_iov(&s->rx, iov);
    if (cnt == 0) {
      // The receive buffer is full, cannot read more data
      return fail(s, SESS_REASON_IO_ERROR);
    }

    // The number of bytes read from the socket
    ssize_t n = readv(s->fd, iov, cnt);
    if (n > 0) {
      rb_commit(&s->rx, (size_t)n);
      if (dispatch(s) == SESS_CLOSE) return SESS_CLOSE;
      continue;
    }

    // Client (Active Closer)                   Server (Passive Closer)
    // | close()                                    |
    // | --- FIN ---------------------------------> |  Kernel sends ACK
    // | <-- ACK ---------------------------------- |  state: CLOSE_WAIT
    // | state: FIN_WAIT_2                          |  readv() == 0  <- handle this case
    // |                                            |
    // |                            (when server calls close())
    // | <-- FIN ---------------------------------- |  state: LAST_ACK
    // | --- ACK ---------------------------------> |  state: CLOSED
    if (n == 0) {
      // No more data would be received from the peer (EOF)
      // Returns SESS_CLOSE to indicate that the session should be closed
      // if this case is not handled, the server will not be able to detect
      // that the client has closed the connection, and it may continue to wait for data indefinitely.
      // - it leads to fd leaks, memory leaks, high CPU usage(busy loop).
      return fail(s, SESS_REASON_EOF);
    }

    if (errno == EINTR) continue;  // Interrupted by signal, retry
    // No more data available now, wait for the next readable event
    if (errno == EAGAIN || errno == EWOULDBLOCK) return SESS_OK;

    return fail(s, SESS_REASON_IO_ERROR);
  }
}

session_status_t session_on_writable(session_t *s) {
  // returns SESS_OK when the session is still active
  // or the ringbuffer is consumed completely.
  while (rb_size(&s->tx) > 0) {
    struct msghdr msg;
    struct iovec iov[2];
    msg.msg_iov = iov;
    msg.msg_iovlen = rb_read_iov(&s->tx, iov);

    // n is the number of bytes sent to the socket
    ssize_t n = sendmsg(s->fd, &msg, 0);
    if (n >= 0) {
      rb_consume(&s->tx, (size_t)n);
      continue;
    }
    if (errno == EINTR) continue;  // Interrupted by signal, retry
    // No more data can be sent now, wait for the next writable event
    if (errno == EAGAIN || errno == EWOULDBLOCK) return SESS_OK;

    return fail(s, SESS_REASON_IO_ERROR);
  }
  return SESS_OK;
}

int session_send(session_t *s, uint16_t id, const uint8_t *payload, size_t len) {
  /**
   * Sends a packet with the specified ID and payload to the session's transmit buffer.
   * Returns 0 on success, or -1 on failure (e.g., if the transmit buffer is full or if the session is closing).
   * The packet format consists of a header (containing the size and ID) followed by the payload.
   * The total size of the packet (header + payload) would not exceed PKT_MAX_SIZE.
   */

  // (err) broken pipe
  if (s->closing) {
    errno = EPIPE;
    return -1;
  }

  // (err) message too long
  if (len > PKT_MAX_SIZE - PKT_HEADER_SIZE) {
    errno = EMSGSIZE;
    return -1;
  }

  // (err) not enough space in the transmit buffer
  size_t total_size = PKT_HEADER_SIZE + len;
  if (rb_space(&s->tx) < total_size) {
    errno = ENOBUFS;
    return -1;
  }

  uint8_t header[PKT_HEADER_SIZE];
  pkt_encode_header(header, (uint16_t)total_size, id);
  rb_write(&s->tx, header, PKT_HEADER_SIZE);
  if (len > 0) rb_write(&s->tx, payload, len);

  return 0;
}

int session_fd(const session_t *s) { return s->fd; }
int session_wants_write(const session_t *s) { return rb_size(&s->tx) > 0; }
int session_is_closing(const session_t *s) { return s->closing; }
session_reason_t session_close_reason(const session_t *s) { return s->close_reason; }

void session_mark_closing(session_t *s) {
  if (!s->closing) {
    s->closing = 1;
    s->close_reason = SESS_REASON_LOCAL;
  }
}
