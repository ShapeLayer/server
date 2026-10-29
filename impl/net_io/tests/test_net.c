#include "core/net.h"
#include "framework/test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

/* 커널이 비어 있는 포트를 고르게 하고(port 0), 실제 번호를 얻는다 */
static uint16_t bound_port(int fd)
{
  struct sockaddr_in a;
  socklen_t len = sizeof a;
  if (getsockname(fd, (struct sockaddr *)&a, &len) < 0)
    return 0;
  return ntohs(a.sin_port);
}

static int get_int_opt(int fd, int level, int name)
{
  int v = -1;
  socklen_t len = sizeof v;
  getsockopt(fd, level, name, &v, &len);
  return v;
}

/* listen fd가 accept 가능해질 때까지 대기 (nonblock 리스너용) */
static int accept_wait(int lfd)
{
  struct pollfd p = {.fd = lfd, .events = POLLIN};
  if (poll(&p, 1, 1000) != 1)
    return -1;
  return accept(lfd, NULL, NULL);
}

/* ===================== net_listen_tcp ===================== */

TEST(net_listen_tcp_returns_listening_nonblock_socket)
{
  int fd = net_listen_tcp("127.0.0.1", 0, 16);
  REQUIRE(fd >= 0);
  CHECK(bound_port(fd) != 0);
  CHECK(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
  CHECK(fcntl(fd, F_GETFD, 0) & FD_CLOEXEC);
  CHECK(get_int_opt(fd, SOL_SOCKET, SO_REUSEADDR) != 0);
  int acc = get_int_opt(fd, SOL_SOCKET, SO_ACCEPTCONN);
  CHECK(acc != 0);
  close(fd);
}

TEST(net_listen_tcp_null_ip_binds_any)
{
  int fd = net_listen_tcp(NULL, 0, 16);
  REQUIRE(fd >= 0);
  struct sockaddr_in a;
  socklen_t len = sizeof a;
  getsockname(fd, (struct sockaddr *)&a, &len);
  CHECK_EQ_U(a.sin_addr.s_addr, htonl(INADDR_ANY));
  close(fd);
}

TEST(net_listen_tcp_invalid_ip_fails_with_einval)
{
  errno = 0;
  CHECK_EQ_I(net_listen_tcp("999.1.1.1", 0, 16), -1);
  CHECK_EQ_I(errno, EINVAL);
  errno = 0;
  CHECK_EQ_I(net_listen_tcp("localhost", 0, 16), -1); /* 이름 해석은 하지 않음 */
  CHECK_EQ_I(errno, EINVAL);
}

TEST(net_listen_tcp_port_in_use_fails_with_eaddrinuse)
{
  int a = net_listen_tcp("127.0.0.1", 0, 16);
  REQUIRE(a >= 0);
  errno = 0;
  int b = net_listen_tcp("127.0.0.1", bound_port(a), 16);
  CHECK_EQ_I(b, -1);
  CHECK_EQ_I(errno, EADDRINUSE); /* SO_REUSEADDR이어도 LISTEN 중인 포트는 가로채지 않음 */
  if (b >= 0)
    close(b);
  close(a);
}

TEST(net_listen_tcp_rebind_after_close_with_time_wait)
{
  /* 서버가 먼저 끊어 TIME_WAIT를 남긴 뒤 같은 포트로 다시 listen (A1 §1.3①) */
  int lfd = net_listen_tcp("127.0.0.1", 0, 16);
  REQUIRE(lfd >= 0);
  uint16_t port = bound_port(lfd);

  int c = net_connect_tcp("127.0.0.1", port);
  REQUIRE(c >= 0);
  int s = accept_wait(lfd);
  REQUIRE(s >= 0);
  close(s); /* 서버 측이 Active Closer → TIME_WAIT */
  char tmp;
  CHECK_EQ_I(read(c, &tmp, 1), 0); /* 클라이언트는 FIN 수신 */
  close(c);
  close(lfd);

  int again = net_listen_tcp("127.0.0.1", port, 16);
  CHECK(again >= 0);
  if (again >= 0)
    close(again);
}

/* ===================== net_connect_tcp ===================== */

TEST(net_connect_tcp_connects_and_transfers)
{
  int lfd = net_listen_tcp("127.0.0.1", 0, 16);
  REQUIRE(lfd >= 0);
  int c = net_connect_tcp("127.0.0.1", bound_port(lfd));
  REQUIRE(c >= 0);
  CHECK((fcntl(c, F_GETFL, 0) & O_NONBLOCK) == 0); /* 클라이언트는 blocking */
  int s = accept_wait(lfd);
  REQUIRE(s >= 0);

  CHECK_EQ_I(write(c, "ping", 4), 4);
  /* accept된 소켓은 BSD/macOS에서 리스너의 O_NONBLOCK을 물려받는다 → 도착을 기다린 뒤 읽음 */
  struct pollfd p = {.fd = s, .events = POLLIN};
  CHECK_EQ_I(poll(&p, 1, 1000), 1);
  char buf[4];
  CHECK_EQ_I(read(s, buf, 4), 4);
  CHECK_MEM(buf, "ping", 4);

  close(s);
  close(c);
  close(lfd);
}

TEST(net_connect_tcp_refused_fails_with_econnrefused)
{
  /* 잠깐 열었다 닫아서 아무도 listen하지 않는 포트를 얻는다 */
  int lfd = net_listen_tcp("127.0.0.1", 0, 1);
  REQUIRE(lfd >= 0);
  uint16_t port = bound_port(lfd);
  close(lfd);

  errno = 0;
  CHECK_EQ_I(net_connect_tcp("127.0.0.1", port), -1);
  CHECK_EQ_I(errno, ECONNREFUSED);
}

TEST(net_connect_tcp_rejects_empty_or_invalid_ip)
{
  errno = 0;
  CHECK_EQ_I(net_connect_tcp(NULL, 80), -1);
  CHECK_EQ_I(errno, EINVAL);
  errno = 0;
  CHECK_EQ_I(net_connect_tcp("not-an-ip", 80), -1);
  CHECK_EQ_I(errno, EINVAL);
}

/* ===================== 옵션 함수 ===================== */

TEST(net_set_nonblock_sets_flag_and_is_idempotent)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  CHECK_EQ_I(net_set_nonblock(fd), 0);
  CHECK(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
  CHECK_EQ_I(net_set_nonblock(fd), 0);
  close(fd);
}

TEST(net_set_nonblock_bad_fd_fails)
{
  errno = 0;
  CHECK_EQ_I(net_set_nonblock(-1), -1);
  CHECK_EQ_I(errno, EBADF);
}

TEST(net_set_nodelay_on_and_off)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  CHECK_EQ_I(net_set_nodelay(fd, 1), 0);
  CHECK(get_int_opt(fd, IPPROTO_TCP, TCP_NODELAY) != 0);
  CHECK_EQ_I(net_set_nodelay(fd, 0), 0);
  CHECK_EQ_I(get_int_opt(fd, IPPROTO_TCP, TCP_NODELAY), 0);
  close(fd);
}

TEST(net_set_nosigpipe_succeeds)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  CHECK_EQ_I(net_set_nosigpipe(fd), 0);
#ifdef SO_NOSIGPIPE
  CHECK(get_int_opt(fd, SOL_SOCKET, SO_NOSIGPIPE) != 0);
#endif
  close(fd);
}

TEST(net_tune_session_applies_all_options)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  CHECK_EQ_I(net_tune_session(fd), 0);
  CHECK(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
  CHECK(get_int_opt(fd, IPPROTO_TCP, TCP_NODELAY) != 0);
  close(fd);
}

TEST(net_tune_session_bad_fd_fails)
{
  CHECK_EQ_I(net_tune_session(-1), -1);
}

/* ===================== net_abort_close ===================== */

TEST(net_abort_close_sends_rst)
{
  int lfd = net_listen_tcp("127.0.0.1", 0, 16);
  REQUIRE(lfd >= 0);
  int c = net_connect_tcp("127.0.0.1", bound_port(lfd));
  REQUIRE(c >= 0);
  int s = accept_wait(lfd);
  REQUIRE(s >= 0);

  net_abort_close(c);

  /* FIN이었다면 read가 0, RST면 ECONNRESET */
  struct pollfd p = {.fd = s, .events = POLLIN};
  CHECK_EQ_I(poll(&p, 1, 1000), 1);
  char buf[1];
  errno = 0;
  ssize_t n = read(s, buf, 1);
  CHECK_EQ_I(n, -1);
  CHECK_EQ_I(errno, ECONNRESET);

  close(s);
  close(lfd);
}

TEST(net_abort_close_negative_fd_is_noop)
{
  net_abort_close(-1);
}
