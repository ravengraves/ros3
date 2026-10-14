#ifndef ROSE_QUERY_H
#define ROSE_QUERY_H

#include "process.h"
#include "ros3.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	i64 requester_pid;
	i64 timestamp;
	int dump;
	int started;
	int complete;
} rose_query;

static inline u64 rose_query_clock(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64) ts.tv_sec * 1000000000ULL + (u64) ts.tv_nsec;
}

// Replies are subscribed/synchronized by the caller before sending a request.
static inline int rose_query_send(rose_query *q, const char *fn,
				  const char *prefix, i64 target_pid) {
	*q = (rose_query) {.requester_pid = (i64) getpid(),
			   .timestamp = (i64) rose_query_clock(),
			   .dump = strcmp(fn, ROSE_MSG_DUMP_ALL) == 0};
	char topic[512];
	if(!rose_topic_to_shm(topic, sizeof(topic), ROSE_TOPIC_REQUEST)) {
		return -1;
	}
	quicksand_connection *conn = NULL;
	if(quicksand_connect(&conn, topic, -1, ROSE_REQUEST_MSG_SIZE,
			     ROSE_REQUEST_RING_SIZE, NULL)
	   != 0) {
		return -1;
	}
	mirage_msg *m = mirage_create(ROSE_REQUEST_MSG_SIZE, NULL);
	int result = -1;
	if(!m) {
		goto cleanup;
	}
	if(mirage_write_start(m) < 0
	   || mirage_write_fn(m, fn, (i64) strlen(fn), q->dump ? 0 : 3) < 0) {
		goto cleanup;
	}
	if(!q->dump) {
		if(mirage_write_i64(m, q->requester_pid) < 0
		   || mirage_write_i64(m, q->timestamp) < 0) {
			goto cleanup;
		}
		if(prefix) {
			if(strlen(prefix) >= ROSE_MAX_NAME_LEN
			   || mirage_write_string(m, prefix, (i64) strlen(prefix)) < 0) {
				goto cleanup;
			}
		} else if(mirage_write_i64(m, target_pid) < 0) {
			goto cleanup;
		}
	}
	if(quicksand_write(conn, m->data, m->cursor) >= 0) {
		result = 0;
	}
cleanup:
	mirage_destroy(&m, NULL);
	quicksand_disconnect(&conn, NULL);
	return result;
}

// 2 = Start, 1 = NodeInfo, 0 = matching Done, -1 = ignored/invalid.
static inline int rose_query_parse(rose_query *q, mirage_msg *m,
				   char *name, size_t name_size, i64 *pid, i64 *heartbeat,
				   char *pubs, size_t pubs_size, char *subs, size_t subs_size) {
	char fn[64];
	i64 len, argc;
	m->cursor = 0;
	if(mirage_read_start(m) < 0
	   || mirage_read_fn(m, fn, &len, sizeof(fn) - 1, &argc) < 0) {
		return -1;
	}
	fn[len] = '\0';
	if(q->dump && strcmp(fn, ROSE_MSG_START) == 0 && argc == 0) {
		q->started = 1;
		return 2;
	}
	int done = strcmp(fn, ROSE_MSG_DONE) == 0;
	if(!done && strcmp(fn, ROSE_MSG_NODE_INFO) != 0) {
		return -1;
	}
	if(argc != (done ? 0 : 5) + (q->dump ? 0 : 2)) {
		return -1;
	}
	if(q->dump) {
		if(!q->started) {
			return -1;
		}
	} else {
		i64 requester, stamp;
		if(mirage_read_i64(m, &requester) < 0 || mirage_read_i64(m, &stamp) < 0
		   || requester != q->requester_pid || stamp != q->timestamp) {
			return -1;
		}
	}
	if(done) {
		q->complete = 1;
		return 0;
	}
	if(mirage_read_string(m, name, &len, (i64) name_size - 1) < 0) {
		return -1;
	}
	name[len] = '\0';
	if(mirage_read_i64(m, pid) < 0 || !rose_valid_pid(*pid)
	   || mirage_read_i64(m, heartbeat) < 0) {
		return -1;
	}
	if(mirage_read_string(m, pubs, &len, (i64) pubs_size - 1) < 0) {
		return -1;
	}
	pubs[len] = '\0';
	if(mirage_read_string(m, subs, &len, (i64) subs_size - 1) < 0) {
		return -1;
	}
	subs[len] = '\0';
	return 1;
}
#endif
