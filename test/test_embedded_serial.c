/* Differential tests against the bridge's actual codec, without desktop Mirage
 * linkage. Mirage embedded handoff is tested by embedded_serial_demo.c. */
#define ROS3_EMBEDDED_NO_MIRAGE
#include "../cmd/roseserial/frame.h"
#include "ros3_embedded.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

u64 rose_now(void) { return 0; } /* frame.h timestamps; no I/O in these tests */

static uint8_t payload[65536], wire[131086], expected[131086], decoded[65537];
static struct serial_parser host;

static size_t encode(const uint8_t *data, size_t length) {
	ros3_writer writer;
	assert(ros3_write_init(&writer, data, length) == 0);
	size_t n = 0;
	while(ros3_write_status(&writer)) {
		assert(n < sizeof(wire));
		wire[n++] = ros3_write_byte(&writer);
	}
	assert(writer.index == length);
	return n;
}

static void parity(size_t length) {
	size_t n = encode(payload, length);
	assert(n <= 2 * (length + 7) + 2);
	size_t m = serial_encode(expected, payload, length);
	assert(n == m && memcmp(wire, expected, n) == 0);
	memset(&host, 0, sizeof(host));
	ros3_frame frame;
	memset(decoded, 0xa5, sizeof(decoded));
	assert(ros3_read_init(&frame, decoded + 1, length) == 0);
	unsigned count = 0;
	for(size_t i = 0; i < n; ++i) {
		int h = serial_feed(&host, wire[i], length);
		int e = ros3_read_byte(&frame, expected[i]);
		assert((h == 1) == (e == 0));
		if(e == 0) {
			++count;
			assert(frame.length == length && frame.index == length);
			assert(!memcmp(decoded + 1, payload, length));
			assert(!memcmp(host.raw + 3, payload, length));
		}
	}
	assert(count == 1 && decoded[0] == 0xa5 && decoded[length + 1] == 0xa5);
}

static void malformed(void) {
	/* Mutate every byte, every bit: delimiters, header, payload, CRC, escapes. */
	size_t n = encode(payload, 256);
	for(size_t bad = 0; bad < n; ++bad) {
		for(unsigned bit = 0; bit < 8; ++bit) {
			ros3_frame frame;
			assert(!ros3_read_init(&frame, decoded, 256));
			wire[bad] ^= 1U << bit;
			for(size_t i = 0; i < n; ++i) {
				assert(ros3_read_byte(&frame, wire[i]) != 0);
			}
			wire[bad] ^= 1U << bit;
			for(size_t i = 0; i < n; ++i) {
				assert((ros3_read_byte(&frame, wire[i]) == 0) == (i == n - 1));
			}
		}
	}
	/* Every truncation, including dangling escape, must resynchronize. */
	for(size_t cut = 1; cut < n; ++cut) {
		ros3_frame frame;
		assert(!ros3_read_init(&frame, decoded, 256));
		for(size_t i = 0; i < cut; ++i) {
			assert(ros3_read_byte(&frame, wire[i]) != 0);
		}
		ros3_read_init(&frame, decoded, 256); /* explicit reinitialization */
		for(size_t i = cut; i < n; ++i) {
			assert(ros3_read_byte(&frame, wire[i]) != 0);
		}
		for(size_t i = 0; i < n; ++i) {
			assert((ros3_read_byte(&frame, wire[i]) == 0) == (i == n - 1));
		}
	}
	ros3_frame frame;
	assert(!ros3_read_init(&frame, decoded + 1, 255));
	memset(decoded, 0xa5, sizeof(decoded));
	for(size_t i = 0; i < n; ++i) {
		assert(ros3_read_byte(&frame, wire[i]) != 0);
	}
	for(size_t i = 0; i < sizeof(decoded); ++i) {
		assert(decoded[i] == 0xa5);
	}
	/* Host encoder can produce an empty body, but both decoders reject it. */
	n = serial_encode(wire, payload, 0);
	for(size_t i = 0; i < n; ++i) {
		assert(ros3_read_byte(&frame, wire[i]) != 0);
	}
	/* Overlong body, late extra bytes and abandoned escapes stay invalid. */
	n = encode(payload, 1);
	for(size_t i = 0; i < n - 1; ++i) {
		assert(ros3_read_byte(&frame, wire[i]) != 0);
	}
	for(size_t i = 0; i < 100000; ++i) {
		assert(ros3_read_byte(&frame, 0) != 0);
	}
	assert(ros3_read_byte(&frame, 0x7e) != 0);
	assert(ros3_read_byte(&frame, 0x7d) != 0);
	assert(ros3_read_byte(&frame, 0x7e) == ROS3_INVALID);
	for(unsigned repeat = 0; repeat < 3; ++repeat) {
		/* Closing delimiter is also an opening delimiter: shared boundaries. */
		for(size_t i = 1; i < n; ++i) {
			assert((ros3_read_byte(&frame, wire[i]) == 0) == (i == n - 1));
		}
	}
	assert(ros3_read_byte(&frame, 0x7e) == ROS3_BEGIN);
}

static void random_differential(void) {
	uint32_t random = 0x12345678;
	ros3_frame frame;
	assert(!ros3_read_init(&frame, decoded + 1, 1024));
	memset(&host, 0, sizeof(host));
	decoded[0] = decoded[1025] = 0xa5;
	for(size_t i = 0; i < 250000; ++i) {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		uint8_t byte = (uint8_t) random;
		int h = serial_feed(&host, byte, 1024);
		int e = ros3_read_byte(&frame, byte);
		assert((h == 1) == (e == 0));
		assert(decoded[0] == 0xa5 && decoded[1025] == 0xa5);
	}
}

int main(void) {
	uint32_t crc = UINT32_MAX;
	for(size_t i = 0; i < 9; ++i) {
		crc = ros3_crc_byte(crc, (uint8_t) "123456789"[i]);
	}
	assert(~crc == UINT32_C(0xcbf43926));
	ros3_writer writer;
	ros3_frame frame;
	assert(ros3_write_init(NULL, payload, 1) == -1);
	assert(ros3_write_init(&writer, NULL, 1) == -1 && !ros3_write_status(&writer));
	assert(ros3_write_init(&writer, payload, 0) == -1 && !ros3_write_status(&writer));
	assert(ros3_write_init(&writer, payload, 65536) == -1 && !ros3_write_status(&writer));
	assert(ros3_read_init(NULL, decoded, 1) == -1);
	assert(ros3_read_init(&frame, NULL, 1) == -1);
	assert(ros3_read_byte(&frame, 0x7e) == ROS3_INVALID);
	assert(ros3_read_init(&frame, decoded, 65536) == -1);
	assert(ros3_read_init(&frame, decoded, 0) == -1);
	for(size_t i = 0; i < sizeof(payload); ++i) {
		payload[i] = (uint8_t) i;
	}
	const size_t lengths[] = {1, 125, 126, 255, 256, 0x7d00, 0x7e00, 32767, 32768, 65535};
	for(size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
		parity(lengths[i]);
	}
	/* Each CRC position must be escaped at least once (not only payload/header). */
	unsigned escaped_crc = 0;
	for(unsigned value = 0; value < 65536 && escaped_crc != 15; ++value) {
		payload[0] = (uint8_t) value;
		payload[1] = (uint8_t) (value >> 8);
		parity(2);
		uint8_t raw[] = {1, 2, 0, payload[0], payload[1]};
		crc = serial_crc(raw, sizeof(raw));
		for(unsigned byte = 0; byte < 4; ++byte) {
			uint8_t b = (uint8_t) (crc >> (8 * byte));
			if(b == 0x7e || b == 0x7d) {
				escaped_crc |= 1U << byte;
			}
		}
	}
	assert(escaped_crc == 15);
	for(size_t i = 0; i < sizeof(payload); ++i) {
		payload[i] = (uint8_t) i;
	}
	malformed();
	random_differential();
	puts("embedded serial: host parity, 65535-byte bounds, corruption, resync passed");
	return 0;
}
