#include "ros3.h"
#include <stdio.h>

static void cb(mirage_msg *msg) {
	i64 fn_len, fn_argc;
	char str[64];
	i64 str_len;
	f64 val;

	int ret = mirage_read_start(msg);
	ret |= mirage_read_fn(msg, 0, &fn_len, 0, &fn_argc);
	ret |= mirage_read_string(msg, str, &str_len, sizeof(str) - 1);
	ret |= mirage_read_f64(msg, &val);

	if(ret < 0) {
		fprintf(stderr, "CB read error\n");
		return;
	}

	char log_msg[128];
	(void) snprintf(log_msg, sizeof(log_msg), "got: '%.*s', %f", (int) str_len, str, val);
	(void) rose_info(log_msg, -1);
}


int main(int argc, char **argv) {
	u64 start = rose_now();
	// create node
	rose_node *node = rose_init(argc, argv, "demo", 0, 0);
	printf("Node name: %s\n", node->name);

	rose_publisher *pub = rose_create_pub(node,
					      "chatter", // topic name
					      -1,	 // use strlen
					      1024,	 // Message size
					      16);	 // Hz
	if(!pub) {
		return 1;
	}

	rose_subscriber *sub = rose_create_sub(node,
					       "chatter", // topic name
					       -1,	  // use strlen
					       1,	  // Latest only
					       cb);	  // Callback
	if(!sub) {
		return 2;
	}

	mirage_msg *msg = mirage_create(1024, 0);
	while(rose_ok(node)) {
		// Build + publish messsage
		mirage_write_start(msg);
		mirage_write_fn(msg, "List", -1, 2); // List with 2 args
		mirage_write_string(msg, "Hello", -1);
		mirage_write_f64(msg, rose_elapsed_seconds(start));
		rose_publish(pub, msg);

		// Check subscribers and run callbacks
		rose_spin_once(node);

		// Try to maintain 1 Hz
		rose_sleep_rate(1.0);
	}
}
