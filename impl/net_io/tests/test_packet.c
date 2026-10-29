/* test_packet.c */

#include "core/packet.h"
#include "core/ringbuf.h"
#include "framework/test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ===================== helpers ===================== */

/* 헤더 + 패턴 페이로드로 패킷 하나를 만든다. 반환: 전체 바이트 수 */
static size_t make_packet(uint8_t *out, uint16_t id, size_t payload_len, uint8_t seed)
{
  pkt_encode_header(out, (uint16_t)(PKT_HEADER_SIZE + payload_len), id);
  for (size_t i = 0; i < payload_len; i++)
    out[PKT_HEADER_SIZE + i] = (uint8_t)(seed + i);
  return PKT_HEADER_SIZE + payload_len;
}

/* 헤더만 원하는 값으로 써 넣는다 (잘못된 size 테스트용) */
static void write_raw_header(ringbuf_t *rb, uint16_t size, uint16_t id)
{
  uint8_t raw[PKT_HEADER_SIZE];
  pkt_encode_header(raw, size, id);
  rb_write(rb, raw, PKT_HEADER_SIZE);
}

/* 화이트박스: 빈 링버퍼의 카운터를 옮겨 래핑 위치를 만든다 */
static void set_pos(ringbuf_t *rb, size_t pos)
{
  rb->rpos = rb->wpos = pos;
}

/* ===================== pkt_encode_header ===================== */

TEST(pkt_encode_header_little_endian_layout)
{
  uint8_t out[PKT_HEADER_SIZE];
  pkt_encode_header(out, 0x0123, 0x03E9);
  CHECK_MEM(out, "\x23\x01\xE9\x03", 4);
}

TEST(pkt_encode_header_zero)
{
  uint8_t out[PKT_HEADER_SIZE];
  memset(out, 0xAA, sizeof out);
  pkt_encode_header(out, 0, 0);
  CHECK_MEM(out, "\x00\x00\x00\x00", 4);
}

TEST(pkt_encode_header_max)
{
  uint8_t out[PKT_HEADER_SIZE];
  pkt_encode_header(out, 0xFFFF, 0xFFFF);
  CHECK_MEM(out, "\xFF\xFF\xFF\xFF", 4);
}

TEST(pkt_encode_header_writes_exactly_four_bytes)
{
  uint8_t out[PKT_HEADER_SIZE + 2];
  memset(out, 0xAA, sizeof out);
  pkt_encode_header(out + 1, 0x1234, 0x5678);
  CHECK_EQ_U(out[0], 0xAA);
  CHECK_MEM(out + 1, "\x34\x12\x78\x56", 4);
  CHECK_EQ_U(out[5], 0xAA);
}

/* ===================== pkt_decode_header ===================== */

TEST(pkt_decode_header_little_endian_layout)
{
  pkt_header_t h;
  pkt_decode_header((const uint8_t *)"\x23\x01\xE9\x03", &h);
  CHECK_EQ_U(h.size, 0x0123);
  CHECK_EQ_U(h.id, 0x03E9);
}

TEST(pkt_decode_header_high_bit_set)
{
  /* 상위 바이트 최상위 비트(0x80)가 부호 확장되지 않는지 */
  pkt_header_t h;
  pkt_decode_header((const uint8_t *)"\x00\x80\xFF\xFF", &h);
  CHECK_EQ_U(h.size, 0x8000);
  CHECK_EQ_U(h.id, 0xFFFF);
}

TEST(pkt_decode_header_unaligned_input)
{
  /* 홀수 주소에서 읽어도 정상 동작해야 함 */
  uint8_t buf[PKT_HEADER_SIZE + 1] = {0x00, 0x10, 0x00, 0xE9, 0x03};
  pkt_header_t h;
  pkt_decode_header(buf + 1, &h);
  CHECK_EQ_U(h.size, 0x0010);
  CHECK_EQ_U(h.id, 0x03E9);
}

TEST(pkt_header_roundtrip_boundary_values)
{
  uint16_t vals[] = {0, 1, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF};
  size_t   n      = sizeof vals / sizeof vals[0];
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < n; j++) {
      uint8_t      raw[PKT_HEADER_SIZE];
      pkt_header_t h;
      pkt_encode_header(raw, vals[i], vals[j]);
      pkt_decode_header(raw, &h);
      CHECK_EQ_U(h.size, vals[i]);
      CHECK_EQ_U(h.id, vals[j]);
    }
  }
}

/* ===================== pkt_extract: PKT_OK ===================== */

TEST(pkt_extract_single_packet)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 64), 0);
  uint8_t pkt[32];
  size_t  len = make_packet(pkt, 1001, 10, 0x40);
  rb_write(&rb, pkt, len);

  pkt_header_t h;
  uint8_t      out[PKT_MAX_SIZE];
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_OK);
  CHECK_EQ_U(h.size, len);
  CHECK_EQ_U(h.id, 1001);
  CHECK_MEM(out, pkt + PKT_HEADER_SIZE, 10);
  CHECK_EQ_U(rb_size(&rb), 0); /* 패킷 전체가 소비됨 */
  rb_free(&rb);
}

TEST(pkt_extract_zero_payload)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 16), 0);
  write_raw_header(&rb, PKT_HEADER_SIZE, 7);

  pkt_header_t h;
  uint8_t      out[4];
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_OK);
  CHECK_EQ_U(h.size, PKT_HEADER_SIZE);
  CHECK_EQ_U(h.id, 7);
  CHECK_EQ_U(rb_size(&rb), 0);
  rb_free(&rb);
}

TEST(pkt_extract_max_size_packet)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, PKT_MAX_SIZE), 0); /* 최대 패킷이 정확히 꽉 차는 크기 */
  static uint8_t pkt[PKT_MAX_SIZE];
  size_t len = make_packet(pkt, 2, PKT_MAX_SIZE - PKT_HEADER_SIZE, 0);
  REQUIRE_EQ_U(rb_write(&rb, pkt, len), PKT_MAX_SIZE);

  pkt_header_t   h;
  static uint8_t out[PKT_MAX_SIZE];
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_OK);
  CHECK_EQ_U(h.size, PKT_MAX_SIZE);
  CHECK_MEM(out, pkt + PKT_HEADER_SIZE, PKT_MAX_SIZE - PKT_HEADER_SIZE);
  rb_free(&rb);
}

TEST(pkt_extract_capacity_exactly_payload_len)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 64), 0);
  uint8_t pkt[32];
  size_t  len = make_packet(pkt, 3, 8, 0);
  rb_write(&rb, pkt, len);

  pkt_header_t h;
  uint8_t      out[8];
  CHECK_EQ_I(pkt_extract(&rb, &h, out, 8), PKT_OK);
  CHECK_MEM(out, pkt + PKT_HEADER_SIZE, 8);
  rb_free(&rb);
}

TEST(pkt_extract_does_not_write_past_payload)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 64), 0);
  uint8_t pkt[32];
  size_t  len = make_packet(pkt, 3, 5, 0x10);
  rb_write(&rb, pkt, len);
  rb_write(&rb, "\xEE\xEE\xEE", 3); /* 다음 패킷의 일부 */

  pkt_header_t h;
  uint8_t      out[16];
  memset(out, 0xAA, sizeof out);
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_OK);
  CHECK_MEM(out, pkt + PKT_HEADER_SIZE, 5);
  CHECK_EQ_U(out[5], 0xAA); /* 페이로드 뒤는 건드리지 않음 */
  CHECK_EQ_U(rb_size(&rb), 3); /* 다음 패킷 조각은 남아 있음 */
  rb_free(&rb);
}

/* ===================== pkt_extract: PKT_NEED_MORE ===================== */

TEST(pkt_extract_empty_buffer_needs_more)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 16), 0);
  pkt_header_t h;
  uint8_t      out[16];
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_NEED_MORE);
  rb_free(&rb);
}

TEST(pkt_extract_partial_header_needs_more)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 16), 0);
  uint8_t pkt[16];
  make_packet(pkt, 1, 4, 0);

  pkt_header_t h;
  uint8_t      out[16];
  for (size_t n = 1; n < PKT_HEADER_SIZE; n++) {
    rb_write(&rb, pkt + n - 1, 1);
    CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_NEED_MORE);
    CHECK_EQ_U(rb_size(&rb), n); /* 아무것도 소비하지 않음 */
  }
  rb_free(&rb);
}

TEST(pkt_extract_partial_payload_needs_more)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 64), 0);
  uint8_t pkt[32];
  size_t  len = make_packet(pkt, 1, 20, 0);
  rb_write(&rb, pkt, len - 1); /* 마지막 1바이트 부족 */

  pkt_header_t h;
  uint8_t      out[32];
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_NEED_MORE);
  CHECK_EQ_U(rb_size(&rb), len - 1);
  CHECK_EQ_U(h.size, len); /* 헤더는 해석된 상태 */

  rb_write(&rb, pkt + len - 1, 1);
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_OK);
  CHECK_MEM(out, pkt + PKT_HEADER_SIZE, 20);
  rb_free(&rb);
}

/* ===================== pkt_extract: PKT_INVALID ===================== */

TEST(pkt_extract_size_smaller_than_header_is_invalid)
{
  uint16_t sizes[] = {0, 1, 2, 3};
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    ringbuf_t rb;
    REQUIRE_EQ_I(rb_init(&rb, 16), 0);
    write_raw_header(&rb, sizes[i], 1);
    pkt_header_t h;
    uint8_t      out[16];
    CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_INVALID);
    CHECK_EQ_U(h.size, sizes[i]); /* 로그용으로 해석한 값이 남음 */
    rb_free(&rb);
  }
}

TEST(pkt_extract_size_over_max_is_invalid)
{
  uint16_t sizes[] = {PKT_MAX_SIZE + 1, 0x8000, 0xFFFF};
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    ringbuf_t rb;
    REQUIRE_EQ_I(rb_init(&rb, 16), 0);
    write_raw_header(&rb, sizes[i], 1);
    pkt_header_t h;
    uint8_t      out[16];
    /* 데이터가 다 오기 전이라도 헤더만 보고 즉시 거부해야 함 */
    CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_INVALID);
    rb_free(&rb);
  }
}

TEST(pkt_extract_payload_over_capacity_is_invalid)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 64), 0);
  uint8_t pkt[32];
  size_t  len = make_packet(pkt, 1, 9, 0);
  rb_write(&rb, pkt, len);

  pkt_header_t h;
  uint8_t      out[16];
  memset(out, 0xAA, sizeof out);
  CHECK_EQ_I(pkt_extract(&rb, &h, out, 8), PKT_INVALID);
  CHECK_EQ_U(out[0], 0xAA); /* out에 아무것도 쓰지 않음 */
  rb_free(&rb);
}

TEST(pkt_extract_invalid_does_not_consume)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 16), 0);
  write_raw_header(&rb, 0, 1);
  pkt_header_t h;
  uint8_t      out[16];
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_INVALID);
  CHECK_EQ_U(rb_size(&rb), PKT_HEADER_SIZE);
  /* 같은 상태로 다시 호출해도 같은 결과 (호출자가 세션을 끊어야 함) */
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_INVALID);
  rb_free(&rb);
}

/* ===================== pkt_extract: 링버퍼 래핑 ===================== */

TEST(pkt_extract_header_across_wrap)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 16), 0);
  set_pos(&rb, 14); /* 헤더 4바이트가 [14,15] + [0,1]에 걸침 */
  uint8_t pkt[16];
  size_t  len = make_packet(pkt, 0x0201, 6, 0x30);
  rb_write(&rb, pkt, len);

  pkt_header_t h;
  uint8_t      out[16];
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_OK);
  CHECK_EQ_U(h.size, len);
  CHECK_EQ_U(h.id, 0x0201);
  CHECK_MEM(out, pkt + PKT_HEADER_SIZE, 6);
  rb_free(&rb);
}

TEST(pkt_extract_payload_across_wrap)
{
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 16), 0);
  set_pos(&rb, 9); /* 헤더는 [9,12], 페이로드는 [13,15] + [0,4] */
  uint8_t pkt[16];
  size_t  len = make_packet(pkt, 5, 8, 0x50);
  rb_write(&rb, pkt, len);

  pkt_header_t h;
  uint8_t      out[16];
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_OK);
  CHECK_MEM(out, pkt + PKT_HEADER_SIZE, 8);
  rb_free(&rb);
}

/* ===================== pkt_extract: 스트림 현상 (A1 §3.1) ===================== */

TEST(pkt_extract_coalesced_packets)
{
  /* send 3번이 recv 1번에 뭉쳐 온 상황 */
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 256), 0);
  uint8_t stream[256];
  size_t  lens[3], off = 0;
  size_t  payloads[3] = {26, 0, 96};
  for (int i = 0; i < 3; i++) {
    lens[i] = make_packet(stream + off, (uint16_t)(100 + i), payloads[i], (uint8_t)(i * 0x20));
    off += lens[i];
  }
  rb_write(&rb, stream, off);

  pkt_header_t h;
  uint8_t      out[PKT_MAX_SIZE];
  size_t       pos = 0;
  for (int i = 0; i < 3; i++) {
    REQUIRE_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_OK);
    CHECK_EQ_U(h.id, 100 + i);
    CHECK_EQ_U(h.size, lens[i]);
    CHECK_MEM(out, stream + pos + PKT_HEADER_SIZE, payloads[i]);
    pos += lens[i];
  }
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_NEED_MORE);
  CHECK_EQ_U(rb_size(&rb), 0);
  rb_free(&rb);
}

TEST(pkt_extract_fragmented_one_byte_at_a_time)
{
  /* 패킷이 1바이트씩 쪼개져 도착하는 상황 */
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, 64), 0);
  uint8_t pkt[64];
  size_t  len = make_packet(pkt, 9, 30, 0x70);

  pkt_header_t h;
  uint8_t      out[64];
  for (size_t i = 0; i < len - 1; i++) {
    rb_write(&rb, pkt + i, 1);
    if (pkt_extract(&rb, &h, out, sizeof out) != PKT_NEED_MORE) {
      FAIL("extract returned before the last byte arrived");
      rb_free(&rb);
      return;
    }
  }
  rb_write(&rb, pkt + len - 1, 1);
  CHECK_EQ_I(pkt_extract(&rb, &h, out, sizeof out), PKT_OK);
  CHECK_EQ_U(h.id, 9);
  CHECK_MEM(out, pkt + PKT_HEADER_SIZE, 30);
  rb_free(&rb);
}

TEST(pkt_extract_fuzz_random_chunks)
{
  /* 무작위 패킷 스트림을 무작위 크기로 쪼개 넣고, 순서·내용이 원본과 같은지 확인 */
  enum { NPKT = 2000, RBCAP = 8192, MAXPAY = 300 };
  ringbuf_t rb;
  REQUIRE_EQ_I(rb_init(&rb, RBCAP), 0);
  set_pos(&rb, SIZE_MAX - 5000); /* 카운터 오버플로 구간도 지나가게 함 */

  srand(424242);
  size_t   stream_cap = (size_t)NPKT * (PKT_HEADER_SIZE + MAXPAY);
  uint8_t *stream     = malloc(stream_cap);
  size_t  *offsets    = malloc(sizeof(size_t) * NPKT);
  if (!stream || !offsets) {
    FAIL("malloc failed");
    goto out;
  }

  size_t total = 0;
  for (int i = 0; i < NPKT; i++) {
    offsets[i] = total;
    total += make_packet(stream + total, (uint16_t)i, (size_t)(rand() % (MAXPAY + 1)), (uint8_t)rand());
  }

  size_t       fed = 0;
  int          got = 0;
  pkt_header_t h;
  uint8_t      out[PKT_MAX_SIZE];
  while (got < NPKT) {
    if (fed < total) {
      size_t chunk = (size_t)(rand() % 64) + 1;
      if (chunk > total - fed)
        chunk = total - fed;
      fed += rb_write(&rb, stream + fed, chunk); /* 공간이 모자라면 일부만 들어감 */
    }

    pkt_result_t r;
    while ((r = pkt_extract(&rb, &h, out, sizeof out)) == PKT_OK) {
      const uint8_t *exp = stream + offsets[got];
      pkt_header_t   eh;
      pkt_decode_header(exp, &eh);
      if (h.id != eh.id || h.size != eh.size ||
          memcmp(out, exp + PKT_HEADER_SIZE, h.size - PKT_HEADER_SIZE) != 0) {
        CHECK_EQ_U(h.id, eh.id);
        CHECK_EQ_U(h.size, eh.size);
        FAIL("payload mismatch");
        goto out;
      }
      got++;
    }
    if (r == PKT_INVALID) {
      FAIL("unexpected PKT_INVALID");
      goto out;
    }
    if (fed == total && r == PKT_NEED_MORE && got < NPKT) {
      CHECK_EQ_I(got, NPKT); /* 다 넣었는데 패킷이 모자람 */
      goto out;
    }
  }
  CHECK_EQ_U(rb_size(&rb), 0);

out:
  free(stream);
  free(offsets);
  rb_free(&rb);
}
