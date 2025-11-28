#include "ros3.h"

#include <stdlib.h>   /* malloc, free */
#include <stdio.h>    /* snprintf */
#include <string.h>   /* strlen, strcpy, memcpy */
#include <unistd.h>   /* getpid */
#include <sys/types.h>/* pid_t */
#include <assert.h>

#define REMAP_SIZE 4096 // at most 4kb of remap strings
#define REMAP_COUNT 64  // at most 64 remaps
#define MAX_PUBSUB 256  // at most n pubsubs

static rose_node *ROSE_STATIC_NODE = NULL;

static void* rose_alloc(rose_node *node, size_t size)
{
    if (node && node->allocator) return node->allocator(size);
    return malloc(size);
}

static void rose_dealloc(rose_node *node, void **ptr)
{
    if (!ptr || !(*ptr)) return;
    if (node && node->deallocator) node->deallocator(ptr);
    else free(ptr);
    *ptr = 0;
}

rose_node *rose_init(char argc, char **argv, char* name_in,
		void *(*alloc)(size_t), void (*dealloc)(void *)) {
	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = (void *(*) (size_t)) alloc;
	}
	void (*deallocate)(void *) = free
	if(dealloc) {
		deallocate = (void (*)(void *)) dealloc;
	}

	i64 *remap_lens = allocate(REMAP_COUNT*2); // lengths
	char *remaps; = allocate(REMAP_SIZE);
	u64 remap_count = 0;

	char *remap_head = remaps; // pointer to front

	char prefix[256] = "/rose/";
	if (name_in[0] == "/"){
		prefix[0] = "\0"; // provided global-namespaced name.
	}
	char name[256];
	snprintf(name, strlen(name_in)+1, "%s", name_in)
	
	size_t remap_count = 0;

	// remap args should be local:=global or __ns:=ns or __name:=name
	// string parsing is so much fun
	for (int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		const size_t arglen = strlen(arg);
		assert(arglen < 254 && "Error: Argument too long.");

		char *remap = strstr(arg, ":=");
		if (!remap) {
			continue; // not a remap, no ":="
		}
		remap += 2; // start after ":="
		assert(remap - arg < arglen && remap-2 != arg &&
				"Error: Remap is empty.");

		// input / global cannot contain ":" or "//"
		assert(strnstr(arg, ":", remap-arg) == 0 && 
			strnstr(remap, ":", (arg+arglen) - remap) == 0 && 
			"Error: Invalid remap contains extra ':'");
		assert(strstr(arg, "//") == 0 &&
			"Error: Invalid remap contains '//'");

		// special __ns or __name remap
		if(strncmp(arg, "__ns:=", strlen("__ns:=")) == 0) {
			if(arg[arglen] == "/") { // strip terminating '/'
				arglen -= 1;
			}
			int nslen = arglen - strlen("__ns:=") + 2 // '/' + null
			if (remap[0] == "/") {  // global prefix, leading '/'
				snprintf(prefix, nslen, "%s/", remap);
			} else {  // default namespace prefix
				nslen += strlen("/rose/");
				snprintf(prefix, nslen, "/rose/%s/", remap);
			}
			continue;
		}

		if(strncmp(arg, "__name:=", strlen("__name:=")) == 0) {
			if(arg[arglen] == "/") { // strip terminating '/'
				arglen -= 1;
			}
			if(remap[0] == "/") { // stkip starting '/'
				remap += 1;
			}
			snprintf(name, remap, arglen-(remap-arg) + 1, "%s", remap);
			continue;
		}

		// else this is a normal remap  local/name:=/global/name
		int local_len = (remap-2)-arg;
		snprintf(remap_head, local_len + 1, "%s", arg);
		remap_head += local_len + 1;
		remap_lens[remap_count*2] = local_len;
		
		int global_len = arglen - (remap-arg);
		snprintf(remap_head, global_len + 1, "%s", remap);
		remap_head += global_len + 1;
		remap_lens[remap_count*2 + 1] = local_len;

		remap_count += 1;
	}

	return rose_create_node(name,
		(i64)strlen(name),
		prefix,
		(i64)strlen(prefix),
		remap_buf,
		(i64 *)remap_lens,
		remap_cnt,
		allocate,
		deallocate);
}

rose_node *rose_create_node(char* in_name, i64 name_len,
		char* in_prefix, i64 prefix_len,
		char *remaps, i64* remap_string_lengths, u64 remap_count,
		void *(*alloc)(size_t), void (*dealloc)(void *)) {
	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = (void *(*) (size_t)) alloc;
	}
	void (*deallocate)(void *) = free
	if(dealloc) {
		deallocate = (void (*)(void *)) dealloc;
	}

	assert(in_name && "Error: name is null");
	assert(name_len > 0 && "Error: name has invalid length");
	assert((prefix_len > 0 || prefix_len == -1)  && "Error: prefix_len is invalid");

	// default to prefix if in_prefix is nullptr.
	char topic[512] = "/rose/";
	if (!in_prefix) {
		in_prefix = topic;
		prefix_len = 6;
	}
	if (prefix_len == -1) {
		prefix_len = strlen(in_prefix)
	}
	if(in_prefix[0] != "/") {
		snprintf(topic + 6, prefix_len + 6 + 1, "%s", in_prefix);
		in_prefix = topic;
		prefix_len =  prefix_len + 6;
	}

	// default to strlen if provided len is -1.
	if (name_len == -1) {
		name_len = strlen(in_name)
	}
	assert(name_len < 255 && "Prefix is too long");

	rose_node n = allocate(sizeof(rose_node));
	n->allocator = allocate;
	n->deallocator = deallocate;

	char *name = rose_alloc(n, name_len + 64);
	memcpy(name, in_name, (size_t)name_len);
	sprintf(name+name_length, "-%d", (int)getpid());
	n->name = name;
	n->name_length = strlen(name);

	char *prefix = rose_alloc(n, prefix_len + 1);
	snprintf(prefix, prefix_len + 1, "%s", in_prefix);
	n->prefix = prefix;
	n->prefix_len = prefix_len;

	n->remaps = remaps;
	n->remap_strlens = remap_string_lengths;
	n->remap_count = remap_count;

	n->pubs = allocate(sizeof(rose_publisher) * MAX_PUBSUB);
	n->pubs_length = 0;
	n->subs = allocate(sizeof(rose_subscriber) * MAX_PUBSUB);;
	n->pubs_length = 0;

	// find where base prefix ends (default "/rose/")
	i64 base_prefix_len = 1;
	while(prefix[base_prefix_len] != "/") {
		base_prefix_len += 1;
	}
	base_prefix_len += 1;

	// map connections
	i64 res = 0;
	memcpy(topic, prefix, base_prefix_len); // base_prefix
	sprintf(topic + base_prefix_len, "%s", "rosemaster");
	res = quicksand_connect(&n->rosemaster_pub, topic, -1, 512, 8192, allocate);
	assert(res == 0 && "Error: could not map rosemaster ring.")
	n->rosemaster_pub.node = n;

	sprintf(topic + base_prefix_len, "%s", "roselog");
	res = quicksand_connect(&n->log_pub, topic, -1, 1024, 8192, allocate);
	assert(res == 0 && "Error: could not map roselog ring.")
	n->log_pub.node = n;

	sprintf(topic + base_prefix_len, "%s", "param_query");
	res = quicksand_connect(&n->param_pub, topic, -1, 512, 8192, allocate);
	assert(re == 0 && "Error: could not map param_query ring.");
	n->param_pub.node = n;

	// full prefix
	memcpy(topic, prefix, prefix_len + 1);
	snprintf(topic + prefix_len, name_len + 2 "%s/", name);
	sprintf(topic + base_prefix_len + name_len + 1, "%s", "params");
	res = quicksand_connect(&n->param_sub, topic, -1, 1024, 1024, allocate);
	assert(res == 0 && "Error: could not map node params ring.")
	n->param_sub.node = n;
	n->param_sub.callback = NULL;

	sprintf(topic + base_prefix_len + name_len + 1, "%s", "control");
	res = quicksand_connect(&n->param_pub, topic, -1, 1024, 1024, allocate);
	assert(res == 0 && "Error: could not map node control ring.")
	n->control_sub.node = n;
	n->control_sub.callback = NULL;

	ROSE_GLOBAL_NODE = n;

	return n;
}

rose_publisher *rose_create_pub(rose_node *n,
	char *name, i64 name_length,
	u64 message_length, u64 message_hz) {
	assert(n && "Error: node is NULL");
	size_t nlen = (name_length == -1) ? strlen(name) : (size_t)name_length;

	/* Build the fully-qualified topic name. */
	char topic[512];
	if (name[0] == '/')
		snprintf(topic, sizeof(topic), "%.*s", (int)nlen, name);
	else
		snprintf(topic, sizeof(topic), "%s%.*s",
	n->prefix, (int)nlen, name);

	rose_publisher pub = n->pubs[pubs_length];
	pub->node = n;

	int64_t ret = quicksand_connect(&pub->pub,
	topic, -1,
	(int64_t)message_length,
	(int64_t)message_hz,
	n->allocator);
	if (ret != 0) {
		return NULL;
	}

	pubs_length += 1;
	return pub;
}

rose_subscriber *rose_create_sub(rose_node *n,
	char *name, i64 name_length,
	u64 latest_only,
	void (*callback)(u8 *)) {
	(void)latest_only;   /* not used in the minimal implementation */

	assert(n && "node is NULL");
	size_t nlen = (name_length == -1) ? strlen(name) : (size_t)name_length;

	char topic[512];
	if (name[0] == '/')
		snprintf(topic, sizeof(topic), "%.*s", (int)nlen, name);
	else
		snprintf(topic, sizeof(topic), "%s%.*s",
	n->prefix, (int)nlen, name);

	rose_publisher sub = n->subs[subs_length];
	sub->node = n;
	sub->callback = callback;

	int64_t rc = quicksand_connect(&sub->sub,
	topic, -1,
	-1, -1,
	n->allocator);
	if (rc != 0) {
		return NULL;
	}

	add_subscriber(sub);
	return sub;
}


i64 rose_publish(rose_publisher *p, mirage_msg *msg)
{
	if (!p || !msg)
		return ROSE_ERR_BADINPUT;
	return quicksand_write(p->pub, msg->data, msg->cursor);
}


i64 rose_read(rose_subscriber *s, rosemsg *msg)
{
	if (!s || !msg)
		return ROSE_ERR_BADINPUT;

	int64_t sz = msg->max_length;
	int64_t rc = quicksand_read(s->sub, msg->data, &sz);
	if (rc >= 0) {
		msg->length = (u64)sz;
		return rc;                 /* number of msgs remaining */
	}
	return ROSE_NO_DATA;               /* nothing to read */
}

i64 rose_readlast(rose_subscriber *s, rosemsg *msg)
{
	if (!s || !msg)
		return ROSE_ERR_BADINPUT;

	int64_t sz = msg->max_length;
	int64_t rc = quicksand_read_latest(s->sub, msg->data, &sz);
	if (rc >= 0) {
		msg->length = (u64)sz;
		return rc;
	}
	return ROSE_NO_DATA;
}


void rose_spin_once(void)
{
	for (size_t i = 0; i < g_sub_cnt; ++i) {
		rose_subscriber *sub = g_subs[i];
		if (!sub->callback)
			continue;

		/* Allocate a temporary buffer sized to the underlying slot. */
		uint64_t slot_sz = sub->sub->buffer->message_size;
		uint8_t *buf = rose_alloc(sub->node, (size_t)slot_sz);
		int64_t sz = (int64_t)slot_sz;

		int64_t rc = quicksand_read_latest(sub->sub, buf, &sz);
		if (rc >= 0) {
			/* The user-provided callback receives the raw payload. */
			sub->callback(buf);
		}
		rose_dealloc(sub->node, (void **)&buf);
	}
}

void rose_spin_forever(void)
{
	while (1) {
		rose_spin_once();
		rose_sleep(1e-6);
	}
}

// -------------------------- Logging ------------------------------------------

/* --------------------------------------------------------------------
 *  rose_log - print to console and publish a Mirage "Log" message.
 *
 *  The function maps the integer level to a console prefix and an
 *  integer error value that is sent as an f64 argument in the Mirage
 *  message.
 *
 *  Console output:
 *      INFO   -> stdout
 *      others -> stderr
 * ------------------------------------------------------------------*/
void rose_log(rose_node *node,
              int level,
              const char *fmt,
              ...)
{
    /* ------------------------------------------------------------------
     *  1.  Map the integer level to a string prefix.
     * ------------------------------------------------------------------*/
    const char *prefix;
    switch (level) {
        case 0: prefix = "[ROSE INFO] ";  break;
        case 1: prefix = "[ROSE WARN] ";  break;
        case 2: prefix = "[ROSE ERROR] "; break;
        case 3: prefix = "[ROSE FATAL] "; break;
        default:
            prefix = "[ROSE INFO] ";
            level  = 0;                     /* treat unknown as INFO */
            break;
    }

    /* ------------------------------------------------------------------
     *  2.  Format the user message (max 1024 bytes).
     * ------------------------------------------------------------------*/
    char user_msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int user_len = vsnprintf(user_msg, sizeof(user_msg), fmt, ap);
    va_end(ap);
    if (user_len < 0) return;                 /* formatting error */
    if ((size_t)user_len >= sizeof(user_msg))
        user_len = (int)sizeof(user_msg) - 1;
    user_msg[user_len] = '\0';

    /* ------------------------------------------------------------------
     *  3.  Compose the console string.
     * ------------------------------------------------------------------*/
    char console_msg[1024];
    int console_len = snprintf(console_msg, sizeof(console_msg),
                               "%s%s", prefix, user_msg);
    if (console_len < 0) return;
    if ((size_t)console_len >= sizeof(console_msg))
        console_len = (int)sizeof(console_msg) - 1;
    console_msg[console_len] = '\0';

    /* ------------------------------------------------------------------
     *  4.  Print to stdout (INFO) or stderr (others).
     * ------------------------------------------------------------------*/
    FILE *stream = (level == 0) ? stdout : stderr;
    fputs(console_msg, stream);
    fputc('\n', stream);
    fflush(stream);

    /* ------------------------------------------------------------------
     *  5.  Build and publish a Mirage message.
     * ------------------------------------------------------------------*/
    static mirage_msg *m = mirage_create(1024, 0);
    if (!m) 
    	m = mirage_create(1024, 0);
    if (!m) 
    	return; /* allocation failure */

    int64_t err = 0;
    err |= mirage_write_start(m);              /* message start marker */
    err |= mirage_write_fn(m, "Log", (int64_t)strlen("Log"), 2);
    err |= mirage_write_f64(m, (f64)level);   /* integer error level as f64 */
    err |= mirage_write_string(m, user_msg,
                               (int64_t)strlen(user_msg));

    if (err >= 0 && node && node->log_pub.pub) {
        quicksand_write(node->log_pub.pub, m->data, (int64_t)m->length);
    }

    mirage_destroy(&m, 0);
}

void rose_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    rose_log_wrapper(ROSE_STATIC_NODE, 0, fmt, ap);
    va_end(ap);
}

void rose_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    rose_log_wrapper(ROSE_STATIC_NODE, 1, fmt, ap);
    va_end(ap);
}

void rose_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    rose_log_wrapper(ROSE_STATIC_NODE, 2, fmt, ap);
    va_end(ap);
}

void rose_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    rose_log_wrapper(ROSE_STATIC_NODE, 3, fmt, ap);
    va_end(ap);
}


// --------------------------Parameter handling --------------------------------

/* ------------------------------------------------------------------
 *  rose_get_param - send a Request and wait (<=1 s) for the reply.
 * -----------------------------------------------------------------*/
i64 rose_get_param(rose_node *n,
                   char *param_name, i64 param_name_length,
                   mirage_msg *param)
{
    if (!n || !param_name || param_name_length <= 0 || !param)
        return ROSE_ERR_BADINPUT;

    /* ---------- 1.  Build the node identifier string ---------- */
    size_t node_id_len = (size_t)n->prefix_length + (size_t)n->name_length;
    char *node_id = rose_alloc(n, node_id_len + 1);
    memcpy(node_id, n->prefix, (size_t)n->prefix_length);
    memcpy(node_id + n->prefix_length, n->name, (size_t)n->name_length);
    node_id[node_id_len] = '\0';

    /* ---------- 2.  Build the request Mirage message ---------- */
    /* Allocate a temporary request buffer - generous size. */
    int64_t req_buf_sz = (int64_t)(param->max_length   /* the param payload   */
                                  + node_id_len        /* node string         */
                                  + 256);              /* overhead            */
    mirage_msg *req = mirage_create(req_buf_sz, 0);
    if (!req) {
        rose_dealloc(n, (void **)&node_id);
        return ROSE_ERR_BADINPUT;
    }

    int64_t err = 0;
    err |= mirage_write_start(req);                                   /* start */
    err |= mirage_write_fn(req, "Request", (int64_t)strlen("Request"), 2);
    /* arg0 : the parameter payload (as a nested Mirage message) */
    err |= mirage_write_msg(req, param);                               /* embed */
    /* arg1 : node identifier string */
    err |= mirage_write_string(req, node_id, (int64_t)node_id_len);
    rose_dealloc(n, (void **)&node_id);

    if (err < 0) {
        mirage_destroy(&req, 0);
        return ROSE_ERR_BADINPUT;
    }

    /* ---------- 3.  Send the request ---------- */
    int64_t rc = quicksand_write(n->param_pub.pub, req->data, (int64_t)req->length);
    mirage_destroy(&req, 0);
    if (rc != 0)
        return rc;               /* propagate quicksand error */

    /* ---------- 4.  Wait for a reply (<=1 s) ---------- */
    uint64_t start_ts = quicksand_now();
    const double timeout_ns = 1e9;          /* 1 second */
    while (quicksand_ns(quicksand_now(), start_ts) < timeout_ns) {
        int64_t sz = (int64_t)param->max_length;
        int64_t r = quicksand_read_latest(n->param_sub.sub,
                                          param->data,
                                          &sz);
        if (r >= 0) {                      /* a message arrived */
            param->length = (u64)sz;
            return 0;                      /* success */
        }
        rose_sleep(1e-3);                 /* 1 ms poll interval */
    }

    /* ---------- 5.  Timeout ---------- */
    fprintf(stderr,
            "rose_get_param: timeout waiting for reply to \"%.*s\"\n",
            (int)param_name_length, param_name);
    return ROSE_NO_DATA;
}

i64 rose_set_param(rose_node *n,
                   char *param_name, i64 param_name_length,
                   mirage_msg *param)
{
    (void)param_name_length;   /* unused */

    if (!n || !param)
        return ROSE_ERR_BADINPUT;

    return quicksand_write(n->param_pub.pub, param->data, (int64_t)param->length);
}


// ------------------------- Timing Utilities ----------------------------------
u64 rose_now(void)
{
    return quicksand_now();
}

void rose_timer_calibrate_seconds(double seconds)
{
    quicksand_ns_calibrate(seconds * 1e9);
}

double rose_seconds(u64 stop, u64 start)
{
    return quicksand_ns(stop, start) * 1e-9;
}

double rose_nanoseconds(u64 stop, u64 start)
{
    return (double)quicksand_ns(stop, start);
}

/* The header is missing a semicolon after the prototype; we provide the
   matching definition anyway. */
double rose_elapsed_seconds(u64 start)
{
    return rose_seconds(quicksand_now(), start);
}

double rose_elapsed_nanoseconds(u64 start)
{
    return rose_nanoseconds(quicksand_now(), start);
}

// ------------------------- Sleep Utilities -----------------------------------
void rose_sleep(double seconds)
{
    quicksand_sleep(seconds * 1e9);
}

/* Simple 1-step rate-maintaining sleep. */
void rose_sleep_rate(double hz)
{
    static u64 last_ts = 0;
    const double period_ns = 1e9 / hz;

    if (last_ts == 0) {
        last_ts = quicksand_now();
        return;
    }

    u64 now = quicksand_now();
    double elapsed = quicksand_ns(now, last_ts);
    double to_sleep = period_ns - elapsed;
    if (to_sleep > 0.0)
        quicksand_sleep(to_sleep);

    last_ts = quicksand_now();
}