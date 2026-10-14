#ifndef ROS3_TEST_EMBEDDED_SERIAL_MESSAGE_H
#define ROS3_TEST_EMBEDDED_SERIAL_MESSAGE_H
#include "ros3_embedded.h"

/* Example application: reset[] restarts the counter in List["Hello world", value]. */
static inline int8_t demo_reply(mirage_msg *message, int16_t value) {
	if(mirage_write_start(message) < 0
	   || mirage_write_fn(message, "List", 4, 2) < 0
	   || mirage_write_string(message, "Hello world", 11) < 0
	   || mirage_write_i16(message, value) < 0) {
		return -1;
	}
	return 0;
}

static inline int8_t demo_reset(const ros3_frame *frame) {
	mirage_msg message;
	char name[5];
	int16_t length, argc;
	if(ros3_read_message(frame, &message) != 0 || mirage_read_start(&message) < 0
	   || mirage_read_fn(&message, name, &length, sizeof(name), &argc) < 0
	   || length != 5 || argc != 0 || message.cursor != message.length) {
		return 0;
	}
	return name[0] == 'r' && name[1] == 'e' && name[2] == 's'
			&& name[3] == 'e' && name[4] == 't';
}
#endif
