// test_net_pub.c - Simple publisher for network liaison testing
//
// Usage: test_net_pub <topic> [count]
//
// Publishes 'count' messages (default 20) to the specified topic at 10Hz

#include "ros3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
	if(argc < 2) {
		fprintf(stderr, "Usage: %s <topic> [count]\n", argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	int count = (argc > 2) ? atoi(argv[2]) : 20;

	// Build shm name with ROSE_BASE prefix
	char shm_name[512];
	rose_topic_to_shm(shm_name, sizeof(shm_name), topic);

	// Create ring buffer as publisher
	quicksand_connection *pub = NULL;
	if(quicksand_connect(&pub, shm_name, -1, 1024, 64, NULL) != 0) {
		fprintf(stderr, "Failed to create topic %s\n", topic);
		return 1;
	}

	printf("[Publisher] Publishing %d messages to %s\n", count, topic);

	mirage_msg *msg = mirage_create(1024, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to create message\n");
		return 1;
	}

	for(int i = 0; i < count; i++) {
		mirage_write_start(msg);
		mirage_write_string(msg, "hello", -1);
		mirage_write_i64(msg, i);

		quicksand_write(pub, msg->data, msg->cursor);
		printf("[Publisher] Sent message %d\n", i);

		quicksand_sleep(100e6); // 100ms = 10Hz
	}

	printf("[Publisher] Done\n");
	mirage_destroy(&msg, NULL);
	return 0;
}
