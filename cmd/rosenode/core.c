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

// Send a LookupName request
static void send_lookup_name_request(quicksand_connection *conn,
				     const char *prefix) {
	mirage_msg *msg = mirage_create(512, NULL);
	if(!msg) {
		return;
	}

	mirage_write_start(msg);
	mirage_write_fn(msg, ROSE_MSG_LOOKUP_NAME,
			(i64) strlen(ROSE_MSG_LOOKUP_NAME), 1);
	mirage_write_string(msg, (char *) prefix, (i64) strlen(prefix));
	quicksand_write(conn, msg->data, msg->cursor);

	mirage_destroy(&msg, NULL);
}

// Send a LookupPid request
static void send_lookup_pid_request(quicksand_connection *conn, i64 pid) {
	mirage_msg *msg = mirage_create(256, NULL);
	if(!msg) {
		return;
	}

	mirage_write_start(msg);
	mirage_write_fn(msg, ROSE_MSG_LOOKUP_PID,
			(i64) strlen(ROSE_MSG_LOOKUP_PID), 1);
	mirage_write_i64(msg, pid);
	quicksand_write(conn, msg->data, msg->cursor);

	mirage_destroy(&msg, NULL);
}

// Parse and print a NodeInfo message, returns 1 if NodeInfo, 0 if Done, -1 error
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

	// Read name
	if(mirage_read_string(msg, name, &str_len, (i64) name_size - 1) < 0) {
		return -1;
	}
	name[str_len] = '\0';

	// Read pid
	if(mirage_read_i64(msg, pid) < 0) {
		return -1;
	}

	// Read last_heartbeat
	if(mirage_read_i64(msg, last_heartbeat) < 0) {
		return -1;
	}

	// Read pubs
	if(mirage_read_string(msg, pubs, &str_len, (i64) pubs_size - 1) < 0) {
		return -1;
	}
	pubs[str_len] = '\0';

	// Read subs
	if(mirage_read_string(msg, subs, &str_len, (i64) subs_size - 1) < 0) {
		return -1;
	}
	subs[str_len] = '\0';

	return 1;
}

// Skip to latest position in ring buffer before sending request
static void skip_to_latest(quicksand_connection *conn) {
	uint8_t buf[4096];
	i64 sz = sizeof(buf);
	quicksand_read_latest(conn, buf, &sz);
}

// Read responses and print node list
static void read_and_print_list(quicksand_connection *conn) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return;
	}

	static char name[256];
	static char pubs[16384];
	static char subs[16384];
	i64 pid, last_heartbeat;

	// Wait for responses (with timeout)
	int timeout_ms = get_query_timeout_ms();
	int poll_ms = get_query_poll_step_ms(timeout_ms);
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
				// Done
				break;
			} else if(res == 1) {
				printf("%s\n", name);
			}
			waited = 0; // Reset timeout on successful read
		} else {
			quicksand_sleep((u64) poll_ms * 1000000ULL);
			waited += poll_ms;
		}
	}

	mirage_destroy(&msg, NULL);
}

// Read responses and print detailed node info
static void read_and_print_info(quicksand_connection *conn) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return;
	}

	static char name[256];
	static char pubs[16384];
	static char subs[16384];
	i64 pid, last_heartbeat;

	int timeout_ms = get_query_timeout_ms();
	int poll_ms = get_query_poll_step_ms(timeout_ms);
	int waited = 0;
	int found = 0;

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
			waited = 0;
		} else {
			quicksand_sleep((u64) poll_ms * 1000000ULL);
			waited += poll_ms;
		}
	}

	if(found == 0) {
		printf("No matching nodes found\n");
	}

	mirage_destroy(&msg, NULL);
}

// Read responses and kill matching nodes
static void read_and_kill(quicksand_connection *conn, int use_sigkill) {
	mirage_msg *msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!msg) {
		fprintf(stderr, "Failed to allocate message buffer\n");
		return;
	}

	static char name[256];
	static char pubs[16384];
	static char subs[16384];
	i64 pid, last_heartbeat;

	int timeout_ms = get_query_timeout_ms();
	int poll_ms = get_query_poll_step_ms(timeout_ms);
	int waited = 0;
	int killed = 0;

	int sig = use_sigkill ? SIGKILL : SIGINT;
	const char *sig_name = use_sigkill ? "SIGKILL" : "SIGINT";

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
				if(kill((pid_t) pid, sig) == 0) {
					printf("Sent %s to %s (pid %ld)\n",
					       sig_name, name, (long) pid);
					killed++;
				} else {
					fprintf(stderr, "Failed to send %s to %s (pid %ld)\n",
						sig_name, name, (long) pid);
				}
			}
			waited = 0;
		} else {
			quicksand_sleep((u64) poll_ms * 1000000ULL);
			waited += poll_ms;
		}
	}

	if(killed == 0) {
		printf("No matching nodes found\n");
	}

	mirage_destroy(&msg, NULL);
}

int rosenode_main(int argc, char **argv) {
	int exit_code = 0;
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

		skip_to_latest(conn);
		send_dump_request(conn);
		read_and_print_list(conn);

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

		skip_to_latest(conn);
		if(is_pid_flag(argv[2])) {
			if(argc < 4) {
				print_usage();
				exit_code = 1;
				goto cleanup;
			}
			i64 pid = 0;
			if(parse_i64_arg(argv[3], &pid) != 0) {
				fprintf(stderr, "Invalid pid: %s\n", argv[3]);
				exit_code = 1;
				goto cleanup;
			}
			send_lookup_pid_request(conn, pid);
		} else {
			send_lookup_name_request(conn, argv[2]);
		}

		read_and_print_info(conn);

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

		skip_to_latest(conn);
		send_lookup_name_request(conn, name_arg);
		read_and_kill(conn, use_sigkill);

	} else {
		print_usage();
		exit_code = 1;
	}

cleanup:
	quicksand_disconnect(&conn, NULL);
	return exit_code;
}
