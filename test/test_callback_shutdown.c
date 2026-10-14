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
	puts("callback shutdown tests passed");
	return 0;
}
