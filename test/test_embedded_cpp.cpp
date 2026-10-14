#include "ros3_embedded.h"
#include <assert.h>

int main() {
	uint8_t source[32] = {}, target[32] = {};
	mirage_msg message;
	mirage_init(&message, source, sizeof(source));
	assert(mirage_write_start(&message) > 0);
	assert(mirage_write_i16(&message, 126) > 0);
	ros3_writer writer;
	ros3_frame frame;
	assert(ros3_read_init(&frame, target, sizeof(target)) == 0);
	assert(ros3_write_init(&writer, source, message.length) == 0);
	while(ros3_write_status(&writer))
		ros3_read_byte(&frame, ros3_write_byte(&writer));
	assert(ros3_read_message(&frame, &message) == 0 && message.data == target);
	assert(mirage_read_start(&message) > 0);
	int16_t value = 0;
	assert(mirage_read_i16(&message, &value) > 0 && value == 126);
	return 0;
}
