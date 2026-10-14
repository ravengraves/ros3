#ifndef ROS3_H
#define ROS3_H

#include <mirage.h>
#include <stdint.h>

#ifdef __cplusplus
// quicksand.h is C11-only (_Atomic), so C++ consumers use an opaque pointer.
typedef struct quicksand_connection quicksand_connection;
#else
#include <quicksand.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;
typedef int8_t i8;

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef float f32;
typedef double f64;

typedef mirage_msg rosemsg;

struct rose_node;
struct rose_publisher;
struct rose_subscriber;

typedef struct rose_node rose_node;
typedef struct rose_publisher rose_publisher;
typedef struct rose_subscriber rose_subscriber;

// --------- Data types --------------------------------------------------------
struct rose_publisher {
	rose_node *node;
	quicksand_connection *pub;
};

struct rose_subscriber {
	rose_node *node;
	quicksand_connection *sub;
	u64 latest_only;
	void (*callback)(mirage_msg *);
};

struct rose_node {
	char *name;
	uint64_t name_length;
	char *prefix;
	uint64_t prefix_length;
	char *remaps;
	uint64_t *remap_strlens;
	uint64_t remap_count;

	rose_publisher *pubs;
	uint64_t pubs_length;
	rose_subscriber *subs;
	uint64_t subs_length;

	rose_publisher rosecore_pub;
	rose_subscriber control_sub;
	rose_publisher param_pub;
	rose_subscriber param_sub;
	rose_publisher log_pub;

	void *(*allocator)(size_t);
	void (*deallocator)(void *);

	u64 last_heartbeat_ns; // Timestamp of last heartbeat sent (nanoseconds)

	// Scratch storage belongs to this node and uses its allocator pair.
	mirage_msg *registration_scratch;
	mirage_msg *callback_scratch;
	mirage_msg *param_request_scratch;
	mirage_msg *param_wrapper_scratch;
	// Per-node dispatch lifetime; all access requires external serialization.
	int spin_active;
	int shutdown_requested;
};


typedef enum {
	ROSE_OK = 0,
	ROSE_NO_DATA = -1,
	ROSE_ERR_BADDATA = -2,
	ROSE_ERR_BUFFERSIZE = -4,
	ROSE_ERR_BADINPUT = -8
} ROSE_RES;

// Thread-local details for the most recent library error on the calling thread.
const char *rose_last_error(void);

i64 rose_last_error_code(void);

// Registration message types
#define ROSE_MSG_NODE_START "NodeStart"
#define ROSE_MSG_PUB_REGISTER "PubRegister"
#define ROSE_MSG_SUB_REGISTER "SubRegister"
#define ROSE_MSG_HEARTBEAT "Heartbeat"

// Query message types
#define ROSE_MSG_DUMP_ALL "DumpAll"
#define ROSE_MSG_LOOKUP_NAME "LookupName"
#define ROSE_MSG_LOOKUP_PID "LookupPid"
#define ROSE_MSG_NODE_INFO "NodeInfo"
#define ROSE_MSG_DONE "Done"
#define ROSE_MSG_START "Start"

// Query topic suffixes (appended to ROSE_BASE)
#define ROSE_TOPIC_REQUEST "rosecore_request"
#define ROSE_REQUEST_RING_SIZE 128
#define ROSE_REQUEST_MSG_SIZE 512

#define ROSE_TOPIC_DUMP "rosecore_dump"
#define ROSE_TOPIC_LOOKUP "rosecore_lookup"
#define ROSE_TOPIC_HEARTBEAT "rosecore_heartbeat"

// Registry bounds include the terminating NUL in each name buffer.
#define ROSE_MAX_NODES 1024
#define ROSE_MAX_TOPICS 64
#define ROSE_MAX_NAME_LEN 256
// Two bounded names plus Mirage framing; no dependence on transport padding.
#define ROSE_REGISTRATION_MSG_SIZE (2 * ROSE_MAX_NAME_LEN + 32)
#define ROSE_TOPIC_CSV_SIZE (ROSE_MAX_TOPICS * ROSE_MAX_NAME_LEN)
// One NodeInfo per slot: name, both CSV lists, and a conservative allowance
// for Mirage framing, function name, field tags/lengths, PID and timestamp.
#define ROSE_QUERY_MSG_SIZE (ROSE_MAX_NAME_LEN + 2 * ROSE_TOPIC_CSV_SIZE + 256)
// Request three dump slots per maximum node; Quicksand rounds up to a
// power of two. Half-ring retention accommodates a full dump plus Done.
#define ROSE_DUMP_RING_SIZE (3 * ROSE_MAX_NODES)
// Sized for targeted lookups; broad prefixes can exceed retained capacity.
#define ROSE_LOOKUP_RING_SIZE 128

/// ---------------- Initialization---------------------------------------------

/* Names use ASCII letters, digits, '_', '-', and '.' in nonempty segments.
 * Topics may have one leading slash, but no trailing slash or '//'. Node
 * names are one segment. ROSE_BASE is one nonempty segment with an optional
 * leading/trailing slash; namespaces may contain slash-separated segments.
 * Literal '%' and ',' are rejected.
 * Topic remaps match fully namespace-resolved sources, last match wins, and
 * apply once. Relative destinations use the node namespace; absolute ones
 * use the ROSE_BASE captured at construction, even if the environment changes.
 * All rules, including shadowed rules, must fully resolve within 255 bytes at
 * construction; invalid configuration rejects the node before attaching rings.
 * All transport names (including private channel suffixes) fit 255 bytes,
 * plus a separate byte for NUL. Standalone rose_topic_to_shm uses current env.
 * n->name and discovery use "namespace/name-PID", or "name-PID" at root.
 * Namespaces are relative to the base. A name must be unique within its
 * namespace and process; duplicate instances there are unsupported.
 * Namespaced identities require updated library, core, CLI and bindings together. */

rose_node *rose_init(int argc, char **argv, char *name,
		     void *(*alloc)(size_t), void (*deallocator)(void *));

rose_node *rose_create_node(char *name, i64 name_length,
			    char *prefix, i64 prefix_len,
			    char *remaps, u64 *remap_string_lengths, u64 remap_count,
			    void *(*alloc)(size_t), void (*deallocator)(void *));

// Allocator hooks must be supplied as a pair, or both NULL for malloc/free.
// Names/prefixes accept -1 for strlen or exact byte spans without embedded NULs.
// Remap storage transfers to the node only on successful construction; on
// failure it remains caller-owned. Transferred storage must use the same hooks.
// Construction returns NULL if initial discovery encoding/allocation/write fails.
// Check rose_last_error_code/message; success means the registration was written
// to the transport, not acknowledged by rosecore.
// Remaps contain 2*count NUL-terminated strings and matching lengths excluding
// NUL. The caller must provide buffers covering those lengths and terminators;
// this API has no aggregate capacities. Do not mutate transferred storage.

// During this node's callback dispatch, shutdown is deferred until the spin
// call returns, and no subsequent callbacks run. Repeated requests during that
// callback are safe. Outside dispatch destruction is immediate. The pointer
// must not be used after destruction.
void rose_shutdown(rose_node *n);

i64 rose_ok(rose_node *n);

/// ---------------- Pub / Sub -------------------------------------------------

// Endpoint constructors return NULL and leave table lengths unchanged if their
// discovery registration fails. Partial connections are disconnected without
// unlinking shared channels; callers may retry after inspecting the last error.

rose_publisher *rose_create_pub(rose_node *n, char *name, i64 name_length,
				u64 message_length, u64 message_hz);

// Callback subscriptions reject transport slots larger than 6,220,800 bytes
// (including transport padding). Use explicit reads for larger slots.
rose_subscriber *rose_create_sub(rose_node *n, char *name, i64 name_length,
				 u64 latest_only, void (*callback)(mirage_msg *));

void rose_delete_subscriber(rose_node *n, rose_subscriber *s);

i64 rose_publish(rose_publisher *p, rosemsg *msg);

i64 rose_read(rose_subscriber *s, rosemsg *msg);

i64 rose_readlast(rose_subscriber *s, rosemsg *msg);

u64 rose_remaining(rose_subscriber *s);

// Spin a node from one thread at a time. Callback scratch is node-owned and
// valid only for the callback; do not retain it beyond callback return.
// Same-node recursive spinning is rejected with ROSE_ERR_BADINPUT. Callback
// messages remain valid through callback return, including after shutdown is
// requested. Both spin functions return after a callback requests shutdown;
// the node is destroyed before that outer spin call returns. Different nodes
// may be spun recursively, provided their lifetimes are externally serialized.
void rose_spin_once(rose_node *n);

// Checks rose_ok each iteration for heartbeats/control. A control stop returns
// without destroying the node; the caller should then call rose_shutdown.
void rose_spin_forever(rose_node *n);

/// ---------------- Parameters ------------------------------------------------

i64 rose_get_param(rose_node *n, char *param_name, i64 param_name_length,
		   mirage_msg *param);

i64 rose_set_param(rose_node *n, char *param_name, i64 param_name_length,
		   mirage_msg *param);

/// ---------------- Logging ---------------------------------------------------

typedef enum {
	ROSE_LOG_INFO = 0,
	ROSE_LOG_WARN = 1,
	ROSE_LOG_ERROR = 2,
	ROSE_LOG_FATAL = 3
} rose_log_level;

// Log a preformatted byte-counted message. Use message_length=-1 for strlen.
// Logs are best-effort and use a 512-byte internal roselog message slot;
// oversize messages are truncated before console output and roselog publish,
// and a warning is printed to stderr.
void rose_log(rose_node *node, int level, const char *message, i64 message_length);

// Convenience wrappers that use the last created node.
void rose_info(const char *message, i64 message_length);
void rose_warn(const char *message, i64 message_length);
void rose_error(const char *message, i64 message_length);
void rose_fatal(const char *message, i64 message_length);


/// ---------------- Time Measurement ------------------------------------------

u64 rose_now(void);

void rose_timer_calibrate_seconds(f64 seconds);

f64 rose_seconds(u64 stop, u64 start);

f64 rose_nanoseconds(u64 stop, u64 start);

f64 rose_elapsed_seconds(u64 stop);

f64 rose_elapsed_nanoseconds(u64 start);

/// ---------------- Sleep -----------------------------------------------------

void rose_sleep(f64 seconds);

void rose_nanosleep(f64 nanoseconds);

void rose_sleep_rate(f64 hz);

/// ---------------- Topic Naming ----------------------------------------------

// Convert topic name to shared memory name with ROSE_BASE prefix.
// Result format: /<ROSE_BASE>/<topic> with '/' converted to '%' after first char.
// If topic starts with '/', it's treated as absolute (prefix stripped).
// Returns pointer to shm buffer for convenience.
char *rose_topic_to_shm(char *shm, size_t shm_size, const char *topic);

// Configure parent-death signal behavior (currently optional/no-op on some OSes).
int rose_set_pdeathsig(int sig);

#ifdef __cplusplus
}
#endif

#endif // ROS3_H
