/* Runs the same Mirage command/reply payloads as the AVR example on the host. */
#include "embedded_serial_message.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct loopback {
	ros3_frame *frame;
	unsigned messages;
};
static void transfer(ros3_writer *writer, const uint8_t *data, size_t length,
		     struct loopback *loop) {
	assert(ros3_write_init(writer, data, length) == 0);
	while(ros3_write_status(writer)) {
		if(ros3_read_byte(loop->frame, ros3_write_byte(writer)) == 0) {
			++loop->messages;
		}
	}
}

int main(void) {
	uint8_t tx[128], rx[128];
	mirage_msg out, in;
	ros3_frame frame;
	ros3_writer writer;
	mirage_init(&out, tx, sizeof(tx));
	assert(ros3_read_init(&frame, rx, sizeof(rx)) == 0);
	struct loopback loop = {&frame, 0};
	assert(mirage_write_start(&out) > 0);
	assert(mirage_write_fn(&out, "reset", 5, 0) > 0);
	transfer(&writer, out.data, out.length, &loop);
	int16_t command = 0;
	assert(demo_reset(&frame));
	assert(ros3_read_message(&frame, &in) == 0);
	assert(in.data == rx && in.cursor == 0 && in.length == frame.length);
	/* Only the exact zero-argument reset function is accepted. */
	assert(mirage_write_start(&out) > 0);
	assert(mirage_write_fn(&out, "other", 5, 0) > 0);
	transfer(&writer, out.data, out.length, &loop);
	assert(!demo_reset(&frame));
	assert(mirage_write_start(&out) > 0);
	assert(mirage_write_fn(&out, "reset", 5, 1) > 0);
	assert(mirage_write_i16(&out, 1) > 0);
	transfer(&writer, out.data, out.length, &loop);
	assert(!demo_reset(&frame));
	assert(demo_reply(&out, command) == 0);
	transfer(&writer, out.data, out.length, &loop);
	assert(loop.messages == 4 && ros3_read_message(&frame, &in) == 0);
	char name[5] = {0}, text[12] = {0};
	int16_t length = 0, argc = 0, value = 0;
	assert(mirage_read_start(&in) > 0);
	assert(mirage_read_fn(&in, name, &length, 4, &argc) > 0);
	assert(length == 4 && argc == 2 && !strcmp(name, "List"));
	assert(mirage_read_string(&in, text, &length, 11) > 0);
	assert(length == 11 && !strcmp(text, "Hello world"));
	assert(mirage_read_i16(&in, &value) > 0 && value == command && in.cursor == in.length);
	printf("zero-copy serial round trip: %s[\"%s\", %d], %u frames\n", name, text, value, loop.messages);
	mirage_msg old = in;
	assert(ros3_read_byte(&frame, 0x7e) != 0);
	assert(ros3_read_message(&frame, &in) == -1 && !memcmp(&in, &old, sizeof(in)));
	/* Signed Mirage limit: never silently narrow a valid wire length. */
	static uint8_t large[65535];
	assert(!ros3_read_init(&frame, large, sizeof(large)));
	const size_t limits[] = {32767, 32768, 65535};
	for(size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
		size_t n = limits[i];
		transfer(&writer, large, n, &loop);
		assert(ros3_read_message(&frame, &in) == (n > INT16_MAX ? -1 : 0));
		if(n <= INT16_MAX) {
			assert(in.data == large && in.length == (int16_t) n);
		}
	}
	puts("Mirage borrowed view, cursor, invalidation and signed length limits passed");
	return 0;
}
