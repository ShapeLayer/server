/* test_session.c */
#include <arpa/inet.h>
#include <netinet/in.h>

#include "core/packet.h"
#include "core/session.h"
#include "framework/test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ===================== recorder ===================== */

#define MAX_REC 4096

typedef struct {
  int              count;
  uint16_t         ids[MAX_REC];
  size_t           lens[MAX_REC];
  uint8_t          last[PKT_MAX_SIZE];
  int              closed;
  session_reason_t reason;
  int              echo;          /* echo every packet back */
  int              close_after;   /* mark closing after N packets (0 = never) */
} rec_t;

static void rec_packet(session_t *s, uint16_t id, const uint8_t *p, size_t len, void *ctx)
{
  rec_t *r = ctx;
  if (r->count < MAX_REC) {
    r->ids[r->count]  = id;
    r->lens[r->count] = len;
  }
  memcpy(r->last, p, len);
  r->count++;
  if (r->echo)
    session_send(s, id, p, len);
  if (r->close_after && r->count >= r->close_after)
    session_mark_closing(s);
}

static void rec_close(session_t *s, session_reason_t reason, void *ctx)
{
  (void)s;
  rec_t *r = ctx;
  r->closed++;
  r->reason = reason;
}

static const session_handler_t HANDLER = {rec_packet, rec_close};

/* ===================== helpers ===================== */

typedef struct {
  int        peer; /* the "client" side */
  session_t *s;
  rec_t      rec;
} fixture_t;

static int setup(fixture_t *f)
{
  memset(f, 0, sizeof *f);
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    return -1;
  fcntl(sv[0], F_SETFL, O_NONBLOCK);
  fcntl(sv[1], F_SETFL, O_NONBLOCK);
  f->peer = sv[1];
  f->s    = session_create(sv[0], &HANDLER, &f->rec);
  return f->s ? 0 : -1;
}

static void teardown(fixture_t *f)
{
  if (f->s)
    session_destroy(f->s, 0);
  if (f->peer >= 0)
    close(f->peer);
}

static size_t make_packet(uint8_t *out, uint16_t id, size_t plen, uint8_t seed)
{
  pkt_encode_header(out, (uint16_t)(PKT_HEADER_SIZE + plen), id);
  for (size_t i = 0; i < plen; i++)
    out[PKT_HEADER_SIZE + i] = (uint8_t)(seed + i);
  return PKT_HEADER_SIZE + plen;
}

/* read everything currently available from fd */
static size_t drain(int fd, uint8_t *buf, size_t cap)
{
  size_t total = 0;
  for (;;) {
    ssize_t n = read(fd, buf + total, cap - total);
    if (n <= 0)
      break;
    total += (size_t)n;
    if (total == cap)
      break;
  }
  return total;
}

/* ===================== create / destroy ===================== */

TEST(session_create_rejects_bad_args)
{
  errno = 0;
  CHECK(session_create(-1, &HANDLER, NULL) == NULL);
  CHECK_EQ_I(errno, EINVAL);
  errno = 0;
  CHECK(session_create(0, NULL, NULL) == NULL);
  CHECK_EQ_I(errno, EINVAL);
}

TEST(session_create_initial_state)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  CHECK(session_fd(f.s) >= 0);
  CHECK_EQ_I(session_wants_write(f.s), 0);
  CHECK_EQ_I(session_is_closing(f.s), 0);
  CHECK_EQ_I(session_close_reason(f.s), SESS_REASON_NONE);
  teardown(&f);
}

TEST(session_destroy_calls_on_close_once_and_closes_fd)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  session_destroy(f.s, 0);
  f.s = NULL;
  CHECK_EQ_I(f.rec.closed, 1);
  char c;
  CHECK_EQ_I(read(f.peer, &c, 1), 0); /* peer sees FIN */
  teardown(&f);
}

TEST(session_destroy_abortive_sends_rst)
{
  /* AF_UNIX에는 RST가 없으므로 TCP로 확인 */
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(lfd >= 0);
  struct sockaddr_in a = {0};
  a.sin_family      = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE_EQ_I(bind(lfd, (struct sockaddr *)&a, sizeof a), 0);
  REQUIRE_EQ_I(listen(lfd, 1), 0);
  socklen_t len = sizeof a;
  getsockname(lfd, (struct sockaddr *)&a, &len);

  int c = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_EQ_I(connect(c, (struct sockaddr *)&a, sizeof a), 0);
  int sfd = accept(lfd, NULL, NULL);
  REQUIRE(sfd >= 0);

  rec_t      r = {0};
  session_t *s = session_create(sfd, &HANDLER, &r);
  REQUIRE(s != NULL);
  session_destroy(s, 1);

  char buf[1];
  errno = 0;
  CHECK_EQ_I(read(c, buf, 1), -1);
  CHECK_EQ_I(errno, ECONNRESET);
  close(c);
  close(lfd);
}

/* ===================== on_readable ===================== */

TEST(session_readable_single_packet)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  uint8_t pkt[64];
  size_t  n = make_packet(pkt, 1001, 10, 0x40);
  REQUIRE_EQ_I(write(f.peer, pkt, n), (long long)n);

  CHECK_EQ_I(session_on_readable(f.s), SESS_OK);
  REQUIRE_EQ_I(f.rec.count, 1);
  CHECK_EQ_U(f.rec.ids[0], 1001);
  CHECK_EQ_U(f.rec.lens[0], 10);
  CHECK_MEM(f.rec.last, pkt + PKT_HEADER_SIZE, 10);
  teardown(&f);
}

TEST(session_readable_without_data_is_ok)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  CHECK_EQ_I(session_on_readable(f.s), SESS_OK); /* EAGAIN */
  CHECK_EQ_I(f.rec.count, 0);
  teardown(&f);
}

TEST(session_readable_fragmented_one_byte_at_a_time)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  uint8_t pkt[64];
  size_t  n = make_packet(pkt, 7, 20, 0);
  for (size_t i = 0; i < n; i++) {
    write(f.peer, pkt + i, 1);
    CHECK_EQ_I(session_on_readable(f.s), SESS_OK);
    CHECK_EQ_I(f.rec.count, i + 1 == n ? 1 : 0);
  }
  CHECK_MEM(f.rec.last, pkt + PKT_HEADER_SIZE, 20);
  teardown(&f);
}

TEST(session_readable_coalesced_packets)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  uint8_t stream[512];
  size_t  off = 0;
  for (int i = 0; i < 10; i++)
    off += make_packet(stream + off, (uint16_t)i, (size_t)i * 3, (uint8_t)i);
  REQUIRE_EQ_I(write(f.peer, stream, off), (long long)off);

  CHECK_EQ_I(session_on_readable(f.s), SESS_OK);
  REQUIRE_EQ_I(f.rec.count, 10);
  for (int i = 0; i < 10; i++) {
    CHECK_EQ_U(f.rec.ids[i], i);
    CHECK_EQ_U(f.rec.lens[i], i * 3);
  }
  teardown(&f);
}

TEST(session_readable_more_than_rx_capacity_in_one_call)
{
  /* rx보다 많은 데이터가 커널에 쌓여 있어도 한 번의 호출로 전부 처리 */
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  enum { PLEN = 1000 };
  int bufsz = (int)SESSION_RX_CAPACITY * 4; /* macOS AF_UNIX 기본 버퍼(8KB)는 rx보다 작음 */
  setsockopt(f.peer, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof bufsz);
  setsockopt(session_fd(f.s), SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof bufsz);
  uint8_t pkt[PKT_HEADER_SIZE + PLEN];
  size_t  n     = make_packet(pkt, 5, PLEN, 0);
  int     sent  = 0;
  size_t  bytes = 0;
  while (bytes < SESSION_RX_CAPACITY * 3 && write(f.peer, pkt, n) == (ssize_t)n) {
    sent++;
    bytes += n;
  }
  REQUIRE(bytes > SESSION_RX_CAPACITY);
  CHECK_EQ_I(session_on_readable(f.s), SESS_OK);
  CHECK_EQ_I(f.rec.count, sent);
  teardown(&f);
}

TEST(session_readable_eof_returns_close)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  close(f.peer);
  f.peer = -1;
  CHECK_EQ_I(session_on_readable(f.s), SESS_CLOSE);
  CHECK_EQ_I(session_close_reason(f.s), SESS_REASON_EOF);
  CHECK(session_is_closing(f.s));
  teardown(&f);
  CHECK_EQ_I(f.rec.reason, SESS_REASON_EOF);
}

TEST(session_readable_delivers_data_before_eof)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  uint8_t pkt[32];
  size_t  n = make_packet(pkt, 9, 4, 0);
  write(f.peer, pkt, n);
  close(f.peer);
  f.peer = -1;
  CHECK_EQ_I(session_on_readable(f.s), SESS_CLOSE);
  CHECK_EQ_I(f.rec.count, 1); /* 닫기 전에 보낸 패킷은 처리됨 */
  teardown(&f);
}

TEST(session_readable_invalid_header_returns_close)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  uint8_t bad[PKT_HEADER_SIZE];
  pkt_encode_header(bad, 0, 1); /* size 0 */
  write(f.peer, bad, sizeof bad);
  CHECK_EQ_I(session_on_readable(f.s), SESS_CLOSE);
  CHECK_EQ_I(session_close_reason(f.s), SESS_REASON_INVALID);
  CHECK_EQ_I(f.rec.count, 0);
  teardown(&f);
}

TEST(session_readable_valid_packets_before_invalid_are_delivered)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  uint8_t stream[64];
  size_t  off = make_packet(stream, 1, 2, 0);
  pkt_encode_header(stream + off, 0xFFFF, 2);
  off += PKT_HEADER_SIZE;
  write(f.peer, stream, off);
  CHECK_EQ_I(session_on_readable(f.s), SESS_CLOSE);
  CHECK_EQ_I(f.rec.count, 1);
  CHECK_EQ_I(session_close_reason(f.s), SESS_REASON_INVALID);
  teardown(&f);
}

TEST(session_readable_stops_when_handler_marks_closing)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  f.rec.close_after = 2;
  uint8_t stream[128];
  size_t  off = 0;
  for (int i = 0; i < 5; i++)
    off += make_packet(stream + off, (uint16_t)i, 1, 0);
  write(f.peer, stream, off);
  CHECK_EQ_I(session_on_readable(f.s), SESS_CLOSE);
  CHECK_EQ_I(f.rec.count, 2); /* 나머지 3개는 처리하지 않음 */
  CHECK_EQ_I(session_close_reason(f.s), SESS_REASON_LOCAL);
  teardown(&f);
}

TEST(session_readable_after_closing_does_nothing)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  session_mark_closing(f.s);
  uint8_t pkt[16];
  write(f.peer, pkt, make_packet(pkt, 1, 0, 0));
  CHECK_EQ_I(session_on_readable(f.s), SESS_CLOSE);
  CHECK_EQ_I(f.rec.count, 0);
  teardown(&f);
}

/* ===================== session_send ===================== */

TEST(session_send_queues_without_syscall)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  CHECK_EQ_I(session_send(f.s, 42, (const uint8_t *)"hi", 2), 0);
  CHECK_EQ_I(session_wants_write(f.s), 1);
  uint8_t buf[16];
  CHECK_EQ_U(drain(f.peer, buf, sizeof buf), 0); /* 아직 아무것도 안 나감 */
  teardown(&f);
}

TEST(session_send_rejects_oversized_payload)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  static uint8_t big[PKT_MAX_SIZE];
  errno = 0;
  CHECK_EQ_I(session_send(f.s, 1, big, PKT_MAX_SIZE - PKT_HEADER_SIZE + 1), -1);
  CHECK_EQ_I(errno, EMSGSIZE);
  CHECK_EQ_I(session_send(f.s, 1, big, PKT_MAX_SIZE - PKT_HEADER_SIZE), 0); /* 경계값은 허용 */
  teardown(&f);
}

TEST(session_send_zero_payload)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  CHECK_EQ_I(session_send(f.s, 3, NULL, 0), 0);
  CHECK_EQ_I(session_on_writable(f.s), SESS_OK);
  uint8_t buf[8];
  REQUIRE_EQ_U(drain(f.peer, buf, sizeof buf), PKT_HEADER_SIZE);
  pkt_header_t h;
  pkt_decode_header(buf, &h);
  CHECK_EQ_U(h.size, PKT_HEADER_SIZE);
  CHECK_EQ_U(h.id, 3);
  teardown(&f);
}

TEST(session_send_all_or_nothing_when_tx_full)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  static uint8_t payload[1000];
  int            ok = 0;
  while (session_send(f.s, 1, payload, sizeof payload) == 0)
    ok++;
  CHECK_EQ_I(errno, ENOBUFS);
  CHECK(ok > 0);

  /* 큐에 들어간 바이트가 정확히 패킷 단위인지: 보내서 받아 보면 ok개가 온전히 옴 */
  size_t expect = (size_t)ok * (PKT_HEADER_SIZE + sizeof payload);
  CHECK(expect <= SESSION_TX_CAPACITY);
  teardown(&f);
}

TEST(session_send_after_closing_fails_with_epipe)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  session_mark_closing(f.s);
  errno = 0;
  CHECK_EQ_I(session_send(f.s, 1, (const uint8_t *)"x", 1), -1);
  CHECK_EQ_I(errno, EPIPE);
  teardown(&f);
}

/* ===================== on_writable ===================== */

TEST(session_writable_flushes_queue)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  session_send(f.s, 1, (const uint8_t *)"abc", 3);
  session_send(f.s, 2, (const uint8_t *)"de", 2);
  CHECK_EQ_I(session_on_writable(f.s), SESS_OK);
  CHECK_EQ_I(session_wants_write(f.s), 0);

  uint8_t buf[64];
  size_t  n = drain(f.peer, buf, sizeof buf);
  REQUIRE_EQ_U(n, (PKT_HEADER_SIZE + 3) + (PKT_HEADER_SIZE + 2));
  pkt_header_t h;
  pkt_decode_header(buf, &h);
  CHECK_EQ_U(h.id, 1);
  CHECK_MEM(buf + PKT_HEADER_SIZE, "abc", 3);
  pkt_decode_header(buf + 7, &h);
  CHECK_EQ_U(h.id, 2);
  CHECK_MEM(buf + 7 + PKT_HEADER_SIZE, "de", 2);
  teardown(&f);
}

TEST(session_writable_with_empty_queue_is_ok)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  CHECK_EQ_I(session_on_writable(f.s), SESS_OK);
  teardown(&f);
}

TEST(session_writable_partial_when_peer_does_not_read)
{
  /* 상대가 읽지 않으면 커널 버퍼가 차고, 남은 데이터는 tx에 유지된다 */
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  static uint8_t payload[1000];
  while (session_send(f.s, 1, payload, sizeof payload) == 0) {
  }

  int rounds = 0;
  while (rounds++ < 64) {
    CHECK_EQ_I(session_on_writable(f.s), SESS_OK);
    if (session_wants_write(f.s))
      break; /* EAGAIN: 커널 송신 버퍼 가득 참 */
    while (session_send(f.s, 1, payload, sizeof payload) == 0) {
    }
  }
  CHECK_EQ_I(session_wants_write(f.s), 1);
  teardown(&f);
}

TEST(session_writable_byte_stream_is_intact_across_partial_writes)
{
  /* 송신과 수신을 번갈아 하며 많은 패킷을 보내고, 받은 스트림을 다시 파싱해 비교 */
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  enum { N = 3000 };
  ringbuf_t rx;
  REQUIRE_EQ_I(rb_init(&rx, 1u << 16), 0);

  srand(777);
  int     queued = 0, received = 0;
  uint8_t payload[512], out[PKT_MAX_SIZE], tmp[4096];
  while (received < N) {
    while (queued < N) {
      size_t plen = (size_t)(rand() % 512);
      for (size_t i = 0; i < plen; i++)
        payload[i] = (uint8_t)((size_t)queued + i);
      if (session_send(f.s, (uint16_t)queued, payload, plen) != 0)
        break;
      queued++;
    }
    if (session_on_writable(f.s) != SESS_OK) {
      FAIL("writable returned CLOSE");
      break;
    }
    ssize_t n = read(f.peer, tmp, (size_t)(rand() % (int)sizeof tmp) + 1);
    if (n > 0)
      rb_write(&rx, tmp, (size_t)n);

    pkt_header_t h;
    while (pkt_extract(&rx, &h, out, sizeof out) == PKT_OK) {
      size_t plen = (size_t)h.size - PKT_HEADER_SIZE;
      if (h.id != received) {
        CHECK_EQ_U(h.id, received);
        goto out;
      }
      for (size_t i = 0; i < plen; i++)
        if (out[i] != (uint8_t)((size_t)received + i)) {
          FAIL("payload mismatch");
          goto out;
        }
      received++;
    }
  }
  CHECK_EQ_I(received, N);
out:
  rb_free(&rx);
  teardown(&f);
}

TEST(session_writable_peer_closed_returns_close)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  close(f.peer);
  f.peer = -1;
  session_send(f.s, 1, (const uint8_t *)"x", 1);
  /* SIGPIPE 없이 EPIPE를 받아야 함 (macOS: SO_NOSIGPIPE 없으면 여기서 프로세스 종료) */
  CHECK_EQ_I(session_on_writable(f.s), SESS_CLOSE);
  CHECK_EQ_I(session_close_reason(f.s), SESS_REASON_IO_ERROR);
  teardown(&f);
}

/* ===================== echo (readable + send + writable) ===================== */

TEST(session_echo_roundtrip)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  f.rec.echo = 1;
  uint8_t pkt[64];
  size_t  n = make_packet(pkt, 77, 30, 0x11);
  write(f.peer, pkt, n);

  CHECK_EQ_I(session_on_readable(f.s), SESS_OK);
  CHECK_EQ_I(session_wants_write(f.s), 1);
  CHECK_EQ_I(session_on_writable(f.s), SESS_OK);

  uint8_t back[64];
  REQUIRE_EQ_U(drain(f.peer, back, sizeof back), n);
  CHECK_MEM(back, pkt, n);
  teardown(&f);
}

/* ===================== mark_closing ===================== */

TEST(session_mark_closing_keeps_first_reason)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  close(f.peer);
  f.peer = -1;
  session_on_readable(f.s); /* EOF */
  session_mark_closing(f.s);
  CHECK_EQ_I(session_close_reason(f.s), SESS_REASON_EOF);
  teardown(&f);
}

/* ===================== 핸들러 수명 ===================== */

TEST(session_handler_with_null_callbacks_is_allowed)
{
  int sv[2];
  REQUIRE_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  fcntl(sv[0], F_SETFL, O_NONBLOCK);
  static const session_handler_t empty = {NULL, NULL};
  session_t *s = session_create(sv[0], &empty, NULL);
  REQUIRE(s != NULL);
  uint8_t pkt[16];
  write(sv[1], pkt, make_packet(pkt, 1, 2, 0));
  CHECK_EQ_I(session_on_readable(s), SESS_OK); /* on_packet이 NULL이어도 크래시 없음 */
  session_destroy(s, 0);                       /* on_close가 NULL이어도 크래시 없음 */
  close(sv[1]);
}

TEST(session_ctx_is_passed_to_callbacks)
{
  fixture_t f;
  REQUIRE_EQ_I(setup(&f), 0);
  uint8_t pkt[16];
  write(f.peer, pkt, make_packet(pkt, 1, 0, 0));
  CHECK_EQ_I(session_on_readable(f.s), SESS_OK);
  CHECK_EQ_I(f.rec.count, 1); /* rec_packet은 ctx(&f.rec)로 기록함 */
  teardown(&f);
  CHECK_EQ_I(f.rec.closed, 1);
}
