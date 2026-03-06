// test_net_pub.c - Test publisher for network liaison testing

#include "ros3.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
	if(argc < 2) {
		fprintf(stderr, "Usage: %s <topic> [count]\n", argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	int count = (argc > 2) ? atoi(argv[2]) : 20;

	rose_node *node = rose_init(argc, argv, "test_net_pub", NULL, NULL);
	rose_publisher *pub = rose_create_pub(node, (char *) topic, -1, 1024, 64);
	if(!pub) {
		fprintf(stderr, "Failed to create topic %s\n", topic);
		return 1;
	}

	mirage_msg *msg = mirage_create(1024, NULL);

	for(int i = 0; i < count && rose_ok(node); i++) {
		mirage_write_start(msg);
		mirage_write_string(msg, "hello", -1);
		mirage_write_i64(msg, i);
		rose_publish(pub, msg);
		rose_sleep(0.1);
	}

	return 0;
}
