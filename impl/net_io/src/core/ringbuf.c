#include "core/ringbuf.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/**
 * RB_MASK
 * 
 * cap 이 2 ^ k일 때 성립: pos % cap
 */
#define RB_MASK(rb, pos) ((pos) & ((rb)->capacity - 1))

int rb_init(ringbuf_t *rb, size_t capacity)
{
  if (capacity == 0 || (capacity & (capacity - 1)) != 0)
    return -1;
  rb->data = malloc(capacity);
  if (!rb->data)
    return -1;
  rb->capacity = capacity;
  rb->rpos = rb->wpos = 0;
  return 0;
}

void rb_free(ringbuf_t *rb)
{
  free(rb->data);
  rb->data = NULL;
  rb->capacity = rb->rpos = rb->wpos = 0;
}

size_t rb_size(const ringbuf_t *rb) { return rb->wpos - rb->rpos; }
size_t rb_space(const ringbuf_t *rb) { return rb->capacity - rb_size(rb); }

size_t rb_write(ringbuf_t *rb, const void *src, size_t n)
{
  size_t space = rb_space(rb);
  if (n > space)
    n = space;

  size_t w = RB_MASK(rb, rb->wpos);
  size_t first = rb->capacity - w; /* 배열 끝까지 */
  if (first > n)
    first = n;

  memcpy(rb->data + w, src, first);
  memcpy(rb->data, (const uint8_t *)src + first, n - first); /* 래핑 부분 (0바이트일 수 있으므로) */
  rb->wpos += n;
  return n;
}

size_t rb_peek(const ringbuf_t *rb, size_t offset, void *dst, size_t n)
/**
 * Copy ringbuf's data in [rpos + offset, rpos + offset + n] to *dst
 */
{
  size_t size = rb_size(rb);
  if (offset >= size)
    return 0;
  if (n > size - offset)
    n = size - offset;

  size_t r = RB_MASK(rb, rb->rpos + offset);
  size_t first = rb->capacity - r;
  if (first > n)
    first = n;

  memcpy(dst, rb->data + r, first);
  memcpy((uint8_t *)dst + first, rb->data, n - first);
  return n;
}

size_t rb_consume(ringbuf_t *rb, size_t n)
/**
 * Implemented by moving buf's index range.
 */
{
  size_t size = rb_size(rb);
  if (n > size)
    n = size;
  rb->rpos += n;
  return n;
}

/* 공통 로직: 논리 위치 pos부터 len바이트를 최대 2조각으로 */
/**
 * Modularize common logic:
 * - Valid data could be 2 part splitted.
 * - Passes valid data range as 1 or 2 (pos, len).
 * - Returns 1 when buffer not be splitted.
 * - Returns 2 when buffer has been splitted.
 */
static int rb_span(const ringbuf_t *rb, size_t pos, size_t len, struct iovec iov[2])
{
  if (len == 0)
    return 0;
  size_t p = RB_MASK(rb, pos);
  size_t first = rb->capacity - p;
  if (first >= len)
  {
    iov[0].iov_base = rb->data + p;
    iov[0].iov_len = len;
    return 1;
  }
  iov[0].iov_base = rb->data + p;
  iov[0].iov_len = first;
  iov[1].iov_base = rb->data;
  iov[1].iov_len = len - first;
  return 2;
}

int rb_write_iov(ringbuf_t *rb, struct iovec iov[2])
{
  return rb_span(rb, rb->wpos, rb_space(rb), iov);
}

int rb_read_iov(const ringbuf_t *rb, struct iovec iov[2])
{
  return rb_span(rb, rb->rpos, rb_size(rb), iov);
}

void rb_commit(ringbuf_t *rb, size_t n)
/**
 * Confirm data's range (mostly after writing)
 */
{
  size_t space = rb_space(rb);
  assert(n <= space); /* readv가 빌려준 것보다 많이 쓸 수는 없음 */
  if (n > space)
    n = space;
  rb->wpos += n;
}
