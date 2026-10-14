#ifndef ROSESERIAL_FRAME_H
#define ROSESERIAL_FRAME_H
#include "ros3.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* V1: 7e, escaped(version=1, u16le length, payload, u32le CRC), 7e.
 * CRC-32/ISO-HDLC covers version, length and payload. */
#define SERIAL_RAW_MAX (65535U + 7U)
struct serial_parser {
	uint8_t raw[SERIAL_RAW_MAX];
	size_t used;
	int active, escaped, dropping;
	u64 start;
};
static uint32_t serial_crc(const uint8_t *p, size_t n) {
	uint32_t crc = UINT32_MAX;
	while(n--) {
		crc ^= *p++;
		for(int j = 0; j < 8; ++j) {
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
		}
	}
	return ~crc;
}
/* Returns 1 complete, 0 incomplete, -1 malformed. A delimiter is always a
 * boundary, even after corruption/overflow. No length-directed discard loop. */
static int serial_feed(struct serial_parser *p, uint8_t byte, size_t limit) {
	if(byte == 0x7e) {
		int result = 0;
		if(p->active && (p->used || p->escaped || p->dropping)) {
			result = -1;
			if(!p->dropping && !p->escaped && p->used >= 7 && p->raw[0] == 1) {
				size_t n = p->raw[1] | ((size_t) p->raw[2] << 8);
				if(n > 0 && n <= limit && p->used == n + 7) {
					uint32_t crc = 0;
					for(int j = 0; j < 4; ++j) {
						crc |= (uint32_t) p->raw[n + 3 + j] << (8 * j);
					}
					if(crc == serial_crc(p->raw, n + 3)) {
						result = 1;
					}
				}
			}
		}
		p->used = 0;
		p->active = 1;
		p->escaped = 0;
		p->dropping = 0;
		p->start = rose_now();
		return result;
	}
	if(!p->active || p->dropping) {
		return 0;
	}
	if(!p->used && !p->escaped) {
		p->start = rose_now();
	}
	if(p->escaped) {
		byte ^= 0x20;
		p->escaped = 0;
	} else if(byte == 0x7d) {
		p->escaped = 1;
		return 0;
	}
	if(p->used == sizeof(p->raw)) {
		p->dropping = 1;
		return 0;
	}
	p->raw[p->used++] = byte;
	if(p->used == 3 && (p->raw[0] != 1 || (size_t) (p->raw[1] | (p->raw[2] << 8)) > limit)) {
		p->dropping = 1;
	}
	return 0;
}
static size_t serial_encode(uint8_t *out, const uint8_t *data, size_t len) {
	if(len > 65535) {
		return 0;
	}
	uint8_t header[3] = {1, len & 255, (len >> 8) & 255};
	uint8_t raw[SERIAL_RAW_MAX];
	memcpy(raw, header, 3);
	memcpy(raw + 3, data, len);
	uint32_t crc = serial_crc(raw, len + 3);
	for(int j = 0; j < 4; ++j) {
		raw[len + 3 + j] = (crc >> (8 * j)) & 255;
	}
	size_t used = 0;
	out[used++] = 0x7e;
	for(size_t i = 0; i < len + 7; ++i) {
		uint8_t byte = raw[i];
		if(byte == 0x7e || byte == 0x7d) {
			out[used++] = 0x7d;
			byte ^= 0x20;
		}
		out[used++] = byte;
	}
	out[used++] = 0x7e;
	return used;
}
#endif
