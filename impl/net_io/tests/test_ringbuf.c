/* test_ringbuf.c */

#include "core/ringbuf.h"
#include "framework/test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 0, 1, 2, ... 순서로 채운 패턴. 래핑 후 바이트 순서 확인용 */
static void fill_seq(uint8_t *buf, size_t n, uint8_t start)
{
  for (size_t i = 0; i < n; i++)
    buf[i] = (uint8_t)(start + i);
}

/* 화이트박스: 카운터를 임의 위치로 옮긴다 (빈 상태에서만 사용) */
static void set_pos(ringbuf_t *rb, size_t pos)
{
  rb->rpos = rb->wpos = pos;
}

/* ===================== rb_init / rb_free ===================== */

TEST(rb_init_accepts_power_of_two)
{
  size_t caps[] = {1, 2, 8, 4096, 1u << 20};
  for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
  {
    ringbuf_t rb;
    REQUIRE_EQ_I(rb_init(&rb, caps[i]), 0);
    CHECK(rb.data != NULL);
    CHECK_EQ_U(rb.capacity, caps[i]);
    CHECK_EQ_U(rb.rpos, 0);
    CHECK_EQ_U(rb.wpos, 0);
    rb_free(&rb);
  }
}

TEST(rb_init_rejects_zero_and_non_power_of_two)
{
  size_t caps[] = {0, 3, 6, 100, 4095};
  for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
  {
    ringbuf_t rb;
    CHECK_EQ_I(rb_init(&rb, caps[i]), -1);
  }
}

TEST(rb_free_resets_fields)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "abc", 3);
  rb_free(&rb);
  CHECK(rb.data == NULL);
  CHECK_EQ_U(rb.capacity, 0);
  CHECK_EQ_U(rb.rpos, 0);
  CHECK_EQ_U(rb.wpos, 0);
}

TEST(rb_free_twice_is_safe)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_free(&rb);
  rb_free(&rb); /* free(NULL) → no-op */
}

/* ===================== rb_size / rb_space ===================== */

TEST(rb_size_space_empty)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 16), 0);
  CHECK_EQ_U(rb_size(&rb), 0);
  CHECK_EQ_U(rb_space(&rb), 16);
  rb_free(&rb);
}

TEST(rb_size_space_sum_is_capacity)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 16), 0);
  uint8_t buf[16] = {0};
  for (size_t n = 0; n <= 16; n++)
  {
    set_pos(&rb, 0);
    rb_write(&rb, buf, n);
    CHECK_EQ_U(rb_size(&rb), n);
    CHECK_EQ_U(rb_size(&rb) + rb_space(&rb), 16);
  }
  rb_free(&rb);
}

TEST(rb_size_correct_across_counter_overflow)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  set_pos(&rb, SIZE_MAX - 2); /* wpos가 3바이트 후 0으로 넘어감 */
  rb_write(&rb, "abcde", 5);
  CHECK(rb.wpos < rb.rpos); /* 실제로 넘어갔는지 */
  CHECK_EQ_U(rb_size(&rb), 5);
  CHECK_EQ_U(rb_space(&rb), 3);
  rb_free(&rb);
}

/* ===================== rb_write ===================== */

TEST(rb_write_basic)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  CHECK_EQ_U(rb_write(&rb, "abc", 3), 3);
  CHECK_EQ_U(rb_size(&rb), 3);
  CHECK_MEM(rb.data, "abc", 3);
  rb_free(&rb);
}

TEST(rb_write_zero_bytes)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  CHECK_EQ_U(rb_write(&rb, "", 0), 0);
  CHECK_EQ_U(rb_size(&rb), 0);
  rb_free(&rb);
}

TEST(rb_write_exact_capacity)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  uint8_t src[8];
  fill_seq(src, 8, 0);
  CHECK_EQ_U(rb_write(&rb, src, 8), 8);
  CHECK_EQ_U(rb_space(&rb), 0);
  CHECK_MEM(rb.data, src, 8);
  rb_free(&rb);
}

TEST(rb_write_truncates_when_insufficient_space)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  uint8_t src[12];
  fill_seq(src, 12, 0);
  CHECK_EQ_U(rb_write(&rb, src, 12), 8);
  CHECK_EQ_U(rb_size(&rb), 8);
  CHECK_MEM(rb.data, src, 8);
  rb_free(&rb);
}

TEST(rb_write_when_full_returns_zero)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 4), 0);
  rb_write(&rb, "abcd", 4);
  CHECK_EQ_U(rb_write(&rb, "x", 1), 0);
  CHECK_EQ_U(rb_size(&rb), 4);
  CHECK_MEM(rb.data, "abcd", 4);
  rb_free(&rb);
}

TEST(rb_write_wraps_around)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  set_pos(&rb, 5); /* 물리 위치 5에서 시작 */
  CHECK_EQ_U(rb_write(&rb, "ABCDEF", 6), 6);
  /* [5..7] = "ABC", [0..2] = "DEF" */
  CHECK_MEM(rb.data + 5, "ABC", 3);
  CHECK_MEM(rb.data, "DEF", 3);
  rb_free(&rb);
}

TEST(rb_write_ends_exactly_at_array_end)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  set_pos(&rb, 5);
  rb_write(&rb, "ABC", 3); /* 5,6,7 */
  CHECK_EQ_U(rb.wpos & 7, 0);
  rb_write(&rb, "D", 1); /* 다음 쓰기는 인덱스 0 */
  CHECK_EQ_U(rb.data[0], 'D');
  rb_free(&rb);
}

/* ===================== rb_peek ===================== */

TEST(rb_peek_does_not_consume)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "abcd", 4);
  uint8_t out[4];
  CHECK_EQ_U(rb_peek(&rb, 0, out, 4), 4);
  CHECK_MEM(out, "abcd", 4);
  CHECK_EQ_U(rb_size(&rb), 4);
  CHECK_EQ_U(rb_peek(&rb, 0, out, 4), 4); /* 다시 읽어도 같음 */
  CHECK_MEM(out, "abcd", 4);
  rb_free(&rb);
}

TEST(rb_peek_with_offset)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "abcdef", 6);
  uint8_t out[3];
  CHECK_EQ_U(rb_peek(&rb, 2, out, 3), 3);
  CHECK_MEM(out, "cde", 3);
  rb_free(&rb);
}

TEST(rb_peek_empty_returns_zero)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  uint8_t out[4] = {0};
  CHECK_EQ_U(rb_peek(&rb, 0, out, 4), 0);
  rb_free(&rb);
}

TEST(rb_peek_offset_at_or_beyond_size_returns_zero)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "abc", 3);
  uint8_t out[4];
  CHECK_EQ_U(rb_peek(&rb, 3, out, 1), 0);
  CHECK_EQ_U(rb_peek(&rb, 100, out, 1), 0);
  rb_free(&rb);
}

TEST(rb_peek_truncates_to_available)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "abc", 3);
  uint8_t out[8];
  CHECK_EQ_U(rb_peek(&rb, 1, out, 8), 2);
  CHECK_MEM(out, "bc", 2);
  rb_free(&rb);
}

TEST(rb_peek_across_wrap_boundary)
{
  /* 패킷 헤더 4바이트가 배열 끝(6,7)과 처음(0,1)에 걸친 경우 */
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  set_pos(&rb, 6);
  rb_write(&rb, "\x01\x02\x03\x04", 4);
  uint8_t out[4];
  CHECK_EQ_U(rb_peek(&rb, 0, out, 4), 4);
  CHECK_MEM(out, "\x01\x02\x03\x04", 4);
  rb_free(&rb);
}

TEST(rb_peek_offset_lands_after_wrap)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  set_pos(&rb, 6);
  rb_write(&rb, "ABCDE", 5); /* A,B는 6,7 / C,D,E는 0,1,2 */
  uint8_t out[2];
  CHECK_EQ_U(rb_peek(&rb, 3, out, 2), 2);
  CHECK_MEM(out, "DE", 2);
  rb_free(&rb);
}

/* ===================== rb_consume ===================== */

TEST(rb_consume_basic)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "abcdef", 6);
  CHECK_EQ_U(rb_consume(&rb, 2), 2);
  CHECK_EQ_U(rb_size(&rb), 4);
  uint8_t out[4];
  rb_peek(&rb, 0, out, 4);
  CHECK_MEM(out, "cdef", 4);
  rb_free(&rb);
}

TEST(rb_consume_clamps_to_size)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "abc", 3);
  CHECK_EQ_U(rb_consume(&rb, 10), 3);
  CHECK_EQ_U(rb_size(&rb), 0);
  CHECK(rb.rpos == rb.wpos); /* rpos가 wpos를 앞지르지 않음 */
  rb_free(&rb);
}

TEST(rb_consume_empty_returns_zero)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  CHECK_EQ_U(rb_consume(&rb, 1), 0);
  CHECK_EQ_U(rb_size(&rb), 0);
  rb_free(&rb);
}

TEST(rb_consume_frees_space_for_write)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 4), 0);
  rb_write(&rb, "abcd", 4);
  rb_consume(&rb, 2);
  CHECK_EQ_U(rb_space(&rb), 2);
  CHECK_EQ_U(rb_write(&rb, "ef", 2), 2);
  uint8_t out[4];
  rb_peek(&rb, 0, out, 4);
  CHECK_MEM(out, "cdef", 4);
  rb_free(&rb);
}

/* ===================== rb_write_iov / rb_commit ===================== */

TEST(rb_write_iov_empty_buffer_single_span)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  struct iovec iov[2];
  REQUIRE_EQ_I(rb_write_iov(&rb, iov), 1);
  CHECK_EQ_PTR(iov[0].iov_base, rb.data);
  CHECK_EQ_U(iov[0].iov_len, 8);
  rb_free(&rb);
}

TEST(rb_write_iov_full_returns_zero)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 4), 0);
  rb_write(&rb, "abcd", 4);
  struct iovec iov[2];
  CHECK_EQ_I(rb_write_iov(&rb, iov), 0);
  rb_free(&rb);
}

TEST(rb_write_iov_two_spans_when_wrapped)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  set_pos(&rb, 5);
  rb_write(&rb, "A", 1); /* rpos=5, wpos=6 → 빈 공간 [6,8) + [0,5) */
  struct iovec iov[2];
  REQUIRE_EQ_I(rb_write_iov(&rb, iov), 2);
  CHECK_EQ_PTR(iov[0].iov_base, rb.data + 6);
  CHECK_EQ_U(iov[0].iov_len, 2);
  CHECK_EQ_PTR(iov[1].iov_base, rb.data);
  CHECK_EQ_U(iov[1].iov_len, 5);
  CHECK_EQ_U(iov[0].iov_len + iov[1].iov_len, rb_space(&rb));
  rb_free(&rb);
}

TEST(rb_write_iov_does_not_change_state)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "ab", 2);
  ringbuf_t before = rb;
  struct iovec iov[2];
  rb_write_iov(&rb, iov);
  CHECK_EQ_U(rb.rpos, before.rpos);
  CHECK_EQ_U(rb.wpos, before.wpos);
  rb_free(&rb);
}

TEST(rb_commit_partial_after_write_iov)
{
  /* readv가 빈 공간 중 일부만 채운 상황 */
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  struct iovec iov[2];
  REQUIRE_EQ_I(rb_write_iov(&rb, iov), 1);
  memcpy(iov[0].iov_base, "xyz", 3);
  rb_commit(&rb, 3);
  CHECK_EQ_U(rb_size(&rb), 3);
  uint8_t out[3];
  rb_peek(&rb, 0, out, 3);
  CHECK_MEM(out, "xyz", 3);
  rb_free(&rb);
}

TEST(rb_commit_across_two_spans)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  set_pos(&rb, 6);
  struct iovec iov[2];
  REQUIRE_EQ_I(rb_write_iov(&rb, iov), 2);
  /* scatter write 흉내: 조각 순서대로 채움 */
  memcpy(iov[0].iov_base, "AB", 2);
  memcpy(iov[1].iov_base, "CDE", 3);
  rb_commit(&rb, 5);
  uint8_t out[5];
  CHECK_EQ_U(rb_peek(&rb, 0, out, 5), 5);
  CHECK_MEM(out, "ABCDE", 5);
  rb_free(&rb);
}

TEST(rb_commit_zero_is_noop)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_commit(&rb, 0);
  CHECK_EQ_U(rb_size(&rb), 0);
  rb_free(&rb);
}

/* ===================== rb_read_iov ===================== */

TEST(rb_read_iov_empty_returns_zero)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  struct iovec iov[2];
  CHECK_EQ_I(rb_read_iov(&rb, iov), 0);
  rb_free(&rb);
}

TEST(rb_read_iov_single_span)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "hello", 5);
  struct iovec iov[2];
  REQUIRE_EQ_I(rb_read_iov(&rb, iov), 1);
  CHECK_EQ_PTR(iov[0].iov_base, rb.data);
  CHECK_EQ_U(iov[0].iov_len, 5);
  CHECK_MEM(iov[0].iov_base, "hello", 5);
  rb_free(&rb);
}

TEST(rb_read_iov_two_spans_when_wrapped)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  set_pos(&rb, 6);
  rb_write(&rb, "ABCDE", 5); /* [6,8) = AB, [0,3) = CDE */
  struct iovec iov[2];
  REQUIRE_EQ_I(rb_read_iov(&rb, iov), 2);
  CHECK_EQ_U(iov[0].iov_len, 2);
  CHECK_MEM(iov[0].iov_base, "AB", 2);
  CHECK_EQ_U(iov[1].iov_len, 3);
  CHECK_MEM(iov[1].iov_base, "CDE", 3);
  rb_free(&rb);
}

TEST(rb_read_iov_full_buffer_at_offset)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  set_pos(&rb, 3);
  uint8_t src[8];
  fill_seq(src, 8, 0);
  rb_write(&rb, src, 8);
  struct iovec iov[2];
  REQUIRE_EQ_I(rb_read_iov(&rb, iov), 2);
  CHECK_EQ_U(iov[0].iov_len + iov[1].iov_len, 8);
  CHECK_MEM(iov[0].iov_base, src, iov[0].iov_len);
  CHECK_MEM(iov[1].iov_base, src + iov[0].iov_len, iov[1].iov_len);
  rb_free(&rb);
}

TEST(rb_read_iov_then_partial_consume)
{
  /* sendmsg가 일부만 보낸 상황 (Partial Write) */
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 8), 0);
  rb_write(&rb, "abcdef", 6);
  struct iovec iov[2];
  rb_read_iov(&rb, iov);
  rb_consume(&rb, 4); /* 4바이트만 전송됨 */
  REQUIRE_EQ_I(rb_read_iov(&rb, iov), 1);
  CHECK_EQ_U(iov[0].iov_len, 2);
  CHECK_MEM(iov[0].iov_base, "ef", 2);
  rb_free(&rb);
}

/* ===================== 퍼징: 참조 구현과 비교 ===================== */

TEST(rb_fuzz_against_linear_model)
{
  enum
  {
    CAP = 64,
    ITER = 100000
  };
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, CAP), 0);
  set_pos(&rb, SIZE_MAX - 1000); /* 카운터 오버플로 구간도 통과하게 함 */

  uint8_t model[CAP]; /* 선형 배열 참조 모델: model[0..mlen) */
  size_t mlen = 0;
  uint8_t next = 0;
  uint8_t tmp[CAP * 2];

  srand(12345); /* 재현 가능하도록 고정 시드 */
  for (int it = 0; it < ITER; it++)
  {
    size_t n = (size_t)(rand() % (CAP + 8));
    switch (rand() % 4)
    {
    case 0:
    { /* write */
      fill_seq(tmp, n, next);
      size_t w = rb_write(&rb, tmp, n);
      size_t exp = n < CAP - mlen ? n : CAP - mlen;
      if (w != exp)
      {
        CHECK_EQ_U(w, exp);
        goto out;
      }
      memcpy(model + mlen, tmp, w);
      mlen += w;
      next = (uint8_t)(next + w);
      break;
    }
    case 1:
    { /* consume */
      size_t c = rb_consume(&rb, n);
      size_t exp = n < mlen ? n : mlen;
      if (c != exp)
      {
        CHECK_EQ_U(c, exp);
        goto out;
      }
      memmove(model, model + c, mlen - c);
      mlen -= c;
      break;
    }
    case 2:
    { /* write_iov + commit 일부 */
      struct iovec iov[2];
      int cnt = rb_write_iov(&rb, iov);
      size_t total = 0;
      for (int k = 0; k < cnt; k++)
        total += iov[k].iov_len;
      if (total != CAP - mlen)
      {
        CHECK_EQ_U(total, CAP - mlen);
        goto out;
      }
      size_t c = n < total ? n : total;
      fill_seq(tmp, c, next);
      size_t done = 0;
      for (int k = 0; k < cnt && done < c; k++)
      {
        size_t part = iov[k].iov_len < c - done ? iov[k].iov_len : c - done;
        memcpy(iov[k].iov_base, tmp + done, part);
        done += part;
      }
      rb_commit(&rb, c);
      memcpy(model + mlen, tmp, c);
      mlen += c;
      next = (uint8_t)(next + c);
      break;
    }
    case 3:
    { /* read_iov 내용 확인 */
      struct iovec iov[2];
      int cnt = rb_read_iov(&rb, iov);
      size_t off = 0;
      for (int k = 0; k < cnt; k++)
      {
        if (memcmp(iov[k].iov_base, model + off, iov[k].iov_len) != 0)
        {
          FAIL("read_iov content mismatch");
          goto out;
        }
        off += iov[k].iov_len;
      }
      if (off != mlen)
      {
        CHECK_EQ_U(off, mlen);
        goto out;
      }
      break;
    }
    }

    if (rb_size(&rb) != mlen)
    {
      CHECK_EQ_U(rb_size(&rb), mlen);
      goto out;
    }
    size_t p = rb_peek(&rb, 0, tmp, CAP);
    if (p != mlen || memcmp(tmp, model, mlen) != 0)
    {
      FAIL("peek content mismatch");
      goto out;
    }
  }
out:
  rb_free(&rb);
}
