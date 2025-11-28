#ifndef ROS3_H
#define ROS3_H

#include <quicksand.h>
#include <quicksand_style.h>
#include <mirage.h>

typedef mirage_msg rosemsg;

typedef struct rose_node;
typedef struct rose_publisher;
typedef struct rose_subscriber;

typedef struct {
	char *name;
	u64 name_length;
	char *prefix;
	u64 prefix_length;
	char *remaps;
	u64 *remap_strlens;
	u64 remap_count;
	rose_publisher *pubs;
	u64 pubs_length;
	rose_subscriber *subs;
	u64 subs_length;
	rose_publisher rosemaster_pub;
	rose_subscriber control_sub; 
	rose_publisher param_pub;
	rose_subscriber param_sub;
	rose_publisher log_pub;
	void *(*allocator)(size_t); // optional allocator
	void (*deallocator)(void *); // optional deallocator
} rose_node;

typedef struct {
	rose_node *node;
	quicksand_connection *pub;
} rose_publisher;

typedef struct {
	rose_node *node;
	quicksand_connection *sub;
	void (*callback)(u8 *); // optional callback function pointer
} rose_subscriber;

typedef enum {
	ROSE_OK = 0,
	ROSE_NO_DATA = -1,
	ROSE_ERR_BADDATA = -2,
	ROSE_ERR_BUFFERSIZE = -4,
	ROSE_ERR_BADINPUT = -8
} ROSE_RES;


/// ---------------- Initialization---------------------------------------------

rose_node *rose_init(char argc, char **argv, char* name,
	void *(*alloc)(size_t), void (*deallocator)(void *));

rose_node *rose_create_node(char* name, i64 name_length, 
	char* prefix, i64 prefix_len,
	char *remaps, i64* remap_string_lengths, u64 remap_count,
	void *(*alloc)(size_t), void (*deallocator)(void *));

/// ---------------- Pub / Sub -------------------------------------------------

rose_publisher *rose_create_pub(rose_node *n, char *name, i64 name_length,
		u64 message_length, u64 message_hz);

rose_subscriber *rose_create_sub(rose_node *n, char *name, i64 name_length,
		u64 latest_only, void (*callback)(u8 *));

i64 rose_publish(rose_publisher *p, rosemsg *msg);

i64 rose_read(rose_subscriber *s, rosemsg *msg);

i64 rose_readlast(rose_subscriber *s, rosemsg *msg);

void rose_spin_once(void);

void rose_spin_forever(void);

/// ---------------- Parameters ------------------------------------------------

i64 rose_get_param(rose_node *n, char* param_name, i64 param_name_length,
	mirage_msg *param);

i64 rose_set_param(rose_node *n, char* param_name, i64 param_name_length, 
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

void rose_timer_calibrate_seconds(double seconds);

double rose_seconds(u64 stop, u64 start);

double rose_nanoseconds(u64 stop, u64 start);

double rose_elapsed_seconds(u64 stop)

double rose_elapsed_nanoseconds(u64 start);

/// ---------------- Sleep -----------------------------------------------------

void rose_sleep(double seconds);

void rose_sleep_rate(double hz);

#endif // ROS3_H