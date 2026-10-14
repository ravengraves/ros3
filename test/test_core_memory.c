#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
// Isolate core memory/ownership behavior from transport allocation and retries.
#define quicksand_connect memory_connect
#define quicksand_disconnect memory_disconnect
#define quicksand_delete memory_delete
#define quicksand_write memory_write
#define quicksand_read memory_read
#include "../src/ros3.c"
#include <assert.h>
#include <pthread.h>

static int connection_count;
static i64 write_result;
static int delete_count, successful_writes;

i64 memory_connect(quicksand_connection **out, char *name, i64 length,
		   i64 size, i64 rate, void *alloc) {
	(void) size;
	(void) rate;
	(void) alloc;
	if(length == -1) {
		length = (i64) strlen(name);
	}
	assert(length > 0 && length <= 255);
	*out = calloc(1, sizeof(**out));
	assert(*out);
	(*out)->buffer = calloc(1, sizeof(*(*out)->buffer));
	assert((*out)->buffer);
	memcpy((*out)->name, name, (size_t) length);
	++connection_count;
	return 0;
}
void memory_disconnect(quicksand_connection **connection, void *dealloc) {
	(void) dealloc;
	if(!connection || !*connection) {
		return;
	}
	free((*connection)->buffer);
	free(*connection);
	*connection = NULL;
	--connection_count;
}
void memory_delete(char *name, i64 length) {
	(void) name;
	(void) length;
	++delete_count;
}
i64 memory_write(quicksand_connection *c, u8 *data, i64 length) {
	assert(c && data && length >= 0);
	if(!write_result) {
		++successful_writes;
	}
	return write_result;
}
i64 memory_read(quicksand_connection *c, u8 *data, i64 *length) {
	if(c->read_index >= atomic_load(&c->buffer->index)) {
		return -1;
	}
	assert(*length >= 1);
	data[0] = 42;
	*length = 1;
	++c->read_index;
	return 0;
}

typedef union {
	max_align_t alignment;
	struct {
		int owner;
	} value;
} allocation_header;
static size_t calls, fail_call, live[3];
static void *arena_alloc(size_t size, int owner) {
	if(++calls == fail_call) {
		return NULL;
	}
	allocation_header *header = malloc(sizeof(*header) + size);
	assert(header);
	header->value.owner = owner;
	++live[owner];
	return header + 1;
}
static void arena_free(void *ptr, int owner) {
	if(!ptr) {
		return;
	}
	allocation_header *header = (allocation_header *) ptr - 1;
	assert(header->value.owner == owner && live[owner]);
	header->value.owner = -1;
	--live[owner];
	free(header);
}
static void *alloc_a(size_t size) { return arena_alloc(size, 1); }
static void *alloc_b(size_t size) { return arena_alloc(size, 2); }
static void free_a(void *ptr) { arena_free(ptr, 1); }
static void free_b(void *ptr) { arena_free(ptr, 2); }
static rose_node *node_a(void) {
	return rose_create_node("node", -1, NULL, -1, NULL, NULL, 0, alloc_a, free_a);
}

static void test_bounds(void) {
	char output[1024], base[700];
	memset(base, 'a', sizeof(base) - 1);
	base[sizeof(base) - 1] = '\0';
	assert(setenv("ROSE_BASE", base, 1) == 0);
	assert(!rose_topic_to_shm(output, sizeof(output), "/ab"));
	assert(!node_a());
	for(size_t length = 0; length <= 1; ++length) {
		base[length] = '\0';
		assert(setenv("ROSE_BASE", length ? "/" : base, 1) == 0);
		assert(!rose_topic_to_shm(output, sizeof(output), "ab"));
		assert(!node_a());
	}
	assert(setenv("ROSE_BASE", "rose", 1) == 0);
	assert(rose_topic_to_shm(output, sizeof(output), "/ab"));
	assert(strcmp(output, "/rose%ab") == 0);
	assert(!rose_topic_to_shm(output, 8, "/ab"));
	char *long_prefix = malloc(700);
	assert(long_prefix);
	memset(long_prefix, 'p', 700);
	assert(!rose_create_node("node", -1, long_prefix, 700, NULL, NULL, 0, alloc_a, free_a));
	free(long_prefix);
	char *exact_name = malloc(3), *exact_prefix = malloc(3);
	assert(exact_name && exact_prefix);
	memcpy(exact_name, "abc", 3);
	memcpy(exact_prefix, "/ns", 3);
	rose_node *n = rose_create_node(exact_name, 3, exact_prefix, 3, NULL, NULL, 0, alloc_a, free_a);
	assert(n && strcmp(n->prefix, "/rose%ns%") == 0);
	char *exact_topic = malloc(3);
	assert(exact_topic);
	memcpy(exact_topic, "/ab", 3);
	rose_publisher *p = rose_create_pub(n, exact_topic, 3, 16, 2);
	assert(p && strcmp((char *) p->pub->name, "/rose%ab") == 0);
	assert(!rose_create_pub(n, exact_topic, 3, 0, 2));
	char embedded[] = {'a', '\0', 'b'};
	assert(!rose_create_pub(n, embedded, 3, 16, 2));
	assert(!rose_create_pub(n, exact_topic, INT64_MAX, 16, 2));
	assert(!rose_create_node(embedded, 3, NULL, -1, NULL, NULL, 0, alloc_a, free_a));
	assert(!rose_create_node("node", -1, embedded, 3, NULL, NULL, 0, alloc_a, free_a));
	rose_shutdown(n);
	free(exact_name);
	free(exact_prefix);
	free(exact_topic);
	char *empty_prefix = malloc(1);
	assert(empty_prefix);
	*empty_prefix = '/';
	n = rose_create_node("node", -1, empty_prefix, 0, NULL, NULL, 0, alloc_a, free_a);
	assert(n && strcmp(n->prefix, "/rose%") == 0);
	rose_shutdown(n);
	free(empty_prefix);
	char bounded[256];
	memset(bounded, 'a', sizeof(bounded));
	int pid_length = snprintf(output, sizeof(output), "-%lld", (long long) rose_platform_getpid());
	i64 max_node = 255 - 6 - pid_length - (i64) strlen("%control");
	n = rose_create_node(bounded, max_node, NULL, -1, NULL, NULL, 0, alloc_a, free_a);
	assert(n && strlen((char *) n->control_sub.sub->name) == 255);
	rose_shutdown(n);
	assert(!rose_create_node(bounded, max_node + 1, NULL, -1, NULL, NULL, 0, alloc_a, free_a));
	assert(!live[1] && !connection_count);
}

static void test_remap_capacity(void) {
	assert(setenv("ROSE_BASE", "r", 1) == 0);
	char rules[18][254];
	char *argv[19] = {"test"};
	for(int i = 0; i < 18; ++i) {
		memset(rules[i], 'a', 250);
		memcpy(rules[i] + 250, ":=b", 4);
		argv[i + 1] = rules[i];
	}
	// 16 * 253 + 48 = 4096 exact storage bytes, including both NULs.
	memset(rules[16], 'c', 45);
	memcpy(rules[16] + 45, ":=b", 4);
	rose_node *n = rose_init(18, argv, "node", alloc_a, free_a);
	assert(n && n->remap_count == 17);
	assert(n->remap_strlens[32] == 45 && n->remap_strlens[33] == 1);
	rose_shutdown(n);
	assert(!rose_init(19, argv, "node", alloc_a, free_a));
	memset(rules[16], 'a', 250);
	memcpy(rules[16] + 250, ":=b", 4);
	assert(!rose_init(18, argv, "node", alloc_a, free_a));
	assert(!live[1] && !connection_count);
	assert(setenv("ROSE_BASE", "rose", 1) == 0);
}

static void test_failures(void) {
	char *argv[] = {"test", "local:=mapped"};
	for(size_t failure = 1; failure <= 8; ++failure) {
		calls = 0;
		fail_call = failure;
		assert(!rose_init(2, argv, "node", alloc_a, free_a));
		assert(!live[1] && !connection_count);
	}
	fail_call = 0;
	assert(!rose_init(2, argv, "node", alloc_a, NULL));
	assert(!rose_create_node("node", -1, NULL, -1, NULL, NULL, 0, NULL, free_a));
	// Direct callers retain their remaps on construction failure.
	char *remaps = alloc_a(16);
	u64 *lengths = alloc_a(2 * sizeof(*lengths));
	memcpy(remaps, "local\0mapped\0", 13);
	lengths[0] = 5;
	lengths[1] = 6;
	calls = 0;
	fail_call = 5;
	assert(!rose_create_node("node", -1, NULL, -1, remaps, lengths, 1, alloc_a, free_a));
	assert(live[1] == 2);
	free_a(remaps);
	free_a(lengths);
	fail_call = 0;
	assert(!live[1]);
}

static int callbacks;
static void callback(mirage_msg *message) {
	assert(message->length == 1 && message->data[0] == 42);
	++callbacks;
}
static void *thread_lifecycle(void *arg) {
	(void) arg;
	rose_node *a = node_a();
	rose_node *b = rose_create_node("other", -1, NULL, -1, NULL, NULL, 0, alloc_b, free_b);
	assert(a && b);
	rose_subscriber *sa = rose_create_sub(a, "one", -1, 0, callback);
	rose_subscriber *sb = rose_create_sub(b, "two", -1, 0, callback);
	assert(sa && sb);
	atomic_store(&sa->sub->buffer->index, 1);
	atomic_store(&sb->sub->buffer->index, 1);
	rose_spin_once(a);
	rose_spin_once(b);
	assert(callbacks == 2 && a->callback_scratch != b->callback_scratch);
	mirage_msg *reply = mirage_create(128, NULL);
	assert(reply);
	write_result = -EIO;
	assert(rose_get_param(a, "first", -1, reply) == -EIO);
	mirage_msg *prior = a->param_request_scratch;
	assert(prior);
	i64 prior_capacity = prior->max_length;
	char longer[300];
	memset(longer, 'k', sizeof(longer));
	fail_call = calls + 1;
	assert(rose_get_param(a, longer, sizeof(longer), reply) == ROSE_ERR_BUFFERSIZE);
	assert(a->param_request_scratch == prior && prior->max_length == prior_capacity);
	fail_call = 0;
	assert(rose_get_param(a, longer, sizeof(longer), reply) == -EIO);
	assert(a->param_request_scratch->max_length > prior_capacity);
	assert(rose_get_param(a, "x", INT64_MAX, reply) == ROSE_ERR_BADINPUT);
	assert(rose_set_param(a, "x", INT64_MAX, reply) == ROSE_ERR_BADINPUT);
	assert(rose_get_param(b, "second", -1, reply) == -EIO);
	assert(rose_set_param(a, "first", -1, reply) == -EIO);
	write_result = 0;
	mirage_destroy(&reply, NULL);
	rose_shutdown(a);
	assert(!live[1]);
	atomic_store(&sb->sub->buffer->index, 2);
	rose_spin_once(b);
	assert(callbacks == 3);
	rose_shutdown(b);
	assert(!live[2] && !connection_count);
	return NULL;
}

int main(void) {
	test_bounds();
	test_remap_capacity();
	test_failures();
	pthread_t thread;
	assert(pthread_create(&thread, NULL, thread_lifecycle, NULL) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!live[1] && !live[2] && !connection_count);
	puts("core memory: namespace/spans, exact remap capacity, allocation failures and allocator-owned scratch passed");
	return 0;
}
