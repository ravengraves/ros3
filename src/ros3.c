#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "ros3.h"
#include "ros3_platform.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>   /* vaargs */
#include <stdio.h>    /* snprintf */
#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* strlen, strcpy, memcpy */

#if defined(_WIN32)
#include "ros3_platform_windows.c"
#else
#include "ros3_platform_posix.c"
#endif


#define REMAP_SIZE 4096			 // at most 4kb of remap strings
#define REMAP_COUNT 64			 // at most 64 remaps
#define MAX_PUBSUB 256			 // at most n pubsubs
#define MAX_MESSAGE_SIZE (1920 * 1080 * 3) // max size for a "spin'd" message
#define ROSE_ERROR_BUFFER_SIZE 1024

static rose_node *ROSE_STATIC_NODE = NULL;
static _Thread_local char ROSE_LAST_ERROR[ROSE_ERROR_BUFFER_SIZE] = {0};
static _Thread_local i64 ROSE_LAST_ERROR_CODE = ROSE_OK;

static void *rose_alloc(rose_node *node, size_t size);
static void rose_free(rose_node *node, void *ptr);

const char *rose_last_error(void) {
	return ROSE_LAST_ERROR;
}

i64 rose_last_error_code(void) {
	return ROSE_LAST_ERROR_CODE;
}

static void rose_clear_error(void) {
	ROSE_LAST_ERROR_CODE = ROSE_OK;
	ROSE_LAST_ERROR[0] = '\0';
}

static const char *rose_errno_name(int err) {
	switch(err) {
	case EINVAL: return "EINVAL";
	case ENOENT: return "ENOENT";
	case ENOMEM: return "ENOMEM";
	case EMSGSIZE: return "EMSGSIZE";
	case ETIMEDOUT: return "ETIMEDOUT";
	case EBADMSG: return "EBADMSG";
	case EPIPE: return "EPIPE";
	case EOVERFLOW: return "EOVERFLOW";
	case EIO: return "EIO";
	case ENAMETOOLONG: return "ENAMETOOLONG";
	case ERANGE: return "ERANGE";
	default: return "ERR";
	}
}

static void rose_format_errno_code(i64 code, char *buf, size_t buf_size) {
	if(!buf || buf_size == 0) {
		return;
	}
	if(code >= 0) {
		snprintf(buf, buf_size, "%lld", (long long) code);
		return;
	}

	int err = (int) -code;
	snprintf(buf, buf_size, "%lld (%s: %s)",
		 (long long) code,
		 rose_errno_name(err),
		 strerror(err));
}

static void rose_record_errorv(i64 code, const char *fmt, va_list ap) {
	ROSE_LAST_ERROR_CODE = code;
	if(!fmt) {
		ROSE_LAST_ERROR[0] = '\0';
		return;
	}

	vsnprintf(ROSE_LAST_ERROR, sizeof(ROSE_LAST_ERROR), fmt, ap);
	fprintf(stderr, "[ros3] %s\n", ROSE_LAST_ERROR);
	(void) fflush(stderr);
}

static void rose_record_error(i64 code, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	rose_record_errorv(code, fmt, ap);
	va_end(ap);
}

static int rose_has_double_slash(const char *value, size_t value_length) {
	if(!value) {
		return 0;
	}
	for(size_t i = 1; i < value_length; ++i) {
		if(value[i - 1] == '/' && value[i] == '/') {
			return 1;
		}
	}
	return 0;
}

static const char *rose_connect_suggestion(i64 rc, int is_create) {
	if(rc == -ENOENT) {
		return is_create
			       ? "Start `rosecore`, confirm `ROSE_BASE` matches across processes, and retry after the shared-memory namespace exists."
			       : "Create the publisher first, then verify the topic spelling, node prefix, and `ROSE_BASE` match the publisher.";
	}
	if(rc == -EINVAL) {
		return is_create
			       ? "Use a topic name under 256 bytes and pass positive `message_length` and `message_hz` values."
			       : "Verify the resolved topic name is under 256 bytes and points at the intended shared-memory object.";
	}
	if(rc == -ENOMEM || rc == -EOVERFLOW) {
		return "Lower `message_length` or `message_hz`, and check available shared-memory capacity such as `/dev/shm`.";
	}
	if(rc == -EBADMSG) {
		return is_create
			       ? "Clear the stale topic with `rosetopic rm <topic_name>`, then recreate it with the new message size/rate. Another process may have created an incompatible quicksand ring buffer."
			       : "Delete and recreate the topic. Another process may have created an incompatible quicksand ring buffer.";
	}
	return is_create
		       ? "Check that `rosecore` or peer processes are running, that `ROSE_BASE` and prefixes match, and that the requested buffer sizing is intentional."
		       : "Check that the publisher is alive and that both processes resolve the same shared-memory topic name.";
}

static const char *rose_connect_meaning(i64 rc, int is_create) {
	if(rc == -ENOENT) {
		return is_create
			       ? "the backing shared-memory namespace could not be opened yet"
			       : "the topic does not exist yet, so there is no shared-memory buffer to attach to";
	}
	if(rc == -EINVAL) {
		return "the topic name or requested sizing is invalid for quicksand";
	}
	if(rc == -ENOMEM) {
		return "the process could not allocate the connection or shared-memory bookkeeping";
	}
	if(rc == -EOVERFLOW) {
		return "the requested buffer size or rate overflowed the ring-buffer size calculation";
	}
	if(rc == -EBADMSG) {
		return "the existing shared-memory topic looks corrupted or was created with an incompatible layout";
	}
	if(rc == -EIO) {
		return "quicksand could not inspect the shared-memory object after opening it";
	}
	return is_create
		       ? "quicksand could not create or attach to the requested shared-memory buffer"
		       : "quicksand could not attach to the requested shared-memory buffer";
}

static const char *rose_write_suggestion(i64 rc) {
	if(rc == -EMSGSIZE) {
		return "Increase `message_length` when creating the publisher, or publish a smaller encoded message.";
	}
	if(rc == -ETIMEDOUT) {
		return "Reduce the publish rate, increase the configured rate/ring capacity, or confirm a subscriber is draining the topic.";
	}
	if(rc == -EPIPE || rc == -EBADMSG) {
		return "Reconnect the publisher or delete and recreate the topic if another process left the shared-memory buffer in a bad state.";
	}
	return "Verify the publisher handle is still valid and that the topic buffer is configured for the payload size and traffic rate you expect.";
}

static const char *rose_write_meaning(i64 rc) {
	if(rc == -EMSGSIZE) {
		return "the payload is larger than the per-message slot size configured for this topic";
	}
	if(rc == -ETIMEDOUT) {
		return "the writer could not reserve or publish a slot before quicksand's timeout expired";
	}
	if(rc == -EPIPE) {
		return "the publisher handle is no longer attached to a valid quicksand buffer";
	}
	if(rc == -EBADMSG) {
		return "the topic buffer failed layout validation and may be corrupted or incompatible";
	}
	if(rc == -EINVAL) {
		return "the publisher handle or payload arguments were invalid";
	}
	return "quicksand rejected the write request";
}

static const char *rose_read_suggestion(i64 rc) {
	if(rc == -EBADMSG) {
		return "Delete and recreate the topic. The subscriber may be attached to a corrupted or incompatible quicksand buffer.";
	}
	if(rc == -EPIPE) {
		return "Reconnect the subscriber or recreate the publisher. The backing topic may have been deleted.";
	}
	if(rc == -EINVAL) {
		return "Pass a valid subscriber handle and a writable `mirage_msg` buffer with positive `max_length`.";
	}
	return "Verify the subscriber is connected to the expected topic and that the destination buffer is large enough for incoming messages.";
}

static const char *rose_read_meaning(i64 rc) {
	if(rc == -EBADMSG) {
		return "the topic buffer failed layout validation and may be corrupted or incompatible";
	}
	if(rc == -EPIPE) {
		return "the subscriber handle is no longer attached to a valid quicksand buffer";
	}
	if(rc == -EINVAL) {
		return "the subscriber handle or destination buffer arguments were invalid";
	}
	return "quicksand could not complete the read request";
}

static i64 rose_ensure_msg_capacity(mirage_msg **msg,
				    i64 min_capacity,
				    void *(*alloc)(size_t),
				    const char *api,
				    const char *purpose) {
	if(min_capacity <= 0) {
		min_capacity = 128;
	}
	if(*msg && (*msg)->max_length >= min_capacity) {
		return ROSE_OK;
	}

	if(*msg) {
		mirage_destroy(msg, 0);
	}

	*msg = mirage_create(min_capacity, alloc);
	if(!*msg) {
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "%s failed to allocate a %s buffer of %lld bytes. Suggestion: lower the requested payload size or check allocator/system memory limits.",
				  api,
				  purpose,
				  (long long) min_capacity);
		return ROSE_ERR_BUFFERSIZE;
	}

	return ROSE_OK;
}

static i64 rose_validate_message_for_write(const char *api, mirage_msg *msg) {
	if(!msg) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received a NULL message pointer. Suggestion: create a `mirage_msg` with `mirage_create(...)` before publishing.",
				  api);
		return ROSE_ERR_BADINPUT;
	}
	if(msg->cursor < 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received a message with a negative cursor (%lld). Suggestion: rebuild the Mirage payload before publishing.",
				  api,
				  (long long) msg->cursor);
		return ROSE_ERR_BADINPUT;
	}
	if(msg->max_length < 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received a message with a negative max_length (%lld). Suggestion: create the buffer with `mirage_create(...)` or repair the caller-owned message metadata.",
				  api,
				  (long long) msg->max_length);
		return ROSE_ERR_BADINPUT;
	}
	if(msg->cursor > msg->max_length) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received a message whose cursor (%lld) exceeds max_length (%lld). Suggestion: finish encoding into a large enough Mirage buffer before publishing.",
				  api,
				  (long long) msg->cursor,
				  (long long) msg->max_length);
		return ROSE_ERR_BADINPUT;
	}
	if(msg->cursor > 0 && !msg->data) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received message bytes without a backing data pointer. Suggestion: allocate the message with `mirage_create(...)` or keep the payload buffer alive for the call.",
				  api);
		return ROSE_ERR_BADINPUT;
	}
	return ROSE_OK;
}

static i64 rose_validate_message_for_read(const char *api, mirage_msg *msg) {
	if(!msg) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received a NULL destination message. Suggestion: allocate a writable `mirage_msg` before reading.",
				  api);
		return ROSE_ERR_BADINPUT;
	}
	if(msg->max_length <= 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received a destination buffer with max_length=%lld. Suggestion: allocate a positive-sized `mirage_msg` to receive topic data.",
				  api,
				  (long long) msg->max_length);
		return ROSE_ERR_BADINPUT;
	}
	if(!msg->data) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received a destination buffer with NULL data. Suggestion: create the message with `mirage_create(...)` or supply writable storage.",
				  api);
		return ROSE_ERR_BADINPUT;
	}
	return ROSE_OK;
}

static i64 rose_resolve_topic_name(rose_node *n,
				   const char *api,
				   char *name,
				   i64 name_length,
				   char *topic,
				   size_t topic_size) {
	if(!n) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received a NULL node pointer. Suggestion: keep the `rose_node*` returned by `rose_init` or `rose_create_node` and pass it back unchanged.",
				  api);
		return ROSE_ERR_BADINPUT;
	}
	if(!name) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received a NULL topic name. Suggestion: pass a topic like `\"chatter\"` or `\"/robot/sensors/imu\"`.",
				  api);
		return ROSE_ERR_BADINPUT;
	}
	if(!topic || topic_size == 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s was given no storage for the resolved topic. Suggestion: pass a fixed buffer large enough for the fully-qualified shared-memory name.",
				  api);
		return ROSE_ERR_BADINPUT;
	}
	if(name_length < -1) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received an invalid topic length (%lld). Suggestion: pass `-1` for NUL-terminated strings or a non-negative byte count.",
				  api,
				  (long long) name_length);
		return ROSE_ERR_BADINPUT;
	}
	if(!n->prefix) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s cannot resolve a topic because the node prefix is NULL. Suggestion: recreate the node and avoid using a partially initialized `rose_node`.",
				  api);
		return ROSE_ERR_BADINPUT;
	}

	size_t nlen = (name_length == -1) ? strlen(name) : (size_t) name_length;
	if(nlen == 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received an empty topic name. Suggestion: use a non-empty topic such as `\"cmd_vel\"` or `\"camera/image\"`.",
				  api);
		return ROSE_ERR_BADINPUT;
	}
	if(rose_has_double_slash(name, nlen)) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received the topic \"%.*s\", which contains `//`. Suggestion: remove empty path segments, for example use `\"camera/image\"` instead of `\"camera//image\"`.",
				  api,
				  (int) nlen,
				  name);
		return ROSE_ERR_BADINPUT;
	}

	int written = 0;
	if(name[0] == '/') {
		written = snprintf(topic, topic_size, "%.*s", (int) nlen, name);
	} else {
		written = snprintf(topic, topic_size, "%s%.*s", n->prefix, (int) nlen, name);
	}
	if(written < 0 || (size_t) written >= topic_size) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s could not resolve topic \"%.*s\" because the fully-qualified name exceeds %zu bytes. Suggestion: shorten the node prefix, topic, or `ROSE_BASE` value.",
				  api,
				  (int) nlen,
				  name,
				  topic_size - 1);
		return ROSE_ERR_BADINPUT;
	}
	if(written > 255) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s resolved topic \"%s\" to %d bytes, but quicksand only accepts topic names up to 255 bytes. Suggestion: shorten the namespace, topic, or `ROSE_BASE` value.",
				  api,
				  topic,
				  written);
		return ROSE_ERR_BADINPUT;
	}

	for(size_t i = 1; i < (size_t) written; i += 1) {
		if(topic[i] == '/') {
			topic[i] = '%';
		}
	}

	return ROSE_OK;
}

static void rose_destroy_node_storage(rose_node *n) {
	if(!n) {
		return;
	}
	if(n->remap_strlens) {
		rose_free(n, n->remap_strlens);
		n->remap_strlens = NULL;
	}
	if(n->remaps) {
		rose_free(n, n->remaps);
		n->remaps = NULL;
	}
	if(n->subs) {
		rose_free(n, n->subs);
		n->subs = NULL;
	}
	if(n->pubs) {
		rose_free(n, n->pubs);
		n->pubs = NULL;
	}
	if(n->prefix) {
		rose_free(n, n->prefix);
		n->prefix = NULL;
	}
	if(n->name) {
		rose_free(n, n->name);
		n->name = NULL;
	}
	rose_free(n, n);
}

static i64 rose_connect_with_retry(rose_node *n,
				   quicksand_connection **connection,
				   char *topic,
				   i64 message_size,
				   i64 message_rate,
				   const char *channel_name) {
	u64 attempts = 0;
	u64 last_report_ns = 0;

	for(;;) {
		i64 rc = quicksand_connect(connection, topic, -1,
					   message_size, message_rate,
					   (void *) n->allocator);
		if(rc == 0) {
			return 0;
		}

		attempts += 1;
		u64 now = quicksand_now();
		if(attempts <= 3
		   || last_report_ns == 0
		   || quicksand_ns(now, last_report_ns) >= 1e9) {
			char code_buf[128];
			rose_format_errno_code(rc, code_buf, sizeof(code_buf));
			rose_record_error(rc,
					  "rose_create_node could not connect internal topic \"%s\" for %s: quicksand_connect returned %s, which means %s, while creating a buffer with size %lld bytes and rate %lld Hz (attempt %llu). Suggestion: %s",
					  topic,
					  channel_name,
					  code_buf,
					  rose_connect_meaning(rc, 1),
					  (long long) message_size,
					  (long long) message_rate,
					  (unsigned long long) attempts,
					  rose_connect_suggestion(rc, 1));
			last_report_ns = now;
		}

		rose_sleep(1e-3);
	}
}

// Send a registration message to rosecore
// msg_type: ROSE_MSG_NODE_START, ROSE_MSG_PUB_REGISTER, ROSE_MSG_SUB_REGISTER, ROSE_MSG_HEARTBEAT
// For NodeStart/Heartbeat: arg2 is NULL, pid is sent as i64
// For PubRegister/SubRegister: arg2 is the topic name
static void _rose_send_registration(rose_node *n, char *msg_type,
				    char *arg2, i64 arg2_len) {
	if(!n || !n->rosecore_pub.pub) {
		return;
	}

	static _Thread_local mirage_msg *m = NULL;
	i64 required = 128 + (i64) strlen(msg_type) + (i64) n->name_length;
	if(arg2 && arg2_len > 0) {
		required += arg2_len;
	}
	if(rose_ensure_msg_capacity(&m, required, NULL,
				    "_rose_send_registration",
				    "registration") < 0) {
		return;
	}

	int64_t err = 0;
	err |= mirage_write_start(m);

	if(strcmp(msg_type, ROSE_MSG_NODE_START) == 0) {
		err |= mirage_write_fn(m, msg_type, (i64) strlen(msg_type), 2);
		err |= mirage_write_string(m, n->name, n->name_length);
		err |= mirage_write_i64(m, rose_platform_getpid());
	} else if(strcmp(msg_type, ROSE_MSG_HEARTBEAT) == 0) {
		i64 now = (i64) quicksand_now();
		err |= mirage_write_fn(m, msg_type, (i64) strlen(msg_type), 3);
		err |= mirage_write_string(m, n->name, n->name_length);
		err |= mirage_write_i64(m, rose_platform_getpid());
		err |= mirage_write_i64(m, now);
	} else {
		// PubRegister or SubRegister
		err |= mirage_write_fn(m, msg_type, (i64) strlen(msg_type), 2);
		err |= mirage_write_string(m, n->name, n->name_length);
		err |= mirage_write_string(m, arg2, arg2_len);
	}

	if(err >= 0) {
		i64 rc = quicksand_write(n->rosecore_pub.pub, m->data, m->cursor);
		if(rc != 0) {
			char code_buf[128];
			rose_format_errno_code(rc, code_buf, sizeof(code_buf));
			rose_record_error(rc,
					  "_rose_send_registration failed for node \"%s\": quicksand_write returned %s, which means %s, while publishing a %s message. Suggestion: %s",
					  n->name,
					  code_buf,
					  rose_write_meaning(rc),
					  msg_type,
					  rose_write_suggestion(rc));
		}
	} else {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "_rose_send_registration could not encode the %s message for node \"%s\". Suggestion: shorten node/topic names or inspect the Mirage payload builder for malformed input.",
				  msg_type,
				  n->name);
	}
}

// Send rate-limited heartbeat (max 1kHz)
static void _rose_send_heartbeat(rose_node *n) {
	if(!n || !n->rosecore_pub.pub) {
		return;
	}

	u64 now = quicksand_now();
	f64 elapsed = quicksand_ns(now, n->last_heartbeat_ns);

	// Rate limit: 1ms minimum interval (1kHz max)
	if(n->last_heartbeat_ns != 0 && elapsed < 1e6) {
		return;
	}

	_rose_send_registration(n, ROSE_MSG_HEARTBEAT, NULL, 0);
	n->last_heartbeat_ns = now;
}

static void *rose_alloc(rose_node *node, size_t size) {
	if(node && node->allocator) {
		return node->allocator(size);
	}
	return malloc(size);
}

static void rose_free(rose_node *node, void *ptr) {
	if(!ptr) {
		return;
	}
	if(node && node->deallocator) {
		node->deallocator(ptr);
	} else {
		free(ptr);
	}
}

// Advance a subscriber to the latest slot to avoid stale reads on startup.
static void rose_subscriber_sync_latest(rose_subscriber *sub) {
	if(!sub || !sub->sub || !sub->sub->buffer) {
		return;
	}

	quicksand_connection *conn = sub->sub;
	u64 buffer_size = conn->buffer->message_size;
	if(buffer_size == 0) {
		conn->read_index = conn->buffer->index;
		conn->read_stamp = quicksand_now();
		return;
	}

	u8 *scratch = rose_alloc(sub->node, (size_t) buffer_size);
	if(!scratch) {
		conn->read_index = conn->buffer->index;
		conn->read_stamp = quicksand_now();
		return;
	}

	i64 sz = (i64) buffer_size;
	quicksand_read_latest(conn, scratch, &sz);
	rose_free(sub->node, scratch);
}

void _rose_shutdown(void) {
	rose_shutdown(ROSE_STATIC_NODE);
}

void _rose_signal_shutdown(int sig) {
	(void) sig;
	exit(128 + sig);
	// exit(0);
}

int rose_set_pdeathsig(int sig) {
	return rose_platform_set_parent_death_signal(sig);
}

rose_node *rose_init(char argc, char **argv, char *name_in,
		     void *(*alloc)(size_t), void (*dealloc)(void *)) {
	rose_clear_error();

	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = (void *(*) (size_t) ) alloc;
	}
	void (*deallocate)(void *) = free;
	if(dealloc) {
		deallocate = (void (*)(void *)) dealloc;
	}

	(void) rose_set_pdeathsig(SIGTERM);

	if(!name_in || name_in[0] == '\0') {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_init requires a non-empty node name. Suggestion: pass a short identifier like `\"camera_node\"` or `\"planner\"`.");
		return NULL;
	}

	size_t name_in_len = strlen(name_in);
	if(name_in_len >= 255) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_init received node name \"%s\" (%zu bytes), but ROS3 node names must stay below 255 bytes before the PID suffix is appended. Suggestion: shorten the node name.",
				  name_in,
				  name_in_len);
		return NULL;
	}

	char *remaps = allocate(REMAP_SIZE);
	u64 *remap_lens = allocate(sizeof(u64) * REMAP_COUNT * 2); // lengths
	if(!remaps || !remap_lens) {
		if(remaps) {
			deallocate(remaps);
		}
		if(remap_lens) {
			deallocate(remap_lens);
		}
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "rose_init could not allocate remap parsing storage. Suggestion: check your custom allocator or retry with more process memory available.");
		return NULL;
	}
	u64 remap_count = 0;

	char *remap_head = remaps; // pointer to front

	char prefix[256] = "/";
	char name[256];
	snprintf(name, sizeof(name), "%s", name_in);

	// remap args should be local:=global or __ns:=ns or __name:=name
	// string parsing is so much fun
	for(int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		size_t arglen = strlen(arg);
		if(arglen >= sizeof(prefix) - 2) {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_init received argv[%d]=\"%s\" (%zu bytes), which is too long for remap parsing. Suggestion: shorten the remap argument or move long values into configuration files.",
					  i,
					  arg,
					  arglen);
			return NULL;
		}

		char *remap = strstr(arg, ":=");
		if(!remap) {
			continue; // not a remap, no ":="
		}
		remap += 2; // start after ":="
		if(remap == arg + 2 || *remap == '\0') {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_init received malformed remap \"%s\". Suggestion: use `from:=to`, for example `camera:=/robot/front/camera`.",
					  arg);
			return NULL;
		}

		// input / global cannot contain ":" or "//"
		if(strstr(remap, ":") != 0 || rose_has_double_slash(arg, arglen)) {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_init received invalid remap \"%s\". Suggestion: avoid extra `:` characters and `//`; valid examples are `input:=/robot/input` or `__ns:=robot1`.",
					  arg);
			return NULL;
		}

		// special __ns or __name remap
		if(strncmp(arg, "__ns:=", strlen("__ns:=")) == 0) {
			if(arg[arglen - 1] == '/') { // strip terminating '/'
				arglen -= 1;
			}
			if(arglen && remap[0] == '/') { // strip leading '/'
				arglen -= 1;
				remap += 1;
			}
			int nslen = (int) arglen - (int) strlen("__ns:=");
			if(nslen <= 0) {
				deallocate(remaps);
				deallocate(remap_lens);
				rose_record_error(ROSE_ERR_BADINPUT,
						  "rose_init received an empty namespace remap \"%s\". Suggestion: use `__ns:=robot1` or remove the remap entirely.",
						  arg);
				return NULL;
			}
			snprintf(prefix + 1, nslen + 2, "%s/", remap);
			continue;
		}

		if(strncmp(arg, "__name:=", strlen("__name:=")) == 0) {
			if(arg[arglen - 1] == '/') { // strip terminating '/'
				arglen -= 1;
			}
			if(arglen && remap[0] == '/') { // stkip starting '/'
				remap += 1;
			}
			if(*remap == '\0') {
				deallocate(remaps);
				deallocate(remap_lens);
				rose_record_error(ROSE_ERR_BADINPUT,
						  "rose_init received an empty name remap \"%s\". Suggestion: use `__name:=camera_node` or remove the remap.",
						  arg);
				return NULL;
			}
			snprintf(name, arglen - (remap - arg) + 1, "%s", remap);
			continue;
		}

		if(strncmp(arg, "__", 2) == 0) {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_init does not recognize special remap \"%s\". Suggestion: only `__name:=...` and `__ns:=...` are supported.",
					  arg);
			return NULL;
		}

		// else this is a normal remap  local/name:=/global/name
		if(remap_count >= REMAP_COUNT) {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_init received more than %d remaps. Suggestion: reduce the number of remap rules or combine them into a smaller namespace scheme.",
					  REMAP_COUNT);
			return NULL;
		}

		int local_len = (int) ((remap - 2) - arg);
		snprintf(remap_head, local_len + 1, "%s", arg);
		remap_head += local_len + 1;
		remap_lens[remap_count * 2] = local_len;

		int global_len = (int) (arglen - (size_t) (remap - arg));
		size_t used = (size_t) (remap_head - remaps);
		if(used + (size_t) global_len + 1 >= REMAP_SIZE) {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_init ran out of remap storage while parsing \"%s\". Suggestion: shorten remap strings or reduce the number of remaps passed on the command line.",
					  arg);
			return NULL;
		}
		snprintf(remap_head, global_len + 1, "%s", remap);
		remap_head += global_len + 1;
		remap_lens[remap_count * 2 + 1] = global_len;

		remap_count += 1;
	}

	atexit(_rose_shutdown);
	rose_platform_install_shutdown_handlers(_rose_signal_shutdown);

	rose_node *node = rose_create_node(name,
					   (i64) strlen(name),
					   prefix,
					   (i64) strlen(prefix),
					   remaps,
					   remap_lens,
					   remap_count,
					   allocate,
					   deallocate);
	if(!node) {
		deallocate(remaps);
		deallocate(remap_lens);
	}
	return node;
}


rose_node *rose_create_node(char *in_name, i64 name_len,
			    char *in_prefix, i64 prefix_len,
			    char *remaps, u64 *remap_string_lengths, u64 remap_count,
			    void *(*alloc)(size_t), void (*dealloc)(void *)) {
	rose_clear_error();

	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = (void *(*) (size_t) ) alloc;
	}
	void (*deallocate)(void *) = free;
	if(dealloc) {
		deallocate = (void (*)(void *)) dealloc;
	}

	if(!in_name) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_node received a NULL node name. Suggestion: pass a short identifier like `\"planner\"` or `\"camera_node\"`.");
		return NULL;
	}
	if(name_len < -1) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_node received invalid name_len=%lld. Suggestion: pass `-1` for NUL-terminated strings or a positive byte count.",
				  (long long) name_len);
		return NULL;
	}
	if(prefix_len < -1) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_node received invalid prefix_len=%lld. Suggestion: pass `-1` for NUL-terminated prefixes or a non-negative byte count.",
				  (long long) prefix_len);
		return NULL;
	}

	// Default ROSE_BASE is /rose/
	char topic[512] = "/rose/";
	char *rose_base = getenv("ROSE_BASE");
	if(rose_base != NULL) {
		// strip trailing/leading slash
		size_t blen = strlen(rose_base);
		if(rose_base[0] == '/') {
			rose_base += 1;
			blen -= 1;
		}
		if(blen && rose_base[blen - 1] == '/') {
			blen -= 1;
		}
		if(blen == 0) {
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_create_node found ROSE_BASE=\"%s\", which becomes empty after trimming slashes. Suggestion: unset ROSE_BASE or set it to a simple token like `robot1`.",
					  getenv("ROSE_BASE"));
			return NULL;
		}
		if(memchr(rose_base, '/', blen) != NULL) {
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_create_node found ROSE_BASE=\"%s\", but ROSE_BASE must not contain `/`. Suggestion: use `robot1` instead of `/robot/1`.",
					  getenv("ROSE_BASE"));
			return NULL;
		}
		snprintf(topic, blen + 3, "/%s/", rose_base);
	}

	if(!in_prefix) { // No prefix, use base.
		in_prefix = topic;
		prefix_len = strlen(topic);
	} else { // Copy provided prefix after base

		// Default to strlen if provided len is -1
		if(prefix_len == -1) {

			prefix_len = strlen(in_prefix);
		}
		if(prefix_len < 0) {
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_create_node could not resolve the namespace prefix length. Suggestion: pass a valid prefix string or NULL to use the default namespace.");
			return NULL;
		}

		// Trim beginning or ending '/'
		if(in_prefix[0] == '/') {
			in_prefix += 1;
			prefix_len -= 1;
		}
		if(prefix_len && in_prefix[prefix_len - 1] == '/') {
			prefix_len -= 1;
		}

		if(prefix_len > 0) {
			snprintf(topic + strlen(topic), prefix_len + 2, "%s/", in_prefix);
		}
		in_prefix = topic;
		prefix_len = strlen(topic);
	}

	// Default to strlen if provided len is -1.
	if(name_len == -1) {
		name_len = strlen(in_name);
	}
	if(name_len <= 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_node received an empty node name. Suggestion: pass a non-empty name such as `\"mapper\"`.");
		return NULL;
	}
	if(name_len >= 255) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_node received node name \"%.*s\" (%lld bytes), but names must stay below 255 bytes before the PID suffix is appended. Suggestion: shorten the node name.",
				  (int) name_len,
				  in_name,
				  (long long) name_len);
		return NULL;
	}

	rose_node *n = allocate(sizeof(rose_node));
	if(!n) {
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "rose_create_node could not allocate the rose_node structure. Suggestion: check your allocator or retry with more process memory available.");
		return NULL;
	}
	memset(n, 0, sizeof(rose_node));  // Zero out to ensure all pointers are NULL
	n->allocator = allocate;
	n->deallocator = deallocate;

	// replace "/" after first char with "%" for valid shared memory name
	if(memchr(in_name, '/', (size_t) name_len) != NULL) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_node received node name \"%.*s\", which contains `/`. Suggestion: use plain node names like `camera_node`; namespaces belong in the prefix, not the node name.",
				  (int) name_len,
				  in_name);
		rose_destroy_node_storage(n);
		return NULL;
	}
	char *name = rose_alloc(n, name_len + 64);
	if(!name) {
		long long name_bytes = (long long) name_len + 64LL;
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "rose_create_node could not allocate %lld bytes for the node name. Suggestion: check the allocator passed into ROS3 or retry with more memory available.",
				  name_bytes);
		rose_destroy_node_storage(n);
		return NULL;
	}
	memcpy(name, in_name, (size_t) name_len);
	sprintf(name + name_len, "-%d", (int) rose_platform_getpid());
	n->name = name;
	n->name_length = strlen(name);


	for(int i = 1; i < prefix_len; i += 1) {
		if(in_prefix[i] == '/') {
			in_prefix[i] = '%';
		}
	}
	char *prefix = rose_alloc(n, prefix_len + 1);
	if(!prefix) {
		long long prefix_bytes = (long long) prefix_len + 1LL;
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "rose_create_node could not allocate %lld bytes for the namespace prefix. Suggestion: shorten the prefix or check the allocator passed into ROS3.",
				  prefix_bytes);
		rose_destroy_node_storage(n);
		return NULL;
	}
	snprintf(prefix, prefix_len + 1, "%s", in_prefix);
	n->prefix = prefix;
	n->prefix_length = prefix_len;


	n->remaps = remaps;
	n->remap_strlens = remap_string_lengths;
	n->remap_count = remap_count;

	n->pubs = allocate(sizeof(rose_publisher) * MAX_PUBSUB);
	if(!n->pubs) {
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "rose_create_node could not allocate publisher storage for %d endpoints. Suggestion: check allocator memory limits before creating the node.",
				  MAX_PUBSUB);
		rose_destroy_node_storage(n);
		return NULL;
	}
	memset(n->pubs, 0, sizeof(rose_publisher) * MAX_PUBSUB);
	n->pubs_length = 0;
	n->subs = allocate(sizeof(rose_subscriber) * MAX_PUBSUB);
	if(!n->subs) {
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "rose_create_node could not allocate subscriber storage for %d endpoints. Suggestion: check allocator memory limits before creating the node.",
				  MAX_PUBSUB);
		rose_destroy_node_storage(n);
		return NULL;
	}
	memset(n->subs, 0, sizeof(rose_subscriber) * MAX_PUBSUB);
	n->subs_length = 0;

	// find where base prefix ends (default "/rose/")
	i64 base_prefix_len = prefix_len;
	for(i64 i = 1; i < prefix_len; ++i) {
		if(prefix[i] == '%') {
			base_prefix_len = i + 1;
			break;
		}
	}

	// map connections (retry with 1ms sleep until connected)
	memcpy(topic, prefix, base_prefix_len); // base_prefix

	sprintf(topic + base_prefix_len, "%s", "rosecore");
	rose_connect_with_retry(n, &n->rosecore_pub.pub, topic, 512, 8192, "rosecore publisher");
	n->rosecore_pub.node = n;

	sprintf(topic + base_prefix_len, "%s", "roselog");
	rose_connect_with_retry(n, &n->log_pub.pub, topic, 1024, 8192, "roselog publisher");
	n->log_pub.node = n;

	sprintf(topic + base_prefix_len, "%s", "param_query");
	rose_connect_with_retry(n, &n->param_pub.pub, topic, 512, 8192, "parameter query publisher");
	n->param_pub.node = n;

	// full prefix
	memcpy(topic, prefix, prefix_len + 1);
	snprintf(topic + prefix_len, n->name_length + 2, "%s%%", n->name);
	sprintf(topic + base_prefix_len + n->name_length + 1, "%s", "params");
	rose_connect_with_retry(n, &n->param_sub.sub, topic, 1024, 1024, "parameter reply subscriber");
	n->param_sub.node = n;
	n->param_sub.callback = NULL;
	rose_subscriber_sync_latest(&n->param_sub);

	sprintf(topic + base_prefix_len + n->name_length + 1, "%s", "control");
	rose_connect_with_retry(n, &n->control_sub.sub, topic, 1024, 1024, "control subscriber");
	n->control_sub.node = n;
	n->control_sub.callback = NULL;
	rose_subscriber_sync_latest(&n->control_sub);

	ROSE_STATIC_NODE = n;

	// Initialize heartbeat timestamp
	n->last_heartbeat_ns = 0;

	// Send NodeStart registration to rosecore
	_rose_send_registration(n, ROSE_MSG_NODE_START, NULL, 0);

	rose_clear_error();
	return n;
}

i64 rose_ok(rose_node *n) {
	if(!n) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_ok received a NULL node pointer. Suggestion: store the result of `rose_init(...)` and check it before entering your main loop.");
		return 0;
	}
	_rose_send_heartbeat(n);
	return rose_remaining(&n->control_sub) == 0;
}

void rose_shutdown(rose_node *n) {
	if(!n) {
		return;
	}

	void *dealloc = n->deallocator ? (void *) n->deallocator : NULL;

	if(n->pubs) {
		for(u64 i = 0; i < n->pubs_length; ++i) {
			quicksand_disconnect(&n->pubs[i].pub, dealloc);
		}
	}
	if(n->subs) {
		for(u64 i = 0; i < n->subs_length; ++i) {
			quicksand_disconnect(&n->subs[i].sub, dealloc);
		}
	}

	quicksand_disconnect(&n->rosecore_pub.pub, dealloc);
	quicksand_disconnect(&n->param_pub.pub, dealloc);
	quicksand_disconnect(&n->log_pub.pub, dealloc);

	if(n->param_sub.sub) {
		quicksand_delete((char *) n->param_sub.sub->name, -1);
	}
	if(n->control_sub.sub) {
		quicksand_delete((char *) n->control_sub.sub->name, -1);
	}
	quicksand_disconnect(&n->param_sub.sub, dealloc);
	quicksand_disconnect(&n->control_sub.sub, dealloc);

	if(ROSE_STATIC_NODE == n) {
		ROSE_STATIC_NODE = NULL;
	}
	rose_destroy_node_storage(n);
}

rose_publisher *rose_create_pub(rose_node *n,
				char *name, i64 name_length,
				u64 message_length, u64 message_hz) {
	rose_clear_error();

	if(!n) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_pub received a NULL node pointer. Suggestion: create the node first and keep its `rose_node*` alive while creating publishers.");
		return NULL;
	}
	if(message_length == 0 || message_hz == 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_pub cannot create topic \"%s\" with message_length=%llu and message_hz=%llu. Suggestion: pass both values as positive integers, for example `message_length=4096, message_hz=100`.",
				  name ? name : "(null)",
				  (unsigned long long) message_length,
				  (unsigned long long) message_hz);
		return NULL;
	}

	char topic[512];
	if(rose_resolve_topic_name(n, "rose_create_pub", name, name_length,
				   topic, sizeof(topic)) < 0) {
		return NULL;
	}
	if(n->pubs_length >= MAX_PUBSUB) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_pub cannot create \"%s\" because this node already has %llu publishers and the ROS3 limit is %d. Suggestion: reuse an existing publisher or raise `MAX_PUBSUB` if the project truly needs more endpoints.",
				  topic,
				  (unsigned long long) n->pubs_length,
				  MAX_PUBSUB);
		return NULL;
	}
	if(!n->pubs) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_pub cannot create \"%s\" because the node publisher table is NULL. Suggestion: recreate the node and avoid using a partially initialized handle.",
				  topic);
		return NULL;
	}

	rose_publisher *pub = n->pubs + n->pubs_length;
	pub->node = n;

	int64_t ret = quicksand_connect(&pub->pub, topic, -1,
					(int64_t) message_length,
					(int64_t) message_hz,
					(void *) n->allocator);
	if(ret != 0) {
		char code_buf[128];
		rose_format_errno_code(ret, code_buf, sizeof(code_buf));
		rose_record_error(ret,
				  "rose_create_pub failed for topic \"%s\": quicksand_connect returned %s, which means %s, while creating a buffer with size %llu bytes and rate %llu Hz. Suggestion: %s",
				  topic,
				  code_buf,
				  rose_connect_meaning(ret, 1),
				  (unsigned long long) message_length,
				  (unsigned long long) message_hz,
				  rose_connect_suggestion(ret, 1));
		pub->pub = NULL;
		return NULL;
	}

	n->pubs_length += 1;

	// Send PubRegister to rosecore
	_rose_send_registration(n, ROSE_MSG_PUB_REGISTER, topic, strlen(topic));

	rose_clear_error();
	return pub;
}

rose_subscriber *rose_create_sub(rose_node *n,
				 char *name, i64 name_length,
				 u64 latest_only,
				 void (*callback)(mirage_msg *)) {
	rose_clear_error();

	char topic[512];
	if(rose_resolve_topic_name(n, "rose_create_sub", name, name_length,
				   topic, sizeof(topic)) < 0) {
		return NULL;
	}

	if(n->subs_length >= MAX_PUBSUB) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_sub cannot create \"%s\" because this node already has %llu subscribers and the ROS3 limit is %d. Suggestion: reuse an existing subscriber or raise `MAX_PUBSUB` if you truly need more endpoints.",
				  topic,
				  (unsigned long long) n->subs_length,
				  MAX_PUBSUB);
		return NULL;
	}
	if(!n->subs) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_sub cannot create \"%s\" because the node subscriber table is NULL. Suggestion: recreate the node and avoid using a partially initialized handle.",
				  topic);
		return NULL;
	}

	rose_subscriber *sub = n->subs + n->subs_length;
	sub->node = n;
	sub->callback = callback;
	sub->latest_only = latest_only;

	int64_t ret = quicksand_connect(&sub->sub, topic, -1, -1, -1, (void *) n->allocator);
	if(ret != 0) {
		char code_buf[128];
		rose_format_errno_code(ret, code_buf, sizeof(code_buf));
		rose_record_error(ret,
				  "rose_create_sub failed for topic \"%s\": quicksand_connect returned %s, which means %s, while attaching to an existing quicksand buffer. Suggestion: %s",
				  topic,
				  code_buf,
				  rose_connect_meaning(ret, 0),
				  rose_connect_suggestion(ret, 0));
		sub->sub = NULL;
		return NULL;
	}

	rose_subscriber_sync_latest(sub);

	n->subs_length += 1;

	// Send SubRegister to rosecore
	_rose_send_registration(n, ROSE_MSG_SUB_REGISTER, topic, strlen(topic));

	rose_clear_error();
	return sub;
}


void rose_delete_subscriber(rose_node *n, rose_subscriber *s) {
	if(!n || !s) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_delete_subscriber requires both the node and subscriber pointers. Suggestion: store the `rose_subscriber*` returned by `rose_create_sub` and pass it back with the same node.");
		return;
	}
	if(!n->subs) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_delete_subscriber was called on a node with no subscriber table. Suggestion: recreate the node and avoid using partially initialized handles.");
		return;
	}

	rose_subscriber *sub = n->subs;
	while(sub < n->subs + n->subs_length) {
		if(sub->sub == s->sub) {
			sub->callback = NULL; // prevents spin from running it.
			rose_clear_error();
			break;
		}
		sub += 1;
	}

	if(sub == n->subs + n->subs_length) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_delete_subscriber could not find the subscriber in the node's registry. Suggestion: pass the exact `rose_subscriber*` returned by `rose_create_sub` for this node.");
	}
}


i64 rose_publish(rose_publisher *p, mirage_msg *msg) {
	rose_clear_error();

	if(!p) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_publish received a NULL publisher pointer. Suggestion: keep the `rose_publisher*` returned by `rose_create_pub` and reuse it for each publish call.");
		return ROSE_ERR_BADINPUT;
	}
	if(!p->pub) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_publish received an uninitialized publisher handle. Suggestion: ensure `rose_create_pub(...)` succeeded before publishing.");
		return ROSE_ERR_BADINPUT;
	}
	if(rose_validate_message_for_write("rose_publish", msg) < 0) {
		return ROSE_ERR_BADINPUT;
	}

	i64 ret = quicksand_write(p->pub, msg->data, msg->cursor);
	if(ret != 0) {
		char code_buf[128];
		i64 buffer_size = (p->pub->buffer) ? (i64) p->pub->buffer->message_size : -1;
		rose_format_errno_code(ret, code_buf, sizeof(code_buf));
		if(buffer_size > 0) {
			rose_record_error(ret,
					  "rose_publish failed on topic \"%s\": quicksand_write returned %s, which means %s, for a payload of %lld bytes into a publisher buffer sized for %lld bytes per message. Suggestion: %s",
					  (char *) p->pub->name,
					  code_buf,
					  rose_write_meaning(ret),
					  (long long) msg->cursor,
					  (long long) buffer_size,
					  rose_write_suggestion(ret));
		} else {
			rose_record_error(ret,
					  "rose_publish failed on topic \"%s\": quicksand_write returned %s, which means %s, for a payload of %lld bytes. Suggestion: %s",
					  (char *) p->pub->name,
					  code_buf,
					  rose_write_meaning(ret),
					  (long long) msg->cursor,
					  rose_write_suggestion(ret));
		}
		return ret;
	}

	return ROSE_OK;
}

// NOT THREAD SAFE - do NOT read same subscriber concurrently across threads
i64 rose_read(rose_subscriber *s, rosemsg *msg) {
	rose_clear_error();

	if(!s) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_read received a NULL subscriber pointer. Suggestion: keep the `rose_subscriber*` returned by `rose_create_sub` and reuse it for reads.");
		return ROSE_ERR_BADINPUT;
	}
	if(!s->sub) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_read received an uninitialized subscriber handle. Suggestion: ensure `rose_create_sub(...)` succeeded before reading.");
		return ROSE_ERR_BADINPUT;
	}
	if(rose_validate_message_for_read("rose_read", msg) < 0) {
		return ROSE_ERR_BADINPUT;
	}

	int64_t sz = msg->max_length;
	int64_t ret = quicksand_read(s->sub, msg->data, &sz);
	if(ret >= 0) {
		msg->length = (u64) sz;
		return ret; /* number of msgs remaining */
	}
	if(ret < -1) {
		char code_buf[128];
		rose_format_errno_code(ret, code_buf, sizeof(code_buf));
		rose_record_error(ret,
				  "rose_read failed on topic \"%s\": quicksand_read returned %s, which means %s, with destination buffer size %lld bytes. Suggestion: %s",
				  (char *) s->sub->name,
				  code_buf,
				  rose_read_meaning(ret),
				  (long long) msg->max_length,
				  rose_read_suggestion(ret));
		return ret;
	}
	return ROSE_NO_DATA; /* nothing to read */
}

// NOT THREAD SAFE - do NOT read same subscriber concurrently across threads
i64 rose_readlast(rose_subscriber *s, rosemsg *msg) {
	rose_clear_error();

	if(!s) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_readlast received a NULL subscriber pointer. Suggestion: keep the `rose_subscriber*` returned by `rose_create_sub` and reuse it for reads.");
		return ROSE_ERR_BADINPUT;
	}
	if(!s->sub) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_readlast received an uninitialized subscriber handle. Suggestion: ensure `rose_create_sub(...)` succeeded before reading.");
		return ROSE_ERR_BADINPUT;
	}
	if(rose_validate_message_for_read("rose_readlast", msg) < 0) {
		return ROSE_ERR_BADINPUT;
	}

	int64_t sz = msg->max_length;
	int64_t ret = quicksand_read_latest(s->sub, msg->data, &sz);
	if(ret >= 0) {
		msg->length = (u64) sz;
		return ret;
	}
	if(ret < -1) {
		char code_buf[128];
		rose_format_errno_code(ret, code_buf, sizeof(code_buf));
		rose_record_error(ret,
				  "rose_readlast failed on topic \"%s\": quicksand_read_latest returned %s, which means %s, with destination buffer size %lld bytes. Suggestion: %s",
				  (char *) s->sub->name,
				  code_buf,
				  rose_read_meaning(ret),
				  (long long) msg->max_length,
				  rose_read_suggestion(ret));
		return ret;
	}
	return ROSE_NO_DATA;
}

u64 rose_remaining(rose_subscriber *s) {
	if(!s || !s->sub || !s->sub->buffer) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_remaining received an invalid subscriber handle. Suggestion: call it only on a subscriber returned by `rose_create_sub(...)`.");
		return 0;
	}
	rose_clear_error();
	return quicksand_read_remaining(s->sub);
}


// NOT THREAD SAFE - only call this from one thread at a time.
void rose_spin_once(rose_node *n) {
	if(!n) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_spin_once received a NULL node pointer. Suggestion: keep the `rose_node*` returned by `rose_init(...)` and only spin after successful initialization.");
		return;
	}

	static _Thread_local mirage_msg *m = NULL;
	if(rose_ensure_msg_capacity(&m, MAX_MESSAGE_SIZE, n->allocator,
				    "rose_spin_once", "callback scratch") < 0) {
		return;
	}

	for(size_t i = 0; i < n->subs_length; ++i) {
		rose_subscriber *sub = n->subs + i;
		if(!sub->callback) {
			continue;
		}
		m->length = m->max_length;
		i64 ret;
		if(sub->latest_only) {
			ret = quicksand_read_latest(sub->sub, m->data, &m->length);
		} else {
			ret = quicksand_read(sub->sub, m->data, &m->length);
		}

		if(ret >= 0) {
			sub->callback(m); // run callback
		} else if(ret < -1) {
			char code_buf[128];
			rose_format_errno_code(ret, code_buf, sizeof(code_buf));
			rose_warn("rose_spin_once failed reading callback data from topic \"%s\": quicksand returned %s, which means %s. Suggestion: %s",
				  sub->sub ? (char *) sub->sub->name : "(unknown)",
				  code_buf,
				  rose_read_meaning(ret),
				  rose_read_suggestion(ret));
		}
	}
}

// NOT THREAD SAFE - only call this from one thread at a time.
void rose_spin_forever(rose_node *node) {
	if(!node) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_spin_forever received a NULL node pointer. Suggestion: only spin after `rose_init(...)` or `rose_create_node(...)` succeeds.");
		return;
	}
	while(1) {
		rose_spin_once(node);
		rose_sleep(1e-3);
	}
}

// -------------------------- Logging ------------------------------------------

// --------------------------------------------------------------------
//  rose_log - print to console and publish a Mirage "Log" message.
//
//  The function maps the integer level to a console prefix and an
//  integer error value that is sent as an f64 argument in the Mirage
//  message.
//
//  Console output:
//      INFO   -> stdout
//      others -> stderr
// ------------------------------------------------------------------
void rose_logv(rose_node *node, int level, const char *fmt, va_list ap) {
	// ------------------------------------------------------------------
	//  Map the integer level to a string prefix.
	// --------------------------------------------------------------
	const char *prefix;
	switch(level) {
	case 0: prefix = "[ROSE INFO] "; break;
	case 1: prefix = "[ROSE WARN] "; break;
	case 2: prefix = "[ROSE ERROR] "; break;
	case 3: prefix = "[ROSE FATAL] "; break;
	default:
		prefix = "[ROSE ERROR] ";
		level = 2; // treat unknown as ERROR
		break;
	}

	// ------------------------------------------------------------------
	//  Format the user message (max 1024 bytes).
	// --------------------------------------------------------------
	char user_msg[1024];
	int user_len = vsnprintf(user_msg, sizeof(user_msg), fmt, ap);
	if(user_len < 0) {
		return; // formatting error
	}
	if((size_t) user_len >= sizeof(user_msg)) { // truncate if needed
		user_len = (int) sizeof(user_msg) - 1;
	}
	user_msg[user_len] = '\0';

	// ------------------------------------------------------------------
	//  Compose the console string.
	// ----------------------------------------------------------------
	char console_msg[1024];
	int console_len = snprintf(console_msg, sizeof(console_msg),
				   "%s%s", prefix, user_msg);
	if(console_len < 0) {
		return;
	}
	if((size_t) console_len >= sizeof(console_msg)) {
		console_len = (int) sizeof(console_msg) - 1;
	}
	console_msg[console_len] = '\0';

	// ------------------------------------------------------------------
	//  Print to stdout (INFO) or stderr (others).
	// --------------------------------------------------------------
	FILE *stream = (level == 0) ? stdout : stderr;
	fputs(console_msg, stream);
	fputc('\n', stream);
	(void) fflush(stream);

	// -----------------------------------------------------------------
	// Build and publish a Mirage message.
	// -------------------------------------------------------------
	static _Thread_local mirage_msg *m = NULL;
	if(!m) {
		m = mirage_create(1024, 0);
		if(!m) {
			return;
		}
	}

	int64_t err = 0;
	err |= mirage_write_start(m); /* message start marker */
	err |= mirage_write_fn(m, "Log", (int64_t) strlen("Log"), 3);
	err |= mirage_write_i64(m, (i64) level);
	err |= mirage_write_string(m, node->name, node->name_length);
	err |= mirage_write_string(m, user_msg, // TODO (Alec, 2025): truncate?
				   (int64_t) strlen(user_msg));

	if(err >= 0 && node && node->log_pub.pub) {
		int64_t msg_len = m->cursor;
		quicksand_write(node->log_pub.pub, m->data, msg_len);
	} else {
		fprintf(stderr, "Log message too big for roselog.\n");
	}

	mirage_destroy(&m, 0);
}

void rose_log(rose_node *node, int level, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	rose_logv(node, level, fmt, ap);
	va_end(ap);
}

void rose_info(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	rose_logv(ROSE_STATIC_NODE, 0, fmt, ap);
	va_end(ap);
}

void rose_warn(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	rose_logv(ROSE_STATIC_NODE, 1, fmt, ap);
	va_end(ap);
}

void rose_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	rose_logv(ROSE_STATIC_NODE, 2, fmt, ap);
	va_end(ap);
}

void rose_fatal(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	rose_logv(ROSE_STATIC_NODE, 3, fmt, ap);
	va_end(ap);
}


// --------------------------Parameter handling --------------------------------

// ------------------------------------------------------------------
//  rose_get_param - send a Request and wait (<=1 s) for the reply.
//  NOT THREAD SAFE - only call it from one thread at a time.
// -----------------------------------------------------------------*/
i64 rose_get_param(rose_node *n,
		   char *param_name, i64 param_name_length,
		   mirage_msg *param) {
	rose_clear_error();

	if(!n) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_get_param received a NULL node pointer. Suggestion: call it on a node returned by `rose_init(...)` or `rose_create_node(...)`.");
		return ROSE_ERR_BADINPUT;
	}
	if(!param_name) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_get_param received a NULL parameter name. Suggestion: pass a non-empty key such as `\"robot.max_speed\"`.");
		return ROSE_ERR_BADINPUT;
	}
	if(param_name_length == -1) {
		param_name_length = (i64) strlen(param_name);
	}
	if(param_name_length <= 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_get_param received an empty parameter name. Suggestion: pass the exact key you expect rosecore to serve.");
		return ROSE_ERR_BADINPUT;
	}
	if(rose_validate_message_for_read("rose_get_param", param) < 0) {
		return ROSE_ERR_BADINPUT;
	}
	if(!n->param_pub.pub || !n->param_sub.sub) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_get_param cannot run because the node parameter channels are not connected. Suggestion: make sure `rose_create_node(...)` finished successfully and that `rosecore` is running.");
		return ROSE_ERR_BADINPUT;
	}

	static _Thread_local mirage_msg *req = NULL;

	/* ---------- 1.  Build the node identifier string ---------- */
	char node_id[512];
	snprintf(node_id, sizeof(node_id), "%s%s", n->prefix, n->name);

	/* ---------- 2.  Build the request Mirage message ---------- */
	i64 req_capacity = 128 + param_name_length + (i64) strlen(node_id);
	if(rose_ensure_msg_capacity(&req, req_capacity, n->allocator,
				    "rose_get_param", "parameter request") < 0) {
		return ROSE_ERR_BUFFERSIZE;
	}


	int64_t err = 0;
	err |= mirage_write_start(req);
	err |= mirage_write_fn(req, "Request", (int64_t) strlen("Request"), 2);
	err |= mirage_write_string(req, param_name, (int64_t) param_name_length);
	err |= mirage_write_string(req, node_id, (int64_t) strlen(node_id));

	if(err < 0) {
		mirage_destroy(&req, 0);
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_get_param could not encode request for parameter \"%.*s\". Suggestion: shorten the parameter name or inspect the caller for malformed Mirage state.",
				  (int) param_name_length,
				  param_name);
		return ROSE_ERR_BADINPUT;
	}

	/* ---------- 3.  Send the request ---------- */
	int64_t rc = quicksand_write(n->param_pub.pub, req->data, (int64_t) req->cursor);
	mirage_destroy(&req, 0);
	if(rc != 0) {
		char code_buf[128];
		rose_format_errno_code(rc, code_buf, sizeof(code_buf));
		rose_record_error(rc,
				  "rose_get_param failed to send request for \"%.*s\": quicksand_write returned %s, which means %s. Suggestion: ensure `rosecore` is running and the node parameter channels were created successfully.",
				  (int) param_name_length,
				  param_name,
				  code_buf,
				  rose_write_meaning(rc));
		return rc;
	}

	/* ---------- 4.  Wait for a reply (<=1 s) ---------- */
	uint64_t start_ts = quicksand_now();
	const f64 timeout_ns = 1e9; /* 1 second */
	while(quicksand_ns(quicksand_now(), start_ts) < timeout_ns) {
		int64_t sz = (int64_t) param->max_length;
		int64_t r = quicksand_read_latest(n->param_sub.sub,
						  param->data,
						  &sz);
		if(r >= 0) {
			param->length = (u64) sz;
			rose_clear_error();
			return 0;
		}
		if(r < -1) {
			char code_buf[128];
			rose_format_errno_code(r, code_buf, sizeof(code_buf));
			rose_record_error(r,
					  "rose_get_param failed while waiting for \"%.*s\": quicksand_read_latest returned %s, which means %s. Suggestion: %s",
					  (int) param_name_length,
					  param_name,
					  code_buf,
					  rose_read_meaning(r),
					  rose_read_suggestion(r));
			return r;
		}
		rose_sleep(1e-3);
	}

	/* ---------- 5.  Timeout ---------- */
	rose_record_error(ROSE_NO_DATA,
			  "rose_get_param timed out waiting 1.0s for parameter \"%.*s\". Suggestion: verify `rosecore` is running, the parameter exists, and the node namespace matches the parameter server configuration.",
			  (int) param_name_length,
			  param_name);
	return ROSE_NO_DATA;
}

i64 rose_set_param(rose_node *n, char *param_name, i64 param_name_length,
		   mirage_msg *param) {
	rose_clear_error();

	if(!n) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_set_param received a NULL node pointer. Suggestion: call it on a node returned by `rose_init(...)` or `rose_create_node(...)`.");
		return ROSE_ERR_BADINPUT;
	}
	if(!param_name) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_set_param received a NULL parameter name. Suggestion: pass a non-empty key such as `\"robot.max_speed\"`.");
		return ROSE_ERR_BADINPUT;
	}
	if(param_name_length == -1) {
		param_name_length = (i64) strlen(param_name);
	}
	if(param_name_length <= 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_set_param received an empty parameter name. Suggestion: pass the exact parameter key you intend to update.");
		return ROSE_ERR_BADINPUT;
	}
	if(rose_validate_message_for_write("rose_set_param", param) < 0) {
		return ROSE_ERR_BADINPUT;
	}
	if(!n->param_pub.pub) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_set_param cannot run because the parameter publisher channel is not connected. Suggestion: ensure `rose_create_node(...)` completed successfully and `rosecore` is available.");
		return ROSE_ERR_BADINPUT;
	}

	/* Static buffer for the wrapped message */
	static _Thread_local mirage_msg *m = NULL;
	i64 required_capacity = 128 + param_name_length + param->cursor;
	if(rose_ensure_msg_capacity(&m, required_capacity, NULL,
				    "rose_set_param", "parameter wrapper") < 0) {
		return ROSE_ERR_BUFFERSIZE;
	}

	/* Reset the message to start a new packet */
	if(mirage_write_start(m) < 0) {
		rose_record_error(ROSE_ERR_BADDATA,
				  "rose_set_param could not reset the temporary Mirage wrapper message. Suggestion: retry with a fresh process or inspect memory corruption around ROS3 message buffers.");
		return ROSE_ERR_BADDATA;
	}

	/* Write the List header - two arguments */
	if(mirage_write_fn(m, "List", (int64_t) strlen("List"), 2) < 0) {
		rose_record_error(ROSE_ERR_BADDATA,
				  "rose_set_param could not encode the List header for \"%.*s\". Suggestion: retry after inspecting the Mirage library state and parameter payload size.",
				  (int) param_name_length,
				  param_name);
		return ROSE_ERR_BADDATA;
	}

	/* First argument: param name - use the node name for simplicity. */
	if(mirage_write_string(m, param_name, param_name_length) < 0) {
		rose_record_error(ROSE_ERR_BADDATA,
				  "rose_set_param could not encode parameter name \"%.*s\". Suggestion: shorten the parameter key or inspect the Mirage encoder state.",
				  (int) param_name_length,
				  param_name);
		return ROSE_ERR_BADDATA;
	}

	/* Second argument: copy of the original message data,
       skipping the 2-byte start marker. */
	int64_t data_len = (param->cursor > 2) ? param->cursor - 2 : 0;
	if(data_len > 0) {
		if(m->cursor + data_len > m->max_length) {
			long long required_size = (long long) m->cursor + (long long) data_len;
			rose_record_error(ROSE_ERR_BUFFERSIZE,
					  "rose_set_param needs %lld bytes to wrap parameter \"%.*s\", but only %lld bytes are available. Suggestion: use a smaller payload or inspect unusually large Mirage messages before calling `rose_set_param`.",
					  required_size,
					  (int) param_name_length,
					  param_name,
					  (long long) m->max_length);
			return ROSE_ERR_BUFFERSIZE;
		}
		memcpy(m->data + m->cursor, param->data + 2, data_len);
		m->cursor += data_len;
	}

	/* Send the wrapped message */
	i64 rc = quicksand_write(n->param_pub.pub, m->data, (int64_t) m->cursor);
	if(rc != 0) {
		char code_buf[128];
		rose_format_errno_code(rc, code_buf, sizeof(code_buf));
		rose_record_error(rc,
				  "rose_set_param failed for \"%.*s\": quicksand_write returned %s, which means %s, for a wrapped payload of %lld bytes. Suggestion: %s",
				  (int) param_name_length,
				  param_name,
				  code_buf,
				  rose_write_meaning(rc),
				  (long long) m->cursor,
				  rose_write_suggestion(rc));
		return rc;
	}

	return ROSE_OK;
}

// ------------------------- Topic Naming --------------------------------------

char *rose_topic_to_shm(char *shm, size_t shm_size, const char *topic) {
	rose_clear_error();

	if(!shm || shm_size == 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_topic_to_shm requires a destination buffer and non-zero size. Suggestion: pass a writable char array such as `char buf[512]`.");
		return NULL;
	}
	if(!topic || topic[0] == '\0') {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_topic_to_shm requires a non-empty topic string. Suggestion: pass a topic such as `\"camera/image\"` or `\"/robot/cmd_vel\"`.");
		return NULL;
	}

	// Build base from ROSE_BASE env var (default: "/rose/")
	char base[512] = "/rose/";
	char *rose_base = getenv("ROSE_BASE");
	if(rose_base != NULL) {
		size_t blen = strlen(rose_base);
		if(rose_base[0] == '/') {
			rose_base += 1;
			blen -= 1;
		}
		if(blen && rose_base[blen - 1] == '/') {
			blen -= 1;
		}
		snprintf(base, blen + 3, "/%.*s/", (int) blen, rose_base);
	}

	// Skip leading / on topic if present
	if(topic[0] == '/') {
		topic++;
	}

	// Build full path: /<ROSE_BASE>/<topic>
	int written = snprintf(shm, shm_size, "%s%s", base, topic);
	if(written < 0 || (size_t) written >= shm_size) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_topic_to_shm could not fit topic \"%s\" into a buffer of %zu bytes. Suggestion: pass a larger output buffer or shorten the topic / ROSE_BASE namespace.",
				  topic,
				  shm_size);
		return NULL;
	}

	// Convert / to % after first char
	for(size_t i = 1; shm[i]; i++) {
		if(shm[i] == '/') {
			shm[i] = '%';
		}
	}

	return shm;
}

// ------------------------- Timing Utilities ----------------------------------
u64 rose_now(void) {
	return quicksand_now();
}

void rose_timer_calibrate_seconds(f64 seconds) {
	quicksand_ns_calibrate(seconds * 1e9);
}

f64 rose_seconds(u64 stop, u64 start) {
	return quicksand_ns(stop, start) * 1e-9;
}

f64 rose_nanoseconds(u64 stop, u64 start) {
	return (f64) quicksand_ns(stop, start);
}

/* The header is missing a semicolon after the prototype; we provide the
   matching definition anyway. */
f64 rose_elapsed_seconds(u64 start) {
	return rose_seconds(quicksand_now(), start);
}

f64 rose_elapsed_nanoseconds(u64 start) {
	return rose_nanoseconds(quicksand_now(), start);
}

// ------------------------- Sleep Utilities -----------------------------------
void rose_sleep(f64 seconds) {
	quicksand_sleep(seconds * 1e9);
}

void rose_nanosleep(f64 nanoseconds) {
	quicksand_sleep(nanoseconds);
}

/* Simple 1-step rate-maintaining sleep. */
void rose_sleep_rate(f64 hz) {
	if(hz <= 0.0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_sleep_rate received hz=%f. Suggestion: pass a positive loop rate such as 10.0 or 60.0.",
				  hz);
		return;
	}

	static _Thread_local u64 last_ts = 0;
	const f64 period_ns = 1e9 / hz;

	if(last_ts == 0) { // Sleep on first call
		quicksand_sleep(period_ns);
		last_ts = quicksand_now();
		return;
	}

	u64 now = quicksand_now();
	f64 elapsed = quicksand_ns(now, last_ts);
	f64 to_sleep = period_ns - elapsed;
	if(to_sleep > 0.0) {
		quicksand_sleep(to_sleep);
	}

	last_ts = quicksand_now();
}
