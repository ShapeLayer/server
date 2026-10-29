#include "core/net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


/**
 * Make a sockaddr_in structure from IP and port.
 * 
 * "0.0.0.0" or NULL IP means INADDR_ANY.
 * Invalid IP will set errno to EINVAL and return -1.
 */
static int make_addr(
  struct sockaddr_in *addr,
  const char *ip,
  uint16_t port
) {
  memset(addr, 0, sizeof(*addr));
#ifdef __APPLE__
  // On macOS, the sin_len field is used to specify the length of the 
  // sockaddr_in structure. It is required for certain system calls 
  // and functions that deal with socket addresses.
  addr->sin_len = sizeof(*addr);  // socket address length
#endif
  addr->sin_family = AF_INET;     // socket address family (IPv4)
  addr->sin_port = htons(port);   // port number (network byte order)

  if (ip == NULL || ip[0] == '\0') {
    addr->sin_addr.s_addr = htonl(INADDR_ANY);
    return 0;
  }
  if (inet_pton(AF_INET, ip, &addr->sin_addr) != 1) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

static void close_keep_errno(int fd) {
  int saved = errno;
  close(fd);
  errno = saved;
}

/**
 * Set the close-on-exec (FD_CLOEXEC) flag on a file descriptor.
 * This ensures that the file descriptor is automatically closed when
 * executing a new program with exec() family of functions.
 */
static int set_cloexec(int fd)
{
  int fl = fcntl(fd, F_GETFD);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}


int net_listen_tcp(const char *ip, uint16_t port, int backlog) {
  struct sockaddr_in addr;
  if (make_addr(&addr, ip, port) < 0) return -1;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int on = 1;
  if (
    // Set the close-on-exec flag
    set_cloexec(fd) < 0 ||
    // Set the SO_REUSEADDR option to allow reusing the address
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0 ||
    // Bind the socket to the specified address and port
    bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
    // Start listening for incoming connections with the specified
    // backlog
    listen(fd, backlog) < 0 ||
    // Set the socket to non-blocking mode
    net_set_nonblock(fd) < 0
  ) {
    // If any of the above operations fail, close the socket and 
    // return -1.
    close_keep_errno(fd);
    return -1;
  }
  return fd;  // done
}

int net_connect_tcp(const char *ip, uint16_t port) {
  struct sockaddr_in addr;
  // Refuse NULL or empty IP address, as it is invalid for connecting.
  if (ip == NULL || ip[0] == '\0') {
    errno = EINVAL;
    return -1;
  }
  if (make_addr(&addr, ip, port) < 0) return -1;
  
  // Create a TCP socket
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  // Connect to the specified address and port
  if (set_cloexec(fd) < 0) {
    close_keep_errno(fd);
    return -1;
  }

  // Attempt to connect, retrying if interrupted by a signal (EINTR)
  int rc;  // result of connect
  do {
    rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  } while (rc < 0 && errno == EINTR);

  // If failed
  if (rc < 0) {
    close_keep_errno(fd);
    return -1;
  }

  return fd;
}

int net_set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (flags & O_NONBLOCK) return 0;  // already non-blocking
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int net_set_nodelay(int fd, int on) {
  int v = on ? 1 : 0;
  return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
}

int net_set_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE  /* macOS, BSD */
  int on = 1;
  return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
  // There is no SO_NOSIGPIPE on Linux, but we can use MSG_NOSIGNAL
  // flag in send() instead.
  (void)fd;  // Suppress unused parameter warning
  return 0;  // No action needed on Linux
#endif
}

int net_tune_session(int fd) {
  if (net_set_nonblock(fd) < 0) return -1;
  if (net_set_nodelay(fd, 1) < 0) return -1;
  if (net_set_nosigpipe(fd) < 0) return -1;
  return 0;
}

void net_abort_close(int fd) {
  if (fd < 0) return;

  /**
   * Linger option: When a socket is closed, the system will wait for
   * any outstanding data to be sent before actually closing the
   * socket. This can help ensure that all data is transmitted before
   * the connection is terminated.
   * 
   * But in this case, we want to abort the connection immediately
   * without waiting for any remaining data to be sent. By setting the
   * linger option with a timeout of 0, we tell the system to discard
   * any unsent data and close the socket immediately.
   */
  struct linger so_linger;
  so_linger.l_onoff = 1;   // Enable linger option
  so_linger.l_linger = 0;  // Set linger time to 0

  setsockopt(fd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
  close(fd);
}
