/* Packet Definition and Handling */

/**
 * Extract a packet from a ring buffer.
 * 
 * header | [0]         [1]        [2]        [3]        |
 *        | Size (LSB)  Size (MSB) ID (LSB)   ID (MSB)   |
 */

#pragma once
#ifndef PACKET_H
#define PACKET_H

#include "core/ringbuf.h"

#define PKT_HEADER_SIZE 4u
#define PKT_MAX_SIZE    4096u

typedef struct {
  uint16_t size;
  uint16_t id;
} pkt_header_t;

typedef enum {
  PKT_OK,
  PKT_NEED_MORE,
  PKT_INVALID
} pkt_result_t;

void pkt_encode_header(uint8_t out[PKT_HEADER_SIZE], uint16_t size, uint16_t id);
void pkt_decode_header(const uint8_t in[PKT_HEADER_SIZE], pkt_header_t *out);

pkt_result_t pkt_extract(ringbuf_t *rb, pkt_header_t *hdr, uint8_t *out, size_t capacity);

#endif  // PACKET_H
