#define _POSIX_C_SOURCE 200809L
#include "ros3.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
	assert(argc == 4);
	rose_node *node = rose_init(argc, argv, "liaison_fixture", NULL, NULL);
	assert(node);
	int publishing = !strcmp(argv[1], "pub");
	rose_publisher *pub = NULL;
	rose_subscriber *sub = NULL;
	if(publishing) {
		pub = rose_create_pub(node, argv[2], -1, strtoull(argv[3], NULL, 10), 16);
		assert(pub);
	} else {
		for(int i = 0; i < 2000 && !sub; ++i) {
			sub = rose_create_sub(node, argv[2], -1, 0, NULL);
			if(!sub) {
				rose_sleep(.001);
			}
		}
		assert(sub);
	}
	mirage_msg *msg = mirage_create(131072, NULL);
	assert(msg);
	puts("READY");
	fflush(stdout);
	unsigned n, seed;
	while(scanf("%u %u", &n, &seed) == 2) {
		assert(n <= 131072);
		if(publishing) {
			for(unsigned i = 0; i < n; ++i) {
				msg->data[i] = (uint8_t) (i + seed);
			}
			msg->length = n;
			msg->cursor = n;
			assert(rose_publish(pub, msg) == 0);
		} else {
			i64 r = -1;
			for(int i = 0; i < 3000 && r < 0; ++i) {
				r = rose_read(sub, msg);
				if(r < 0) {
					rose_sleep(.001);
				}
			}
			assert(r >= 0 && msg->length == n);
			for(unsigned i = 0; i < n; ++i) {
				assert(msg->data[i] == (uint8_t) (i + seed));
			}
		}
		puts("OK");
		fflush(stdout);
	}
	mirage_destroy(&msg, NULL);
	rose_shutdown(node);
	return 0;
}
