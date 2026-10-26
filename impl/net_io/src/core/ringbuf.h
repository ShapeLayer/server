/* Ring Buffer */

/**
 * This ring buffer is intended to be used for storing tcp byte stream.
 * - rx: When after buffer stored stream data, other module must assemble into packet.
 * - tx: When before send data, other module would use this for converting into byte stream.
 * 
 * Consider:
 * - RX data might be splitted.
 *   > Anaylzing byte as message would be handled by other module.
 * - Packet not received completly must not be disposed.
 *   > Seperate lookup(peek) and dispose(consume).
 * - Sending can be partial behavior.
 *   > Consuming range must be decided.
 * - Minimizing syscall and copy.
 *   > Non-copy API(iovec), readv, sendmsg.
 * - Memory is finite.
 *   > Decide fixed size.
 * - Each session must be handled by single event loop.
 *   > no lock. thread-safty must be considered by caller.
 */

#pragma once
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

typedef struct {
  uint8_t *data;
  size_t   capacity;
  size_t   rpos;  // Read Position; 단조 증가, &(capacity - 1)
  size_t   wpos;  // Write Position; size = wpos - rpos;
} ringbuf_t;

int    rb_init(ringbuf_t *rb, size_t capacity);
void   rb_free(ringbuf_t *rb);
size_t rb_size(const ringbuf_t *rb);
size_t rb_space(const ringbuf_t *rb);

// copy
size_t rb_write(ringbuf_t *rb, const void *src, size_t n);
size_t rb_peek(const ringbuf_t *rb, size_t offset, void *dst, size_t n);
size_t rb_consume(ringbuf_t *rb, size_t n);
/**
 * rb_consume: for reader
 * disposes used data and return space
 */

// non-copy
                                // [rpos, end) ~ [0, wpos)
int    rb_write_iov(ringbuf_t *rb, struct iovec iov[2]);
/**
 * rb_write_iov: for writer
 * notice empty space to kernel for writing
 */
void   rb_commit(ringbuf_t *rb, size_t n);
int    rb_read_iov(const ringbuf_t *rb, struct iovec iov[2]);
/**
 * rb_read_iov: for reader
 * notice filled space's position
 */

#endif  // RINGBUF_H
