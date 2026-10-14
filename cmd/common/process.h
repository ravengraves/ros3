#ifndef ROSE_PROCESS_H
#define ROSE_PROCESS_H

#include <stdint.h>
#include <sys/types.h>

// Linux and macOS use signed pid_t. Reject narrowing and group targets.
static inline int rose_valid_pid(int64_t pid) {
	return pid > 0 && (int64_t) (pid_t) pid == pid;
}

#endif
