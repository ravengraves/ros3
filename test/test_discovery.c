#define _GNU_SOURCE
#include "ros3.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>

static int writes, heartbeats, connections, heartbeat_reads, fail_write, fail_connect;
static i64 discovery_write(quicksand_connection *c, u8 *data, i64 size) {
	if(strstr((char *) c->name, "%rosecore") && !strstr((char *) c->name, "heartbeat")) {
		if(fail_write) {
			return -EAGAIN;
		}
		mirage_msg m = {.data = data, .length = size, .max_length = size};
		char type[32];
		i64 length, argc;
		assert(mirage_read_start(&m) >= 0);
		assert(mirage_read_fn(&m, type, &length, sizeof(type) - 1, &argc) >= 0);
		type[length] = 0;
		if(strcmp(type, ROSE_MSG_HEARTBEAT) == 0) {
			++heartbeats;
		} else {
			++writes;
		}
	}
	return quicksand_write(c, data, size);
}
static i64 discovery_connect(quicksand_connection **c, char *name, i64 len,
			     i64 size, i64 rate, void *alloc) {
	++connections;
	if(fail_connect && strstr(name, "%rosecore_heartbeat")) {
		return -ENOENT;
	}
	return quicksand_connect(c, name, len, size, rate, alloc);
}
static i64 discovery_read_latest(quicksand_connection *c, u8 *data, i64 *size) {
	if(strstr((char *) c->name, "%rosecore_heartbeat")) {
		++heartbeat_reads;
	}
	return quicksand_read_latest(c, data, size);
}
#define quicksand_write discovery_write
#define quicksand_connect discovery_connect
#define quicksand_read_latest discovery_read_latest
#include "../src/ros3.c"
#undef quicksand_write
#undef quicksand_connect
#undef quicksand_read_latest

static u64 interval_ticks, timeout_ticks;

static void test_concurrent_startup(void) {
	for(int round = 0; round < 4; ++round) {
		char base[80], topic[256];
		snprintf(base, sizeof(base), "discovery_start_%ld_%d", (long) getpid(), round);
		assert(setenv("ROSE_BASE", base, 1) == 0);
		int gate[2];
		pid_t children[16];
		assert(pipe(gate) == 0);
		for(int i = 0; i < 16; ++i) {
			children[i] = fork();
			assert(children[i] >= 0);
			if(children[i] == 0) {
				close(gate[1]);
				char start;
				assert(read(gate[0], &start, 1) == 0); // All start when the parent closes the gate.
				close(gate[0]);
				rose_node *n = rose_create_node("concurrent", -1, NULL, -1, NULL, NULL, 0, NULL, NULL);
				assert(n && rose_ok(n));
				assert(writes == 0); // No core exists yet.
				rose_shutdown(n);
				_exit(0);
			}
		}
		close(gate[0]);
		close(gate[1]);
		for(int i = 0; i < 16; ++i) {
			int status;
			assert(waitpid(children[i], &status, 0) == children[i]);
			assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		}
		const char *channels[] = {"rosecore", "roselog", "param_query"};
		for(int i = 0; i < 3; ++i) {
			assert(rose_topic_to_shm(topic, sizeof(topic), channels[i]));
			quicksand_delete(topic, -1);
		}
	}
	puts("discovery: 64 concurrent node startups passed");
}

static void tick(rose_node *n, quicksand_connection *h, rose_core_heartbeat *frame, u64 *now) {
	*now += interval_ticks;
	frame->timestamp = *now;
	assert(quicksand_write(h, (u8 *) frame, sizeof(*frame)) == 0);
	rose_maintain_discovery(n, *now);
}

static void test_recovery(void) {
	f64 ns_per_tick = quicksand_ns(1, 0);
	interval_ticks = (u64) (ROSE_CORE_INTERVAL_NS / ns_per_tick) + 1;
	timeout_ticks = (u64) (ROSE_CORE_TIMEOUT_NS / ns_per_tick) + 1;
	char base[80], heartbeat_topic[256];
	snprintf(base, sizeof(base), "discovery_unit_%ld", (long) getpid());
	assert(setenv("ROSE_BASE", base, 1) == 0);
	rose_node *n = rose_create_node("node", -1, "robot", -1, NULL, NULL, 0, NULL, NULL);
	assert(n);
	rose_node_storage *s = (rose_node_storage *) n;
	int initial_connections = connections;
	rose_spin_once(n);
	assert(connections == initial_connections && s->last_check == 0);
	assert(rose_ok(n));
	assert(writes == 0 && heartbeats == 0 && !n->last_heartbeat_ns);
	u64 now = quicksand_now();
	assert(rose_topic_to_shm(heartbeat_topic, sizeof(heartbeat_topic), ROSE_TOPIC_HEARTBEAT));
	// Maintenance uses the base captured at construction, including late attaches.
	assert(setenv("ROSE_BASE", "different_base", 1) == 0);
	for(int i = 0; i < 20; ++i) {
		char name[32];
		snprintf(name, sizeof(name), "data%d", i);
		assert(rose_create_pub(n, name, -1, 64, 16));
		assert(rose_create_sub(n, name, -1, 0, NULL)); // Polling subscriptions count too.
	}
	assert(writes == 0 && !n->registration_scratch);
	quicksand_connection *registration = n->rosecore_pub.pub;
	quicksand_connection *app_pub = n->pubs[0].pub, *app_sub = n->subs[0].sub;
	quicksand_connection *heartbeat = NULL;
	assert(quicksand_connect(&heartbeat, heartbeat_topic, -1, 32, 256, NULL) == 0);
	rose_core_heartbeat frame = {1, ROSE_CORE_STARTING, getpid(), now, now};
	fail_connect = 1;
	tick(n, heartbeat, &frame, &now);
	assert(!s->core_heartbeat && writes == 0 && heartbeats == 0);
	fail_connect = 0;
	tick(n, heartbeat, &frame, &now);
	assert(s->core_heartbeat && !s->core.timestamp && writes == 0 && heartbeats == 0);
	quicksand_connection *reader = s->core_heartbeat;
	frame.state = ROSE_CORE_READY;
	frame.version = 99;
	tick(n, heartbeat, &frame, &now);
	assert(!s->core.timestamp);
	frame.version = 1;
	// Future, stale, truncated and oversized frames cannot establish readiness.
	frame.timestamp = now + 100 * interval_ticks;
	assert(quicksand_write(heartbeat, (u8 *) &frame, sizeof(frame)) == 0);
	rose_maintain_discovery(n, now += interval_ticks);
	frame.timestamp = now - timeout_ticks - 1;
	assert(quicksand_write(heartbeat, (u8 *) &frame, sizeof(frame)) == 0);
	rose_maintain_discovery(n, now += interval_ticks);
	u8 malformed[40] = {0};
	assert(quicksand_write(heartbeat, malformed, 8) == 0);
	rose_maintain_discovery(n, now += interval_ticks);
	assert(quicksand_write(heartbeat, malformed, sizeof(malformed)) == 0);
	rose_maintain_discovery(n, now += interval_ticks);
	assert(!s->core.timestamp && writes == 0 && heartbeats == 0);
	fail_write = 1;
	tick(n, heartbeat, &frame, &now); // One READY frame is enough; failed writes stay pending.
	assert(s->core.timestamp == now && !s->registered_node && writes == 0);
	assert(heartbeats == 0 && !n->last_heartbeat_ns); // NodeStart must succeed first.
	fail_write = 0;
	tick(n, heartbeat, &frame, &now);
	assert(writes == 16 && s->registered_pubs == 15 && heartbeats == 1);
	int connects = connections;
	int reads = heartbeat_reads;
	for(int i = 0; i < 1000; ++i) {
		rose_maintain_discovery(n, now + i * (interval_ticks / 1000));
	}
	assert(writes == 16 && heartbeats == 1 && connections == connects && heartbeat_reads == reads);
	// Endpoints added during replay join the pending list without extra writes.
	assert(rose_create_pub(n, "added", -1, 64, 16));
	assert(rose_create_sub(n, "added", -1, 0, NULL));
	assert(writes == 16);
	tick(n, heartbeat, &frame, &now);
	assert(writes == 32 && heartbeats == 2);
	tick(n, heartbeat, &frame, &now);
	assert(writes == 43 && s->registered_pubs == 21 && s->registered_subs == 21 && heartbeats == 3);
	// Latest-only polling skips older queued identities; no periodic replay.
	writes = heartbeats = 0;
	for(int i = 0; i < 100; ++i) {
		rose_core_heartbeat obsolete = frame;
		++obsolete.pid;
		assert(quicksand_write(heartbeat, (u8 *) &obsolete, sizeof(obsolete)) == 0);
		tick(n, heartbeat, &frame, &now);
		assert(s->core.pid == frame.pid && s->core.timestamp == now);
	}
	assert(writes == 0 && heartbeats == 100 && connections == connects + 2);
	// STARTING pauses sending, even if the prior READY frame was still fresh.
	rose_core_heartbeat starting = frame;
	starting.state = ROSE_CORE_STARTING;
	++starting.pid;
	tick(n, heartbeat, &starting, &now);
	assert(!s->core.timestamp && s->core.pid == frame.pid);
	assert(writes == 0 && heartbeats == 100);
	rose_maintain_discovery(n, now += interval_ticks);
	assert(writes == 0 && heartbeats == 100);
	tick(n, heartbeat, &frame, &now);
	assert(writes == 0 && heartbeats == 101); // Original READY core resumes without replay.
	// A stale heartbeat stops sending but retains all connections and progress.
	rose_maintain_discovery(n, now += timeout_ticks + 1);
	assert(!rose_core_heartbeat_valid(&s->core, now));
	assert(s->core_heartbeat == reader && writes == 0 && heartbeats == 101);
	assert(rose_create_pub(n, "outage", -1, 64, 16));
	assert(rose_create_sub(n, "outage", -1, 0, NULL));
	assert(writes == 0);
	tick(n, heartbeat, &frame, &now);
	assert(writes == 2 && s->registered_pubs == 22 && s->registered_subs == 22);
	writes = 0;
	fail_write = 1;
	u64 last_heartbeat = n->last_heartbeat_ns;
	tick(n, heartbeat, &frame, &now);
	assert(n->last_heartbeat_ns == last_heartbeat);
	fail_write = 0;
	// Graceful restart reuses persistent rings and announces a new instance.
	u64 index = atomic_load(&heartbeat->buffer->index);
	quicksand_disconnect(&heartbeat, NULL);
	assert(quicksand_connect(&heartbeat, heartbeat_topic, -1, 32, 256, NULL) == 0);
	assert(atomic_load(&heartbeat->buffer->index) == index);
	frame.instance_stamp = now;
	frame.state = ROSE_CORE_STARTING;
	tick(n, heartbeat, &frame, &now);
	assert(writes == 0);
	frame.state = ROSE_CORE_READY;
	index = atomic_load(&registration->buffer->index);
	tick(n, heartbeat, &frame, &now);
	assert(writes == 16 && atomic_load(&registration->buffer->index) == index + 17);
	// A crash/restart during unfinished replay resets progress, even with the same PID.
	frame.instance_stamp = now;
	writes = 0;
	tick(n, heartbeat, &frame, &now);
	assert(writes == 16 && s->registered_pubs == 15);
	// Changed PID is also a new core, even with the same start timestamp.
	++frame.pid;
	writes = 0;
	tick(n, heartbeat, &frame, &now);
	assert(writes == 16 && s->registered_pubs == 15);
	assert(n->rosecore_pub.pub == registration && s->core_heartbeat == reader);
	assert(n->pubs[0].pub == app_pub && n->subs[0].sub == app_sub);
	assert(connections == connects + 4); // Only the two pairs of added endpoints.
	u8 stop = 1;
	assert(quicksand_write(n->control_sub.sub, &stop, 1) == 0);
	assert(!rose_ok(n));
	for(u64 i = 0; i < n->pubs_length; ++i) {
		quicksand_delete((char *) n->pubs[i].pub->name, -1);
	}
	quicksand_delete((char *) n->rosecore_pub.pub->name, -1);
	quicksand_delete((char *) n->log_pub.pub->name, -1);
	quicksand_delete((char *) n->param_pub.pub->name, -1);
	rose_shutdown(n);
	quicksand_disconnect(&heartbeat, NULL);
	quicksand_delete(heartbeat_topic, -1);
	puts("discovery: latest-only polling, single rate limit, instance-only replay, retries and persistent mappings passed");
}

static volatile sig_atomic_t stopped;
static void stop_fixture(int sig) {
	(void) sig;
	stopped = 1;
}

static void run_node(int with_spin) {
	char *args[] = {"test", with_spin ? "__ns:=spin" : "__ns:=ok", "input:=data"};
	rose_node *n = rose_init(3, args, "c_node", NULL, NULL);
	assert(n);
	signal(SIGINT, stop_fixture);
	signal(SIGTERM, stop_fixture);
	rose_publisher *pub = rose_create_pub(n, "input", -1, 64, 16);
	rose_subscriber *sub = rose_create_sub(n, "input", -1, 0, NULL);
	assert(pub && sub);
	assert(fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == 0);
	puts("READY");
	fflush(stdout);
	for(unsigned i = 0; !stopped; ++i) {
		if(!rose_ok(n)) {
			break;
		}
		if(with_spin) {
			rose_spin_once(n);
		}
		u8 data[64] = {42};
		mirage_msg message = {.data = data, .length = 1, .cursor = 1, .max_length = sizeof(data)};
		assert(rose_publish(pub, &message) == ROSE_OK);
		assert(rose_read(sub, &message) == ROSE_OK && data[0] == 42);
		char command;
		if(read(STDIN_FILENO, &command, 1) == 1 && command == 'a') {
			assert(rose_create_pub(n, "added", -1, 64, 16));
			assert(rose_create_sub(n, "added", -1, 0, NULL));
			puts("ADDED");
		}
		if(i % 10 == 0) {
			puts("TICK");
			fflush(stdout);
		}
		rose_sleep(.01);
	}
	rose_shutdown(n);
}

int main(int argc, char **argv) {
	if(argc == 1) {
		test_concurrent_startup();
		test_recovery();
	} else {
		run_node(strcmp(argv[1], "spin") == 0);
	}
	return 0;
}
