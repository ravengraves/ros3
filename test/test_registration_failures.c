#define main core_memory_main
#include "test_core_memory.c"
#undef main

static void test_node_registration_failures(void) {
	char *argv[] = {"test", "local:=mapped"};
	rose_node *existing = node_a();
	assert(existing);
	size_t existing_live = live[1];
	int existing_connections = connection_count;
	int deletes = delete_count, writes = successful_writes;
	// Include every construction allocation, including Mirage's scratch allocation.
	calls = 0;
	rose_node *probe = rose_init(2, argv, "probe", alloc_a, free_a);
	assert(probe);
	size_t construction_calls = calls;
	rose_shutdown(probe);
	ROSE_STATIC_NODE = existing;
	deletes = delete_count;
	writes = successful_writes;
	for(size_t failure = 1; failure <= construction_calls; ++failure) {
		calls = 0;
		fail_call = failure;
		assert(!rose_init(2, argv, "node", alloc_a, free_a));
		assert(rose_last_error_code() == ROSE_ERR_BUFFERSIZE);
		assert(live[1] == existing_live && connection_count == existing_connections);
		assert(ROSE_STATIC_NODE == existing);
		assert(delete_count == deletes && successful_writes == writes);
	}
	fail_call = 0;
	write_result = -EIO;
	assert(!rose_init(2, argv, "node", alloc_a, free_a));
	assert(rose_last_error_code() == -EIO);
	assert(live[1] == existing_live && connection_count == existing_connections);
	assert(ROSE_STATIC_NODE == existing && delete_count == deletes);
	// Direct callers still own remap storage after late transport failure.
	char *remaps = alloc_a(13);
	u64 *lengths = alloc_a(2 * sizeof(*lengths));
	memcpy(remaps, "local\0mapped\0", 13);
	lengths[0] = 5;
	lengths[1] = 6;
	assert(!rose_create_node("node", -1, NULL, -1, remaps, lengths, 1, alloc_a, free_a));
	assert(rose_last_error_code() == -EIO && live[1] == existing_live + 2);
	assert(strcmp(remaps, "local") == 0 && lengths[1] == 6);
	free_a(remaps);
	free_a(lengths);
	assert(connection_count == existing_connections && delete_count == deletes);
	assert(ROSE_STATIC_NODE == existing && successful_writes == writes);
	write_result = 0;
	rose_shutdown(existing);
	assert(!live[1] && !connection_count && !ROSE_STATIC_NODE);
}

static void test_endpoint_registration_failures(void) {
	rose_node *n = node_a();
	assert(n);
	rose_publisher *prior_pub = rose_create_pub(n, "prior", -1, 16, 2);
	rose_subscriber *prior_sub = rose_create_sub(n, "prior", -1, 0, callback);
	assert(prior_pub && prior_sub);
	quicksand_connection *pub_connection = prior_pub->pub, *sub_connection = prior_sub->sub;
	char topic[220];
	memset(topic, 't', sizeof(topic) - 1);
	topic[sizeof(topic) - 1] = 0;
	int connections = connection_count, writes = successful_writes, deletes = delete_count;
	size_t allocations = live[1];
	mirage_msg *scratch = n->registration_scratch;
	i64 capacity = scratch->max_length;
	for(int subscriber = 0; subscriber < 2; ++subscriber) {
		fail_call = calls + 1;
		void *endpoint = subscriber ? (void *) rose_create_sub(n, topic, -1, 0, callback)
					    : (void *) rose_create_pub(n, topic, -1, 16, 2);
		assert(!endpoint && rose_last_error_code() == ROSE_ERR_BUFFERSIZE);
		assert(n->registration_scratch == scratch && scratch->max_length == capacity);
		assert(n->pubs_length == 1 && n->subs_length == 1);
		assert(!n->pubs[1].pub && !n->subs[1].sub);
		assert(!n->pubs[1].node && !n->subs[1].node);
		assert(live[1] == allocations && connection_count == connections);
		assert(successful_writes == writes && delete_count == deletes);
		fail_call = 0;
		write_result = -EIO;
		endpoint = subscriber ? (void *) rose_create_sub(n, "retry", -1, 0, callback)
				      : (void *) rose_create_pub(n, "retry", -1, 16, 2);
		assert(!endpoint && rose_last_error_code() == -EIO);
		assert(n->pubs_length == 1 && n->subs_length == 1);
		assert(connection_count == connections && successful_writes == writes);
		assert(delete_count == deletes);
		write_result = 0;
	}
	assert(prior_pub->pub == pub_connection && prior_sub->sub == sub_connection);
	assert(rose_create_pub(n, topic, -1, 16, 2) == &n->pubs[1]);
	assert(rose_create_sub(n, topic, -1, 0, callback) == &n->subs[1]);
	assert(n->pubs_length == 2 && n->subs_length == 2 && successful_writes == writes + 2);
	assert(rose_last_error_code() == 0);
	rose_shutdown(n);
	assert(!live[1] && !connection_count);
}

int main(void) {
	assert(setenv("ROSE_BASE", "rose", 1) == 0);
	test_node_registration_failures();
	test_endpoint_registration_failures();
	puts("registration failures: allocation/write rollback, ownership, prior state and retry passed");
	return 0;
}
