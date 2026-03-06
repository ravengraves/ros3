// rosemaster.c - Central registration daemon for ROS3
//
// This process must be started before any ROS3 nodes.
// It creates the rosemaster topic and logs all node registrations.

#define _POSIX_C_SOURCE 200809L

#include "ros3.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_NODES 2048
#define MAX_TOPICS 64
#define MAX_NAME_LEN 256

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
static u64 master_start_time = 0;

static volatile int running = 1;

static void handle_signal(int sig) {
	(void) sig;
	running = 0;
}

// Check if a process is still alive using kill(pid, 0)
// This is portable across Linux and macOS (POSIX)
static int is_process_alive(i64 pid) {
	if(pid <= 0) {
		return 0;
	}
	int ret = kill((pid_t) pid, 0);
	if(ret == 0) {
		return 1; // Process exists and we can signal it
	}
	if(errno == EPERM) {
		return 1; // Process exists but we don't have permission
	}
	// ESRCH means process doesn't exist
	return 0;
}

// Check nodes with stale heartbeats (>1s) and remove dead ones
static void check_nodes_alive(void) {
	u64 now = quicksand_now();
	for(int i = 0; i < node_count; i++) {
		tracked_node *n = &nodes[i];
		// Only check if heartbeat is older than 1 second
		if(quicksand_ns(now, n->last_heartbeat) < 1e9) {
			continue;
		}
		if(!is_process_alive(n->pid)) {
			printf("[rosemaster] Node '%s' (pid=%ld) is no longer alive, removing\n",
			       n->name, (long) n->pid);
			// Remove by shifting remaining nodes down
			for(int j = i; j < node_count - 1; j++) {
				nodes[j] = nodes[j + 1];
			}
			node_count--;
			i--; // Re-check this index since we shifted
		}
	}
}

// Find node by name, or create a new one if not found
static tracked_node *find_or_create_node(const char *name) {
	for(int i = 0; i < node_count; i++) {
		if(strcmp(nodes[i].name, name) == 0) {
			return &nodes[i];
		}
	}
	if(node_count >= MAX_NODES) {
		return NULL;
	}
	tracked_node *n = &nodes[node_count++];
	memset(n, 0, sizeof(*n));
	strncpy(n->name, name, MAX_NAME_LEN - 1);
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
		strncpy(n->pubs[n->pub_count++], topic, MAX_NAME_LEN - 1);
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
		strncpy(n->subs[n->sub_count++], topic, MAX_NAME_LEN - 1);
	}
}

// Print current node status
static void print_status(void) {
	u64 now = quicksand_now();
	f64 uptime = quicksand_ns(now, master_start_time) * 1e-9;

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
	fflush(stdout);
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
static void send_node_info(quicksand_connection *conn, mirage_msg *out,
			   tracked_node *n) {
	static char pubs_csv[MAX_TOPICS * MAX_NAME_LEN];
	static char subs_csv[MAX_TOPICS * MAX_NAME_LEN];

	build_topic_csv(pubs_csv, sizeof(pubs_csv), n->pubs, n->pub_count);
	build_topic_csv(subs_csv, sizeof(subs_csv), n->subs, n->sub_count);

	i64 r;
	// Reset length to max for writing (may have been set smaller by previous read)
	out->length = out->max_length;

	r = mirage_write_start(out);
	if(r < 0) {
		return;
	}

	r = mirage_write_fn(out, ROSE_MSG_NODE_INFO, (i64) strlen(ROSE_MSG_NODE_INFO), 5);
	if(r < 0) {
		return;
	}

	r = mirage_write_string(out, n->name, (i64) strlen(n->name));
	if(r < 0) {
		return;
	}

	r = mirage_write_i64(out, n->pid);
	if(r < 0) {
		return;
	}

	r = mirage_write_i64(out, (i64) n->last_heartbeat);
	if(r < 0) {
		return;
	}

	r = mirage_write_string(out, pubs_csv, (i64) strlen(pubs_csv));
	if(r < 0) {
		return;
	}

	r = mirage_write_string(out, subs_csv, (i64) strlen(subs_csv));
	if(r < 0) {
		return;
	}

	quicksand_write(conn, out->data, out->cursor);
}

// Send Done response
static void send_done(quicksand_connection *conn, mirage_msg *out) {
	out->length = out->max_length;

	int64_t err = 0;
	err |= mirage_write_start(out);
	err |= mirage_write_fn(out, ROSE_MSG_DONE,
			       (i64) strlen(ROSE_MSG_DONE), 0);

	if(err >= 0) {
		quicksand_write(conn, out->data, out->cursor);
	}
}

// Handle dump request - send all nodes then Done
static void handle_dump_request(quicksand_connection *conn, mirage_msg *in,
				mirage_msg *out) {
	static char fn_name[64];
	i64 fn_len, fn_argc;

	in->cursor = 0;
	if(mirage_read_start(in) < 0) {
		return;
	}
	if(mirage_read_fn(in, fn_name, &fn_len, sizeof(fn_name) - 1, &fn_argc) < 0) {
		return;
	}
	fn_name[fn_len] = '\0';

	if(strcmp(fn_name, ROSE_MSG_DUMP_ALL) != 0) {
		return;
	}

	printf("[rosemaster] DumpAll request received\n");

	// Send all active nodes
	for(int i = 0; i < node_count; i++) {
		if(nodes[i].active) {
			send_node_info(conn, out, &nodes[i]);
		}
	}

	// Send done
	send_done(conn, out);
}

// Handle lookup request - send matching nodes then Done
static void handle_lookup_request(quicksand_connection *conn, mirage_msg *in,
				  mirage_msg *out) {
	static char fn_name[64];
	static char name_prefix[256];
	i64 fn_len, fn_argc;
	i64 name_len;
	i64 pid;

	in->cursor = 0;
	if(mirage_read_start(in) < 0) {
		return;
	}
	if(mirage_read_fn(in, fn_name, &fn_len, sizeof(fn_name) - 1, &fn_argc) < 0) {
		return;
	}
	fn_name[fn_len] = '\0';

	if(strcmp(fn_name, ROSE_MSG_LOOKUP_NAME) == 0) {
		if(mirage_read_string(in, name_prefix, &name_len,
				      sizeof(name_prefix) - 1)
		   < 0) {
			return;
		}
		name_prefix[name_len] = '\0';

		printf("[rosemaster] LookupName request: prefix='%s'\n", name_prefix);

		// Send matching nodes (prefix match)
		for(int i = 0; i < node_count; i++) {
			if(nodes[i].active && strncmp(nodes[i].name, name_prefix, name_len) == 0) {
				send_node_info(conn, out, &nodes[i]);
			}
		}
	} else if(strcmp(fn_name, ROSE_MSG_LOOKUP_PID) == 0) {
		if(mirage_read_i64(in, &pid) < 0) {
			return;
		}

		printf("[rosemaster] LookupPid request: pid=%ld\n", (long) pid);

		// Send matching nodes (exact pid match)
		for(int i = 0; i < node_count; i++) {
			if(nodes[i].active && nodes[i].pid == pid) {
				send_node_info(conn, out, &nodes[i]);
			}
		}
	} else {
		return;
	}

	// Send done
	send_done(conn, out);
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

	tracked_node *node = find_or_create_node(arg1);

	if(strcmp(fn_name, ROSE_MSG_NODE_START) == 0) {
		// NodeStart: check if alive, then set pid
		if(mirage_read_i64(msg, &pid) >= 0) {
			if(!is_process_alive(pid)) {
				return;
			}
			if(node) {
				node->pid = pid;
				node->last_heartbeat = quicksand_now();
			}
			printf("[rosemaster] %s: node=%s pid=%ld\n",
			       fn_name, arg1, (long) pid);
		}
	} else if(strcmp(fn_name, ROSE_MSG_HEARTBEAT) == 0) {
		// Heartbeat: read pid, timestamp, and validate
		i64 msg_timestamp;
		if(mirage_read_i64(msg, &pid) >= 0 && mirage_read_i64(msg, &msg_timestamp) >= 0) {
			u64 now = quicksand_now();
			f64 age = quicksand_ns(now, (u64) msg_timestamp) * 1e-9;
			// Ignore heartbeats older than 1 second
			if(age > 1.0) {
				return;
			}
			// Check if process is still alive
			if(!is_process_alive(pid)) {
				return;
			}
			if(node) {
				node->pid = pid;
				node->last_heartbeat = now;
			}
		}
	} else if(strcmp(fn_name, ROSE_MSG_PUB_REGISTER) == 0) {
		// PubRegister: add topic to node's pub list
		if(mirage_read_string(msg, arg2, &arg2_len, sizeof(arg2) - 1) >= 0) {
			arg2[arg2_len] = '\0';
			add_publisher(node, arg2);
			printf("[rosemaster] %s: node=%s topic=%s\n",
			       fn_name, arg1, arg2);
		}
	} else if(strcmp(fn_name, ROSE_MSG_SUB_REGISTER) == 0) {
		// SubRegister: add topic to node's sub list
		if(mirage_read_string(msg, arg2, &arg2_len, sizeof(arg2) - 1) >= 0) {
			arg2[arg2_len] = '\0';
			add_subscriber(node, arg2);
			printf("[rosemaster] %s: node=%s topic=%s\n",
			       fn_name, arg1, arg2);
		}
	}
}

int main(int argc, char **argv) {
	(void) argc;
	(void) argv;

	if(rose_set_pdeathsig(SIGTERM) != 0) {
		perror("[rosemaster] prctl(PR_SET_PDEATHSIG)");
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	printf("[rosemaster] Starting rosemaster daemon...\n");

	// Build the rosemaster topic name: /<ROSE_BASE>/rosemaster
	char topic[512];
	rose_topic_to_shm(topic, sizeof(topic), "rosemaster");

	printf("[rosemaster] Creating topic: %s\n", topic);

	// Create the rosemaster topic - we're a publisher but we'll read from it
	quicksand_connection *pub = NULL;
	int64_t ret = quicksand_connect(&pub, topic, -1, 512, 8192, NULL);
	if(ret != 0) {
		fprintf(stderr, "[rosemaster] Failed to create topic: %ld\n", (long) ret);
		return 1;
	}

	// Build dump topic name
	char dump_topic[512];
	snprintf(dump_topic, sizeof(dump_topic), "%s_dump", topic);
	printf("[rosemaster] Creating dump topic: %s\n", dump_topic);

	quicksand_connection *dump_conn = NULL;
	ret = quicksand_connect(&dump_conn, dump_topic, -1,
				ROSE_QUERY_MSG_SIZE, ROSE_DUMP_RING_SIZE, NULL);
	if(ret != 0) {
		fprintf(stderr, "[rosemaster] Failed to create dump topic: %ld\n", (long) ret);
		return 1;
	}

	// Build lookup topic name
	char lookup_topic[512];
	snprintf(lookup_topic, sizeof(lookup_topic), "%s_lookup", topic);
	printf("[rosemaster] Creating lookup topic: %s\n", lookup_topic);

	quicksand_connection *lookup_conn = NULL;
	ret = quicksand_connect(&lookup_conn, lookup_topic, -1,
				ROSE_QUERY_MSG_SIZE, ROSE_LOOKUP_RING_SIZE, NULL);
	if(ret != 0) {
		fprintf(stderr, "[rosemaster] Failed to create lookup topic: %ld\n", (long) ret);
		return 1;
	}

	printf("[rosemaster] Ready. Listening for node registrations...\n");
	fflush(stdout);

	mirage_msg *msg = mirage_create(1024, NULL);
	if(!msg) {
		fprintf(stderr, "[rosemaster] Failed to allocate message buffer\n");
		return 1;
	}

	// Larger buffer for query responses
	mirage_msg *query_msg = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	if(!query_msg) {
		fprintf(stderr, "[rosemaster] Failed to allocate query message buffer\n");
		return 1;
	}

	master_start_time = quicksand_now();
	u64 last_status_print = master_start_time;

	i64 sz = msg->max_length;
	// quicksand_read_latest(pub, msg->data, &sz); // skip - read historical node data (TODO Alec, 2026: use timestamps to filter old)
	sz = query_msg->max_length;
	quicksand_read_latest(dump_conn, query_msg->data, &sz);
	sz = query_msg->max_length;
	quicksand_read_latest(lookup_conn, query_msg->data, &sz);

	while(running) {
		// Read all registration messages
		i64 sz = msg->max_length;
		i64 r = quicksand_read(pub, msg->data, &sz);
		while(r >= 0) {
			msg->length = sz;
			process_registration(msg);
			sz = msg->max_length;
			r = quicksand_read(pub, msg->data, &sz);
		}

		// Handle dump requests
		sz = query_msg->max_length;
		r = quicksand_read(dump_conn, query_msg->data, &sz);
		if(r >= 0) {
			query_msg->length = sz;
			handle_dump_request(dump_conn, query_msg, query_msg);
		}

		// Handle lookup requests
		sz = query_msg->max_length;
		r = quicksand_read(lookup_conn, query_msg->data, &sz);
		if(r >= 0) {
			query_msg->length = sz;
			handle_lookup_request(lookup_conn, query_msg, query_msg);
		}

		// Check node liveness and print status every 1 second
		u64 now = quicksand_now();
		if(quicksand_ns(now, last_status_print) >= 1e9) {
			check_nodes_alive();
			print_status();
			last_status_print = now;
		}

		quicksand_sleep(10e6); // 10ms poll
	}

	printf("[rosemaster] Shutting down...\n");
	// mirage_destroy(&msg, NULL);

	return 0;
}
