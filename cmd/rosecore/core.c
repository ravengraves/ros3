// rosecore.c - Central registration daemon for ROS3
//
// This process must be started before any ROS3 nodes.
// It creates the rosecore topic and logs all node registrations.

#define _POSIX_C_SOURCE 200809L

#include "core.h"
#include "ros3.h"
#include "../common/process.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_NODES ROSE_MAX_NODES
#define MAX_TOPICS ROSE_MAX_TOPICS
#define MAX_NAME_LEN ROSE_MAX_NAME_LEN
#define CORE_HEARTBEAT_MSG_SIZE ((i64) sizeof(i64))
#define CORE_HEARTBEAT_RATE 256
#define CORE_HEARTBEAT_STARTUP_WINDOW_NS 300000000.0
#define CORE_HEARTBEAT_STARTUP_PUB_NS 10000000.0
#define CORE_HEARTBEAT_STEADY_PUB_NS 100000000.0
#define CORE_HEARTBEAT_POLL_SLEEP_NS 1000000ULL

typedef struct {
	char name[MAX_NAME_LEN];
	i64 pid;
	char pubs[MAX_TOPICS][MAX_NAME_LEN];
	int pub_count;
	char subs[MAX_TOPICS][MAX_NAME_LEN];
	int sub_count;
	u64 last_heartbeat; // timestamp of last heartbeat
	int active;
} tracked_node;

static tracked_node nodes[MAX_NODES];
static int node_count = 0;
static u64 core_start_time = 0;

static volatile int running = 1;

static void copy_bounded_string(char *dst, size_t dst_size, const char *src) {
	if(!dst || dst_size == 0) {
		return;
	}
	if(!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_size, "%s", src);
}

static int is_help_arg(const char *arg) {
	return strcmp(arg, "-h") == 0
	    || strcmp(arg, "--help") == 0
	    || strcmp(arg, "/h") == 0
	    || strcmp(arg, "/help") == 0
	    || strcmp(arg, "/?") == 0;
}

static void print_usage(const char *argv0) {
	fprintf(stderr, "Usage: %s\n", argv0);
	fprintf(stderr, "       %s /?\n", argv0);
}

static void handle_signal(int sig) {
	(void) sig;
	fprintf(stderr, "[rosecore] Rosebud...\n");
	running = 0;
}

static void unlink_topic_if_present(const char *topic) {
	if(!topic || !topic[0]) {
		return;
	}
	quicksand_delete((char *) topic, -1);
}

static void cleanup_core_topics(const char *topic,
				const char *dump_topic,
				const char *lookup_topic,
				const char *heartbeat_topic,
				const char *request_topic) {
	unlink_topic_if_present(topic);
	unlink_topic_if_present(dump_topic);
	unlink_topic_if_present(lookup_topic);
	unlink_topic_if_present(heartbeat_topic);
	unlink_topic_if_present(request_topic);
}

static int send_CORE_HEARTBEAT(quicksand_connection *conn, i64 self_pid) {
	if(!conn) {
		return -EINVAL;
	}
	return (int) quicksand_write(conn, (u8 *) &self_pid, CORE_HEARTBEAT_MSG_SIZE);
}

static int read_CORE_HEARTBEAT(quicksand_connection *conn, i64 *pid) {
	i64 sz = CORE_HEARTBEAT_MSG_SIZE;
	i64 read_pid = 0;
	i64 ret = quicksand_read(conn, (u8 *) &read_pid, &sz);
	if(ret < 0) {
		return (int) ret;
	}
	if(sz != CORE_HEARTBEAT_MSG_SIZE) {
		return -EINVAL;
	}
	*pid = read_pid;
	return 0;
}

static void skip_old_CORE_HEARTBEATs(quicksand_connection *conn) {
	i64 ignored_pid = 0;
	i64 sz = CORE_HEARTBEAT_MSG_SIZE;
	quicksand_read_latest(conn, (u8 *) &ignored_pid, &sz);
}

static int startup_heartbeat_resolution(quicksand_connection *pub,
					quicksand_connection *sub,
					i64 self_pid) {
	u64 start = quicksand_now();
	u64 last_pub = 0;

	skip_old_CORE_HEARTBEATs(sub);

	while(quicksand_ns(quicksand_now(), start) < CORE_HEARTBEAT_STARTUP_WINDOW_NS) {
		u64 now = quicksand_now();
		if(last_pub == 0
		   || quicksand_ns(now, last_pub) >= CORE_HEARTBEAT_STARTUP_PUB_NS) {
			if(send_CORE_HEARTBEAT(pub, self_pid) < 0) {
				fprintf(stderr, "[rosecore] Failed to publish startup heartbeat\n");
				return -1;
			}
			last_pub = now;
		}

		for(;;) {
			i64 seen_pid = 0;
			int ret = read_CORE_HEARTBEAT(sub, &seen_pid);
			if(ret < 0) {
				break;
			}
			if(seen_pid != self_pid) {
				fprintf(stderr,
					"[rosecore] Another rosecore is active (pid=%ld)\n",
					(long) seen_pid);
				return 1;
			}
		}

		quicksand_sleep(CORE_HEARTBEAT_POLL_SLEEP_NS);
	}

	return 0;
}

// Check if a process is still alive using kill(pid, 0)
// This is portable across Linux and macOS (POSIX)
static int is_process_alive(i64 pid) {
	if(!rose_valid_pid(pid)) {
		return 0;
	}
	int ret = kill((pid_t) pid, 0);
	if(ret == 0) {
		return 1; // Process exists and we can signal it
	}
	if(errno == EPERM) {
		return 1; // Process exists but we don't have permission
	}
	// Only ESRCH proves absence; other errors must not evict a live node.
	return errno != ESRCH;
}

// Mark the registry slot unused so it can be recycled.
static void release_node(tracked_node *n) {
	n->pid = -1;
	n->active = 0;
}

static int bind_node_process(tracked_node *n, i64 pid) {
	if(!is_process_alive(pid)) {
		return 0;
	}
	if(n->pid > 0 && n->pid != pid) {
		// A replacement process must not inherit its predecessor's topics.
		n->pub_count = 0;
		n->sub_count = 0;
	}
	n->pid = pid;
	return 1;
}

static void check_nodes_alive(void) {
	u64 now = quicksand_now();
	for(int i = 0; i < node_count; i++) {
		tracked_node *n = &nodes[i];
		if(!n->active) {
			continue;
		}
		// Allow provisional topic registrations one second to acquire a PID.
		if(n->pid <= 0 && quicksand_ns(now, n->last_heartbeat) < 1e9) {
			continue;
		}
		if(!is_process_alive(n->pid)) {
			printf("[rosecore] Node '%s' (pid=%ld) is no longer alive, removing\n",
			       n->name, (long) n->pid);
			release_node(n);
		}
	}
}

// Find node by name, or create a new one if not found
static tracked_node *find_or_create_node(const char *name) {
	tracked_node *free_slot = NULL;
	for(int i = 0; i < node_count; i++) {
		if(!nodes[i].active) {
			if(!free_slot) free_slot = &nodes[i];
		} else if(strcmp(nodes[i].name, name) == 0) {
			return &nodes[i];
		}
	}
	if(!free_slot) {
		if(node_count >= MAX_NODES) return NULL;
		free_slot = &nodes[node_count++];
	}
	tracked_node *n = free_slot;
	memset(n, 0, sizeof(*n));
	n->pid = -1;
	n->last_heartbeat = quicksand_now();
	copy_bounded_string(n->name, sizeof(n->name), name);
	n->active = 1;
	return n;
}

// Add a topic to a node's publisher list (if not already present)
static void add_publisher(tracked_node *n, const char *topic) {
	if(!n) {
		return;
	}
	for(int i = 0; i < n->pub_count; i++) {
		if(strcmp(n->pubs[i], topic) == 0) {
			return;
		}
	}
	if(n->pub_count < MAX_TOPICS) {
		copy_bounded_string(n->pubs[n->pub_count], sizeof(n->pubs[n->pub_count]),
				    topic);
		n->pub_count++;
	}
}

// Add a topic to a node's subscriber list (if not already present)
static void add_subscriber(tracked_node *n, const char *topic) {
	if(!n) {
		return;
	}
	for(int i = 0; i < n->sub_count; i++) {
		if(strcmp(n->subs[i], topic) == 0) {
			return;
		}
	}
	if(n->sub_count < MAX_TOPICS) {
		copy_bounded_string(n->subs[n->sub_count], sizeof(n->subs[n->sub_count]),
				    topic);
		n->sub_count++;
	}
}

// Print current node status
static void print_status(void) {
	u64 now = quicksand_now();
	f64 uptime = quicksand_ns(now, core_start_time) * 1e-9;

	printf("\n=== Node Status (%.1fs) ===\n", uptime);
	for(int i = 0; i < node_count; i++) {
		tracked_node *n = &nodes[i];
		if(!n->active) {
			continue;
		}

		f64 heartbeat_age = (n->last_heartbeat != 0)
				? quicksand_ns(now, n->last_heartbeat) * 1e-9
				: -1.0;

		printf("Node: %s (pid=%ld); ", n->name, (long) n->pid);
		// Heartbeat
		if(heartbeat_age < 0) {
			printf("Last heartbeat: (never); ");
		} else {
			printf("Last heartbeat: %.3fs ago; ", heartbeat_age);
		}

		// Publishers
		printf("Publishers: ");
		if(n->pub_count == 0) {
			printf("(none)");
		} else {
			for(int j = 0; j < n->pub_count; j++) {
				if(j > 0) {
					printf(", ");
				}
				printf("%s", n->pubs[j]);
			}
		}
		printf("; ");

		// Subscribers
		printf("Subscribers: ");
		if(n->sub_count == 0) {
			printf("(none)");
		} else {
			for(int j = 0; j < n->sub_count; j++) {
				if(j > 0) {
					printf(", ");
				}
				printf("%s", n->subs[j]);
			}
		}
		printf("\n");
	}
	printf("===========================\n\n");
	(void) fflush(stdout);
}

// Build comma-separated list of topics
static void build_topic_csv(char *buf, size_t buf_size,
			    char topics[][MAX_NAME_LEN], int count) {
	buf[0] = '\0';
	size_t pos = 0;
	for(int i = 0; i < count && pos < buf_size - 1; i++) {
		if(i > 0 && pos < buf_size - 1) {
			buf[pos++] = ',';
		}
		size_t len = strlen(topics[i]);
		if(pos + len >= buf_size) {
			break;
		}
		memcpy(buf + pos, topics[i], len);
		pos += len;
	}
	buf[pos] = '\0';
}

// Send NodeInfo response for a tracked node
static int send_node_info(quicksand_connection *conn, mirage_msg *out,
			   tracked_node *n, i64 requester_pid, i64 request_timestamp) {
	static char pubs_csv[MAX_TOPICS * MAX_NAME_LEN];
	static char subs_csv[MAX_TOPICS * MAX_NAME_LEN];

	build_topic_csv(pubs_csv, sizeof(pubs_csv), n->pubs, n->pub_count);
	build_topic_csv(subs_csv, sizeof(subs_csv), n->subs, n->sub_count);

	i64 r;
	// Reset length to max for writing (may have been set smaller by previous read)
	out->length = out->max_length;

	r = mirage_write_start(out);
	if(r < 0) {
		return -1;
	}

	r = mirage_write_fn(out, ROSE_MSG_NODE_INFO, (i64) strlen(ROSE_MSG_NODE_INFO), requester_pid ? 7 : 5);
	if(r < 0) {
		return -1;
	}

	if(requester_pid && (mirage_write_i64(out, requester_pid) < 0
			    || mirage_write_i64(out, request_timestamp) < 0)) return -1;

	r = mirage_write_string(out, n->name, (i64) strlen(n->name));
	if(r < 0) {
		return -1;
	}

	r = mirage_write_i64(out, n->pid);
	if(r < 0) {
		return -1;
	}

	r = mirage_write_i64(out, (i64) n->last_heartbeat);
	if(r < 0) {
		return -1;
	}

	r = mirage_write_string(out, pubs_csv, (i64) strlen(pubs_csv));
	if(r < 0) {
		return -1;
	}

	r = mirage_write_string(out, subs_csv, (i64) strlen(subs_csv));
	if(r < 0) {
		return -1;
	}

	return quicksand_write(conn, out->data, out->cursor) < 0 ? -1 : 0;
}

// Dump markers have no arguments; lookup Done echoes the request identity.
static int send_query_marker(quicksand_connection *conn, mirage_msg *out,
	const char *fn, i64 requester_pid, i64 request_timestamp) {
	if(mirage_write_start(out) < 0
	   || mirage_write_fn(out, fn, (i64) strlen(fn), requester_pid ? 2 : 0) < 0)
		return -1;
	if(requester_pid && (mirage_write_i64(out, requester_pid) < 0
			    || mirage_write_i64(out, request_timestamp) < 0)) return -1;
	return quicksand_write(conn, out->data, out->cursor) < 0 ? -1 : 0;
}

// Read only the request ring. Responses never re-enter the request queue.
static void handle_query_request(quicksand_connection *dump,
	quicksand_connection *lookup, mirage_msg *in, mirage_msg *out) {
	char fn[64], prefix[ROSE_MAX_NAME_LEN];
	i64 len, argc, prefix_len = 0, target_pid = 0;
	i64 requester_pid = 0, request_timestamp = 0;
	in->cursor = 0;
	if(mirage_read_start(in) < 0
	   || mirage_read_fn(in, fn, &len, sizeof(fn) - 1, &argc) < 0) return;
	fn[len] = '\0';
	int is_dump = strcmp(fn, ROSE_MSG_DUMP_ALL) == 0;
	int by_name = strcmp(fn, ROSE_MSG_LOOKUP_NAME) == 0;
	int by_pid = strcmp(fn, ROSE_MSG_LOOKUP_PID) == 0;
	if(!is_dump && !by_name && !by_pid) return;
	if(argc != (is_dump ? 0 : 3)) return;
	if(!is_dump) {
		if(mirage_read_i64(in, &requester_pid) < 0 || !rose_valid_pid(requester_pid)
		   || mirage_read_i64(in, &request_timestamp) < 0 || request_timestamp <= 0) return;
		if(by_name) {
			if(mirage_read_string(in, prefix, &prefix_len, sizeof(prefix) - 1) < 0) return;
			prefix[prefix_len] = '\0';
		} else if(mirage_read_i64(in, &target_pid) < 0 || !rose_valid_pid(target_pid)) return;
	}
	quicksand_connection *conn = is_dump ? dump : lookup;
	if(is_dump && send_query_marker(conn, out, ROSE_MSG_START, 0, 0) < 0) return;
	for(int i = 0; i < node_count; i++) {
		tracked_node *n = &nodes[i];
		if(!n->active || !rose_valid_pid(n->pid)) continue;
		if(!is_dump && !(by_name ? strncmp(n->name, prefix, (size_t) prefix_len) == 0
					: n->pid == target_pid)) continue;
		if(send_node_info(conn, out, n, requester_pid, request_timestamp) < 0) {
			fprintf(stderr, "[rosecore] Failed to publish query response\n");
			return; // Do not report successful completion of a partial response.
		}
	}
	if(send_query_marker(conn, out, ROSE_MSG_DONE, requester_pid, request_timestamp) < 0)
		fprintf(stderr, "[rosecore] Failed to publish query completion\n");
}

/* Validate wire names through the same conversion as library and CLI input.
 * Percent is a wire separator only; literal percent and comma are never legal
 * user name characters, keeping slash encoding and topology CSV reversible. */
static int valid_registration_name(const char *name, i64 length, int topic) {
	if(length <= 0 || length > 255 || memchr(name, '\0', (size_t) length)) return 0;
	char canonical[256];
	if(!topic) {
		return !memchr(name, '/', (size_t) length) &&
			rose_topic_to_shm(canonical, sizeof(canonical), name) != NULL;
	}
	if(name[0] != '/') return 0;
	const char *separator = memchr(name, '%', (size_t) length);
	if(!separator || separator == name + length - 1) return 0;
	char display[256];
	size_t tail_length = (size_t) (name + length - separator - 1);
	for(size_t i = 0; i < tail_length; ++i) {
		char c = separator[1 + i];
		/* Wire topics contain no slash except the initial slash. */
		if(c == '/') return 0;
		display[i] = c == '%' ? '/' : c;
	}
	display[tail_length] = '\0';
	return rose_topic_to_shm(canonical, sizeof(canonical), display) &&
		strcmp(canonical, name) == 0;
}

// Process a registration message and update node state
static void process_registration(mirage_msg *msg) {
	static char fn_name[64];
	i64 fn_len, fn_argc;
	static char arg1[256], arg2[256];
	i64 arg1_len, arg2_len;
	i64 pid;

	msg->cursor = 0; // Reset cursor for reading

	if(mirage_read_start(msg) < 0) {
		return;
	}
	if(mirage_read_fn(msg, fn_name, &fn_len, sizeof(fn_name) - 1, &fn_argc) < 0) {
		return;
	}
	fn_name[fn_len] = '\0';

	if(mirage_read_string(msg, arg1, &arg1_len, sizeof(arg1) - 1) < 0) {
		return;
	}
	arg1[arg1_len] = '\0';
	if(!valid_registration_name(arg1, arg1_len, 0)) return;

	tracked_node *node = NULL;

	if(strcmp(fn_name, ROSE_MSG_NODE_START) == 0) {
		// NodeStart: check if alive, then set pid
		if(mirage_read_i64(msg, &pid) >= 0) {
			if(!rose_valid_pid(pid)) return;
			node = find_or_create_node(arg1);
			if(node && bind_node_process(node, pid)) {
				node->last_heartbeat = quicksand_now();
			}
			printf("[rosecore] %s: node=%s pid=%ld\n",
			       fn_name, arg1, (long) pid);
		}
	} else if(strcmp(fn_name, ROSE_MSG_HEARTBEAT) == 0) {
		// Heartbeat: read pid, timestamp, and validate
		i64 msg_timestamp;
		if(mirage_read_i64(msg, &pid) >= 0 && mirage_read_i64(msg, &msg_timestamp) >= 0) {
			u64 now = quicksand_now();
			f64 age = quicksand_ns(now, (u64) msg_timestamp) * 1e-9;
			// Ignore heartbeats older than 1 second
			if(age < 0.0 || age > 1.0) {
				return;
			}
			// Check if process is still alive
			if(!rose_valid_pid(pid)) return;
			node = find_or_create_node(arg1);
			if(node && bind_node_process(node, pid)) {
				node->last_heartbeat = now;
			}
		}
	} else if(strcmp(fn_name, ROSE_MSG_PUB_REGISTER) == 0) {
		// PubRegister: add topic to node's pub list
		if(mirage_read_string(msg, arg2, &arg2_len, sizeof(arg2) - 1) >= 0) {
			arg2[arg2_len] = '\0';
			if(!valid_registration_name(arg2, arg2_len, 1)) return;
			node = find_or_create_node(arg1);
			add_publisher(node, arg2);
			printf("[rosecore] %s: node=%s topic=%s\n",
			       fn_name, arg1, arg2);
		}
	} else if(strcmp(fn_name, ROSE_MSG_SUB_REGISTER) == 0) {
		// SubRegister: add topic to node's sub list
		if(mirage_read_string(msg, arg2, &arg2_len, sizeof(arg2) - 1) >= 0) {
			arg2[arg2_len] = '\0';
			if(!valid_registration_name(arg2, arg2_len, 1)) return;
			node = find_or_create_node(arg1);
			add_subscriber(node, arg2);
			printf("[rosecore] %s: node=%s topic=%s\n",
			       fn_name, arg1, arg2);
		}
	}
}

int rosecore_main(int argc, char **argv) {
	if(argc > 1) {
		if(is_help_arg(argv[1])) {
			print_usage(argv[0]);
			return 0;
		}
		print_usage(argv[0]);
		return 1;
	}

	(void) argc;
	(void) argv;
	int exit_code = 0;
	char topic[512] = {0};
	char dump_topic[512] = {0};
	char lookup_topic[512] = {0};
	char heartbeat_topic[512] = {0};
	char request_topic[512] = {0};
	quicksand_connection *pub = NULL;
	quicksand_connection *dump_conn = NULL;
	quicksand_connection *request_conn = NULL;
	quicksand_connection *lookup_conn = NULL;
	quicksand_connection *heartbeat_pub = NULL;
	quicksand_connection *heartbeat_sub = NULL;
	mirage_msg *msg = NULL;
	mirage_msg *query_msg = NULL;
	int core_claimed = 0;
	i64 self_pid = (i64) getpid();
	int64_t ret = 0;

	(void) rose_set_pdeathsig(SIGTERM);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	printf("[rosecore] Starting rosecore daemon...\n");

	if(!rose_topic_to_shm(heartbeat_topic, sizeof(heartbeat_topic),
			     "rosecore_heartbeat")) {
		fprintf(stderr, "[rosecore] %s\n", rose_last_error());
		exit_code = 1;
		goto cleanup;
	}
	printf("[rosecore] Opening heartbeat topic: %s\n", heartbeat_topic);

	ret = quicksand_connect(&heartbeat_pub, heartbeat_topic, -1,
				CORE_HEARTBEAT_MSG_SIZE,
				CORE_HEARTBEAT_RATE, NULL);
	if(ret != 0) {
		fprintf(stderr, "[rosecore] Failed to create heartbeat topic: %ld\n",
			(long) ret);
		exit_code = 1;
		goto cleanup;
	}

	ret = quicksand_connect(&heartbeat_sub, heartbeat_topic, -1, 0, 0, NULL);
	if(ret != 0) {
		fprintf(stderr, "[rosecore] Failed to subscribe to heartbeat topic: %ld\n",
			(long) ret);
		exit_code = 1;
		goto cleanup;
	}

	ret = startup_heartbeat_resolution(heartbeat_pub, heartbeat_sub, self_pid);
	if(ret != 0) {
		exit_code = 1;
		goto cleanup;
	}
	core_claimed = 1;
	quicksand_disconnect(&heartbeat_sub, NULL);

	// Build the rosecore topic name: /<ROSE_BASE>/rosecore
	rose_topic_to_shm(topic, sizeof(topic), "rosecore");

	printf("[rosecore] Creating topic: %s\n", topic);

	// Create the rosecore topic - we're a publisher but we'll read from it
	ret = quicksand_connect(&pub, topic, -1, 512, 8192, NULL);
	if(ret != 0) {
		fprintf(stderr, "[rosecore] Failed to create topic: %ld\n", (long) ret);
		exit_code = 1;
		goto cleanup;
	}

	// Build dump topic name
	snprintf(dump_topic, sizeof(dump_topic), "%s_dump", topic);
	printf("[rosecore] Creating dump topic: %s\n", dump_topic);

	ret = quicksand_connect(&dump_conn, dump_topic, -1,
				ROSE_QUERY_MSG_SIZE, ROSE_DUMP_RING_SIZE, NULL);
	if(ret != 0) {
		fprintf(stderr, "[rosecore] Failed to create dump topic: %ld\n", (long) ret);
		exit_code = 1;
		goto cleanup;
	}

	// Build lookup topic name
	snprintf(lookup_topic, sizeof(lookup_topic), "%s_lookup", topic);
	printf("[rosecore] Creating lookup topic: %s\n", lookup_topic);

	ret = quicksand_connect(&lookup_conn, lookup_topic, -1,
				ROSE_QUERY_MSG_SIZE, ROSE_LOOKUP_RING_SIZE, NULL);
	if(ret != 0) {
		fprintf(stderr, "[rosecore] Failed to create lookup topic: %ld\n", (long) ret);
		exit_code = 1;
		goto cleanup;
	}

	rose_topic_to_shm(request_topic, sizeof(request_topic), ROSE_TOPIC_REQUEST);
	ret = quicksand_connect(&request_conn, request_topic, -1,
		ROSE_REQUEST_MSG_SIZE, ROSE_REQUEST_RING_SIZE, NULL);
	if(ret != 0) {
		fprintf(stderr, "[rosecore] Failed to create request topic: %ld\n", (long) ret);
		exit_code = 1;
		goto cleanup;
	}
	// Discard requests from before this daemon instance was ready.
	request_conn->read_index = atomic_load_explicit(&request_conn->buffer->index,
						      memory_order_acquire);
	request_conn->read_stamp = quicksand_now();

	printf("[rosecore] Ready. Listening for node registrations...\n");
	(void) fflush(stdout);

	msg = mirage_create(1024, NULL);
	if(!msg) {
		fprintf(stderr, "[rosecore] Failed to allocate message buffer\n");
		exit_code = 1;
		goto cleanup;
	}

	// Larger buffer for query responses
	query_msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!query_msg) {
		fprintf(stderr, "[rosecore] Failed to allocate query message buffer\n");
		exit_code = 1;
		goto cleanup;
	}

	core_start_time = quicksand_now();
	u64 last_status_print = core_start_time;
	u64 last_CORE_HEARTBEAT = core_start_time;


	while(running) {
		u64 now = quicksand_now();
		if(quicksand_ns(now, last_CORE_HEARTBEAT) >= CORE_HEARTBEAT_STEADY_PUB_NS) {
			if(send_CORE_HEARTBEAT(heartbeat_pub, self_pid) < 0) {
				fprintf(stderr, "[rosecore] Failed to publish heartbeat\n");
			} else {
				last_CORE_HEARTBEAT = now;
			}
		}

		// Read all registration messages
		i64 sz = msg->max_length;
		i64 r = quicksand_read(pub, msg->data, &sz);
		while(r >= 0) {
			msg->length = sz;
			process_registration(msg);
			sz = msg->max_length;
			r = quicksand_read(pub, msg->data, &sz);
		}

		// Drain a bounded batch so response traffic cannot delay new requests.
		for(int requests = 0; requests < ROSE_REQUEST_RING_SIZE; requests++) {
			sz = query_msg->max_length;
			r = quicksand_read(request_conn, query_msg->data, &sz);
			if(r < 0) break;
			query_msg->length = sz;
			handle_query_request(dump_conn, lookup_conn, query_msg, query_msg);
		}

		// Check node liveness and print status every 1 second
		now = quicksand_now();
		if(quicksand_ns(now, last_status_print) >= 1e9) {
			check_nodes_alive();
			print_status();
			last_status_print = now;
		}

		quicksand_sleep(10e6); // 10ms poll
	}

	printf("[rosecore] Shutting down...\n");
cleanup:
	for(int i = 0; i < node_count; i++) {
		if(nodes[i].active) release_node(&nodes[i]);
	}
	node_count = 0;
	mirage_destroy(&query_msg, NULL);
	mirage_destroy(&msg, NULL);
	quicksand_disconnect(&heartbeat_sub, NULL);
	quicksand_disconnect(&heartbeat_pub, NULL);
	quicksand_disconnect(&request_conn, NULL);
	quicksand_disconnect(&lookup_conn, NULL);
	quicksand_disconnect(&dump_conn, NULL);
	quicksand_disconnect(&pub, NULL);
	if(core_claimed) {
		cleanup_core_topics(topic, dump_topic, lookup_topic, heartbeat_topic, request_topic);
	}
	return exit_code;
}
