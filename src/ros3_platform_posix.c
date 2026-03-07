#include "ros3_platform.h"

#include <signal.h>
#include <unistd.h>

int rose_platform_set_parent_death_signal(int sig) {
	(void) sig;
	// Parent-death behavior is intentionally skipped for now.
	return 0;
}

void rose_platform_install_shutdown_handlers(void (*handler)(int)) {
	signal(SIGINT, handler);
	signal(SIGTERM, handler);
	signal(SIGABRT, handler);
}

i64 rose_platform_getpid(void) {
	return (i64) getpid();
}
