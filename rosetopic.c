// rosetopic.c - CLI tool for querying and monitoring ROS3 topics
//
// Usage:
//   rosetopic list              - List all topics
//   rosetopic info <topic>      - Show publishers/subscribers for a topic
//   rosetopic echo <topic>      - Subscribe and dump messages as hex

#define _POSIX_C_SOURCE 200809L

#include "ros3.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int running = 1;

static void handle_signal(int sig) {
	(void) sig;
	running = 0;
}

static void print_usage(void) {
	fprintf(stderr, "Usage: rosetopic <command> [args...]\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "  list              - List all topics\n");
	fprintf(stderr, "  info <topic>      - Show publishers/subscribers\n");
	fprintf(stderr, "  echo <topic>      - Subscribe and dump hex\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Environment:\n");
	fprintf(stderr, "  ROSE_BASE  Namespace (default: 'rose')\n");
}

// Convert shm topic name to display format: start from first %, replace % with /
static void topic_to_display(char *out, size_t out_size, const char *shm_name) {
	// Skip to first %
	while(*shm_name && *shm_name != '%') {
		shm_name++;
	}
	// Copy from first %, replacing % with /
	size_t i = 0;
	while(*shm_name && i < out_size - 1) {
		out[i++] = (*shm_name == '%') ? '/' : *shm_name;
		shm_name++;
	}
	out[i] = '\0';
}

// Convert user input to shm format: replace / with % (but leave existing % alone)
static void input_to_shm(char *out, size_t out_size, const char *input) {
	size_t i = 0;
	while(*input && i < out_size - 1) {
		out[i++] = (*input == '/') ? '%' : *input;
		input++;
	}
	out[i] = '\0';
}

// Build query topic name using rose_topic_to_shm helper
static void build_topic_name(char *buf, size_t buf_size, const char *suffix) {
	char topic[256];
	snprintf(topic, sizeof(topic), "rosemaster_%s", suffix);
	rose_topic_to_shm(buf, buf_size, topic);
}

// Skip to latest position in ring buffer before sending request
static void skip_to_latest(quicksand_connection *conn) {
	uint8_t buf[4096];
	i64 sz = sizeof(buf);
	quicksand_read_latest(conn, buf, &sz);
}

// Send a DumpAll request
static void send_dump_request(quicksand_connection *conn) {
	mirage_msg *msg = mirage_create(256, NULL);
	if(!msg) {
		return;
	}

	mirage_write_start(msg);
	mirage_write_fn(msg, ROSE_MSG_DUMP_ALL, (i64) strlen(ROSE_MSG_DUMP_ALL), 0);
	quicksand_write(conn, msg->data, msg->cursor);

	mirage_destroy(&msg, NULL);
}

// Parse a NodeInfo message
static int parse_node_info(mirage_msg *msg, char *name, size_t name_size,
			   i64 *pid, i64 *last_heartbeat,
			   char *pubs, size_t pubs_size,
			   char *subs, size_t subs_size) {
	static char fn_name[64];
	i64 fn_len, fn_argc;
	i64 str_len;

	msg->cursor = 0;
	if(mirage_read_start(msg) < 0) {
		return -1;
	}
	if(mirage_read_fn(msg, fn_name, &fn_len, sizeof(fn_name) - 1, &fn_argc) < 0) {
		return -1;
	}
	fn_name[fn_len] = '\0';

	if(strcmp(fn_name, ROSE_MSG_DONE) == 0) {
		return 0;
	}

	if(strcmp(fn_name, ROSE_MSG_NODE_INFO) != 0) {
		return -1;
	}

	if(mirage_read_string(msg, name, &str_len, (i64) name_size - 1) < 0) {
		return -1;
	}
	name[str_len] = '\0';

	if(mirage_read_i64(msg, pid) < 0) {
		return -1;
	}

	if(mirage_read_i64(msg, last_heartbeat) < 0) {
		return -1;
	}

	if(mirage_read_string(msg, pubs, &str_len, (i64) pubs_size - 1) < 0) {
		return -1;
	}
	pubs[str_len] = '\0';

	if(mirage_read_string(msg, subs, &str_len, (i64) subs_size - 1) < 0) {
		return -1;
	}
	subs[str_len] = '\0';

	return 1;
}

// Add topics from CSV to a list (avoiding duplicates)
static int add_topics_from_csv(char topics[][256], int *count, int max,
			       const char *csv) {
	if(!csv || !csv[0]) {
		return 0;
	}

	char buf[256];
	const char *p = csv;

	while(*p) {
		// Skip leading comma
		if(*p == ',') {
			p++;
			continue;
		}

		// Copy topic name
		int i = 0;
		while(*p && *p != ',' && i < 255) {
			buf[i++] = *p++;
		}
		buf[i] = '\0';

		if(i == 0) {
			continue;
		}

		// Check for duplicate
		int found = 0;
		for(int j = 0; j < *count; j++) {
			if(strcmp(topics[j], buf) == 0) {
				found = 1;
				break;
			}
		}

		if(!found && *count < max) {
			strcpy(topics[*count], buf);
			(*count)++;
		}
	}

	return 0;
}

// Compare for qsort
static int topic_cmp(const void *a, const void *b) {
	return strcmp((const char *) a, (const char *) b);
}

// Read responses and collect all unique topics
static void read_and_print_topics(quicksand_connection *conn) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return;
	}

	static char name[256];
	static char pubs[16384];
	static char subs[16384];
	i64 pid, last_heartbeat;

	// Collect unique topics
	static char topics[1024][256];
	int topic_count = 0;

	int timeout_ms = 1000;
	int waited = 0;

	while(waited < timeout_ms) {
		i64 sz = msg->max_length;
		i64 r = quicksand_read(conn, msg->data, &sz);
		if(r >= 0) {
			msg->length = sz;
			int res = parse_node_info(msg, name, sizeof(name),
						  &pid, &last_heartbeat,
						  pubs, sizeof(pubs),
						  subs, sizeof(subs));
			if(res == 0) {
				break;
			} else if(res == 1) {
				add_topics_from_csv(topics, &topic_count, 1024, pubs);
				add_topics_from_csv(topics, &topic_count, 1024, subs);
			}
			waited = 0;
		} else {
			quicksand_sleep(10e6);
			waited += 10;
		}
	}

	// Sort and print with display formatting
	qsort(topics, (size_t) topic_count, sizeof(topics[0]), topic_cmp);
	for(int i = 0; i < topic_count; i++) {
		char display[256];
		topic_to_display(display, sizeof(display), topics[i]);
		printf("%s\n", display);
	}

	mirage_destroy(&msg, NULL);
}

// Read responses and print info for a specific topic
static void read_and_print_topic_info(quicksand_connection *conn,
				      const char *topic_filter) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return;
	}

	static char name[256];
	static char pubs[16384];
	static char subs[16384];
	i64 pid, last_heartbeat;

	// Collect publishers and subscribers
	static char publishers[256][256];
	static char subscribers[256][256];
	int pub_count = 0, sub_count = 0;

	int timeout_ms = 1000;
	int waited = 0;

	while(waited < timeout_ms) {
		i64 sz = msg->max_length;
		i64 r = quicksand_read(conn, msg->data, &sz);
		if(r >= 0) {
			msg->length = sz;
			int res = parse_node_info(msg, name, sizeof(name),
						  &pid, &last_heartbeat,
						  pubs, sizeof(pubs),
						  subs, sizeof(subs));
			if(res == 0) {
				break;
			} else if(res == 1) {
				// Check if this node publishes to topic
				if(strstr(pubs, topic_filter)) {
					if(pub_count < 256) {
						snprintf(publishers[pub_count], 256, "%s (pid %ld)",
							 name, (long) pid);
						pub_count++;
					}
				}
				// Check if this node subscribes to topic
				if(strstr(subs, topic_filter)) {
					if(sub_count < 256) {
						snprintf(subscribers[sub_count], 256, "%s (pid %ld)",
							 name, (long) pid);
						sub_count++;
					}
				}
			}
			waited = 0;
		} else {
			quicksand_sleep(10e6);
			waited += 10;
		}
	}

	char display[256];
	topic_to_display(display, sizeof(display), topic_filter);
	printf("Topic: %s\n\n", display);
	printf("Publishers:\n");
	if(pub_count == 0) {
		printf("  (none)\n");
	} else {
		for(int i = 0; i < pub_count; i++) {
			printf("  %s\n", publishers[i]);
		}
	}

	printf("\nSubscribers:\n");
	if(sub_count == 0) {
		printf("  (none)\n");
	} else {
		for(int i = 0; i < sub_count; i++) {
			printf("  %s\n", subscribers[i]);
		}
	}

	mirage_destroy(&msg, NULL);
}

// Print data dump: ASCII, decimal, hex (80 chars wide, 8 bytes per line)
static void hex_dump(const uint8_t *data, size_t len) {
	const int bpl = 8; // bytes per line

	for(size_t offset = 0; offset < len; offset += bpl) {
		size_t line_len = (offset + bpl <= len) ? (size_t) bpl : len - offset;

		// Offset (4 hex digits)
		printf("%04zx  ", offset);

		// ASCII representation (the star)
		printf("|");
		for(size_t i = 0; i < (size_t) bpl; i++) {
			if(i < line_len) {
				uint8_t c = data[offset + i];
				putchar(isprint(c) ? c : '.');
			} else {
				putchar(' ');
			}
		}
		printf("|  ");

		// Decimal representation
		for(size_t i = 0; i < (size_t) bpl; i++) {
			if(i < line_len) {
				printf("%3u ", data[offset + i]);
			} else {
				printf("    ");
			}
		}
		printf(" ");

		// Hex representation
		for(size_t i = 0; i < (size_t) bpl; i++) {
			if(i < line_len) {
				printf("%02x ", data[offset + i]);
			} else {
				printf("   ");
			}
		}

		printf("\n");
	}
	printf("\n");
}

// Subscribe to topic and dump messages as hex
static void echo_topic(const char *topic_name) {
	// Convert topic name to shared memory format with ROSE_BASE
	char shm_name[512];
	rose_topic_to_shm(shm_name, sizeof(shm_name), topic_name);

	quicksand_connection *sub = NULL;
	int64_t ret = quicksand_connect(&sub, shm_name, -1, 0, 0, NULL);
	if(ret != 0) {
		fprintf(stderr, "Failed to subscribe to %s (shm: %s)\n",
			topic_name, shm_name);
		return;
	}

	printf("Subscribed to %s (Ctrl+C to stop)\n\n", topic_name);

	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	u64 msg_num = 0;
	while(running) {
		i64 sz = msg->max_length;
		i64 r = quicksand_read(sub, msg->data, &sz);
		if(r >= 0) {
			msg_num++;
			printf("--- Message %lu (%ld bytes) ---\n",
			       (unsigned long) msg_num, (long) sz);
			hex_dump(msg->data, (size_t) sz);
		} else {
			quicksand_sleep(10e6); // 10ms poll
		}
	}

	printf("\nStopped.\n");
	mirage_destroy(&msg, NULL);
}

int main(int argc, char **argv) {
	if(argc < 2) {
		print_usage();
		return 1;
	}

	if(rose_set_pdeathsig(SIGTERM) != 0) {
		perror("[rosetopic] prctl(PR_SET_PDEATHSIG)");
	}

	const char *cmd = argv[1];

	if(strcmp(cmd, "list") == 0) {
		char topic[512];
		build_topic_name(topic, sizeof(topic), "dump");

		quicksand_connection *conn = NULL;
		int64_t ret = quicksand_connect(&conn, topic, -1,
						ROSE_QUERY_MSG_SIZE,
						ROSE_DUMP_RING_SIZE, NULL);
		if(ret != 0) {
			fprintf(stderr, "Failed to connect to %s\n", topic);
			return 1;
		}

		skip_to_latest(conn);
		send_dump_request(conn);
		read_and_print_topics(conn);

	} else if(strcmp(cmd, "info") == 0) {
		if(argc < 3) {
			print_usage();
			return 1;
		}

		char topic[512];
		build_topic_name(topic, sizeof(topic), "dump");

		quicksand_connection *conn = NULL;
		int64_t ret = quicksand_connect(&conn, topic, -1,
						ROSE_QUERY_MSG_SIZE,
						ROSE_DUMP_RING_SIZE, NULL);
		if(ret != 0) {
			fprintf(stderr, "Failed to connect to %s\n", topic);
			return 1;
		}

		// Convert user input: / -> %
		char topic_filter[256];
		input_to_shm(topic_filter, sizeof(topic_filter), argv[2]);

		skip_to_latest(conn);
		send_dump_request(conn);
		read_and_print_topic_info(conn, topic_filter);

	} else if(strcmp(cmd, "echo") == 0) {
		if(argc < 3) {
			print_usage();
			return 1;
		}

		echo_topic(argv[2]);

	} else {
		print_usage();
		return 1;
	}

	return 0;
}
