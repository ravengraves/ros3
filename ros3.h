#ifndef ROS3_H
#define ROS3_H

#include <mirage.h>
#include <quicksand.h>

#include <stdint.h>

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

	rose_publisher rosemaster_pub;
	rose_subscriber control_sub;
	rose_publisher param_pub;
	rose_subscriber param_sub;
	rose_publisher log_pub;

	void *(*allocator)(size_t);
	void (*deallocator)(void *);
};


typedef enum {
	ROSE_OK = 0,
	ROSE_NO_DATA = -1,
	ROSE_ERR_BADDATA = -2,
	ROSE_ERR_BUFFERSIZE = -4,
	ROSE_ERR_BADINPUT = -8
} ROSE_RES;


/// ---------------- Initialization---------------------------------------------

rose_node *rose_init(char argc, char **argv, char *name,
		     void *(*alloc)(size_t), void (*deallocator)(void *));

rose_node *rose_create_node(char *name, i64 name_length,
			    char *prefix, i64 prefix_len,
			    char *remaps, u64 *remap_string_lengths, u64 remap_count,
			    void *(*alloc)(size_t), void (*deallocator)(void *));

void rose_shutdown(rose_node *n);

i64 rose_ok(rose_node *n);

/// ---------------- Pub / Sub -------------------------------------------------

rose_publisher *rose_create_pub(rose_node *n, char *name, i64 name_length,
				u64 message_length, u64 message_hz);

rose_subscriber *rose_create_sub(rose_node *n, char *name, i64 name_length,
				 u64 latest_only, void (*callback)(mirage_msg *));

void rose_delete_subscriber(rose_node *n, rose_subscriber *s);

i64 rose_publish(rose_publisher *p, rosemsg *msg);

i64 rose_read(rose_subscriber *s, rosemsg *msg);

i64 rose_readlast(rose_subscriber *s, rosemsg *msg);

u64 rose_remaining(rose_subscriber *s);

void rose_spin_once(rose_node *n);

void rose_spin_forever(rose_node *n);

/// ---------------- Parameters ------------------------------------------------

i64 rose_get_param(rose_node *n, char *param_name, i64 param_name_length,
		   mirage_msg *param);

i64 rose_set_param(rose_node *n, char *param_name, i64 param_name_length,
		   mirage_msg *param);

/// ---------------- Parameters ------------------------------------------------

void rose_log(rose_node *node,
	      int level,
	      const char *fmt,
	      ...);

// Convenience wrappers that use the last crated node.
void rose_info(const char *fmt, ...);
void rose_warn(const char *fmt, ...);
void rose_error(const char *fmt, ...);
void rose_fatal(const char *fmt, ...);


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

#endif // ROS3_H
