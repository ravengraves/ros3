#define _POSIX_C_SOURCE 200809L
#define quicksand_write capture_write
#include "../cmd/common/query.h"
static i64 election_read(quicksand_connection *, u8 *, i64 *);
#define quicksand_read election_read
#include "../cmd/rosecore/main.c"
#undef quicksand_read
#undef quicksand_write
#include <assert.h>
#include <sys/wait.h>

static i64 writes, largest;
static rose_query expected = {.dump = 1};
static int election_testing, election_reads;
static rose_core_heartbeat election_peer;

static i64 election_read(quicksand_connection *c, u8 *data, i64 *size) {
	(void) c;
	if(election_reads++) {
		return -1;
	}
	assert(*size >= (i64) sizeof(election_peer));
	election_peer.timestamp = quicksand_now();
	memcpy(data, &election_peer, sizeof(election_peer));
	*size = sizeof(election_peer);
	return 0;
}

i64 capture_write(quicksand_connection *c, u8 *data, i64 size) {
	(void) c;
	if(election_testing) {
		assert(size == sizeof(rose_core_heartbeat));
		return 0;
	}
	assert(data && size > 0 && size <= ROSE_QUERY_MSG_SIZE);
	mirage_msg m = {.data = data, .length = size, .max_length = size};
	static char name[ROSE_MAX_NAME_LEN], pubs[ROSE_TOPIC_CSV_SIZE], subs[ROSE_TOPIC_CSV_SIZE];
	i64 pid, heartbeat;
	int result = rose_query_parse(&expected, &m, name, sizeof(name), &pid, &heartbeat,
				      pubs, sizeof(pubs), subs, sizeof(subs));
	assert(result >= 0);
	if(expected.dump && writes == 0) {
		assert(result == 2);
	}
	writes++;
	if(size > largest) {
		largest = size;
	}
	return 0;
}

int main(void) {
	// Drive the real election loop: READY always wins, otherwise lower PID wins.
	election_testing = 1;
	for(int scenario = 0; scenario < 4; ++scenario) {
		quicksand_ringbuffer ring = {0};
		quicksand_connection conn = {.buffer = &ring};
		rose_core_heartbeat self = {1, ROSE_CORE_STARTING, 1000, quicksand_now(), 0};
		election_peer = self;
		election_peer.pid = scenario == 0 ? 999 : 1001;
		if(scenario == 2) {
			election_peer.state = ROSE_CORE_READY;
		}
		if(scenario == 3) {
			election_peer = self; // Ignore own heartbeat.
		}
		election_reads = 0;
		assert(startup_heartbeat_resolution(&conn, &self) == (scenario == 0 || scenario == 2));
	}
	election_testing = 0;
	puts("core election: lower STARTING PID and any READY peer take priority");
	char canonical_topic[256];
	assert(rose_topic_to_shm(canonical_topic, sizeof(canonical_topic), "robot/topic"));
	assert(valid_registration_name(canonical_topic, (i64) strlen(canonical_topic), 1));
	assert(!valid_registration_name("/rose%a,b", 9, 1));
	assert(!valid_registration_name("/rose%a%%b", 10, 1));
	assert(valid_registration_name("one/node-123", 12, 0));
	assert(valid_registration_name("two/node-123", 12, 0));
	assert(!valid_registration_name("/one/node-123", 13, 0));
	assert(!valid_registration_name("one//node-123", 13, 0));
	assert(!valid_registration_name("bad,name", 8, 0));
	assert(!valid_registration_name("bad%name", 8, 0));
	assert(!valid_registration_name("ok\0hidden", 9, 0));
	int pipefd[2];
	assert(pipe(pipefd) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if(child == 0) {
		close(pipefd[1]);
		char byte;
		(void) read(pipefd[0], &byte, 1);
		_exit(0);
	}
	close(pipefd[0]);
	mirage_msg *m = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	assert(m);
	assert(mirage_write_start(m) >= 0);
	assert(mirage_write_fn(m, ROSE_MSG_PUB_REGISTER, (i64) strlen(ROSE_MSG_PUB_REGISTER), 2) >= 0);
	assert(mirage_write_string(m, "rejected", 8) >= 0);
	assert(mirage_write_string(m, "/rose%a,b", 9) >= 0);
	process_registration(m);
	assert(node_count == 0);
	assert(mirage_write_start(m) >= 0);
	assert(mirage_write_fn(m, ROSE_MSG_HEARTBEAT, 9, 3) >= 0);
	assert(mirage_write_string(m, "child", 5) >= 0);
	assert(mirage_write_i64(m, child) >= 0);
	assert(mirage_write_i64(m, (i64) quicksand_now()) >= 0);
	process_registration(m);
	tracked_node *n = &nodes[0];
	assert(n->active && n->pid == child && is_process_alive(n->pid));
	add_publisher(n, "retained_topic");
	process_registration(m);
	assert(n->pid == child && n->pub_count == 1);
	assert(!bind_node_process(n, 0));
	assert(!bind_node_process(n, -1));
	assert(!bind_node_process(n, INT64_MAX));
	assert(n->pid == child && n->pub_count == 1);
	close(pipefd[1]);
	assert(waitpid(child, NULL, 0) == child);
	check_nodes_alive();
	assert(!n->active && n->pid == -1);
	assert(find_or_create_node("replacement") == n);
	for(int i = 1; i < MAX_NODES; i++) {
		char name[32];
		snprintf(name, sizeof(name), "node%d", i);
		assert(find_or_create_node(name));
	}
	assert(!find_or_create_node("overflow"));
	for(int i = 0; i < MAX_NODES; i++) {
		n = &nodes[i];
		memset(n->name, 'n', MAX_NAME_LEN - 1);
		n->pid = getpid();
		n->last_heartbeat = UINT64_MAX;
		n->pub_count = n->sub_count = MAX_TOPICS;
		for(int j = 0; j < MAX_TOPICS; j++) {
			memset(n->pubs[j], 'p', MAX_NAME_LEN - 1);
			memset(n->subs[j], 's', MAX_NAME_LEN - 1);
		}
	}
	mirage_msg *request = mirage_create(256, NULL);
	assert(request);
	assert(mirage_write_start(request) >= 0);
	assert(mirage_write_fn(request, ROSE_MSG_DUMP_ALL, 7, 0) >= 0);
	handle_query_request(NULL, NULL, request, m);
	assert(writes == MAX_NODES + 2 && expected.complete);
	assert(largest > ROSE_MAX_NAME_LEN - 1 + 2 * (ROSE_TOPIC_CSV_SIZE - 1));
	assert(largest <= ROSE_QUERY_MSG_SIZE);
	writes = 0;
	expected = (rose_query) {.requester_pid = getpid(), .timestamp = 12345};
	assert(mirage_write_start(request) >= 0);
	assert(mirage_write_fn(request, ROSE_MSG_LOOKUP_NAME, 10, 3) >= 0);
	assert(mirage_write_i64(request, expected.requester_pid) >= 0);
	assert(mirage_write_i64(request, expected.timestamp) >= 0);
	assert(mirage_write_string(request, "n", 1) >= 0);
	handle_query_request(NULL, NULL, request, m);
	assert(writes == MAX_NODES + 1 && expected.complete);
	assert(ROSE_DUMP_RING_SIZE > MAX_NODES + 2);
	// A targeted request, one NodeInfo and Done fit in the retained window.
	assert(ROSE_LOOKUP_RING_SIZE / 2 >= 3);
	mirage_destroy(&request, NULL);
	mirage_destroy(&m, NULL);
	printf("registry checks passed: %ld replies, largest %ld bytes\n", (long) writes, (long) largest);
	return 0;
}
