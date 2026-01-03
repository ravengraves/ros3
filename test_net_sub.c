// test_net_sub.c - Simple subscriber for network liaison testing
//
// Usage: test_net_sub <topic> [timeout_sec]
//
// Subscribes to topic and prints received messages.
// Exits after receiving a message or timeout (default 10s).

#include "ros3.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int running = 1;

static void handle_signal(int sig) {
	(void) sig;
	running = 0;
}

int main(int argc, char **argv) {
	if(argc < 2) {
		fprintf(stderr, "Usage: %s <topic> [timeout_sec]\n", argv[0]);
		return 1;
	}

	const char *topic = argv[1];
	int timeout_sec = (argc > 2) ? atoi(argv[2]) : 10;

	// Build shm name with ROSE_BASE prefix
	char shm_name[512];
	rose_topic_to_shm(shm_name, sizeof(shm_name), topic);

	// Connect to ring buffer as subscriber
	quicksand_connection *sub = NULL;
	if(quicksand_connect(&sub, shm_name, -1, 0, 0, NULL) != 0) {
		fprintf(stderr, "[Subscriber] Failed to connect to topic %s\n", topic);
		return 1;
	}

	printf("[Subscriber] Listening on %s (timeout %ds)\n", topic, timeout_sec);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	u64 start = quicksand_now();
	u64 timeout_ns = (u64) timeout_sec * 1000000000ULL;
	int received = 0;

	uint8_t buf[1024];
	while(running) {
		i64 sz = sizeof(buf);
		if(quicksand_read(sub, buf, &sz) >= 0) {
			// Parse the message
			mirage_msg msg = {.data = buf, .length = sz, .cursor = 0};
			char str[64];
			i64 str_len;
			i64 num;

			if(mirage_read_start(&msg) >= 0 && mirage_read_string(&msg, str, &str_len, sizeof(str) - 1) >= 0 && mirage_read_i64(&msg, &num) >= 0) {
				str[str_len] = '\0';
				printf("[Subscriber] Received: '%s' %ld\n", str, (long) num);
				received++;

				// Exit after first message for test verification
				break;
			}
		}

		// Check timeout
		u64 elapsed = quicksand_now() - start;
		if(elapsed > timeout_ns) {
			fprintf(stderr, "[Subscriber] Timeout waiting for messages\n");
			break;
		}

		quicksand_sleep(10e6); // 10ms poll
	}

	if(received > 0) {
		printf("[Subscriber] Success: received %d message(s)\n", received);
		return 0;
	} else {
		fprintf(stderr, "[Subscriber] Failed: no messages received\n");
		return 1;
	}
}
