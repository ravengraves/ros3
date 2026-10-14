// rosenode.c - CLI tool for querying and managing ROS3 nodes
//
// Usage:
//   rosenode list              - List all nodes
//   rosenode info <name>       - Show info for nodes matching name prefix
//   rosenode info --pid <pid>  - Show info for node with given PID
//   rosenode kill <name>       - Send SIGINT to nodes matching name prefix
//   rosenode kill -9 <name>    - Send SIGKILL to nodes matching name prefix

#define _POSIX_C_SOURCE 200809L

#include "core.h"
#include "../common/process.h"
#include "../common/query.h"
#include "ros3.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int is_help_arg(const char *arg) {
	return strcmp(arg, "-h") == 0
			|| strcmp(arg, "--help") == 0
			|| strcmp(arg, "/h") == 0
			|| strcmp(arg, "/help") == 0
			|| strcmp(arg, "/?") == 0;
}

static int is_pid_flag(const char *arg) {
	return strcmp(arg, "--pid") == 0 || strcmp(arg, "/pid") == 0;
}

static int parse_i64_arg(const char *text, i64 *value) {
	char *end = NULL;
	long long parsed;

	if(!text || !value) {
		return -1;
	}

	errno = 0;
	parsed = strtoll(text, &end, 10);
	if(errno != 0 || end == text || *end != '\0') {
		return -1;
	}

	*value = (i64) parsed;
	return 0;
}

static void print_usage(void) {
	fprintf(stderr, "Usage: rosenode <command> [args...]\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "  list              - List all nodes\n");
	fprintf(stderr, "  info <name>       - Show info for nodes matching name\n");
	fprintf(stderr, "  info --pid <pid>  - Show info for node with PID\n");
	fprintf(stderr, "  kill <name>       - Send SIGINT to matching nodes\n");
	fprintf(stderr, "  kill -9 <name>    - Send SIGKILL to matching nodes\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Environment:\n");
	fprintf(stderr, "  ROSE_BASE  Namespace (default: 'rose')\n");
	fprintf(stderr, "  ROSE_QUERY_TIMEOUT_MS  Query timeout in milliseconds (default: 1000)\n");
}

static int get_query_timeout_ms(void) {
	const char *s = getenv("ROSE_QUERY_TIMEOUT_MS");
	if(!s || !s[0]) {
		return 1000;
	}

	char *end = NULL;
	long v = strtol(s, &end, 10);
	if(end == s || *end != '\0' || v <= 0 || v > 60000) {
		return 1000;
	}

	return (int) v;
}

static int get_query_poll_step_ms(int timeout_ms) {
	if(timeout_ms <= 20) {
		return 1;
	}
	if(timeout_ms <= 200) {
		return 2;
	}
	return 10;
}

// Build query topic name using rose_topic_to_shm helper
static void build_topic_name(char *buf, size_t buf_size, const char *suffix) {
	char topic[256];
	snprintf(topic, sizeof(topic), "rosecore_%s", suffix);
	rose_topic_to_shm(buf, buf_size, topic);
}

// Skip to latest position in ring buffer before sending request
static int skip_to_latest(quicksand_connection *conn) {
	static uint8_t buf[ROSE_QUERY_MSG_SIZE];
	i64 sz = sizeof(buf);
	i64 ret = quicksand_read_latest(conn, buf, &sz);
	// -1 is Quicksand's empty-ring result.
	return ret >= 0 || ret == -1 ? 0 : -1;
}

// Read responses and print node list
static int read_and_print_list(quicksand_connection *conn, rose_query *query) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return 1;
	}

	static char name[ROSE_MAX_NAME_LEN];
	static char pubs[ROSE_TOPIC_CSV_SIZE];
	static char subs[ROSE_TOPIC_CSV_SIZE];
	i64 pid, last_heartbeat;

	// Wait for responses (with timeout)
	int timeout_ms = get_query_timeout_ms();
	int poll_ms = get_query_poll_step_ms(timeout_ms);
	u64 wait_start = rose_query_clock();

	while(rose_query_clock() - wait_start < (u64) timeout_ms * 1000000ULL) {
		i64 sz = msg->max_length;
		i64 r = quicksand_read(conn, msg->data, &sz);
		if(r >= 0) {
			msg->length = sz;
			int res = rose_query_parse(query, msg, name, sizeof(name),
						   &pid, &last_heartbeat,
						   pubs, sizeof(pubs),
						   subs, sizeof(subs));
			if(res == 0) {
				// Done
				break;
			} else if(res == 1) {
				printf("%s\n", name);
			}
		} else {
			quicksand_sleep((u64) poll_ms * 1000000ULL);
		}
	}

	mirage_destroy(&msg, NULL);
	if(!query->complete) {
		fprintf(stderr, "Query timed out before Done\n");
	}
	return query->complete ? 0 : 1;
}

// Read responses and print detailed node info
static int read_and_print_info(quicksand_connection *conn, rose_query *query, const char *prefix, i64 expected_pid) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return 1;
	}

	static char name[ROSE_MAX_NAME_LEN];
	static char pubs[ROSE_TOPIC_CSV_SIZE];
	static char subs[ROSE_TOPIC_CSV_SIZE];
	i64 pid, last_heartbeat;

	int timeout_ms = get_query_timeout_ms();
	int poll_ms = get_query_poll_step_ms(timeout_ms);
	u64 wait_start = rose_query_clock();
	int found = 0;

	while(rose_query_clock() - wait_start < (u64) timeout_ms * 1000000ULL) {
		i64 sz = msg->max_length;
		i64 r = quicksand_read(conn, msg->data, &sz);
		if(r >= 0) {
			msg->length = sz;
			int res = rose_query_parse(query, msg, name, sizeof(name),
						   &pid, &last_heartbeat,
						   pubs, sizeof(pubs),
						   subs, sizeof(subs));
			if(res == 0) {
				break;
			} else if(res == 1 && (prefix ? strncmp(name, prefix, strlen(prefix)) == 0 : pid == expected_pid)) {
				found++;
				printf("Node: %s\n", name);
				printf("  PID: %ld\n", (long) pid);

				u64 now = quicksand_now();
				f64 age = quicksand_ns(now, (u64) last_heartbeat) * 1e-9;
				printf("  Last heartbeat: %.3fs ago\n", age);

				printf("  Publishers: %s\n", pubs[0] ? pubs : "(none)");
				printf("  Subscribers: %s\n", subs[0] ? subs : "(none)");
				printf("\n");
			}
		} else {
			quicksand_sleep((u64) poll_ms * 1000000ULL);
		}
	}

	if(query->complete && found == 0) {
		printf("No matching nodes found\n");
	}

	mirage_destroy(&msg, NULL);
	if(!query->complete) {
		fprintf(stderr, "Query timed out before Done\n");
	}
	return query->complete ? 0 : 1;
}

// Read responses and kill matching nodes
static int read_and_kill(quicksand_connection *conn, rose_query *query, int use_sigkill, const char *prefix) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return 1;
	}

	static char name[ROSE_MAX_NAME_LEN];
	static char pubs[ROSE_TOPIC_CSV_SIZE];
	static char subs[ROSE_TOPIC_CSV_SIZE];
	i64 pid, last_heartbeat;

	int timeout_ms = get_query_timeout_ms();
	int poll_ms = get_query_poll_step_ms(timeout_ms);
	u64 wait_start = rose_query_clock();
	int killed = 0;

	int sig = use_sigkill ? SIGKILL : SIGINT;
	const char *sig_name = use_sigkill ? "SIGKILL" : "SIGINT";

	while(rose_query_clock() - wait_start < (u64) timeout_ms * 1000000ULL) {
		i64 sz = msg->max_length;
		i64 r = quicksand_read(conn, msg->data, &sz);
		if(r >= 0) {
			msg->length = sz;
			int res = rose_query_parse(query, msg, name, sizeof(name),
						   &pid, &last_heartbeat,
						   pubs, sizeof(pubs),
						   subs, sizeof(subs));
			if(res == 0) {
				break;
			} else if(res == 1 && rose_valid_pid(pid)
				  && strncmp(name, prefix, strlen(prefix)) == 0) {
				if(kill((pid_t) pid, sig) == 0) {
					printf("Sent %s to %s (pid %ld)\n",
					       sig_name, name, (long) pid);
					killed++;
				} else {
					fprintf(stderr, "Failed to send %s to %s (pid %ld)\n",
						sig_name, name, (long) pid);
				}
			}
		} else {
			quicksand_sleep((u64) poll_ms * 1000000ULL);
		}
	}

	if(query->complete && killed == 0) {
		printf("No matching nodes found\n");
	}

	mirage_destroy(&msg, NULL);
	if(!query->complete) {
		fprintf(stderr, "Query timed out before Done\n");
	}
	return query->complete ? 0 : 1;
}

int rosenode_main(int argc, char **argv) {
	int exit_code = 0;
	rose_query query = {0};
	quicksand_connection *conn = NULL;
	int64_t ret;
	char topic[512];

	if(argc < 2) {
		print_usage();
		return 1;
	}
	if(is_help_arg(argv[1])) {
		print_usage();
		return 0;
	}

	(void) rose_set_pdeathsig(SIGTERM);
	if(!rose_topic_to_shm(topic, sizeof(topic), "rosecore_lookup")) {
		fprintf(stderr, "%s\n", rose_last_error());
		return 1;
	}

	const char *cmd = argv[1][0] == '/' ? argv[1] + 1 : argv[1];

	if(strcmp(cmd, "list") == 0) {
		build_topic_name(topic, sizeof(topic), "dump");
		ret = quicksand_connect(&conn, topic, -1,
					ROSE_QUERY_MSG_SIZE, ROSE_DUMP_RING_SIZE, NULL);
		if(ret != 0) {
			fprintf(stderr, "Failed to connect to %s\n", topic);
			exit_code = 1;
			goto cleanup;
		}

		if(skip_to_latest(conn) < 0) {
			fprintf(stderr, "Failed to synchronize query channel\n");
			exit_code = 1;
			goto cleanup;
		}
		if(rose_query_send(&query, ROSE_MSG_DUMP_ALL, NULL, 0) < 0) {
			fprintf(stderr, "Failed to send dump request\n");
			exit_code = 1;
			goto cleanup;
		}
		exit_code = read_and_print_list(conn, &query);

	} else if(strcmp(cmd, "info") == 0) {
		if(argc < 3) {
			print_usage();
			exit_code = 1;
			goto cleanup;
		}

		build_topic_name(topic, sizeof(topic), "lookup");
		ret = quicksand_connect(&conn, topic, -1,
					ROSE_QUERY_MSG_SIZE, ROSE_LOOKUP_RING_SIZE, NULL);
		if(ret != 0) {
			fprintf(stderr, "Failed to connect to %s\n", topic);
			exit_code = 1;
			goto cleanup;
		}

		if(skip_to_latest(conn) < 0) {
			fprintf(stderr, "Failed to synchronize query channel\n");
			exit_code = 1;
			goto cleanup;
		}
		i64 expected_pid = 0;
		const char *prefix = NULL;
		if(is_pid_flag(argv[2])) {
			if(argc < 4) {
				print_usage();
				exit_code = 1;
				goto cleanup;
			}
			i64 pid = 0;
			if(parse_i64_arg(argv[3], &pid) != 0 || !rose_valid_pid(pid)) {
				fprintf(stderr, "Invalid pid: %s\n", argv[3]);
				exit_code = 1;
				goto cleanup;
			}
			expected_pid = pid;
			if(rose_query_send(&query, ROSE_MSG_LOOKUP_PID, NULL, pid) < 0) {
				fprintf(stderr, "Failed to send lookup request\n");
				exit_code = 1;
				goto cleanup;
			}
		} else {
			prefix = argv[2];
			if(rose_query_send(&query, ROSE_MSG_LOOKUP_NAME, prefix, 0) < 0) {
				fprintf(stderr, "Failed to send lookup request\n");
				exit_code = 1;
				goto cleanup;
			}
		}

		exit_code = read_and_print_info(conn, &query, prefix, expected_pid);

	} else if(strcmp(cmd, "kill") == 0) {
		if(argc < 3) {
			print_usage();
			exit_code = 1;
			goto cleanup;
		}

		int use_sigkill = 0;
		const char *name_arg;

		if(strcmp(argv[2], "-9") == 0) {
			if(argc < 4) {
				print_usage();
				exit_code = 1;
				goto cleanup;
			}
			use_sigkill = 1;
			name_arg = argv[3];
		} else {
			name_arg = argv[2];
		}

		build_topic_name(topic, sizeof(topic), "lookup");
		ret = quicksand_connect(&conn, topic, -1,
					ROSE_QUERY_MSG_SIZE, ROSE_LOOKUP_RING_SIZE, NULL);
		if(ret != 0) {
			fprintf(stderr, "Failed to connect to %s\n", topic);
			exit_code = 1;
			goto cleanup;
		}

		if(skip_to_latest(conn) < 0) {
			fprintf(stderr, "Failed to synchronize query channel\n");
			exit_code = 1;
			goto cleanup;
		}
		if(rose_query_send(&query, ROSE_MSG_LOOKUP_NAME, name_arg, 0) < 0) {
			fprintf(stderr, "Failed to send lookup request\n");
			exit_code = 1;
			goto cleanup;
		}
		exit_code = read_and_kill(conn, &query, use_sigkill, name_arg);

	} else {
		print_usage();
		exit_code = 1;
	}

cleanup:
	quicksand_disconnect(&conn, NULL);
	return exit_code;
}
