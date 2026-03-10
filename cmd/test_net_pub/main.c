// test_net_pub.c - Test publisher for network liaison testing

#include "ros3.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_int_arg(const char *text, int *value) {
	char *end = NULL;
	long parsed;

	if(!text || !value) {
		return -1;
	}

	errno = 0;
	parsed = strtol(text, &end, 10);
	if(errno != 0 || end == text || *end != '\0'
	   || parsed < INT_MIN || parsed > INT_MAX) {
		return -1;
	}

	*value = (int) parsed;
	return 0;
}

int main(int argc, char **argv) {
	if(argc < 2) {
		fprintf(stderr, "Usage: %s <topic> [count]\n", argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	int count = 20;
	if(argc > 2 && parse_int_arg(argv[2], &count) != 0) {
		fprintf(stderr, "Invalid count: %s\n", argv[2]);
		return 1;
	}

	rose_node *node = rose_init(argc, argv, "test_net_pub", NULL, NULL);
	rose_publisher *pub = rose_create_pub(node, (char *) topic, -1, 1024, 64);
	if(!pub) {
		fprintf(stderr, "Failed to create topic %s\n", topic);
		rose_shutdown(node);
		return 1;
	}

	mirage_msg *msg = mirage_create(1024, NULL);
	if(!msg) {
		rose_shutdown(node);
		return 1;
	}

	for(int i = 0; i < count && rose_ok(node); i++) {
		mirage_write_start(msg);
		mirage_write_string(msg, "hello", -1);
		mirage_write_i64(msg, i);
		rose_publish(pub, msg);
		rose_sleep(0.1);
	}

	mirage_destroy(&msg, NULL);
	rose_shutdown(node);
	return 0;
}
