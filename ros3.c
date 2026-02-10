#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "ros3.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>   /* vaargs */
#include <stdio.h>    /* snprintf */
#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* strlen, strcpy, memcpy */
#include <sys/types.h>/* pid_t */
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>   /* getpid */


#define REMAP_SIZE 4096			 // at most 4kb of remap strings
#define REMAP_COUNT 64			 // at most 64 remaps
#define MAX_PUBSUB 256			 // at most n pubsubs
#define MAX_MESSAGE_SIZE 1920 * 1080 * 3 // max size for a "spin'd" message

static rose_node *ROSE_STATIC_NODE = NULL;

// Send a registration message to rosemaster
// msg_type: ROSE_MSG_NODE_START, ROSE_MSG_PUB_REGISTER, ROSE_MSG_SUB_REGISTER, ROSE_MSG_HEARTBEAT
// For NodeStart/Heartbeat: arg2 is NULL, pid is sent as i64
// For PubRegister/SubRegister: arg2 is the topic name
static void _rose_send_registration(rose_node *n, char *msg_type,
				    char *arg2, i64 arg2_len) {
	if(!n || !n->rosemaster_pub.pub) {
		return;
	}

	static _Thread_local mirage_msg *m = NULL;
	if(!m) {
		m = mirage_create(512, NULL);
		if(!m) {
			return;
		}
	}

	int64_t err = 0;
	err |= mirage_write_start(m);

	if(strcmp(msg_type, ROSE_MSG_NODE_START) == 0) {
		err |= mirage_write_fn(m, msg_type, (i64) strlen(msg_type), 2);
		err |= mirage_write_string(m, n->name, n->name_length);
		err |= mirage_write_i64(m, (i64) getpid());
	} else if(strcmp(msg_type, ROSE_MSG_HEARTBEAT) == 0) {
		i64 now = (i64) quicksand_now();
		err |= mirage_write_fn(m, msg_type, (i64) strlen(msg_type), 3);
		err |= mirage_write_string(m, n->name, n->name_length);
		err |= mirage_write_i64(m, (i64) getpid());
		err |= mirage_write_i64(m, now);
	} else {
		// PubRegister or SubRegister
		err |= mirage_write_fn(m, msg_type, (i64) strlen(msg_type), 2);
		err |= mirage_write_string(m, n->name, n->name_length);
		err |= mirage_write_string(m, arg2, arg2_len);
	}

	if(err >= 0) {
		quicksand_write(n->rosemaster_pub.pub, m->data, m->cursor);
	}
}

// Send rate-limited heartbeat (max 1kHz)
static void _rose_send_heartbeat(rose_node *n) {
	if(!n || !n->rosemaster_pub.pub) {
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
#if defined(__linux__)
	return prctl(PR_SET_PDEATHSIG, (long) sig);
#else
	(void) sig;
	errno = ENOSYS;
	return -1;
#endif
}

rose_node *rose_init(char argc, char **argv, char *name_in,
		     void *(*alloc)(size_t), void (*dealloc)(void *)) {
	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = (void *(*) (size_t) ) alloc;
	}
	void (*deallocate)(void *) = free;
	if(dealloc) {
		deallocate = (void (*)(void *)) dealloc;
	}

	if(rose_set_pdeathsig(SIGTERM) != 0) {
		fprintf(stderr, "rose_init: failed to set parent death signal: %s\n",
			strerror(errno));
	}

	char *remaps = allocate(REMAP_SIZE);
	u64 *remap_lens = allocate(REMAP_COUNT * 2); // lengths
	u64 remap_count = 0;

	char *remap_head = remaps; // pointer to front

	char prefix[256] = "/";
	char name[256];
	snprintf(name, strlen(name_in) + 1, "%s", name_in);

	// remap args should be local:=global or __ns:=ns or __name:=name
	// string parsing is so much fun
	for(int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		size_t arglen = strlen(arg);
		assert(arglen < 254 && "Error: Argument too long.");

		char *remap = strstr(arg, ":=");
		if(!remap) {
			continue; // not a remap, no ":="
		}
		remap += 2; // start after ":="
		assert(remap - arg < (long) arglen && remap - 2 != arg && "Error: Remap is empty.");

		// input / global cannot contain ":" or "//"
		assert(strstr(remap, ":") == 0 && "Error: Invalid remap contains extra ':'");
		assert(strstr(arg, "//") == 0 && "Error: Invalid remap contains '//'");

		// special __ns or __name remap
		if(strncmp(arg, "__ns:=", strlen("__ns:=")) == 0) {
			if(arg[arglen - 1] == '/') { // strip terminating '/'
				arglen -= 1;
			}
			if(arglen && remap[0] == '/') { // strip leading '/'
				arglen -= 1;
				remap += 1;
			}
			int nslen = arglen - strlen("__ns:="); // ending '/' + null
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
			snprintf(name, arglen - (remap - arg) + 1, "%s", remap);
			continue;
		}

		assert(strncmp(arg, "__", 2) != 0 && "Error: only __name and __ns are recognized special remaps starting with '__'.");

		// else this is a normal remap  local/name:=/global/name
		int local_len = (remap - 2) - arg;
		snprintf(remap_head, local_len + 1, "%s", arg);
		remap_head += local_len + 1;
		remap_lens[remap_count * 2] = local_len;

		int global_len = arglen - (remap - arg);
		snprintf(remap_head, global_len + 1, "%s", remap);
		remap_head += global_len + 1;
		remap_lens[remap_count * 2 + 1] = local_len;

		remap_count += 1;
	}

	atexit(_rose_shutdown);
	signal(SIGINT, _rose_signal_shutdown);
	signal(SIGTERM, _rose_signal_shutdown);
	signal(SIGABRT, _rose_signal_shutdown);

	return rose_create_node(name,
				(i64) strlen(name),
				prefix,
				(i64) strlen(prefix),
				remaps,
				remap_lens,
				remap_count,
				allocate,
				deallocate);
}


rose_node *rose_create_node(char *in_name, i64 name_len,
			    char *in_prefix, i64 prefix_len,
			    char *remaps, u64 *remap_string_lengths, u64 remap_count,
			    void *(*alloc)(size_t), void (*dealloc)(void *)) {
	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = (void *(*) (size_t) ) alloc;
	}
	void (*deallocate)(void *) = free;
	if(dealloc) {
		deallocate = (void (*)(void *)) dealloc;
	}

	assert(in_name && "Error: name is null");
	assert(name_len > 0 && "Error: name has invalid length");
	assert((prefix_len > 0 || prefix_len == -1) && "Error: prefix_len is invalid");

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
		rose_base[blen] = '\0';
		assert(strchr(rose_base, '/') == NULL && "Error: ROSE_BASE should not contain '/'");
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
	assert(name_len < 255 && "Name is too long");

	rose_node *n = allocate(sizeof(rose_node));
	memset(n, 0, sizeof(rose_node));  // Zero out to ensure all pointers are NULL
	n->allocator = allocate;
	n->deallocator = deallocate;

	// replace "/" after first char with "%" for valid shared memory name
	assert(strchr(in_name, '/') == NULL && "Error: Node name should not contain '/'");
	char *name = rose_alloc(n, name_len + 64);
	memcpy(name, in_name, (size_t) name_len);
	sprintf(name + name_len, "-%d", (int) getpid());
	n->name = name;
	n->name_length = strlen(name);


	for(int i = 1; i < prefix_len; i += 1) {
		if(in_prefix[i] == '/') {
			in_prefix[i] = '%';
		}
	}
	char *prefix = rose_alloc(n, prefix_len + 1);
	snprintf(prefix, prefix_len + 1, "%s", in_prefix);
	n->prefix = prefix;
	n->prefix_length = prefix_len;


	n->remaps = remaps;
	n->remap_strlens = remap_string_lengths;
	n->remap_count = remap_count;

	n->pubs = allocate(sizeof(rose_publisher) * MAX_PUBSUB);
	memset(n->pubs, 0, sizeof(rose_publisher) * MAX_PUBSUB);
	n->pubs_length = 0;
	n->subs = allocate(sizeof(rose_subscriber) * MAX_PUBSUB);
	memset(n->subs, 0, sizeof(rose_subscriber) * MAX_PUBSUB);
	n->subs_length = 0;

	// find where base prefix ends (default "/rose/")
	i64 base_prefix_len = 1;
	while(prefix[base_prefix_len] != '%') {
		base_prefix_len += 1;
	}
	base_prefix_len += 1;

	// map connections (retry with 1ms sleep until connected)
	i64 res = 0;
	memcpy(topic, prefix, base_prefix_len); // base_prefix

	sprintf(topic + base_prefix_len, "%s", "rosemaster");
	for(res = -1; res != 0;) {
		res = quicksand_connect(&n->rosemaster_pub.pub, topic, -1, 512, 8192, 0);
		if(res != 0) rose_sleep(1e-3);
	}
	n->rosemaster_pub.node = n;

	sprintf(topic + base_prefix_len, "%s", "roselog");
	for(res = -1; res != 0;) {
		res = quicksand_connect(&n->log_pub.pub, topic, -1, 1024, 8192, (void *) allocate);
		if(res != 0) rose_sleep(1e-3);
	}
	n->log_pub.node = n;

	sprintf(topic + base_prefix_len, "%s", "param_query");
	for(res = -1; res != 0;) {
		res = quicksand_connect(&n->param_pub.pub, topic, -1, 512, 8192, (void *) allocate);
		if(res != 0) rose_sleep(1e-3);
	}
	n->param_pub.node = n;

	// full prefix
	memcpy(topic, prefix, prefix_len + 1);
	snprintf(topic + prefix_len, n->name_length + 2, "%s%%", n->name);
	sprintf(topic + base_prefix_len + n->name_length + 1, "%s", "params");
	for(res = -1; res != 0;) {
		res = quicksand_connect(&n->param_sub.sub, topic, -1, 1024, 1024, (void *) allocate);
		if(res != 0) rose_sleep(1e-3);
	}
	n->param_sub.node = n;
	n->param_sub.callback = NULL;
	rose_subscriber_sync_latest(&n->param_sub);

	sprintf(topic + base_prefix_len + n->name_length + 1, "%s", "control");
	for(res = -1; res != 0;) {
		res = quicksand_connect(&n->control_sub.sub, topic, -1, 1024, 1024, (void *) allocate);
		if(res != 0) rose_sleep(1e-3);
	}
	n->control_sub.node = n;
	n->control_sub.callback = NULL;
	rose_subscriber_sync_latest(&n->control_sub);

	ROSE_STATIC_NODE = n;

	// Initialize heartbeat timestamp
	n->last_heartbeat_ns = 0;

	// Send NodeStart registration to rosemaster
	_rose_send_registration(n, ROSE_MSG_NODE_START, NULL, 0);

	return n;
}

i64 rose_ok(rose_node *n) {
	if(!n) {
		return 0;
	}
	_rose_send_heartbeat(n);
	return rose_remaining(&n->control_sub) == 0;
}

void rose_shutdown(rose_node *n) {
	if(!n) {
		return;
	}

	if(n->param_sub.sub) {
		// printf("deleting %s\n", n->param_sub.sub->name);
		quicksand_delete((char *) n->param_sub.sub->name, -1);
		n->param_sub.sub = NULL;
	}
	if(n->control_sub.sub) {
		// printf("deleting %s\n", n->control_sub.sub->name);
		quicksand_delete((char *) n->control_sub.sub->name, -1);
		n->control_sub.sub = NULL;
	}
}

rose_publisher *rose_create_pub(rose_node *n,
				char *name, i64 name_length,
				u64 message_length, u64 message_hz) {
	assert(n && "Error: node is NULL");
	if(message_length == 0 || message_hz == 0) {
		return NULL;
	}
	size_t nlen = (name_length == -1) ? strlen(name) : (size_t) name_length;

	/* Build the fully-qualified topic name. */
	char topic[512];
	if(name[0] == '/') {
		snprintf(topic, sizeof(topic), "%.*s", (int) nlen, name);
	} else {
		snprintf(topic, sizeof(topic), "%s%.*s", n->prefix, (int) nlen, name);
	}
	if(n->pubs_length >= MAX_PUBSUB) {
		return NULL;
	}
	for(u64 i = 1; i < strlen(topic); i += 1) {
		if(topic[i] == '/') {
			topic[i] = '%';
		}
	}

	rose_publisher *pub = n->pubs + n->pubs_length;
	pub->node = n;

	int64_t ret = quicksand_connect(&pub->pub, topic, -1,
					(int64_t) message_length,
					(int64_t) message_hz,
					(void *) n->allocator);
	if(ret != 0) {
		return NULL;
	}

	n->pubs_length += 1;

	// Send PubRegister to rosemaster
	_rose_send_registration(n, ROSE_MSG_PUB_REGISTER, topic, strlen(topic));

	return pub;
}

rose_subscriber *rose_create_sub(rose_node *n,
				 char *name, i64 name_length,
				 u64 latest_only,
				 void (*callback)(mirage_msg *)) {
	assert(n && "node is NULL");
	size_t nlen = (name_length == -1) ? strlen(name) : (size_t) name_length;

	char topic[512];
	if(name[0] == '/') {
		snprintf(topic, sizeof(topic), "%.*s", (int) nlen, name);
	} else {
		snprintf(topic, sizeof(topic), "%s%.*s", n->prefix, (int) nlen, name);
	}
	for(u64 i = 1; i < strlen(topic); i += 1) {
		if(topic[i] == '/') {
			topic[i] = '%';
		}
	}

	if(n->subs_length >= MAX_PUBSUB) {
		return NULL;
	}

	rose_subscriber *sub = n->subs + n->subs_length;
	sub->node = n;
	sub->callback = callback;
	sub->latest_only = latest_only;

	int64_t ret = quicksand_connect(&sub->sub, topic, -1, -1, -1, (void *) n->allocator);
	if(ret != 0) {
		return NULL;
	}

	rose_subscriber_sync_latest(sub);

	n->subs_length += 1;

	// Send SubRegister to rosemaster
	_rose_send_registration(n, ROSE_MSG_SUB_REGISTER, topic, strlen(topic));

	return sub;
}


void rose_delete_subscriber(rose_node *n, rose_subscriber *s) {
	rose_subscriber *sub = n->subs;
	while(sub < n->subs + n->subs_length) {
		if(sub->sub == s->sub) {
			sub->callback = NULL; // prevents spin from running it.
			break;
		}
		sub += 1;
	}
}


i64 rose_publish(rose_publisher *p, mirage_msg *msg) {
	if(!p || !msg) {
		return ROSE_ERR_BADINPUT;
	}
	return quicksand_write(p->pub, msg->data, msg->cursor);
}

// NOT THREAD SAFE - do NOT read same subscriber concurrently across threads
i64 rose_read(rose_subscriber *s, rosemsg *msg) {
	if(!s || !msg) {
		return ROSE_ERR_BADINPUT;
	}

	int64_t sz = msg->max_length;
	int64_t ret = quicksand_read(s->sub, msg->data, &sz);
	if(ret >= 0) {
		msg->length = (u64) sz;
		return ret; /* number of msgs remaining */
	}
	return ROSE_NO_DATA; /* nothing to read */
}

// NOT THREAD SAFE - do NOT read same subscriber concurrently across threads
i64 rose_readlast(rose_subscriber *s, rosemsg *msg) {
	if(!s || !msg) {
		return ROSE_ERR_BADINPUT;
	}

	int64_t sz = msg->max_length;
	int64_t ret = quicksand_read_latest(s->sub, msg->data, &sz);
	if(ret >= 0) {
		msg->length = (u64) sz;
		return ret;
	}
	return ROSE_NO_DATA;
}

u64 rose_remaining(rose_subscriber *s) {
	return quicksand_read_remaining(s->sub);
}


// NOT THREAD SAFE - only call this from one thread at a time.
void rose_spin_once(rose_node *n) {
	if(!n) {
		return;
	}

	static _Thread_local mirage_msg *m = NULL;
	if(!m) {
		m = mirage_create(MAX_MESSAGE_SIZE, n->allocator);
		if(!m) {
			return; // allocation failure
		}
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
			rose_warn("Failed reading callback data");
		}
	}
}

// NOT THREAD SAFE - only call this from one thread at a time.
void rose_spin_forever(rose_node *node) {
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
	fflush(stream);

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
	if(!n || !param_name || param_name_length <= 0 || !param) {
		return ROSE_ERR_BADINPUT;
	}

	static _Thread_local mirage_msg *req = NULL;

	/* ---------- 1.  Build the node identifier string ---------- */
	char node_id[512];
	snprintf(node_id, sizeof(node_id), "%s%s", n->prefix, n->name);

	/* ---------- 2.  Build the request Mirage message ---------- */
	/* Allocate a temporary request buffer - generous size. */
	if(!req) {
		req = mirage_create(1024, n->allocator);
		if(!req) {
			return ROSE_ERR_BUFFERSIZE;
		}
	}


	int64_t err = 0;
	err |= mirage_write_start(req);
	err |= mirage_write_fn(req, "Request", (int64_t) strlen("Request"), 2);
	err |= mirage_write_string(req, param_name, (int64_t) param_name_length);
	err |= mirage_write_string(req, node_id, (int64_t) strlen(node_id));

	if(err < 0) {
		mirage_destroy(&req, 0);
		return ROSE_ERR_BADINPUT;
	}

	/* ---------- 3.  Send the request ---------- */
	int64_t rc = quicksand_write(n->param_pub.pub, req->data, (int64_t) req->length);
	mirage_destroy(&req, 0);
	if(rc != 0) {
		return rc; /* propagate quicksand error */
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
			return 0;
		}
		rose_sleep(1e-3);
	}

	/* ---------- 5.  Timeout ---------- */
	fprintf(stderr,
		"rose_get_param: timeout waiting for reply to \"%.*s\"\n",
		(int) param_name_length, param_name);
	return ROSE_NO_DATA;
}

i64 rose_set_param(rose_node *n, char *param_name, i64 param_name_length,
		   mirage_msg *param) {
	if(!n || !param) {
		return ROSE_ERR_BADINPUT;
	}

	/* Static buffer for the wrapped message */
	static _Thread_local mirage_msg *m = NULL;
	if(!m) {
		m = mirage_create(8192, 0);
		if(!m) {
			return ROSE_ERR_BUFFERSIZE;
		}
	}

	/* Reset the message to start a new packet */
	mirage_write_start(m);

	/* Write the List header - two arguments */
	mirage_write_fn(m, "List", (int64_t) strlen("List"), 2);

	/* First argument: param name - use the node name for simplicity. */
	mirage_write_string(m, param_name, param_name_length);

	/* Second argument: copy of the original message data,
       skipping the 2-byte start marker. */
	int64_t data_len = (param->cursor > 2) ? param->cursor - 2 : 0;
	if(data_len > 0) {
		memcpy(m->data + m->cursor, param->data + 2, data_len);
		m->cursor += data_len;
	}

	/* Send the wrapped message */
	return quicksand_write(n->param_pub.pub, m->data, (int64_t) m->cursor);
}

// ------------------------- Topic Naming --------------------------------------

char *rose_topic_to_shm(char *shm, size_t shm_size, const char *topic) {
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
	snprintf(shm, shm_size, "%s%s", base, topic);

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
