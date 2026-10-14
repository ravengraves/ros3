// Intercept signaling: malformed responses must never reach the OS.
#define kill test_kill
#include "../cmd/rosenode/main.c"
#undef kill
#include <assert.h>
#include <sys/wait.h>

static int signals_sent;
static rose_query expected = {.requester_pid = 987, .timestamp = 12345};
int test_kill(pid_t pid, int sig) {
	assert(pid == 123 && sig == SIGINT);
	signals_sent++;
	return 0;
}

static void reply(quicksand_connection *c, const char *name, i64 pid) {
	mirage_msg *m = mirage_create(ROSE_QUERY_MSG_SIZE, NULL);
	assert(m);
	assert(mirage_write_start(m) >= 0);
	assert(mirage_write_fn(m, ROSE_MSG_NODE_INFO, 8, 7) >= 0);
	assert(mirage_write_i64(m, expected.requester_pid) >= 0);
	assert(mirage_write_i64(m, expected.timestamp) >= 0);
	assert(mirage_write_string(m, name, (i64) strlen(name)) >= 0);
	assert(mirage_write_i64(m, pid) >= 0);
	assert(mirage_write_i64(m, 0) >= 0);
	assert(mirage_write_string(m, "", 0) >= 0);
	assert(mirage_write_string(m, "", 0) >= 0);
	assert(quicksand_write(c, m->data, m->cursor) >= 0);
	mirage_destroy(&m, NULL);
}

static void marker(quicksand_connection *c, const char *fn, int tagged) {
	mirage_msg *m = mirage_create(256, NULL);
	assert(m);
	assert(mirage_write_start(m) >= 0);
	assert(mirage_write_fn(m, fn, (i64) strlen(fn), tagged ? 2 : 0) >= 0);
	if(tagged) {
		assert(mirage_write_i64(m, expected.requester_pid) >= 0);
		assert(mirage_write_i64(m, expected.timestamp) >= 0);
	}
	assert(quicksand_write(c, m->data, m->cursor) >= 0);
	mirage_destroy(&m, NULL);
}

int main(void) {
	assert(!rose_valid_pid(0));
	assert(!rose_valid_pid(-1));
	assert(!rose_valid_pid(INT64_MAX));
	assert(rose_valid_pid(123));
	char topic[128];
	snprintf(topic, sizeof(topic), "/rose_control_test_%ld", (long) getpid());
	quicksand_connection *c = NULL;
	assert(quicksand_connect(&c, topic, -1, ROSE_QUERY_MSG_SIZE, 32, NULL) == 0);
	static u8 large[ROSE_QUERY_MSG_SIZE];
	assert(quicksand_write(c, large, sizeof(large)) >= 0);
	assert(skip_to_latest(c) == 0);
	i64 size = sizeof(large);
	assert(quicksand_read(c, large, &size) == -1);
	rose_query query = expected;
	// Wrong caller, then an old request from the same caller: neither Done
	// nor NodeInfo may affect the active lookup.
	expected.requester_pid++;
	marker(c, ROSE_MSG_DONE, 1);
	reply(c, "target", 456);
	expected = query;
	expected.timestamp--;
	marker(c, ROSE_MSG_DONE, 1);
	reply(c, "target", 456);
	expected = query;
	reply(c, "target", -1);
	reply(c, "target", 0);
	reply(c, "target", INT64_MAX);
	reply(c, "unrelated", 456);
	reply(c, "target", 123);
	marker(c, ROSE_MSG_DONE, 1);
	assert(setenv("ROSE_QUERY_TIMEOUT_MS", "50", 1) == 0);
	assert(read_and_kill(c, &query, 0, "target") == 0);
	assert(signals_sent == 1);
	// An old Done before Start must not complete a new dump.
	query = (rose_query) {.dump = 1};
	marker(c, ROSE_MSG_DONE, 0);
	marker(c, ROSE_MSG_START, 0);
	marker(c, ROSE_MSG_DONE, 0);
	assert(read_and_print_list(c, &query) == 0 && query.started);
	query = (rose_query) {.dump = 1};
	marker(c, ROSE_MSG_DONE, 0);
	assert(read_and_print_list(c, &query) == 1 && !query.started);
	// Continuous unrelated traffic must not extend the absolute deadline.
	pid_t writer = fork();
	assert(writer >= 0);
	if(writer == 0) {
		u64 start = rose_query_clock();
		while(rose_query_clock() - start < 200000000ULL) {
			marker(c, ROSE_MSG_DONE, 0);
			quicksand_sleep(1000000);
		}
		_exit(0);
	}
	query = expected;
	u64 start = rose_query_clock();
	assert(read_and_print_info(c, &query, NULL, 123) == 1);
	assert(rose_query_clock() - start < 180000000ULL);
	assert(waitpid(writer, NULL, 0) == writer);
	quicksand_disconnect(&c, NULL);
	quicksand_delete(topic, -1);
	puts("control requester checks passed");
	return 0;
}
