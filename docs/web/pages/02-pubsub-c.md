## Pub/Sub in C

### Publisher

```c
#include <ros3.h>
#include <mirage.h>

int main(int argc, char **argv) {
	rose_node *n = rose_init(argc, argv, "c_pub", NULL, NULL);
	rose_publisher *p = rose_create_pub(n, "/chatter", -1, 1024, 128);
	mirage_msg *m = mirage_create(1024, NULL);
	while (rose_ok(n)) {
		mirage_write_start(m);
		mirage_write_string(m, "hello from c", -1);
		rose_publish(p, m);
		rose_sleep(0.1);
	}
	return 0;
}
```

### Subscriber

```c
#include <ros3.h>
#include <mirage.h>
#include <stdio.h>

int main(int argc, char **argv) {
	rose_node *n = rose_init(argc, argv, "c_sub", NULL, NULL);
	rose_subscriber *s = rose_create_sub(n, "/chatter", -1, 1, NULL);
	mirage_msg *m = mirage_create(1024, NULL);
	char text[256]; i64 len = 0;
	while (rose_ok(n)) {
		if (rose_read(s, m) >= 0) {
			mirage_read_start(m);
			mirage_read_string(m, text, &len, sizeof(text));
			printf("%.*s\n", (int)len, text);
		}
		rose_sleep(0.01);
	}
	return 0;
}
```
