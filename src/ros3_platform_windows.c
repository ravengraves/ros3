#include "ros3_platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int rose_platform_set_parent_death_signal(int sig) {
	(void) sig;
	// Parent-death behavior is intentionally skipped for now on Windows.
	return 0;
}

void rose_platform_install_shutdown_handlers(void (*handler)(int)) {
	(void) handler;
	// Windows CTRL handler wiring is handled at the tool layer for now.
}

i64 rose_platform_getpid(void) {
	return (i64) GetCurrentProcessId();
}
