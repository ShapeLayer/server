#include "core/packet.h"

static inline void wr_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);  // least significant byte
  p[1] = (uint8_t)(v >> 8);     // most significant byte
}

static inline uint16_t rd_le16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

void pkt_encode_header(uint8_t out[PKT_HEADER_SIZE], uint16_t size, uint16_t id) {
  wr_le16(out + 0, size);
  wr_le16(out + 2, id);
}

void pkt_decode_header(const uint8_t in[PKT_HEADER_SIZE], pkt_header_t *out) {
  out->size = rd_le16(in + 0);
  out->id = rd_le16(in + 2);
}

pkt_result_t pkt_extract(ringbuf_t *rb, pkt_header_t *hdr, uint8_t *out, size_t capacity) {
  uint8_t raw[PKT_HEADER_SIZE];

  // If the size of ring buffer's valid data is less than defined size,
  // packet's header has not been received completely.
  if (rb_size(rb) < PKT_HEADER_SIZE) return PKT_NEED_MORE;
  
  rb_peek(rb, 0, raw, PKT_HEADER_SIZE);  // Fetch data from ring buffer
  pkt_decode_header(raw, hdr);

  if (hdr->size < PKT_HEADER_SIZE || hdr->size > PKT_MAX_SIZE) return PKT_INVALID;
  if (rb_size(rb) < hdr->size) return PKT_NEED_MORE;

  // Fetch and consume ring buffer
  size_t pktlen = hdr->size - PKT_HEADER_SIZE;
  if (pktlen > capacity) return PKT_INVALID;
  rb_peek(rb, PKT_HEADER_SIZE, out, pktlen);
  rb_consume(rb, hdr->size);

  return PKT_OK;
}
