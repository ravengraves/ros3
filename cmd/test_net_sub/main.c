// test_net_sub.c - Test subscriber for network liaison testing

#include "ros3.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
	if(argc < 2) {
		fprintf(stderr, "Usage: %s <topic> [timeout_sec]\n", argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	int timeout_sec = (argc > 2) ? atoi(argv[2]) : 10;

	rose_node *node = rose_init(argc, argv, "test_net_sub", NULL, NULL);
	rose_subscriber *sub = rose_create_sub(node, (char *) topic, -1, 0, NULL);
	if(!sub) {
		fprintf(stderr, "Failed to subscribe to %s\n", topic);
		return 1;
	}

	mirage_msg *msg = mirage_create(1024, NULL);
	u64 start = rose_now();
	u64 timeout_ns = (u64) timeout_sec * 1000000000ULL;

	rose_readlast(sub, msg);

	while(rose_ok(node)) {
		msg->length = msg->max_length;
		if(rose_read(sub, msg) >= 0) {
			return 0; // Success - received a message
		}
		if(rose_now() - start > timeout_ns) {
			fprintf(stderr, "Timeout\n");
			return 1;
		}
		rose_sleep(0.01);
	}

	return 1;
}
