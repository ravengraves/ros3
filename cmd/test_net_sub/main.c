// test_net_sub.c - Test subscriber for network liaison testing

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
	int exit_code = 1;
	if(argc < 2) {
		fprintf(stderr, "Usage: %s <topic> [timeout_sec]\n", argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	int timeout_sec = 10;
	if(argc > 2 && parse_int_arg(argv[2], &timeout_sec) != 0) {
		fprintf(stderr, "Invalid timeout: %s\n", argv[2]);
		return 1;
	}

	rose_node *node = rose_init(argc, argv, "test_net_sub", NULL, NULL);
	rose_subscriber *sub = rose_create_sub(node, (char *) topic, -1, 0, NULL);
	if(!sub) {
		fprintf(stderr, "Failed to subscribe to %s\n", topic);
		rose_shutdown(node);
		return 1;
	}

	mirage_msg *msg = mirage_create(1024, NULL);
	if(!msg) {
		rose_shutdown(node);
		return 1;
	}
	u64 start = rose_now();
	u64 timeout_ns = (u64) timeout_sec * 1000000000ULL;

	rose_readlast(sub, msg);

	while(rose_ok(node)) {
		msg->length = msg->max_length;
		if(rose_read(sub, msg) >= 0) {
			exit_code = 0;
			break;
		}
		if(rose_now() - start > timeout_ns) {
			fprintf(stderr, "Timeout\n");
			break;
		}
		rose_sleep(0.01);
	}

	mirage_destroy(&msg, NULL);
	rose_shutdown(node);
	return exit_code;
}
