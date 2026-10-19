#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "ros3.h"
#include "ros3_platform.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h> /* vaargs */
#include <stdio.h>  /* snprintf */
#include <stdlib.h> /* malloc, free */
#include <string.h> /* strlen, strcpy, memcpy */

#if defined(_WIN32)
#include "ros3_platform_windows.c"
#else
#include "ros3_platform_posix.c"
#endif


#define REMAP_SIZE 4096			   // at most 4kb of remap strings
#define REMAP_COUNT 64			   // at most 64 remaps
#define MAX_PUBSUB 256			   // at most n pubsubs
#define MAX_MESSAGE_SIZE (1920 * 1080 * 3) // max size for a "spin'd" message
#define ROSE_ERROR_BUFFER_SIZE 1024
#define ROSE_LOG_MESSAGE_SIZE 512

// Keep the public node layout unchanged; this storage has the same lifetime.
typedef struct {
	rose_node node;
	quicksand_connection *core_heartbeat;
	rose_core_heartbeat core; // Retain identity and replay progress across heartbeat gaps.
	u64 last_check, last_notice;
	int registered_node;
	u64 registered_pubs, registered_subs;
} rose_node_storage;

static rose_node *ROSE_STATIC_NODE = NULL;
static _Thread_local char ROSE_LAST_ERROR[ROSE_ERROR_BUFFER_SIZE] = {0};
static _Thread_local i64 ROSE_LAST_ERROR_CODE = ROSE_OK;

static void *rose_alloc(rose_node *node, size_t size);
static void rose_free(rose_node *node, void *ptr);
static i64 rose_quicksand_connect(rose_node *node,
				  quicksand_connection **connection,
				  char *topic, i64 topic_length,
				  i64 message_size, i64 message_rate);
static void rose_quicksand_disconnect(rose_node *node,
				      quicksand_connection **connection);

const char *rose_last_error(void) {
	return ROSE_LAST_ERROR;
}

i64 rose_last_error_code(void) {
	return ROSE_LAST_ERROR_CODE;
}

int rose_core_heartbeat_valid(const rose_core_heartbeat *h, u64 now) {
	return h && h->version == 1 && h->pid > 0
			&& (h->state == ROSE_CORE_STARTING || h->state == ROSE_CORE_READY)
			&& h->instance_stamp > 0 && h->instance_stamp <= h->timestamp
			&& h->timestamp <= now && quicksand_ns(now, h->timestamp) <= ROSE_CORE_TIMEOUT_NS;
}

static void rose_clear_error(void) {
	ROSE_LAST_ERROR_CODE = ROSE_OK;
	ROSE_LAST_ERROR[0] = '\0';
}

static const char *rose_errno_name(int err) {
	switch(err) {
	case EINVAL: return "EINVAL";
	case EACCES: return "EACCES";
	case EAGAIN: return "EAGAIN";
	case EINTR: return "EINTR";
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

// Validate exact byte spans before indexing or passing them to string APIs.
static int rose_validate_name_span(const char *name, size_t length, int allow_path) {
	if(!name || length == 0 || length > 255) {
		return 0;
	}
	for(size_t i = 0; i < length; ++i) {
		unsigned char c = (unsigned char) name[i];
		if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		   || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
			continue;
		}
		if(allow_path && c == '/' && (i == 0 || name[i - 1] != '/')) {
			continue;
		}
		return 0;
	}
	return 1;
}

static i64 rose_namespace_base(char *base, size_t capacity, size_t *length) {
	const char *value = getenv("ROSE_BASE");
	if(!value) {
		value = "rose";
	}
	size_t len = strlen(value);
	if(len && value[0] == '/') {
		value++;
		len--;
	}
	if(len && value[len - 1] == '/') {
		len--;
	}
	// Keep the accepted base valid for every global internal channel.
	const char *suffixes[] = {"rosecore", "roselog", "param_query",
				  ROSE_TOPIC_REQUEST, ROSE_TOPIC_DUMP, ROSE_TOPIC_LOOKUP, ROSE_TOPIC_HEARTBEAT};
	size_t longest = 0;
	for(size_t i = 0; i < sizeof(suffixes) / sizeof(*suffixes); ++i) {
		size_t suffix_length = strlen(suffixes[i]);
		if(suffix_length > longest) {
			longest = suffix_length;
		}
	}
	if(!rose_validate_name_span(value, len, 0)
	   || len > ROSE_MAX_TOPIC_LEN - 2 - longest
	   || capacity < 3 || len > capacity - 3) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "ROSE_BASE must be a non-empty namespace token that fits internal shared-memory names.");
		return ROSE_ERR_BADINPUT;
	}
	base[0] = '/';
	memcpy(base + 1, value, len);
	base[len + 1] = '/';
	base[len + 2] = '\0';
	*length = len + 2;
	return ROSE_OK;
}

static char *rose_topic_to_shm_span(char *shm, size_t capacity,
				    const char *topic, size_t length) {
	if(!shm || !capacity || !rose_validate_name_span(topic, length, 1)
	   || topic[length - 1] == '/') {
		rose_record_error(ROSE_ERR_BADINPUT, "Invalid topic name or output buffer.");
		return NULL;
	}
	char base[256];
	size_t base_length;
	if(rose_namespace_base(base, sizeof(base), &base_length) < 0) {
		return NULL;
	}
	if(topic[0] == '/') {
		topic++;
		length--;
	}
	if(length > ROSE_MAX_TOPIC_LEN - base_length || base_length + length >= capacity) {
		rose_record_error(ROSE_ERR_BADINPUT, "Resolved topic exceeds the %d-byte name limit or output capacity.", ROSE_MAX_TOPIC_LEN);
		return NULL;
	}
	memcpy(shm, base, base_length);
	memcpy(shm + base_length, topic, length);
	shm[base_length + length] = '\0';
	for(size_t i = 1; i < base_length + length; ++i) {
		if(shm[i] == '/') {
			shm[i] = '%';
		}
	}
	return shm;
}

static const char *rose_connect_suggestion(i64 rc, int is_create) {
	if(rc == -ENOENT) {
		return is_create
				? "Start `rosecore`, confirm `ROSE_BASE` matches across processes, and retry after the shared-memory namespace exists."
				: "Create the publisher first, then verify the topic spelling, node prefix, and `ROSE_BASE` match the publisher.";
	}
	if(rc == -EINVAL) {
		return is_create
				? "Shorten the topic or namespace and pass positive `message_length` and `message_hz` values."
				: "Verify the resolved topic name fits the ROS3 name limit and points at the intended shared-memory object.";
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
				    void (*dealloc)(void *),
				    const char *api,
				    const char *purpose) {
	if(min_capacity <= 0) {
		min_capacity = 128;
	}
	if(*msg && (*msg)->max_length >= min_capacity) {
		return ROSE_OK;
	}

	mirage_msg *replacement = mirage_create(min_capacity, alloc);
	if(!replacement) {
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "%s failed to allocate a %s buffer of %lld bytes. Suggestion: lower the requested payload size or check allocator/system memory limits.",
				  api,
				  purpose,
				  (long long) min_capacity);
		return ROSE_ERR_BUFFERSIZE;
	}
	mirage_destroy(msg, dealloc);
	*msg = replacement;

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

/* Resolve once, without remapping, so sources and destinations use the same
 * namespace and remaps cannot recurse or depend on their ordering. */
static i64 rose_qualify_topic(rose_node *n, const char *name, size_t length,
			      char *topic, size_t capacity) {
	if(!rose_validate_name_span(name, length, 1) || name[length - 1] == '/') {
		rose_record_error(ROSE_ERR_BADINPUT, "Invalid topic name: use nonempty slash-separated ASCII letters, digits, '_', '-', or '.'.");
		return ROSE_ERR_BADINPUT;
	}
	size_t prefix_length = (size_t) n->prefix_length;
	if(name[0] == '/') {
		// The first encoded separator ends the base captured at construction.
		// Never consult the mutable process environment for node-owned names.
		const char *separator = strchr(n->prefix, '%');
		if(!separator) {
			return ROSE_ERR_BADINPUT;
		}
		prefix_length = (size_t) (separator - n->prefix) + 1;
		++name;
		--length;
	}
	if(prefix_length + length > ROSE_MAX_TOPIC_LEN || prefix_length + length >= capacity) {
		rose_record_error(ROSE_ERR_BADINPUT, "Resolved topic exceeds the %d-byte name limit or output capacity.", ROSE_MAX_TOPIC_LEN);
		return ROSE_ERR_BADINPUT;
	}
	memcpy(topic, n->prefix, prefix_length);
	memcpy(topic + prefix_length, name, length);
	topic[prefix_length + length] = '\0';
	for(size_t i = prefix_length; i < prefix_length + length; ++i) {
		if(topic[i] == '/') {
			topic[i] = '%';
		}
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
	if(!rose_validate_name_span(name, nlen, 1) || name[nlen - 1] == '/') {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "%s received an invalid topic byte span. Use nonempty slash-separated ASCII letters, digits, '_', '-', or '.', without a trailing slash; resolved names must fit %d bytes.",
				  api, ROSE_MAX_TOPIC_LEN);
		return ROSE_ERR_BADINPUT;
	}

	i64 status = rose_qualify_topic(n, name, nlen, topic, topic_size);
	if(status != ROSE_OK) {
		return status;
	}
	char original[256];
	memcpy(original, topic, strlen(topic) + 1);
	const char *selected = NULL;
	size_t selected_length = 0;
	const char *rule = n->remaps;
	for(u64 i = 0; i < n->remap_count; ++i) {
		size_t from_length = (size_t) n->remap_strlens[2 * i];
		size_t to_length = (size_t) n->remap_strlens[2 * i + 1];
		const char *destination = rule + from_length + 1;
		char source[256];
		status = rose_qualify_topic(n, rule, from_length, source, sizeof(source));
		if(status != ROSE_OK) {
			return status;
		}
		if(strcmp(original, source) == 0) {
			selected = destination;
			selected_length = to_length;
		}
		rule = destination + to_length + 1;
	}
	return selected ? rose_qualify_topic(n, selected, selected_length, topic, topic_size)
			: ROSE_OK;
}

static void rose_destroy_node_storage(rose_node *n) {
	if(!n) {
		return;
	}
	rose_node_storage *s = (rose_node_storage *) n;
	rose_quicksand_disconnect(n, &s->core_heartbeat);
	mirage_destroy(&n->registration_scratch, n->deallocator);
	mirage_destroy(&n->callback_scratch, n->deallocator);
	mirage_destroy(&n->param_request_scratch, n->deallocator);
	mirage_destroy(&n->param_wrapper_scratch, n->deallocator);
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
	u64 start = quicksand_now();
	for(;;) {
		i64 rc = rose_quicksand_connect(n, connection, topic, -1, message_size, message_rate);
		if(rc == 0) {
			return 0;
		}
		++attempts;
		double elapsed_ms = quicksand_ns(quicksand_now(), start) / 1e6;
		// Another creator can expose the segment before its size/header is ready.
		int transient = rc == -EINTR || rc == -EAGAIN || rc == -ENOENT
				|| rc == -EINVAL || rc == -EPROTONOSUPPORT;
		int expired = elapsed_ms >= 1000.0;
		if(attempts == 1 || !transient || expired) {
			char code[128];
			rose_format_errno_code(rc, code, sizeof(code));
			rose_record_error(expired && transient ? -ETIMEDOUT : rc,
					  "rose_create_node: %s connecting %s topic \"%s\" after %llu attempts (%.1f ms, deadline 1000 ms): %s (%s); slot=%lld bytes, count=%lld. %s",
					  expired ? "deadline reached" : transient ? "retrying"
										   : "failed",
					  channel_name, topic, (unsigned long long) attempts, elapsed_ms,
					  code, rose_connect_meaning(rc, 1), (long long) message_size,
					  (long long) message_rate, rose_connect_suggestion(rc, 1));
		}
		if(!transient || expired) {
			return expired && transient ? -ETIMEDOUT : rc;
		}
		rose_sleep(1e-3);
	}
}

// Send a registration message to rosecore
// msg_type: ROSE_MSG_NODE_START, ROSE_MSG_PUB_REGISTER, ROSE_MSG_SUB_REGISTER, ROSE_MSG_HEARTBEAT
// For NodeStart/Heartbeat: arg2 is NULL, pid is sent as i64
// For PubRegister/SubRegister: arg2 is the topic name
static i64 _rose_send_registration(rose_node *n, char *msg_type,
				   char *arg2, i64 arg2_len) {
	if(!n || !n->rosecore_pub.pub) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "Registration requires a connected rosecore publisher.");
		return ROSE_ERR_BADINPUT;
	}

	i64 required = 128 + (i64) strlen(msg_type) + (i64) n->name_length;
	if(arg2 && arg2_len > 0) {
		required += arg2_len;
	}
	i64 capacity_status = rose_ensure_msg_capacity(&n->registration_scratch, required,
						       n->allocator, n->deallocator,
						       "_rose_send_registration",
						       "registration");
	if(capacity_status < 0) {
		return capacity_status;
	}
	mirage_msg *m = n->registration_scratch;

	int heartbeat = strcmp(msg_type, ROSE_MSG_HEARTBEAT) == 0;
	i64 err = mirage_write_start(m);
	err |= mirage_write_fn(m, msg_type, (i64) strlen(msg_type), heartbeat ? 3 : 2);
	err |= mirage_write_string(m, n->name, n->name_length);
	if(arg2) {
		err |= mirage_write_string(m, arg2, arg2_len);
	} else {
		err |= mirage_write_i64(m, rose_platform_getpid());
		if(heartbeat) {
			err |= mirage_write_i64(m, (i64) quicksand_now());
		}
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
			return rc;
		}
	} else {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "_rose_send_registration could not encode the %s message for node \"%s\". Suggestion: shorten node/topic names or inspect the Mirage payload builder for malformed input.",
				  msg_type,
				  n->name);
		return ROSE_ERR_BADINPUT;
	}
	return ROSE_OK;
}

// Replay at most 16 registrations per check, retaining the cursor on failure.
static void rose_replay_registration(rose_node *n, rose_node_storage *s) {
	for(int sent = 0; sent < 16; ++sent) {
		char *type;
		char *topic = NULL;
		u64 *progress = NULL;
		if(!s->registered_node) {
			// NodeStart must succeed before any endpoints are announced.
			type = ROSE_MSG_NODE_START;
		} else if(s->registered_pubs < n->pubs_length) {
			type = ROSE_MSG_PUB_REGISTER;
			topic = (char *) n->pubs[s->registered_pubs].pub->name;
			progress = &s->registered_pubs;
		} else if(s->registered_subs < n->subs_length) {
			type = ROSE_MSG_SUB_REGISTER;
			topic = (char *) n->subs[s->registered_subs].sub->name;
			progress = &s->registered_subs;
		} else {
			break;
		}
		if(_rose_send_registration(n, type, topic, topic ? (i64) strlen(topic) : 0) != ROSE_OK) {
			break;
		}
		if(progress) {
			++*progress;
		} else {
			s->registered_node = 1;
		}
	}
}

// One cooperative 10 Hz pass for discovery, registration and node heartbeats.
static void rose_maintain_discovery(rose_node *n, u64 now) {
	rose_node_storage *s = (rose_node_storage *) n;
	if(s->last_check && quicksand_ns(now, s->last_check) < ROSE_CORE_INTERVAL_NS) {
		return;
	}
	s->last_check = now;
	char topic[256];
	snprintf(topic, sizeof(topic), "%.*s_heartbeat", ROSE_MAX_TOPIC_LEN, n->rosecore_pub.pub->name);
	if(!s->core_heartbeat) {
		rose_quicksand_connect(n, &s->core_heartbeat, topic, -1, 0, 0);
	}
	if(s->core_heartbeat) {
		rose_core_heartbeat h;
		i64 size = sizeof(h);
		i64 rc = quicksand_read_latest(s->core_heartbeat, (u8 *) &h, &size);
		if(rc >= 0 && size == sizeof(h) && rose_core_heartbeat_valid(&h, now)) {
			if(h.state == ROSE_CORE_READY) {
				if(h.pid != s->core.pid || h.instance_stamp != s->core.instance_stamp) {
					s->registered_node = 0;
					s->registered_pubs = s->registered_subs = 0;
				}
				s->core = h;
			} else {
				s->core.timestamp = 0; // Suspend sending without losing the last READY identity.
			}
		} else if(rc < -1) {
			// Do not get stuck retrying an oversized/malformed record.
			s->core_heartbeat->read_index = atomic_load(&s->core_heartbeat->buffer->index);
		}
	}
	if(!rose_core_heartbeat_valid(&s->core, now)) {
		if(quicksand_ns(now, s->last_notice) >= ROSE_CORE_TIMEOUT_NS) {
			fprintf(stderr, "[ros3] Node '%s': has not received rosecore heartbeat on %s; node continues running.\n", n->name, topic);
			s->last_notice = now;
		}
		return;
	}
	s->last_notice = now;
	rose_replay_registration(n, s);
	if(s->registered_node && _rose_send_registration(n, ROSE_MSG_HEARTBEAT, NULL, 0) == ROSE_OK) {
		n->last_heartbeat_ns = now;
	}
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

static i64 rose_quicksand_connect(rose_node *node,
				  quicksand_connection **connection,
				  char *topic, i64 topic_length,
				  i64 message_size, i64 message_rate) {
	(void) node;
	// quicksand exposes allocator hooks as void*, which is not ISO C-compatible
	// for function pointers. Use the system allocator for quicksand-owned state.
	return quicksand_connect(connection, topic, topic_length,
				 message_size, message_rate, NULL);
}

static void rose_quicksand_disconnect(rose_node *node,
				      quicksand_connection **connection) {
	(void) node;
	quicksand_disconnect(connection, NULL);
}

// Discard all messages committed before this startup snapshot. Quicksand's
// index is the next unread position after the latest committed message.
// Advancing directly avoids allocating/copying a payload just to discard it.
static void rose_subscriber_sync_latest(rose_subscriber *sub) {
	if(!sub || !sub->sub || !sub->sub->buffer) {
		return;
	}

	quicksand_connection *conn = sub->sub;
	conn->read_index = atomic_load_explicit(&conn->buffer->index,
						memory_order_acquire);
	conn->read_stamp = quicksand_now();
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

rose_node *rose_init(int argc, char **argv, char *name_in,
		     void *(*alloc)(size_t), void (*dealloc)(void *)) {
	rose_clear_error();
	if((alloc == NULL) != (dealloc == NULL)) {
		rose_record_error(ROSE_ERR_BADINPUT, "rose_init requires matching allocator and deallocator hooks.");
		return NULL;
	}
	if(argc < 0 || (argc > 0 && !argv)) {
		rose_record_error(ROSE_ERR_BADINPUT, "rose_init requires nonnegative argc and argv when argc is positive.");
		return NULL;
	}

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
		if(!arg) {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT, "rose_init received a NULL argument.");
			return NULL;
		}
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
			size_t nslen = strlen(remap);
			if(!rose_validate_name_span(remap, nslen, 1)) {
				deallocate(remaps);
				deallocate(remap_lens);
				rose_record_error(ROSE_ERR_BADINPUT,
						  "rose_init received an empty namespace remap \"%s\". Suggestion: use `__ns:=robot1` or remove the remap entirely.",
						  arg);
				return NULL;
			}
			if(remap[0] == '/') {
				++remap;
				--nslen;
			}
			if(nslen && remap[nslen - 1] == '/') {
				--nslen;
			}
			prefix[0] = '/';
			memcpy(prefix + 1, remap, nslen);
			if(nslen) {
				prefix[++nslen] = '/';
			}
			prefix[nslen + 1] = '\0';
			continue;
		}

		if(strncmp(arg, "__name:=", strlen("__name:=")) == 0) {
			if(!rose_validate_name_span(remap, strlen(remap), 0)) {
				deallocate(remaps);
				deallocate(remap_lens);
				rose_record_error(ROSE_ERR_BADINPUT,
						  "rose_init received an empty name remap \"%s\". Suggestion: use `__name:=camera_node` or remove the remap.",
						  arg);
				return NULL;
			}
			snprintf(name, sizeof(name), "%s", remap);
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
		size_t from_length = (size_t) (remap - 2 - arg);
		size_t to_length = strlen(remap);
		if(!rose_validate_name_span(arg, from_length, 1) || !rose_validate_name_span(remap, to_length, 1) || arg[from_length - 1] == '/' || remap[to_length - 1] == '/') {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT, "rose_init received an invalid topic remap name.");
			return NULL;
		}
		if(remap_count >= REMAP_COUNT) {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_init received more than %d remaps. Suggestion: reduce the number of remap rules or combine them into a smaller namespace scheme.",
					  REMAP_COUNT);
			return NULL;
		}

		int local_len = (int) ((remap - 2) - arg);
		int global_len = (int) (arglen - (size_t) (remap - arg));
		size_t used = (size_t) (remap_head - remaps);
		size_t required = (size_t) local_len + (size_t) global_len + 2;
		if(required > REMAP_SIZE - used) {
			deallocate(remaps);
			deallocate(remap_lens);
			rose_record_error(ROSE_ERR_BADINPUT,
					  "rose_init ran out of remap storage while parsing \"%s\". Suggestion: shorten remap strings or reduce the number of remaps passed on the command line.",
					  arg);
			return NULL;
		}
		memcpy(remap_head, arg, (size_t) local_len);
		remap_head[local_len] = '\0';
		remap_head += local_len + 1;
		remap_lens[remap_count * 2] = (u64) local_len;
		memcpy(remap_head, remap, (size_t) global_len);
		remap_head[global_len] = '\0';
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
	if(remap_count > REMAP_COUNT || (remap_count && (!remaps || !remap_string_lengths))) {
		rose_record_error(ROSE_ERR_BADINPUT, "Invalid remap table: supply at most 64 complete source/destination pairs.");
		return NULL;
	}
	const char *remap_cursor = remaps;
	for(u64 i = 0; i < remap_count * 2; ++i) {
		u64 length = remap_string_lengths[i];
		if(length > 255 || !rose_validate_name_span(remap_cursor, (size_t) length, 1)
		   || remap_cursor[length - 1] == '/' || remap_cursor[length] != '\0') {
			rose_record_error(ROSE_ERR_BADINPUT, "Invalid name in remap table.");
			return NULL;
		}
		remap_cursor += length + 1;
	}
	if((alloc == NULL) != (dealloc == NULL)) {
		rose_record_error(ROSE_ERR_BADINPUT, "rose_create_node requires matching allocator and deallocator hooks.");
		return NULL;
	}

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

	char topic[512];
	size_t base_length;
	if(rose_namespace_base(topic, sizeof(topic), &base_length) < 0) {
		return NULL;
	}
	if(in_prefix) {
		if(prefix_len == -1) {
			prefix_len = (i64) strlen(in_prefix);
		}
		if(prefix_len > 255 || (prefix_len > 0 && !rose_validate_name_span(in_prefix, (size_t) prefix_len, 1))) {
			rose_record_error(ROSE_ERR_BADINPUT, "Invalid namespace prefix byte span.");
			return NULL;
		}
		if(prefix_len > 0 && in_prefix[0] == '/') {
			in_prefix++;
			prefix_len--;
		}
		if(prefix_len > 0 && in_prefix[prefix_len - 1] == '/') {
			prefix_len--;
		}
		if(prefix_len > 0) {
			if((size_t) prefix_len > ROSE_MAX_TOPIC_LEN - base_length - 1) {
				rose_record_error(ROSE_ERR_BADINPUT, "Namespace prefix exceeds shared-memory name capacity.");
				return NULL;
			}
			memcpy(topic + base_length, in_prefix, (size_t) prefix_len);
			topic[base_length + (size_t) prefix_len] = '/';
			topic[base_length + (size_t) prefix_len + 1] = '\0';
		}
	}
	in_prefix = topic;
	prefix_len = (i64) strlen(topic);
	size_t namespace_length = (size_t) prefix_len - base_length;
	for(i64 i = 1; i < prefix_len; ++i) {
		if(topic[i] == '/') {
			topic[i] = '%';
		}
	}
	// Reject the entire invalid configuration, including unused or shadowed
	// rules, before allocations, transport attachment or ownership transfer.
	rose_node resolver = {.prefix = topic, .prefix_length = (u64) prefix_len};
	remap_cursor = remaps;
	for(u64 i = 0; i < remap_count * 2; ++i) {
		char resolved[256];
		size_t length = (size_t) remap_string_lengths[i];
		if(rose_qualify_topic(&resolver, remap_cursor, length, resolved, sizeof(resolved)) < 0) {
			return NULL;
		}
		remap_cursor += length + 1;
	}

	if(name_len == -1) {
		name_len = (i64) strlen(in_name);
	}
	if(name_len <= 0 || name_len > 255
	   || !rose_validate_name_span(in_name, (size_t) name_len, 0)) {
		rose_record_error(ROSE_ERR_BADINPUT, "Invalid node name byte span.");
		return NULL;
	}
	char pid_suffix[32];
	int pid_length = snprintf(pid_suffix, sizeof(pid_suffix), "-%lld",
				  (long long) rose_platform_getpid());
	// The longest private channel is <prefix><name>-<pid>%control.
	if(pid_length <= 0 || (size_t) pid_length >= sizeof(pid_suffix)
	   || (size_t) name_len + (size_t) pid_length > 255
	   || (size_t) prefix_len + (size_t) name_len + (size_t) pid_length
					   + strlen("%control")
			   > ROSE_MAX_TOPIC_LEN) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "Node namespace and PID-suffixed name exceed internal shared-memory name capacity.");
		return NULL;
	}

	rose_node *n = allocate(sizeof(rose_node_storage));
	if(!n) {
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "rose_create_node could not allocate the rose_node structure. Suggestion: check your allocator or retry with more process memory available.");
		return NULL;
	}
	memset(n, 0, sizeof(rose_node_storage)); // Zero out to ensure all pointers are NULL
	n->allocator = allocate;
	n->deallocator = deallocate;
	((rose_node_storage *) n)->last_notice = quicksand_now();

	// replace "/" after first char with "%" for valid shared memory name
	if(memchr(in_name, '/', (size_t) name_len) != NULL) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_create_node received node name \"%.*s\", which contains `/`. Suggestion: use plain node names like `camera_node`; namespaces belong in the prefix, not the node name.",
				  (int) name_len,
				  in_name);
		rose_destroy_node_storage(n);
		return NULL;
	}
	size_t identity_length = namespace_length + (size_t) name_len + (size_t) pid_length;
	char *name = rose_alloc(n, identity_length + 1);
	if(!name) {
		long long name_bytes = (long long) identity_length + 1;
		rose_record_error(ROSE_ERR_BUFFERSIZE,
				  "rose_create_node could not allocate %lld bytes for the node name. Suggestion: check the allocator passed into ROS3 or retry with more memory available.",
				  name_bytes);
		rose_destroy_node_storage(n);
		return NULL;
	}
	// Registry/display identity excludes ROSE_BASE (each base has its own core)
	// but includes the namespace so same-process names cannot alias across it.
	for(size_t i = 0; i < namespace_length; ++i) {
		char c = topic[base_length + i];
		name[i] = c == '%' ? '/' : c;
	}
	memcpy(name + namespace_length, in_name, (size_t) name_len);
	memcpy(name + namespace_length + (size_t) name_len, pid_suffix, (size_t) pid_length + 1);
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

	// Connect internal channels; transient failures have a one-second deadline.
	memcpy(topic, prefix, base_prefix_len); // base_prefix

	sprintf(topic + base_prefix_len, "%s", "rosecore");
	if(rose_connect_with_retry(n, &n->rosecore_pub.pub, topic, ROSE_REGISTRATION_MSG_SIZE, 8192, "rosecore publisher") != 0) {
		goto connection_failed;
	}
	n->rosecore_pub.node = n;

	sprintf(topic + base_prefix_len, "%s", "roselog");
	if(rose_connect_with_retry(n, &n->log_pub.pub, topic, ROSE_LOG_MESSAGE_SIZE, 8192, "roselog publisher") != 0) {
		goto connection_failed;
	}
	n->log_pub.node = n;

	sprintf(topic + base_prefix_len, "%s", "param_query");
	if(rose_connect_with_retry(n, &n->param_pub.pub, topic, 512, 8192, "parameter query publisher") != 0) {
		goto connection_failed;
	}
	n->param_pub.node = n;

	// full prefix
	memcpy(topic, prefix, prefix_len + 1);
	size_t leaf_length = n->name_length - namespace_length;
	snprintf(topic + prefix_len, leaf_length + 2, "%s%%", n->name + namespace_length);
	sprintf(topic + prefix_len + leaf_length + 1, "%s", "params");
	if(rose_connect_with_retry(n, &n->param_sub.sub, topic, 1024, 1024, "parameter reply subscriber") != 0) {
		goto connection_failed;
	}
	n->param_sub.node = n;
	n->param_sub.callback = NULL;
	rose_subscriber_sync_latest(&n->param_sub);

	sprintf(topic + prefix_len + leaf_length + 1, "%s", "control");
	if(rose_connect_with_retry(n, &n->control_sub.sub, topic, 1024, 1024, "control subscriber") != 0) {
		goto connection_failed;
	}
	n->control_sub.node = n;
	n->control_sub.callback = NULL;
	rose_subscriber_sync_latest(&n->control_sub);

	// Initialize heartbeat timestamp
	n->last_heartbeat_ns = 0;

	// Registration is deferred to rose_ok after a live core is confirmed.
	// Caller retains remaps on every failed construction.
	n->remaps = remaps;
	n->remap_strlens = remap_string_lengths;
	n->remap_count = remap_count;
	ROSE_STATIC_NODE = n;
	rose_clear_error();
	return n;

connection_failed:
	// Attached channels may belong to another process: never unlink on failure.
	rose_quicksand_disconnect(n, &n->rosecore_pub.pub);
	rose_quicksand_disconnect(n, &n->param_pub.pub);
	rose_quicksand_disconnect(n, &n->log_pub.pub);
	rose_quicksand_disconnect(n, &n->param_sub.sub);
	rose_quicksand_disconnect(n, &n->control_sub.sub);
	rose_destroy_node_storage(n);
	return NULL;
}

i64 rose_ok(rose_node *n) {
	if(!n) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_ok received a NULL node pointer. Suggestion: store the result of `rose_init(...)` and check it before entering your main loop.");
		return 0;
	}
	rose_maintain_discovery(n, quicksand_now());
	return rose_remaining(&n->control_sub) == 0;
}

void rose_shutdown(rose_node *n) {
	if(!n) {
		return;
	}
	if(n->spin_active) {
		n->shutdown_requested = 1;
		return;
	}

	if(n->pubs) {
		for(u64 i = 0; i < n->pubs_length; ++i) {
			rose_quicksand_disconnect(n, &n->pubs[i].pub);
		}
	}
	if(n->subs) {
		for(u64 i = 0; i < n->subs_length; ++i) {
			rose_quicksand_disconnect(n, &n->subs[i].sub);
		}
	}

	rose_quicksand_disconnect(n, &n->rosecore_pub.pub);
	rose_quicksand_disconnect(n, &n->param_pub.pub);
	rose_quicksand_disconnect(n, &n->log_pub.pub);

	if(n->param_sub.sub) {
		quicksand_delete((char *) n->param_sub.sub->name, -1);
	}
	if(n->control_sub.sub) {
		quicksand_delete((char *) n->control_sub.sub->name, -1);
	}
	rose_quicksand_disconnect(n, &n->param_sub.sub);
	rose_quicksand_disconnect(n, &n->control_sub.sub);

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
				  "rose_create_pub cannot create a topic with message_length=%llu and message_hz=%llu. Suggestion: pass both values as positive integers, for example `message_length=4096, message_hz=100`.",
				  (unsigned long long) message_length,
				  (unsigned long long) message_hz);
		return NULL;
	}

	char topic[512];
	if(rose_resolve_topic_name(n, "rose_create_pub", name, name_length,
				   topic, sizeof(topic))
	   < 0) {
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

	int64_t ret = rose_quicksand_connect(n, &pub->pub, topic, -1,
					     (int64_t) message_length,
					     (int64_t) message_hz);
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
				   topic, sizeof(topic))
	   < 0) {
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

	int64_t ret = rose_quicksand_connect(n, &sub->sub, topic, -1, -1, -1);
	if(ret != 0) {
		if(ret == -ENOENT) {
			ROSE_LAST_ERROR_CODE = ret;
			snprintf(ROSE_LAST_ERROR, sizeof(ROSE_LAST_ERROR),
				 "Subscription failed: publisher topic \"%s\" does not exist. Retry after the publisher creates it.",
				 topic);
		} else {
			char code_buf[128];
			rose_format_errno_code(ret, code_buf, sizeof(code_buf));
			rose_record_error(ret,
					  "rose_create_sub failed for topic \"%s\": quicksand_connect returned %s, which means %s, while attaching to an existing quicksand buffer. Suggestion: %s",
					  topic,
					  code_buf,
					  rose_connect_meaning(ret, 0),
					  rose_connect_suggestion(ret, 0));
		}
		sub->sub = NULL;
		return NULL;
	}

	if(callback && sub->sub->buffer->message_size > MAX_MESSAGE_SIZE) {
		rose_record_error(-EMSGSIZE,
				  "rose_create_sub: topic \"%s\" has a %llu-byte slot, exceeding the max callback size of %d bytes (including slot padding). Use a smaller publisher slot or explicit rose_read with a sufficiently large buffer.",
				  topic, (unsigned long long) sub->sub->buffer->message_size, MAX_MESSAGE_SIZE);
		rose_quicksand_disconnect(n, &sub->sub);
		memset(sub, 0, sizeof(*sub));
		return NULL;
	}

	rose_subscriber_sync_latest(sub);

	n->subs_length += 1;

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
		msg->cursor = 0;
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
		msg->cursor = 0;
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


// The outer public spin call owns node lifetime, including between iterations.
// External serialization is required; this guard only handles same-thread
// reentrancy and is deliberately not a substitute for synchronization.
static int rose_begin_spin(rose_node *n) {
	if(!n) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "Cannot spin a NULL node pointer.");
		return 0;
	}
	if(n->spin_active) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "Recursive spinning of the same node is not supported.");
		return 0;
	}
	n->spin_active = 1;
	return 1;
}

static void rose_dispatch_once(rose_node *n) {
	if(rose_ensure_msg_capacity(&n->callback_scratch, MAX_MESSAGE_SIZE,
				    n->allocator, n->deallocator,
				    "rose_spin_once", "callback scratch")
	   < 0) {
		return;
	}
	mirage_msg *m = n->callback_scratch;

	for(size_t i = 0; !n->shutdown_requested && i < n->subs_length; ++i) {
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
			m->cursor = 0;
			sub->callback(m); // run callback
		} else if(ret < -1) {
			char code_buf[128];
			char log_msg[1024];
			rose_format_errno_code(ret, code_buf, sizeof(code_buf));
			(void) snprintf(log_msg, sizeof(log_msg),
					"rose_spin_once failed reading callback data from topic \"%s\": quicksand returned %s, which means %s. Suggestion: %s",
					sub->sub ? (char *) sub->sub->name : "(unknown)",
					code_buf,
					rose_read_meaning(ret),
					rose_read_suggestion(ret));
			(void) rose_log(n, ROSE_LOG_WARN, log_msg, -1);
		}
	}
}

static void rose_end_spin(rose_node *n) {
	n->spin_active = 0;
	if(n->shutdown_requested) {
		rose_shutdown(n);
	}
}

void rose_spin_once(rose_node *n) {
	if(!rose_begin_spin(n)) {
		return;
	}
	rose_dispatch_once(n);
	rose_end_spin(n);
}

void rose_spin_forever(rose_node *node) {
	if(!rose_begin_spin(node)) {
		return;
	}
	while(!node->shutdown_requested && rose_ok(node)) {
		rose_dispatch_once(node);
		if(node->shutdown_requested) {
			break;
		}
		rose_sleep(1e-3);
	}
	rose_end_spin(node);
}

// -------------------------- Logging ------------------------------------------

static const char *rose_log_prefix(int *level) {
	switch(*level) {
	case ROSE_LOG_INFO: return "[ROSE INFO] ";
	case ROSE_LOG_WARN: return "[ROSE WARN] ";
	case ROSE_LOG_ERROR: return "[ROSE ERROR] ";
	case ROSE_LOG_FATAL: return "[ROSE FATAL] ";
	default:
		*level = ROSE_LOG_ERROR;
		return "[ROSE ERROR] ";
	}
}

static i64 rose_log_write_msg(mirage_msg *m,
			      int level,
			      const char *node_name,
			      i64 node_name_length,
			      const char *message,
			      i64 message_length) {
	if(mirage_write_start(m) < 0
	   || mirage_write_fn(m, "Log", (i64) strlen("Log"), 3) < 0
	   || mirage_write_i64(m, (i64) level) < 0
	   || mirage_write_string(m, node_name, node_name_length) < 0
	   || mirage_write_string(m, message, message_length) < 0) {
		return ROSE_ERR_BUFFERSIZE;
	}
	return ROSE_OK;
}

static i64 rose_log_truncate_to_fit(mirage_msg *m,
				    int level,
				    const char *node_name,
				    i64 node_name_length,
				    const char *message,
				    i64 message_length) {
	if(rose_log_write_msg(m, level, node_name, node_name_length,
			      message, message_length)
	   == ROSE_OK) {
		return message_length;
	}

	i64 lo = 0;
	i64 hi = message_length;
	while(lo < hi) {
		i64 mid = lo + (hi - lo + 1) / 2;
		if(rose_log_write_msg(m, level, node_name, node_name_length,
				      message, mid)
		   == ROSE_OK) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}

	(void) rose_log_write_msg(m, level, node_name, node_name_length, message, lo);
	return lo;
}

void rose_log(rose_node *node, int level, const char *message, i64 message_length) {
	if(message_length == -1) {
		if(!message) {
			message = "";
			message_length = 0;
		} else {
			message_length = (i64) strlen(message);
		}
	}
	if(message_length < 0) {
		message_length = 0;
	}
	if(!message) {
		message = "";
		message_length = 0;
	}
	i64 original_message_length = message_length;

	const char *prefix = rose_log_prefix(&level);
	i64 console_length = message_length;
	if(console_length > ROSE_LOG_MESSAGE_SIZE) {
		console_length = ROSE_LOG_MESSAGE_SIZE;
	}

	mirage_msg *m = NULL;
	if(node && node->log_pub.pub) {
		m = mirage_create(ROSE_LOG_MESSAGE_SIZE, NULL);
		if(m) {
			const char *node_name = node->name ? node->name : "";
			i64 node_name_length = node->name ? (i64) node->name_length : 0;
			console_length = rose_log_truncate_to_fit(m, level,
								  node_name,
								  node_name_length,
								  message,
								  console_length);
		}
	}

	FILE *stream = (level == ROSE_LOG_INFO) ? stdout : stderr;
	fputs(prefix, stream);
	if(console_length > 0) {
		(void) fwrite(message, 1, (size_t) console_length, stream);
	}
	fputc('\n', stream);
	(void) fflush(stream);
	if(console_length < original_message_length) {
		fprintf(stderr,
			"[ROSE WARN] truncated log message from %lld to %lld bytes to fit the %d-byte roselog slot\n",
			(long long) original_message_length,
			(long long) console_length,
			ROSE_LOG_MESSAGE_SIZE);
		(void) fflush(stderr);
	}

	if(!m) {
		return;
	}

	i64 payload_length = m->cursor;
	(void) quicksand_write(node->log_pub.pub, m->data, payload_length);
	mirage_destroy(&m, NULL);
	(void) payload_length;
}

void rose_info(const char *message, i64 message_length) {
	rose_log(ROSE_STATIC_NODE, ROSE_LOG_INFO, message, message_length);
}

void rose_warn(const char *message, i64 message_length) {
	rose_log(ROSE_STATIC_NODE, ROSE_LOG_WARN, message, message_length);
}

void rose_error(const char *message, i64 message_length) {
	rose_log(ROSE_STATIC_NODE, ROSE_LOG_ERROR, message, message_length);
}

void rose_fatal(const char *message, i64 message_length) {
	rose_log(ROSE_STATIC_NODE, ROSE_LOG_FATAL, message, message_length);
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

	/* ---------- 1.  Build the node identifier string ---------- */
	char node_id[512];
	const char *leaf = strrchr(n->name, '/');
	snprintf(node_id, sizeof(node_id), "%s%s", n->prefix, leaf ? leaf + 1 : n->name);

	/* ---------- 2.  Build the request Mirage message ---------- */
	i64 node_id_length = (i64) strlen(node_id);
	if(param_name_length > INT64_MAX - 128 - node_id_length) {
		rose_record_error(ROSE_ERR_BADINPUT, "Parameter request capacity exceeds the supported size.");
		return ROSE_ERR_BADINPUT;
	}
	i64 req_capacity = 128 + param_name_length + node_id_length;
	if(rose_ensure_msg_capacity(&n->param_request_scratch, req_capacity,
				    n->allocator, n->deallocator,
				    "rose_get_param", "parameter request")
	   < 0) {
		return ROSE_ERR_BUFFERSIZE;
	}
	mirage_msg *req = n->param_request_scratch;


	int64_t err = 0;
	err |= mirage_write_start(req);
	err |= mirage_write_fn(req, "Request", (int64_t) strlen("Request"), 2);
	err |= mirage_write_string(req, param_name, (int64_t) param_name_length);
	err |= mirage_write_string(req, node_id, (int64_t) strlen(node_id));

	if(err < 0) {
		rose_record_error(ROSE_ERR_BADINPUT,
				  "rose_get_param could not encode request for parameter \"%.*s\". Suggestion: shorten the parameter name or inspect the caller for malformed Mirage state.",
				  (int) param_name_length,
				  param_name);
		return ROSE_ERR_BADINPUT;
	}

	/* ---------- 3.  Send the request ---------- */
	int64_t rc = quicksand_write(n->param_pub.pub, req->data, (int64_t) req->cursor);
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

	/* Node-owned buffer for the wrapped message. */
	if(param_name_length > INT64_MAX - 128
	   || param->cursor > INT64_MAX - 128 - param_name_length) {
		rose_record_error(ROSE_ERR_BADINPUT, "Parameter wrapper capacity exceeds the supported size.");
		return ROSE_ERR_BADINPUT;
	}
	i64 required_capacity = 128 + param_name_length + param->cursor;
	if(rose_ensure_msg_capacity(&n->param_wrapper_scratch, required_capacity,
				    n->allocator, n->deallocator,
				    "rose_set_param", "parameter wrapper")
	   < 0) {
		return ROSE_ERR_BUFFERSIZE;
	}
	mirage_msg *m = n->param_wrapper_scratch;

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
	return rose_topic_to_shm_span(shm, shm_size, topic, topic ? strlen(topic) : 0);
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
