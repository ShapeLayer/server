/* test_poller.c */

/**
 * poller는 OS별 구현(poller_epoll.c / poller_kqueue.c)과 무관하게 같은 계약을 지켜야 한다.
 * 네트워크 없이 socketpair(AF_UNIX)로 연결된 소켓 쌍을 사용한다.
 */

#include "core/poller.h"
#include "framework/test.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAXEV 16

/* udata로 쓸 서로 다른 주소 */
static int TAG_A, TAG_B;

/* ===================== helpers ===================== */

/* non-blocking 소켓 쌍을 만든다. sv[0]을 poller에 등록하고 sv[1]로 자극한다 */
static int make_pair(int sv[2])
{
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    return -1;
  fcntl(sv[0], F_SETFL, O_NONBLOCK);
  fcntl(sv[1], F_SETFL, O_NONBLOCK);
  return 0;
}

static void close_pair(int sv[2])
{
  close(sv[0]);
  close(sv[1]);
}

/*
 * 한 번 wait한 결과에서 udata에 해당하는 이벤트 비트를 OR로 합친다.
 * kqueue는 읽기/쓰기를 별도 항목으로 돌려주므로 같은 udata가 두 번 나올 수 있다.
 */
static int wait_mask(poller_t *p, void *udata, int timeout_ms)
{
  poll_event_t evs[MAXEV];
  int          n    = poller_wait(p, evs, MAXEV, timeout_ms);
  int          mask = 0;
  for (int i = 0; i < n; i++)
    if (evs[i].udata == udata)
      mask |= evs[i].events;
  return mask;
}

/* 송신 버퍼가 가득 찰 때까지 쓴다 (EAGAIN) */
static void fill_send_buffer(int fd)
{
  char junk[4096] = {0};
  while (write(fd, junk, sizeof junk) > 0) {
  }
}

/* ===================== create / destroy ===================== */

TEST(poller_create_returns_instance)
{
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  poller_destroy(p);
}

TEST(poller_destroy_null_is_noop)
{
  poller_destroy(NULL);
}

TEST(poller_multiple_instances_are_independent)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p1 = poller_create();
  poller_t *p2 = poller_create();
  REQUIRE(p1 != NULL);
  REQUIRE(p2 != NULL);
  REQUIRE_EQ_I(poller_add(p1, sv[0], &TAG_A, POLLER_IN), 0);
  write(sv[1], "x", 1);
  CHECK(wait_mask(p1, &TAG_A, 1000) & POLLER_IN);

  poll_event_t evs[MAXEV];
  CHECK_EQ_I(poller_wait(p2, evs, MAXEV, 0), 0); /* 등록하지 않은 쪽은 조용함 */
  poller_destroy(p1);
  poller_destroy(p2);
  close_pair(sv);
}

/* ===================== poller_wait ===================== */

TEST(poller_wait_empty_times_out_immediately)
{
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  poll_event_t evs[MAXEV];
  CHECK_EQ_I(poller_wait(p, evs, MAXEV, 0), 0);
  poller_destroy(p);
}

TEST(poller_wait_empty_times_out_after_delay)
{
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  poll_event_t evs[MAXEV];
  CHECK_EQ_I(poller_wait(p, evs, MAXEV, 20), 0);
  poller_destroy(p);
}

TEST(poller_wait_rejects_non_positive_max_events)
{
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  poll_event_t evs[1];
  errno = 0;
  CHECK_EQ_I(poller_wait(p, evs, 0, 0), -1);
  CHECK_EQ_I(errno, EINVAL);
  errno = 0;
  CHECK_EQ_I(poller_wait(p, evs, -1, 0), -1);
  CHECK_EQ_I(errno, EINVAL);
  poller_destroy(p);
}

TEST(poller_wait_respects_max_events)
{
  enum { N = 4 };
  int       sv[N][2];
  int       tags[N];
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  for (int i = 0; i < N; i++) {
    REQUIRE_EQ_I(make_pair(sv[i]), 0);
    REQUIRE_EQ_I(poller_add(p, sv[i][0], &tags[i], POLLER_IN), 0);
    write(sv[i][1], "x", 1);
  }

  poll_event_t evs[2];
  CHECK_EQ_I(poller_wait(p, evs, 2, 1000), 2);

  /* Level-Triggered라 남은 것도 그대로 남아 있다: 큰 배열로 받으면 모두 보임 */
  poll_event_t all[MAXEV];
  CHECK_EQ_I(poller_wait(p, all, MAXEV, 0), N);

  poller_destroy(p);
  for (int i = 0; i < N; i++)
    close_pair(sv[i]);
}

TEST(poller_wait_grows_internal_buffer)
{
  /* 처음에 작은 max_events로 호출한 뒤 큰 값으로 호출해도 정상 동작 (realloc 경로) */
  enum { N = 8 };
  int       sv[N][2];
  int       tags[N];
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  for (int i = 0; i < N; i++) {
    REQUIRE_EQ_I(make_pair(sv[i]), 0);
    REQUIRE_EQ_I(poller_add(p, sv[i][0], &tags[i], POLLER_IN), 0);
    write(sv[i][1], "x", 1);
  }

  poll_event_t evs[MAXEV];
  CHECK_EQ_I(poller_wait(p, evs, 1, 1000), 1);
  CHECK_EQ_I(poller_wait(p, evs, MAXEV, 0), N);

  /* 모든 udata가 정확히 한 번씩 나와야 함 (IN만 등록했으므로 fd당 1개) */
  int seen[N] = {0};
  int n       = poller_wait(p, evs, MAXEV, 0);
  for (int i = 0; i < n; i++)
    for (int k = 0; k < N; k++)
      if (evs[i].udata == &tags[k])
        seen[k]++;
  for (int k = 0; k < N; k++)
    CHECK_EQ_I(seen[k], 1);

  poller_destroy(p);
  for (int i = 0; i < N; i++)
    close_pair(sv[i]);
}

/* ===================== POLLER_IN ===================== */

TEST(poller_in_not_reported_before_data)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN), 0);
  CHECK_EQ_I(wait_mask(p, &TAG_A, 0), 0);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_in_reported_with_udata_after_write)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN), 0);
  CHECK_EQ_I(write(sv[1], "x", 1), 1);

  poll_event_t evs[MAXEV];
  int          n = poller_wait(p, evs, MAXEV, 1000);
  REQUIRE_EQ_I(n, 1);
  CHECK_EQ_PTR(evs[0].udata, &TAG_A);
  CHECK(evs[0].events & POLLER_IN);
  CHECK_EQ_I(evs[0].events & (POLLER_OUT | POLLER_ERR | POLLER_HUP), 0);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_in_is_level_triggered)
{
  /* 데이터를 읽지 않으면 다음 wait에서도 다시 보고된다 */
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN), 0);
  write(sv[1], "xy", 2);
  CHECK(wait_mask(p, &TAG_A, 1000) & POLLER_IN);
  CHECK(wait_mask(p, &TAG_A, 0) & POLLER_IN);

  char c;
  CHECK_EQ_I(read(sv[0], &c, 1), 1); /* 일부만 읽으면 계속 보고 */
  CHECK(wait_mask(p, &TAG_A, 0) & POLLER_IN);

  CHECK_EQ_I(read(sv[0], &c, 1), 1); /* 다 비우면 더 이상 보고되지 않음 */
  CHECK_EQ_I(wait_mask(p, &TAG_A, 0) & POLLER_IN, 0);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_in_reports_only_ready_fd)
{
  int s1[2], s2[2];
  REQUIRE_EQ_I(make_pair(s1), 0);
  REQUIRE_EQ_I(make_pair(s2), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, s1[0], &TAG_A, POLLER_IN), 0);
  REQUIRE_EQ_I(poller_add(p, s2[0], &TAG_B, POLLER_IN), 0);
  write(s2[1], "x", 1);

  poll_event_t evs[MAXEV];
  int          n = poller_wait(p, evs, MAXEV, 1000);
  REQUIRE_EQ_I(n, 1);
  CHECK_EQ_PTR(evs[0].udata, &TAG_B);
  poller_destroy(p);
  close_pair(s1);
  close_pair(s2);
}

TEST(poller_in_works_with_null_udata)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], NULL, POLLER_IN), 0);
  write(sv[1], "x", 1);
  poll_event_t evs[MAXEV];
  REQUIRE_EQ_I(poller_wait(p, evs, MAXEV, 1000), 1);
  CHECK_EQ_PTR(evs[0].udata, NULL);
  poller_destroy(p);
  close_pair(sv);
}

/* ===================== POLLER_OUT ===================== */

TEST(poller_out_reported_on_fresh_socket)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_OUT), 0);
  int m = wait_mask(p, &TAG_A, 1000);
  CHECK(m & POLLER_OUT);
  CHECK_EQ_I(m & POLLER_IN, 0);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_out_not_reported_when_send_buffer_full)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  fill_send_buffer(sv[0]);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_OUT), 0);
  CHECK_EQ_I(wait_mask(p, &TAG_A, 0) & POLLER_OUT, 0);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_out_reported_again_after_peer_drains)
{
  /* Zero Window가 풀리는 상황: 상대가 읽어 가면 다시 쓸 수 있게 됨 */
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  fill_send_buffer(sv[0]);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_OUT), 0);
  CHECK_EQ_I(wait_mask(p, &TAG_A, 0) & POLLER_OUT, 0);

  char drain[4096];
  while (read(sv[1], drain, sizeof drain) > 0) {
  }
  CHECK(wait_mask(p, &TAG_A, 1000) & POLLER_OUT);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_in_and_out_together)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN | POLLER_OUT), 0);
  write(sv[1], "x", 1);
  int m = wait_mask(p, &TAG_A, 1000);
  CHECK(m & POLLER_IN);
  CHECK(m & POLLER_OUT);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_add_with_no_interest_reports_nothing)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, 0), 0);
  write(sv[1], "x", 1);
  CHECK_EQ_I(wait_mask(p, &TAG_A, 0), 0);
  poller_destroy(p);
  close_pair(sv);
}

/* ===================== poller_mod ===================== */

TEST(poller_mod_removes_out_interest)
{
  /* tx를 다 보낸 뒤 OUT을 끄는 상황: 끄지 않으면 busy loop */
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN | POLLER_OUT), 0);
  CHECK(wait_mask(p, &TAG_A, 1000) & POLLER_OUT);
  REQUIRE_EQ_I(poller_mod(p, sv[0], &TAG_A, POLLER_IN), 0);
  CHECK_EQ_I(wait_mask(p, &TAG_A, 0), 0);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_mod_adds_out_interest)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN), 0);
  CHECK_EQ_I(wait_mask(p, &TAG_A, 0), 0);
  REQUIRE_EQ_I(poller_mod(p, sv[0], &TAG_A, POLLER_IN | POLLER_OUT), 0);
  CHECK(wait_mask(p, &TAG_A, 1000) & POLLER_OUT);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_mod_removes_in_interest)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN), 0);
  write(sv[1], "x", 1);
  CHECK(wait_mask(p, &TAG_A, 1000) & POLLER_IN);
  REQUIRE_EQ_I(poller_mod(p, sv[0], &TAG_A, 0), 0);
  CHECK_EQ_I(wait_mask(p, &TAG_A, 0) & POLLER_IN, 0);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_mod_replaces_udata)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN | POLLER_OUT), 0);
  REQUIRE_EQ_I(poller_mod(p, sv[0], &TAG_B, POLLER_IN | POLLER_OUT), 0);
  write(sv[1], "x", 1);

  poll_event_t evs[MAXEV];
  int          n = poller_wait(p, evs, MAXEV, 1000);
  REQUIRE(n >= 1);
  for (int i = 0; i < n; i++) /* kqueue는 읽기/쓰기 두 항목 모두 갱신되어야 함 */
    CHECK_EQ_PTR(evs[i].udata, &TAG_B);
  poller_destroy(p);
  close_pair(sv);
}

/* ===================== poller_del ===================== */

TEST(poller_del_stops_events)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN | POLLER_OUT), 0);
  REQUIRE_EQ_I(poller_del(p, sv[0]), 0);
  write(sv[1], "x", 1);
  poll_event_t evs[MAXEV];
  CHECK_EQ_I(poller_wait(p, evs, MAXEV, 0), 0);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_del_then_add_again)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN), 0);
  REQUIRE_EQ_I(poller_del(p, sv[0]), 0);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_B, POLLER_IN), 0);
  write(sv[1], "x", 1);
  poll_event_t evs[MAXEV];
  REQUIRE_EQ_I(poller_wait(p, evs, MAXEV, 1000), 1);
  CHECK_EQ_PTR(evs[0].udata, &TAG_B);
  poller_destroy(p);
  close_pair(sv);
}

TEST(poller_del_one_keeps_others)
{
  int s1[2], s2[2];
  REQUIRE_EQ_I(make_pair(s1), 0);
  REQUIRE_EQ_I(make_pair(s2), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, s1[0], &TAG_A, POLLER_IN), 0);
  REQUIRE_EQ_I(poller_add(p, s2[0], &TAG_B, POLLER_IN), 0);
  REQUIRE_EQ_I(poller_del(p, s1[0]), 0);
  write(s1[1], "x", 1);
  write(s2[1], "x", 1);
  poll_event_t evs[MAXEV];
  REQUIRE_EQ_I(poller_wait(p, evs, MAXEV, 1000), 1);
  CHECK_EQ_PTR(evs[0].udata, &TAG_B);
  poller_destroy(p);
  close_pair(s1);
  close_pair(s2);
}

TEST(poller_del_unregistered_fd_fails_with_enoent)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  errno = 0;
  CHECK_EQ_I(poller_del(p, sv[0]), -1);
  CHECK_EQ_I(errno, ENOENT);
  poller_destroy(p);
  close_pair(sv);
}

/* ===================== 에러 경로 ===================== */

TEST(poller_add_bad_fd_fails_with_ebadf)
{
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  errno = 0;
  CHECK_EQ_I(poller_add(p, -1, &TAG_A, POLLER_IN), -1);
  CHECK_EQ_I(errno, EBADF);
  poller_destroy(p);
}

TEST(poller_mod_bad_fd_fails)
{
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  CHECK_EQ_I(poller_mod(p, -1, &TAG_A, POLLER_IN), -1);
  poller_destroy(p);
}

/* ===================== HUP ===================== */

TEST(poller_hup_and_in_when_peer_closes)
{
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN), 0);
  close(sv[1]);

  int m = wait_mask(p, &TAG_A, 1000);
  CHECK(m & POLLER_HUP);
  CHECK(m & POLLER_IN); /* read하면 0(EOF)이 나오므로 IN도 함께 와야 함 */
  char c;
  CHECK_EQ_I(read(sv[0], &c, 1), 0);
  poller_destroy(p);
  close(sv[0]);
}

TEST(poller_pending_data_readable_after_peer_closes)
{
  /* 상대가 데이터를 보내고 바로 닫아도, 남은 데이터를 먼저 읽을 수 있어야 함 */
  int sv[2];
  REQUIRE_EQ_I(make_pair(sv), 0);
  poller_t *p = poller_create();
  REQUIRE(p != NULL);
  REQUIRE_EQ_I(poller_add(p, sv[0], &TAG_A, POLLER_IN), 0);
  write(sv[1], "bye", 3);
  close(sv[1]);

  int m = wait_mask(p, &TAG_A, 1000);
  CHECK(m & POLLER_IN);
  char buf[8];
  CHECK_EQ_I(read(sv[0], buf, sizeof buf), 3);
  CHECK_MEM(buf, "bye", 3);
  CHECK_EQ_I(read(sv[0], buf, sizeof buf), 0);
  poller_destroy(p);
  close(sv[0]);
}
