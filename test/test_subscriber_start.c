#include "../src/ros3.c"
#include <assert.h>

int main(void) {
	char topic[128];
	snprintf(topic, sizeof(topic), "/rose_sub_start_%ld", (long) getpid());
	quicksand_connection *writer = NULL;
	assert(quicksand_connect(&writer, topic, -1, 8192, 16, NULL) == 0);
	static u8 payload[8192];
	for(int history = 0; history < 2; history++) {
		if(history) {
			assert(quicksand_write(writer, payload, sizeof(payload)) >= 0);
			assert(quicksand_write(writer, payload, sizeof(payload)) >= 0);
		}
		rose_subscriber sub = {0};
		assert(quicksand_connect(&sub.sub, topic, -1, 0, 0, NULL) == 0);
		rose_subscriber_sync_latest(&sub);
		i64 size = sizeof(payload);
		assert(quicksand_read(sub.sub, payload, &size) == -1);
		payload[0] = 42;
		assert(quicksand_write(writer, payload, sizeof(payload)) >= 0);
		payload[0] = 0;
		size = sizeof(payload);
		assert(quicksand_read(sub.sub, payload, &size) >= 0);
		assert(size == sizeof(payload) && payload[0] == 42);
		size = sizeof(payload);
		assert(quicksand_read(sub.sub, payload, &size) == -1);
		quicksand_disconnect(&sub.sub, NULL);
	}
	quicksand_disconnect(&writer, NULL);
	quicksand_delete(topic, -1);
	puts("subscriber startup: empty/history skipped, new messages delivered");
	return 0;
}
