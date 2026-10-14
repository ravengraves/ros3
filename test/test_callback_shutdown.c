// Reuse the isolated transport and shifted-pointer allocator harness.
#define main core_memory_main
#include "test_core_memory.c"
#undef main

static rose_node *active, *other;
static int callback_count, later_count;
static rose_subscriber *removed;

static void later_callback(mirage_msg *m) {
	(void) m;
	++later_count;
}

static void shutdown_callback(mirage_msg *m) {
	++callback_count;
	assert(m->length == 1 && m->data[0] == 42);
	size_t allocated = live[1];
	int connected = connection_count;
	rose_spin_once(active);
	assert(rose_last_error_code() == ROSE_ERR_BADINPUT);
	rose_spin_forever(active);
	assert(rose_last_error_code() == ROSE_ERR_BADINPUT);
	assert(m->length == 1 && m->data[0] == 42);
	rose_shutdown(active);
	rose_shutdown(active);
	assert(live[1] == allocated && connection_count == connected);
	assert(active->shutdown_requested && active->spin_active);
	assert(m->length == 1 && m->data[0] == 42);
	m->data[0] = 99; // Scratch remains writable until the callback returns.
}

static void other_shutdown_callback(mirage_msg *m) {
	assert(m->data[0] == 42);
	rose_shutdown(other);
}

static void nested_callback(mirage_msg *m) {
	assert(m->data[0] == 42);
	rose_spin_once(other);
	other = NULL;
	assert(live[2] == 0);
	assert(m->data[0] == 42 && !active->shutdown_requested);
	rose_delete_subscriber(active, removed);
}

static void ancestor_shutdown_callback(mirage_msg *m) {
	assert(m->data[0] == 42);
	rose_shutdown(active);
	assert(active->shutdown_requested && live[1]);
}

static void ancestor_dispatch_callback(mirage_msg *m) {
	rose_spin_once(other);
	assert(m->data[0] == 42 && active->shutdown_requested);
	assert(!other->shutdown_requested && !other->spin_active);
}

static rose_subscriber *ready(rose_node *n, char *topic, void (*cb)(mirage_msg *)) {
	rose_subscriber *s = rose_create_sub(n, topic, -1, 0, cb);
	assert(s);
	atomic_store(&s->sub->buffer->index, 1);
	return s;
}

int main(void) {
	assert(setenv("ROSE_BASE", "callbacktest", 1) == 0);
	for(int forever = 0; forever < 2; ++forever) {
		active = node_a();
		assert(active && !active->spin_active && !active->shutdown_requested);
		ready(active, "first", shutdown_callback);
		ready(active, "later", later_callback);
		if(forever) {
			rose_spin_forever(active);
		} else {
			rose_spin_once(active);
		}
		active = NULL;
		assert(!live[1] && !connection_count && !later_count);
	}
	assert(callback_count == 2);
	active = node_a();
	other = rose_create_node("other", -1, NULL, -1, NULL, NULL, 0, alloc_b, free_b);
	assert(active && other);
	ready(other, "other", other_shutdown_callback);
	ready(active, "nested", nested_callback);
	removed = ready(active, "removed", later_callback);
	rose_spin_once(active);
	assert(!active->spin_active && !active->shutdown_requested && !later_count);
	assert(live[1] && !live[2]);
	rose_shutdown(active);
	assert(!live[1] && !live[2] && !connection_count);
	// Shutdown of an idle second node remains immediate during a callback.
	active = node_a();
	other = rose_create_node("other", -1, NULL, -1, NULL, NULL, 0, alloc_b, free_b);
	assert(active && other);
	ready(active, "other", other_shutdown_callback);
	rose_spin_once(active);
	assert(!live[2] && !active->shutdown_requested);
	rose_shutdown(active);
	assert(!live[1] && !connection_count);
	// A nested node may request shutdown of its still-dispatching ancestor.
	active = node_a();
	other = rose_create_node("other", -1, NULL, -1, NULL, NULL, 0, alloc_b, free_b);
	assert(active && other);
	ready(other, "ancestor", ancestor_shutdown_callback);
	ready(active, "nested", ancestor_dispatch_callback);
	ready(active, "later", later_callback);
	rose_spin_once(active);
	assert(!live[1] && live[2] && !later_count);
	rose_shutdown(other);
	assert(!live[2] && !connection_count);
	// Scratch allocation failure must also release the dispatch lifetime guard.
	active = node_a();
	assert(active);
	fail_call = calls + 1;
	rose_spin_once(active);
	assert(!active->spin_active && !active->callback_scratch);
	fail_call = 0;
	rose_shutdown(active);
	assert(!live[1] && !connection_count);
	// Control stops spin before callbacks; the caller still owns the node.
	active = node_a();
	assert(active);
	ready(active, "pending", later_callback);
	atomic_store(&active->control_sub.sub->buffer->index, 1);
	active->control_sub.sub->buffer->length = 2;
	rose_spin_forever(active);
	assert(!later_count && !active->spin_active && active->last_heartbeat_ns);
	rose_shutdown(active);
	assert(!live[1] && !connection_count);

	// Oversized slots fail before registration; manual reads remain allowed.
	active = node_a();
	source_slot_size = MAX_MESSAGE_SIZE + 64;
	int before = connection_count, writes = successful_writes;
	assert(!rose_create_sub(active, "large", -1, 0, later_callback));
	assert(rose_last_error_code() == -EMSGSIZE);
	assert(strstr(rose_last_error(), "max callback size"));
	assert(connection_count == before && successful_writes == writes && !active->subs_length);
	assert(rose_create_sub(active, "large", -1, 0, NULL));
	source_slot_size = MAX_MESSAGE_SIZE;
	assert(rose_create_sub(active, "boundary", -1, 0, later_callback));
	source_slot_size = 0;
	rose_shutdown(active);
	assert(!live[1] && !connection_count);

	// Permanent errors fail immediately, rolling back earlier connections.
	connect_calls = 0;
	connect_fail_after = 2;
	connect_error = -EACCES;
	assert(!node_a());
	assert(connect_calls == 3 && rose_last_error_code() == -EACCES);
	assert(!live[1] && !connection_count);
	// Transient errors stop at the deadline and preserve the underlying cause.
	connect_calls = connect_fail_after = 0;
	connect_error = -EAGAIN;
	u64 started = quicksand_now();
	assert(!node_a());
	double elapsed = quicksand_ns(quicksand_now(), started) / 1e9;
	assert(elapsed >= 1.0 && elapsed < 3.0);
	assert(rose_last_error_code() == -ETIMEDOUT);
	assert(strstr(rose_last_error(), "deadline reached"));
	assert(!live[1] && !connection_count);
	connect_error = 0;
	puts("callback shutdown tests passed");
	return 0;
}
