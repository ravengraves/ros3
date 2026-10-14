/*  ros3/_ros3.c - thin C wrapper for ros3 shared libraries
 *
 *  Purpose
 *  --------
 *  - Load symbols from platform-appropriate shared libraries at import time
 *  - Expose those functions to Python with proper argument/return conversion
 *  - Provide low-level primitives for the high-level Python API
 *
 *  Author:  Alec Graves
 *  Licence: AGPLv3
 */

#define _DARWIN_SOURCE
#define _GNU_SOURCE
#define PY_SSIZE_T_CLEAN

#include <Python.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HMODULE dynlib_handle;
#define DYNLIB_FLAG_NOW 0
#define DYNLIB_FLAG_GLOBAL 0

static const char *dynlib_error_string(void) {
	static char buf[256];
	DWORD err = GetLastError();
	if(err == 0) {
		return "unknown error";
	}
	DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM
						 | FORMAT_MESSAGE_IGNORE_INSERTS,
				 NULL,
				 err,
				 0,
				 buf,
				 (DWORD) sizeof(buf),
				 NULL);
	if(n == 0) {
		snprintf(buf, sizeof(buf), "Windows error %lu",
			 (unsigned long) err);
	} else {
		while(n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
			buf[n - 1] = '\0';
			n -= 1;
		}
	}
	return buf;
}

static dynlib_handle dynlib_open(const char *name, int flags) {
	(void) flags;
	SetLastError(0);
	return LoadLibraryA(name);
}

static void *dynlib_symbol(dynlib_handle handle, const char *sym) {
	SetLastError(0);
	return (void *) GetProcAddress(handle, sym);
}
#else
#include <dlfcn.h>
typedef void *dynlib_handle;
#define DYNLIB_FLAG_NOW RTLD_NOW
#define DYNLIB_FLAG_GLOBAL RTLD_GLOBAL

static const char *dynlib_error_string(void) {
	const char *err = dlerror();
	return err ? err : "unknown error";
}

static dynlib_handle dynlib_open(const char *name, int flags) {
	return dlopen(name, flags);
}

static void *dynlib_symbol(dynlib_handle handle, const char *sym) {
	return dlsym(handle, sym);
}
#endif

#include "ros3.h"

/* ---------------- 1.  symbol pointers -------------------------------------- */

static dynlib_handle lib_handle = NULL;

/* Node management */
static rose_node *(*p_rose_init)(int, char **, char *,
				 void *(*) (size_t), void (*)(void *)) = NULL;
static rose_node *(*p_rose_create_node)(char *, int64_t, char *, int64_t,
					char *, uint64_t *, uint64_t,
					void *(*) (size_t), void (*)(void *)) = NULL;
static void (*p_rose_shutdown)(rose_node *) = NULL;
static int64_t (*p_rose_ok)(rose_node *) = NULL;
static const char *(*p_rose_last_error)(void) = NULL;
static void (*p_rose_log)(rose_node *, int, const char *, int64_t) = NULL;

/* Publisher/Subscriber */
static rose_publisher *(*p_rose_create_pub)(rose_node *, char *, int64_t,
					    uint64_t, uint64_t) = NULL;
static rose_subscriber *(*p_rose_create_sub)(rose_node *, char *, int64_t,
					     uint64_t, void (*)(mirage_msg *)) = NULL;
static void (*p_rose_delete_subscriber)(rose_node *, rose_subscriber *) = NULL;
static int64_t (*p_rose_publish)(rose_publisher *, rosemsg *) = NULL;
static int64_t (*p_rose_read)(rose_subscriber *, rosemsg *) = NULL;
static int64_t (*p_rose_readlast)(rose_subscriber *, rosemsg *) = NULL;
static uint64_t (*p_rose_remaining)(rose_subscriber *) = NULL;
static void (*p_rose_spin_once)(rose_node *) = NULL;
static void (*p_rose_spin_forever)(rose_node *) = NULL;

/* Timing */
static uint64_t (*p_rose_now)(void) = NULL;
static void (*p_rose_timer_calibrate_seconds)(double) = NULL;
static double (*p_rose_seconds)(uint64_t, uint64_t) = NULL;
static double (*p_rose_nanoseconds)(uint64_t, uint64_t) = NULL;
static double (*p_rose_elapsed_seconds)(uint64_t) = NULL;
static double (*p_rose_elapsed_nanoseconds)(uint64_t) = NULL;

/* Sleep */
static void (*p_rose_sleep)(double) = NULL;
static void (*p_rose_nanosleep)(double) = NULL;
static void (*p_rose_sleep_rate)(double) = NULL;

/* Topic naming */
static char *(*p_rose_topic_to_shm)(char *, size_t, const char *) = NULL;

/* Mirage functions (for message handling) */
static mirage_msg *(*p_mirage_create)(int64_t, void *(*) (size_t) ) = NULL;
static void (*p_mirage_destroy)(mirage_msg **, void (*)(void *)) = NULL;

/* ------------------------ 2.  load a single symbol ------------------------- */

#if defined(_WIN32)
static const char *const MIRAGE_LIBRARY_CANDIDATES[] = {
		"mirage.dll",
		"libmirage.dll",
};
static const char *const QUICKSAND_LIBRARY_CANDIDATES[] = {
		"quicksand.dll",
		"libquicksand.dll",
};
static const char *const ROS3_LIBRARY_CANDIDATES[] = {
		"ros3.dll",
		"libros3.dll",
};
#elif defined(__APPLE__)
static const char *const MIRAGE_LIBRARY_CANDIDATES[] = {
		"libmirage.dylib",
		"libmirage.so",
};
static const char *const QUICKSAND_LIBRARY_CANDIDATES[] = {
		"libquicksand.dylib",
		"libquicksand.so",
};
static const char *const ROS3_LIBRARY_CANDIDATES[] = {
		"libros3.dylib",
		"libros3.so",
};
#else
static const char *const MIRAGE_LIBRARY_CANDIDATES[] = {
		"libmirage.so",
		"libmirage.dylib",
};
static const char *const QUICKSAND_LIBRARY_CANDIDATES[] = {
		"libquicksand.so",
		"libquicksand.dylib",
};
static const char *const ROS3_LIBRARY_CANDIDATES[] = {
		"libros3.so",
		"libros3.dylib",
};
#endif

static dynlib_handle
load_library_candidates(const char *logical_name, const char *const *candidates,
			size_t candidate_count, int flags) {
	char attempts[1024] = {0};
	for(size_t i = 0; i < candidate_count; ++i) {
		dynlib_handle handle = dynlib_open(candidates[i], flags);
		if(handle) {
			return handle;
		}

		const char *err = dynlib_error_string();
		size_t used = strlen(attempts);
		if(used < sizeof(attempts) - 1) {
			snprintf(attempts + used, sizeof(attempts) - used, "%s%s (%s)",
				 used ? "; " : "", candidates[i], err);
		}
	}

	PyErr_Format(PyExc_RuntimeError,
		     "Failed to load shared library for %s. Tried: %s",
		     logical_name,
		     attempts[0] ? attempts : "(none)");
	return NULL;
}

static int
load_symbol(dynlib_handle handle, const char *sym, void **ptr) {
	void *addr = dynlib_symbol(handle, sym);
	if(!addr) {
		PyErr_Format(PyExc_RuntimeError,
			     "Failed to load symbol %s: %s",
			     sym, dynlib_error_string());
		return -1;
	}
	*ptr = addr;
	return 0;
}

/* ----------- 3.  Create PyCapsules for opaque pointers --------------------- */

/* All node operations, including destruction, share this lock. Waiting for it
 * releases the GIL, so an in-flight operation can finish and reacquire the GIL.
 * Endpoints retain the owning node capsule, rather than a dangling C pointer. */
static PyThread_type_lock native_lock;
typedef struct {
	void *pointer;
	PyObject *owner;
} native_handle;

static void native_enter(void) {
	Py_BEGIN_ALLOW_THREADS;
	PyThread_acquire_lock(native_lock, WAIT_LOCK);
	Py_END_ALLOW_THREADS;
}
static void native_leave(void) { PyThread_release_lock(native_lock); }

static native_handle *get_handle(PyObject *capsule, const char *name) {
	if(!PyCapsule_IsValid(capsule, name)) {
		PyErr_Format(PyExc_TypeError, "expected a %s capsule", name);
		return NULL;
	}
	return PyCapsule_GetPointer(capsule, name);
}
static void handle_destructor(PyObject *capsule) {
	const char *name = PyCapsule_GetName(capsule);
	native_handle *handle = PyCapsule_GetPointer(capsule, name);
	if(!handle) {
		PyErr_Clear();
		return;
	}
	native_enter();
	if(!handle->owner && handle->pointer) {
		Py_BEGIN_ALLOW_THREADS;
		p_rose_shutdown(handle->pointer);
		Py_END_ALLOW_THREADS;
	} else if(handle->owner && handle->pointer && strcmp(name, "rose_subscriber") == 0) {
		native_handle *parent = PyCapsule_GetPointer(handle->owner, "rose_node");
		if(parent->pointer) {
			p_rose_delete_subscriber(parent->pointer, handle->pointer);
		}
	}
	native_leave();
	Py_XDECREF(handle->owner);
	PyMem_Free(handle);
}
static PyObject *new_handle(void *pointer, const char *name, PyObject *owner) {
	native_handle *handle = PyMem_Calloc(1, sizeof(*handle));
	if(!handle) {
		return PyErr_NoMemory();
	}
	handle->pointer = pointer;
	Py_XINCREF(owner);
	handle->owner = owner;
	PyObject *capsule = PyCapsule_New(handle, name, handle_destructor);
	if(!capsule) {
		Py_XDECREF(handle->owner);
		PyMem_Free(handle);
	}
	return capsule;
}
/* Dispose an unpublished capsule while dispatch already owns native_lock. */
static void discard_handle(PyObject *capsule) {
	native_handle *handle = PyCapsule_GetPointer(capsule, PyCapsule_GetName(capsule));
	PyCapsule_SetDestructor(capsule, NULL);
	Py_XDECREF(handle->owner);
	PyMem_Free(handle);
	Py_DECREF(capsule);
}
static void *live_pointer(PyObject *capsule, const char *name) {
	native_handle *handle = get_handle(capsule, name);
	if(!handle) {
		return NULL;
	}
	native_handle *parent = handle->owner ? get_handle(handle->owner, "rose_node") : handle;
	if(!parent || !parent->pointer || !handle->pointer) {
		PyErr_SetString(PyExc_RuntimeError, "ROS3 handle is closed");
		return NULL;
	}
	return handle->pointer;
}
static rose_node *node_from_capsule(PyObject *capsule) {
	return live_pointer(capsule, "rose_node");
}
static rose_publisher *pub_from_capsule(PyObject *capsule) {
	return live_pointer(capsule, "rose_publisher");
}
static rose_subscriber *sub_from_capsule(PyObject *capsule) {
	return live_pointer(capsule, "rose_subscriber");
}

static void
msg_capsule_destructor(PyObject *capsule) {
	mirage_msg *msg = (mirage_msg *) PyCapsule_GetPointer(capsule, "mirage_msg");
	if(msg) {
		p_mirage_destroy(&msg, NULL);
	}
}

static PyObject *
capsule_from_msg(mirage_msg *msg) {
	return PyCapsule_New((void *) msg, "mirage_msg", msg_capsule_destructor);
}

static mirage_msg *
msg_from_capsule(PyObject *capsule) {
	if(!PyCapsule_CheckExact(capsule)) {
		PyErr_SetString(PyExc_TypeError, "expected a mirage_msg capsule");
		return NULL;
	}
	return (mirage_msg *) PyCapsule_GetPointer(capsule, "mirage_msg");
}

/* -------------------- 4.  Python exposed API ------------------------------- */

/* Node management */

static PyObject *
py_rose_create_node(PyObject *self, PyObject *args, PyObject *kwds) {
	static const char *kwlist[] = {"name", "prefix", NULL};
	const char *name;
	const char *prefix = NULL;
	Py_ssize_t name_len, prefix_len = 0;

	if(!PyArg_ParseTupleAndKeywords(args, kwds, "s#|s#", (char **) kwlist,
					&name, &name_len, &prefix, &prefix_len)) {
		return NULL;
	}

	/* Empty prefix selects the root namespace within the captured ROSE_BASE.
	   Nonempty names and prefixes use exact spans, including NUL rejection. */
	char *actual_prefix = NULL;
	int64_t actual_prefix_len = -1;

	if(prefix && prefix_len > 0) {
		actual_prefix = (char *) prefix;
		actual_prefix_len = (int64_t) prefix_len;
	}

	rose_node *node = p_rose_create_node((char *) name, (int64_t) name_len,
					     actual_prefix, actual_prefix_len,
					     NULL, NULL, 0,
					     NULL, NULL);
	if(!node) {
		const char *detail = p_rose_last_error ? p_rose_last_error() : NULL;
		PyErr_SetString(PyExc_RuntimeError,
				(detail && detail[0]) ? detail : "Failed to create rose node");
		return NULL;
	}

	/* Return capsule and namespace-aware registry name, including PID suffix. */
	PyObject *name_obj = PyUnicode_FromStringAndSize(node->name, node->name_length);
	PyObject *capsule = name_obj ? new_handle(node, "rose_node", NULL) : NULL;
	if(!capsule) {
		Py_XDECREF(name_obj);
		p_rose_shutdown(node);
		return NULL;
	}
	PyObject *result = PyTuple_Pack(2, capsule, name_obj);
	if(!result) {
		p_rose_shutdown(node);
		discard_handle(capsule);
	} else {
		Py_DECREF(capsule);
	}
	Py_DECREF(name_obj);
	return result;
}

static PyObject *
py_rose_shutdown(PyObject *self, PyObject *arg) {
	native_handle *handle = get_handle(arg, "rose_node");
	if(!handle) {
		return NULL;
	}
	rose_node *node = handle->pointer;
	if(!node) {
		Py_RETURN_NONE;
	}
	handle->pointer = NULL;
	Py_BEGIN_ALLOW_THREADS;
	p_rose_shutdown(node);
	Py_END_ALLOW_THREADS;

	Py_RETURN_NONE;
}

static PyObject *
py_rose_ok(PyObject *self, PyObject *arg) {
	rose_node *node = node_from_capsule(arg);
	if(!node) {
		return NULL;
	}

	int64_t result;
	Py_BEGIN_ALLOW_THREADS;
	result = p_rose_ok(node);
	Py_END_ALLOW_THREADS;

	return PyBool_FromLong(result);
}

static PyObject *
py_rose_log(PyObject *self, PyObject *args) {
	PyObject *node_capsule, *data_obj;
	int level;
	Py_buffer view;

	if(!PyArg_ParseTuple(args, "OiO", &node_capsule, &level, &data_obj)) {
		return NULL;
	}

	rose_node *node = node_from_capsule(node_capsule);
	if(!node) {
		return NULL;
	}

	if(PyObject_GetBuffer(data_obj, &view, PyBUF_SIMPLE) < 0) {
		return NULL;
	}

	Py_BEGIN_ALLOW_THREADS;
	p_rose_log(node, level, (const char *) view.buf, (int64_t) view.len);
	Py_END_ALLOW_THREADS;

	PyBuffer_Release(&view);

	Py_RETURN_NONE;
}

/* Publisher/Subscriber */

static PyObject *
py_rose_create_pub(PyObject *self, PyObject *args) {
	PyObject *node_capsule;
	const char *topic;
	Py_ssize_t topic_len;
	uint64_t message_size, message_hz;

	if(!PyArg_ParseTuple(args, "Os#KK", &node_capsule, &topic, &topic_len,
			     &message_size, &message_hz)) {
		return NULL;
	}

	rose_node *node = node_from_capsule(node_capsule);
	if(!node) {
		return NULL;
	}

	/* Allocate the Python owner before registering native resources. */
	PyObject *capsule = new_handle(NULL, "rose_publisher", node_capsule);
	if(!capsule) {
		return NULL;
	}
	rose_publisher *pub = p_rose_create_pub(node, (char *) topic, (int64_t) topic_len,
						message_size, message_hz);
	if(!pub) {
		const char *detail = p_rose_last_error ? p_rose_last_error() : NULL;
		PyErr_SetString(PyExc_RuntimeError,
				(detail && detail[0]) ? detail : "Failed to create publisher");
		discard_handle(capsule);
		return NULL;
	}

	((native_handle *) PyCapsule_GetPointer(capsule, "rose_publisher"))->pointer = pub;
	return capsule;
}

static PyObject *
py_rose_create_sub(PyObject *self, PyObject *args) {
	PyObject *node_capsule;
	const char *topic;
	Py_ssize_t topic_len;
	uint64_t latest_only;

	if(!PyArg_ParseTuple(args, "Os#K", &node_capsule, &topic, &topic_len,
			     &latest_only)) {
		return NULL;
	}

	rose_node *node = node_from_capsule(node_capsule);
	if(!node) {
		return NULL;
	}

	PyObject *capsule = new_handle(NULL, "rose_subscriber", node_capsule);
	if(!capsule) {
		return NULL;
	}
	/* Note: callback is NULL - we use polling from Python */
	rose_subscriber *sub = p_rose_create_sub(node, (char *) topic, (int64_t) topic_len,
						 latest_only, NULL);
	if(!sub) {
		const char *detail = p_rose_last_error ? p_rose_last_error() : NULL;
		PyErr_SetString(PyExc_RuntimeError,
				(detail && detail[0]) ? detail : "Failed to create subscriber");
		discard_handle(capsule);
		return NULL;
	}

	/* Also return the message_size from the subscriber's connection */
	uint64_t msg_size = sub->sub->buffer->message_size;

	((native_handle *) PyCapsule_GetPointer(capsule, "rose_subscriber"))->pointer = sub;
	PyObject *result = Py_BuildValue("(OK)", capsule, msg_size);
	if(!result) {
		p_rose_delete_subscriber(node, sub);
		discard_handle(capsule);
	} else {
		Py_DECREF(capsule);
	}
	return result;
}

static PyObject *
py_rose_delete_subscriber(PyObject *self, PyObject *args) {
	PyObject *node_capsule, *sub_capsule;

	if(!PyArg_ParseTuple(args, "OO", &node_capsule, &sub_capsule)) {
		return NULL;
	}

	rose_node *node = node_from_capsule(node_capsule);
	if(!node) {
		return NULL;
	}

	rose_subscriber *sub = sub_from_capsule(sub_capsule);
	if(!sub) {
		return NULL;
	}

	native_handle *handle = get_handle(sub_capsule, "rose_subscriber");
	if(handle->owner != node_capsule) {
		PyErr_SetString(PyExc_ValueError, "subscriber belongs to a different node");
		return NULL;
	}
	handle->pointer = NULL;
	p_rose_delete_subscriber(node, sub);

	Py_RETURN_NONE;
}

static PyObject *
py_rose_publish(PyObject *self, PyObject *args) {
	PyObject *pub_capsule, *data_obj;
	Py_buffer view;

	if(!PyArg_ParseTuple(args, "OO", &pub_capsule, &data_obj)) {
		return NULL;
	}

	rose_publisher *pub = pub_from_capsule(pub_capsule);
	if(!pub) {
		return NULL;
	}

	if(PyObject_GetBuffer(data_obj, &view, PyBUF_SIMPLE) < 0) {
		return NULL;
	}

	/* Create a temporary mirage_msg to hold the data */
	mirage_msg msg;
	msg.data = (uint8_t *) view.buf;
	msg.length = (int64_t) view.len;
	msg.cursor = (int64_t) view.len;
	msg.max_length = (int64_t) view.len;

	int64_t rc;
	Py_BEGIN_ALLOW_THREADS;
	rc = p_rose_publish(pub, &msg);
	Py_END_ALLOW_THREADS;

	PyBuffer_Release(&view);

	if(rc < 0) {
		const char *detail = p_rose_last_error ? p_rose_last_error() : NULL;
		if(detail && detail[0]) {
			PyErr_SetString(PyExc_RuntimeError, detail);
		} else {
			PyErr_Format(PyExc_RuntimeError, "rose_publish failed: %lld", (long long) rc);
		}
		return NULL;
	}

	Py_RETURN_NONE;
}

static PyObject *
py_rose_read(PyObject *self, PyObject *args) {
	PyObject *sub_capsule, *buf_obj;
	Py_buffer view;

	if(!PyArg_ParseTuple(args, "OO", &sub_capsule, &buf_obj)) {
		return NULL;
	}

	rose_subscriber *sub = sub_from_capsule(sub_capsule);
	if(!sub) {
		return NULL;
	}

	if(PyObject_GetBuffer(buf_obj, &view, PyBUF_WRITABLE | PyBUF_SIMPLE) < 0) {
		return NULL;
	}

	/* Create a temporary mirage_msg to receive data */
	mirage_msg msg;
	msg.data = (uint8_t *) view.buf;
	msg.length = 0;
	msg.cursor = 0;
	msg.max_length = (int64_t) view.len;

	int64_t rc;
	Py_BEGIN_ALLOW_THREADS;
	rc = sub->latest_only ? p_rose_readlast(sub, &msg) : p_rose_read(sub, &msg);
	Py_END_ALLOW_THREADS;

	PyBuffer_Release(&view);

	if(rc == ROSE_NO_DATA) {
		Py_RETURN_NONE;
	}
	if(rc < 0) {
		const char *detail = p_rose_last_error ? p_rose_last_error() : NULL;
		if(detail && detail[0]) {
			PyErr_SetString(PyExc_RuntimeError, detail);
		} else {
			PyErr_Format(PyExc_RuntimeError, "rose_read failed: %lld", (long long) rc);
		}
		return NULL;
	}

	/* Return (data_length, remaining_messages) */
	return Py_BuildValue("(LL)", (long long) msg.length, (long long) rc);
}

static PyObject *
py_rose_readlast(PyObject *self, PyObject *args) {
	PyObject *sub_capsule, *buf_obj;
	Py_buffer view;

	if(!PyArg_ParseTuple(args, "OO", &sub_capsule, &buf_obj)) {
		return NULL;
	}

	rose_subscriber *sub = sub_from_capsule(sub_capsule);
	if(!sub) {
		return NULL;
	}

	if(PyObject_GetBuffer(buf_obj, &view, PyBUF_WRITABLE | PyBUF_SIMPLE) < 0) {
		return NULL;
	}

	/* Create a temporary mirage_msg to receive data */
	mirage_msg msg;
	msg.data = (uint8_t *) view.buf;
	msg.length = 0;
	msg.cursor = 0;
	msg.max_length = (int64_t) view.len;

	int64_t rc;
	Py_BEGIN_ALLOW_THREADS;
	rc = p_rose_readlast(sub, &msg);
	Py_END_ALLOW_THREADS;

	PyBuffer_Release(&view);

	if(rc == ROSE_NO_DATA) {
		Py_RETURN_NONE;
	}
	if(rc < 0) {
		const char *detail = p_rose_last_error ? p_rose_last_error() : NULL;
		if(detail && detail[0]) {
			PyErr_SetString(PyExc_RuntimeError, detail);
		} else {
			PyErr_Format(PyExc_RuntimeError, "rose_readlast failed: %lld", (long long) rc);
		}
		return NULL;
	}

	/* Return data_length */
	return PyLong_FromLongLong(msg.length);
}

static PyObject *
py_rose_remaining(PyObject *self, PyObject *arg) {
	rose_subscriber *sub = sub_from_capsule(arg);
	if(!sub) {
		return NULL;
	}

	return PyLong_FromUnsignedLongLong(p_rose_remaining(sub));
}

static PyObject *
py_rose_spin_once(PyObject *self, PyObject *arg) {
	rose_node *node = node_from_capsule(arg);
	if(!node) {
		return NULL;
	}

	Py_BEGIN_ALLOW_THREADS;
	p_rose_spin_once(node);
	Py_END_ALLOW_THREADS;

	Py_RETURN_NONE;
}

/* Timing functions */

static PyObject *
py_rose_now(PyObject *self, PyObject *Py_UNUSED(ignored)) {
	return PyLong_FromUnsignedLongLong(p_rose_now());
}

static PyObject *
py_rose_timer_calibrate(PyObject *self, PyObject *args) {
	double seconds;
	if(!PyArg_ParseTuple(args, "d", &seconds)) {
		return NULL;
	}

	Py_BEGIN_ALLOW_THREADS;
	p_rose_timer_calibrate_seconds(seconds);
	Py_END_ALLOW_THREADS;

	Py_RETURN_NONE;
}

static PyObject *
py_rose_seconds(PyObject *self, PyObject *args) {
	uint64_t stop, start;
	if(!PyArg_ParseTuple(args, "KK", &stop, &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_rose_seconds(stop, start));
}

static PyObject *
py_rose_nanoseconds(PyObject *self, PyObject *args) {
	uint64_t stop, start;
	if(!PyArg_ParseTuple(args, "KK", &stop, &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_rose_nanoseconds(stop, start));
}

static PyObject *
py_rose_elapsed_seconds(PyObject *self, PyObject *args) {
	uint64_t start;
	if(!PyArg_ParseTuple(args, "K", &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_rose_elapsed_seconds(start));
}

static PyObject *
py_rose_elapsed_nanoseconds(PyObject *self, PyObject *args) {
	uint64_t start;
	if(!PyArg_ParseTuple(args, "K", &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_rose_elapsed_nanoseconds(start));
}

/* Sleep functions */

static PyObject *
py_rose_sleep(PyObject *self, PyObject *args) {
	double seconds;
	if(!PyArg_ParseTuple(args, "d", &seconds)) {
		return NULL;
	}

	Py_BEGIN_ALLOW_THREADS;
	p_rose_sleep(seconds);
	Py_END_ALLOW_THREADS;

	Py_RETURN_NONE;
}

static PyObject *
py_rose_nanosleep(PyObject *self, PyObject *args) {
	double nanoseconds;
	if(!PyArg_ParseTuple(args, "d", &nanoseconds)) {
		return NULL;
	}

	Py_BEGIN_ALLOW_THREADS;
	p_rose_nanosleep(nanoseconds);
	Py_END_ALLOW_THREADS;

	Py_RETURN_NONE;
}

static PyObject *
py_rose_sleep_rate(PyObject *self, PyObject *args) {
	double hz;
	if(!PyArg_ParseTuple(args, "d", &hz)) {
		return NULL;
	}

	Py_BEGIN_ALLOW_THREADS;
	p_rose_sleep_rate(hz);
	Py_END_ALLOW_THREADS;

	Py_RETURN_NONE;
}

/* Topic naming */

static PyObject *
py_rose_topic_to_shm(PyObject *self, PyObject *args) {
	const char *topic;
	if(!PyArg_ParseTuple(args, "s", &topic)) {
		return NULL;
	}

	char shm[256];
	if(!p_rose_topic_to_shm(shm, sizeof(shm), topic)) {
		PyErr_SetString(PyExc_ValueError, p_rose_last_error());
		return NULL;
	}

	return PyUnicode_FromString(shm);
}

/* -------------------- 5.  Module method table ------------------------------ */

/* Dispatch retains Python arguments and serializes validation plus native I/O.
 * The native functions may release the GIL while this lock remains held. */
#define SERIAL_WRAPPER(name)                                            \
	static PyObject *locked_##name(PyObject *self, PyObject *arg) { \
		native_enter();                                         \
		PyObject *result = name(self, arg);                     \
		native_leave();                                         \
		return result;                                          \
	}
SERIAL_WRAPPER(py_rose_shutdown)
SERIAL_WRAPPER(py_rose_ok)
SERIAL_WRAPPER(py_rose_log)
SERIAL_WRAPPER(py_rose_create_pub)
SERIAL_WRAPPER(py_rose_create_sub)
SERIAL_WRAPPER(py_rose_delete_subscriber)
SERIAL_WRAPPER(py_rose_publish)
SERIAL_WRAPPER(py_rose_read)
SERIAL_WRAPPER(py_rose_readlast)
SERIAL_WRAPPER(py_rose_remaining)
SERIAL_WRAPPER(py_rose_spin_once)
static PyObject *locked_create_node(PyObject *self, PyObject *args, PyObject *kwds) {
	native_enter();
	PyObject *result = py_rose_create_node(self, args, kwds);
	native_leave();
	return result;
}

static PyMethodDef Ros3Methods[] = {
		/* Node management */
		{"create_node", (PyCFunction) locked_create_node, METH_VARARGS | METH_KEYWORDS,
		 "Create a ROS3 node."},
		{"shutdown", locked_py_rose_shutdown, METH_O, "Shutdown a node."},
		{"ok", locked_py_rose_ok, METH_O, "Check if node is OK and send heartbeat."},
		{"log", locked_py_rose_log, METH_VARARGS, "Log a byte-counted message."},

		/* Publisher/Subscriber */
		{"create_pub", locked_py_rose_create_pub, METH_VARARGS, "Create a publisher."},
		{"create_sub", locked_py_rose_create_sub, METH_VARARGS, "Create a subscriber."},
		{"delete_sub", locked_py_rose_delete_subscriber, METH_VARARGS, "Delete a subscriber."},
		{"publish", locked_py_rose_publish, METH_VARARGS, "Publish a message."},
		{"read", locked_py_rose_read, METH_VARARGS, "Read a message from subscriber."},
		{"readlast", locked_py_rose_readlast, METH_VARARGS, "Read latest message from subscriber."},
		{"remaining", locked_py_rose_remaining, METH_O, "Get remaining message count."},
		{"spin_once", locked_py_rose_spin_once, METH_O, "Process callbacks once."},

		/* Timing */
		{"now", py_rose_now, METH_NOARGS, "Get current monotonic timestamp."},
		{"timer_calibrate", py_rose_timer_calibrate, METH_VARARGS, "Calibrate timer."},
		{"seconds", py_rose_seconds, METH_VARARGS, "Convert timestamps to seconds."},
		{"nanoseconds", py_rose_nanoseconds, METH_VARARGS, "Convert timestamps to nanoseconds."},
		{"elapsed_seconds", py_rose_elapsed_seconds, METH_VARARGS, "Get elapsed seconds from start."},
		{"elapsed_nanoseconds", py_rose_elapsed_nanoseconds, METH_VARARGS, "Get elapsed nanoseconds from start."},

		/* Sleep */
		{"sleep", py_rose_sleep, METH_VARARGS, "Sleep for seconds."},
		{"nanosleep", py_rose_nanosleep, METH_VARARGS, "Sleep for nanoseconds."},
		{"sleep_rate", py_rose_sleep_rate, METH_VARARGS, "Sleep to maintain rate (Hz)."},

		/* Topic naming */
		{"topic_to_shm", py_rose_topic_to_shm, METH_VARARGS, "Convert topic to shared memory name."},

		{NULL, NULL, 0, NULL}};

/* -------------------------- 6.  Module definition -------------------------- */

static struct PyModuleDef ros3module = {
		PyModuleDef_HEAD_INIT,
		"ros3._ros3",
		"Thin C wrapper for ros3 shared library",
		-1,
		Ros3Methods,
		NULL,
		NULL,
		NULL,
		NULL};

/* ----------------------------- 7.  Module init ----------------------------- */

PyMODINIT_FUNC
PyInit__ros3(void) {
	if(!native_lock) {
		native_lock = PyThread_allocate_lock();
	}
	if(!native_lock) {
		return PyErr_NoMemory();
	}
	PyObject *m = PyModule_Create(&ros3module);
	if(!m) {
		return NULL;
	}

	/*
	 * IMPORTANT: Load dependency libraries with global symbol visibility
	 * so that when ros3 calls quicksand/mirage functions internally,
	 * those symbols are visible.
	 * Load in dependency order: mirage, quicksand, then ros3.
	 */

	/* Load mirage first (no dependencies) */
	dynlib_handle mirage_handle =
			load_library_candidates("mirage",
						MIRAGE_LIBRARY_CANDIDATES,
						sizeof(MIRAGE_LIBRARY_CANDIDATES) / sizeof(MIRAGE_LIBRARY_CANDIDATES[0]),
						DYNLIB_FLAG_NOW | DYNLIB_FLAG_GLOBAL);
	if(!mirage_handle) {
		return NULL;
	}

	/* Load quicksand second (depends on mirage) */
	dynlib_handle quicksand_handle =
			load_library_candidates("quicksand",
						QUICKSAND_LIBRARY_CANDIDATES,
						sizeof(QUICKSAND_LIBRARY_CANDIDATES) / sizeof(QUICKSAND_LIBRARY_CANDIDATES[0]),
						DYNLIB_FLAG_NOW | DYNLIB_FLAG_GLOBAL);
	if(!quicksand_handle) {
		return NULL;
	}
	(void) quicksand_handle; /* We just need it loaded globally */

	/* Load ros3 last (depends on mirage and quicksand) */
	lib_handle = load_library_candidates("ros3",
					     ROS3_LIBRARY_CANDIDATES,
					     sizeof(ROS3_LIBRARY_CANDIDATES) / sizeof(ROS3_LIBRARY_CANDIDATES[0]),
					     DYNLIB_FLAG_NOW | DYNLIB_FLAG_GLOBAL);
	if(!lib_handle) {
		return NULL;
	}

	/* Load all ros3 symbols */
	if(load_symbol(lib_handle, "rose_create_node", (void **) &p_rose_create_node) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_shutdown", (void **) &p_rose_shutdown) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_ok", (void **) &p_rose_ok) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_last_error", (void **) &p_rose_last_error) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_log", (void **) &p_rose_log) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_create_pub", (void **) &p_rose_create_pub) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_create_sub", (void **) &p_rose_create_sub) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_delete_subscriber", (void **) &p_rose_delete_subscriber) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_publish", (void **) &p_rose_publish) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_read", (void **) &p_rose_read) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_readlast", (void **) &p_rose_readlast) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_remaining", (void **) &p_rose_remaining) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_spin_once", (void **) &p_rose_spin_once) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_spin_forever", (void **) &p_rose_spin_forever) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_now", (void **) &p_rose_now) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_timer_calibrate_seconds", (void **) &p_rose_timer_calibrate_seconds) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_seconds", (void **) &p_rose_seconds) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_nanoseconds", (void **) &p_rose_nanoseconds) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_elapsed_seconds", (void **) &p_rose_elapsed_seconds) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_elapsed_nanoseconds", (void **) &p_rose_elapsed_nanoseconds) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_sleep", (void **) &p_rose_sleep) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_nanosleep", (void **) &p_rose_nanosleep) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_sleep_rate", (void **) &p_rose_sleep_rate) < 0) {
		return NULL;
	}
	if(load_symbol(lib_handle, "rose_topic_to_shm", (void **) &p_rose_topic_to_shm) < 0) {
		return NULL;
	}

	/* Load mirage symbols */
	if(load_symbol(mirage_handle, "mirage_create", (void **) &p_mirage_create) < 0) {
		return NULL;
	}
	if(load_symbol(mirage_handle, "mirage_destroy", (void **) &p_mirage_destroy) < 0) {
		return NULL;
	}

	/* Add constants */
	PyModule_AddIntConstant(m, "ROSE_OK", 0);
	PyModule_AddIntConstant(m, "ROSE_NO_DATA", -1);
	PyModule_AddIntConstant(m, "ROSE_ERR_BADDATA", -2);
	PyModule_AddIntConstant(m, "ROSE_ERR_BUFFERSIZE", -4);
	PyModule_AddIntConstant(m, "ROSE_ERR_BADINPUT", -8);
	PyModule_AddIntConstant(m, "ROSE_LOG_INFO", ROSE_LOG_INFO);
	PyModule_AddIntConstant(m, "ROSE_LOG_WARN", ROSE_LOG_WARN);
	PyModule_AddIntConstant(m, "ROSE_LOG_ERROR", ROSE_LOG_ERROR);
	PyModule_AddIntConstant(m, "ROSE_LOG_FATAL", ROSE_LOG_FATAL);

	return m;
}
