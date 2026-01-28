/*  ros3/_ros3.c - thin C wrapper for libros3.so
 *
 *  Purpose
 *  --------
 *  - Load the symbols from libros3.so at import time
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
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ros3.h"

/* ---------------- 1.  symbol pointers -------------------------------------- */

static void *lib_handle = NULL;

/* Node management */
static rose_node *(*p_rose_init)(char, char **, char *,
                                  void *(*)(size_t), void (*)(void *)) = NULL;
static rose_node *(*p_rose_create_node)(char *, int64_t, char *, int64_t,
                                         char *, uint64_t *, uint64_t,
                                         void *(*)(size_t), void (*)(void *)) = NULL;
static void (*p_rose_shutdown)(rose_node *) = NULL;
static int64_t (*p_rose_ok)(rose_node *) = NULL;

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
static mirage_msg *(*p_mirage_create)(int64_t, void *(*)(size_t)) = NULL;
static void (*p_mirage_destroy)(mirage_msg **, void (*)(void *)) = NULL;

/* ------------------------ 2.  load a single symbol ------------------------- */

static int
load_symbol(void *handle, const char *sym, void **ptr) {
	void *addr = dlsym(handle, sym);
	if (!addr) {
		PyErr_Format(PyExc_RuntimeError,
		             "Failed to load symbol %s: %s",
		             sym, dlerror());
		return -1;
	}
	*ptr = addr;
	return 0;
}

/* ----------- 3.  Create PyCapsules for opaque pointers --------------------- */

static PyObject *
capsule_from_node(rose_node *node) {
	return PyCapsule_New((void *)node, "rose_node", NULL);
}

static rose_node *
node_from_capsule(PyObject *capsule) {
	if (!PyCapsule_CheckExact(capsule)) {
		PyErr_SetString(PyExc_TypeError, "expected a rose_node capsule");
		return NULL;
	}
	return (rose_node *)PyCapsule_GetPointer(capsule, "rose_node");
}

static PyObject *
capsule_from_pub(rose_publisher *pub) {
	return PyCapsule_New((void *)pub, "rose_publisher", NULL);
}

static rose_publisher *
pub_from_capsule(PyObject *capsule) {
	if (!PyCapsule_CheckExact(capsule)) {
		PyErr_SetString(PyExc_TypeError, "expected a rose_publisher capsule");
		return NULL;
	}
	return (rose_publisher *)PyCapsule_GetPointer(capsule, "rose_publisher");
}

static PyObject *
capsule_from_sub(rose_subscriber *sub) {
	return PyCapsule_New((void *)sub, "rose_subscriber", NULL);
}

static rose_subscriber *
sub_from_capsule(PyObject *capsule) {
	if (!PyCapsule_CheckExact(capsule)) {
		PyErr_SetString(PyExc_TypeError, "expected a rose_subscriber capsule");
		return NULL;
	}
	return (rose_subscriber *)PyCapsule_GetPointer(capsule, "rose_subscriber");
}

static void
msg_capsule_destructor(PyObject *capsule) {
	mirage_msg *msg = (mirage_msg *)PyCapsule_GetPointer(capsule, "mirage_msg");
	if (msg) {
		p_mirage_destroy(&msg, NULL);
	}
}

static PyObject *
capsule_from_msg(mirage_msg *msg) {
	return PyCapsule_New((void *)msg, "mirage_msg", msg_capsule_destructor);
}

static mirage_msg *
msg_from_capsule(PyObject *capsule) {
	if (!PyCapsule_CheckExact(capsule)) {
		PyErr_SetString(PyExc_TypeError, "expected a mirage_msg capsule");
		return NULL;
	}
	return (mirage_msg *)PyCapsule_GetPointer(capsule, "mirage_msg");
}

/* -------------------- 4.  Python exposed API ------------------------------- */

/* Node management */

static PyObject *
py_rose_create_node(PyObject *self, PyObject *args, PyObject *kwds) {
	static const char *kwlist[] = {"name", "prefix", NULL};
	const char *name;
	const char *prefix = NULL;
	Py_ssize_t name_len, prefix_len = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "s#|s#", (char **)kwlist,
	                                  &name, &name_len, &prefix, &prefix_len)) {
		return NULL;
	}

	/* If prefix is NULL or empty, pass NULL to use default /rose/ prefix
	   Otherwise pass the actual prefix with length -1 to use strlen */
	char *actual_prefix = NULL;
	int64_t actual_prefix_len = -1;

	if (prefix && prefix_len > 0) {
		actual_prefix = (char *)prefix;
		actual_prefix_len = (int64_t)prefix_len;
	}

	rose_node *node = p_rose_create_node((char *)name, (int64_t)name_len,
	                                      actual_prefix, actual_prefix_len,
	                                      NULL, NULL, 0,
	                                      NULL, NULL);
	if (!node) {
		PyErr_SetString(PyExc_RuntimeError, "Failed to create rose node");
		return NULL;
	}

	/* Return node capsule and the actual name (includes PID suffix) */
	return Py_BuildValue("(Ns#)", capsule_from_node(node),
	                     node->name, (Py_ssize_t)node->name_length);
}

static PyObject *
py_rose_shutdown(PyObject *self, PyObject *arg) {
	rose_node *node = node_from_capsule(arg);
	if (!node) return NULL;

	Py_BEGIN_ALLOW_THREADS;
	p_rose_shutdown(node);
	Py_END_ALLOW_THREADS;

	Py_RETURN_NONE;
}

static PyObject *
py_rose_ok(PyObject *self, PyObject *arg) {
	rose_node *node = node_from_capsule(arg);
	if (!node) return NULL;

	int64_t result;
	Py_BEGIN_ALLOW_THREADS;
	result = p_rose_ok(node);
	Py_END_ALLOW_THREADS;

	return PyBool_FromLong(result);
}

/* Publisher/Subscriber */

static PyObject *
py_rose_create_pub(PyObject *self, PyObject *args) {
	PyObject *node_capsule;
	const char *topic;
	Py_ssize_t topic_len;
	uint64_t message_size, message_hz;

	if (!PyArg_ParseTuple(args, "Os#KK", &node_capsule, &topic, &topic_len,
	                      &message_size, &message_hz)) {
		return NULL;
	}

	rose_node *node = node_from_capsule(node_capsule);
	if (!node) return NULL;

	rose_publisher *pub = p_rose_create_pub(node, (char *)topic, (int64_t)topic_len,
	                                         message_size, message_hz);
	if (!pub) {
		PyErr_SetString(PyExc_RuntimeError, "Failed to create publisher");
		return NULL;
	}

	return capsule_from_pub(pub);
}

static PyObject *
py_rose_create_sub(PyObject *self, PyObject *args) {
	PyObject *node_capsule;
	const char *topic;
	Py_ssize_t topic_len;
	uint64_t latest_only;

	if (!PyArg_ParseTuple(args, "Os#K", &node_capsule, &topic, &topic_len,
	                      &latest_only)) {
		return NULL;
	}

	rose_node *node = node_from_capsule(node_capsule);
	if (!node) return NULL;

	/* Note: callback is NULL - we use polling from Python */
	rose_subscriber *sub = p_rose_create_sub(node, (char *)topic, (int64_t)topic_len,
	                                          latest_only, NULL);
	if (!sub) {
		PyErr_SetString(PyExc_RuntimeError, "Failed to create subscriber");
		return NULL;
	}

	/* Also return the message_size from the subscriber's connection */
	uint64_t msg_size = sub->sub->buffer->message_size;

	return Py_BuildValue("(NK)", capsule_from_sub(sub), msg_size);
}

static PyObject *
py_rose_delete_subscriber(PyObject *self, PyObject *args) {
	PyObject *node_capsule, *sub_capsule;

	if (!PyArg_ParseTuple(args, "OO", &node_capsule, &sub_capsule)) {
		return NULL;
	}

	rose_node *node = node_from_capsule(node_capsule);
	if (!node) return NULL;

	rose_subscriber *sub = sub_from_capsule(sub_capsule);
	if (!sub) return NULL;

	p_rose_delete_subscriber(node, sub);

	Py_RETURN_NONE;
}

static PyObject *
py_rose_publish(PyObject *self, PyObject *args) {
	PyObject *pub_capsule, *data_obj;
	Py_buffer view;

	if (!PyArg_ParseTuple(args, "OO", &pub_capsule, &data_obj)) {
		return NULL;
	}

	rose_publisher *pub = pub_from_capsule(pub_capsule);
	if (!pub) return NULL;

	if (PyObject_GetBuffer(data_obj, &view, PyBUF_SIMPLE) < 0) {
		return NULL;
	}

	/* Create a temporary mirage_msg to hold the data */
	mirage_msg msg;
	msg.data = (uint8_t *)view.buf;
	msg.length = (int64_t)view.len;
	msg.cursor = (int64_t)view.len;
	msg.max_length = (int64_t)view.len;

	int64_t rc;
	Py_BEGIN_ALLOW_THREADS;
	rc = p_rose_publish(pub, &msg);
	Py_END_ALLOW_THREADS;

	PyBuffer_Release(&view);

	if (rc < 0) {
		PyErr_Format(PyExc_RuntimeError, "rose_publish failed: %lld", (long long)rc);
		return NULL;
	}

	Py_RETURN_NONE;
}

static PyObject *
py_rose_read(PyObject *self, PyObject *args) {
	PyObject *sub_capsule, *buf_obj;
	Py_buffer view;

	if (!PyArg_ParseTuple(args, "OO", &sub_capsule, &buf_obj)) {
		return NULL;
	}

	rose_subscriber *sub = sub_from_capsule(sub_capsule);
	if (!sub) return NULL;

	if (PyObject_GetBuffer(buf_obj, &view, PyBUF_WRITABLE | PyBUF_SIMPLE) < 0) {
		return NULL;
	}

	/* Create a temporary mirage_msg to receive data */
	mirage_msg msg;
	msg.data = (uint8_t *)view.buf;
	msg.length = 0;
	msg.cursor = 0;
	msg.max_length = (int64_t)view.len;

	int64_t rc;
	Py_BEGIN_ALLOW_THREADS;
	rc = p_rose_read(sub, &msg);
	Py_END_ALLOW_THREADS;

	PyBuffer_Release(&view);

	if (rc < 0) {
		/* No data available */
		Py_RETURN_NONE;
	}

	/* Return (data_length, remaining_messages) */
	return Py_BuildValue("(LL)", (long long)msg.length, (long long)rc);
}

static PyObject *
py_rose_readlast(PyObject *self, PyObject *args) {
	PyObject *sub_capsule, *buf_obj;
	Py_buffer view;

	if (!PyArg_ParseTuple(args, "OO", &sub_capsule, &buf_obj)) {
		return NULL;
	}

	rose_subscriber *sub = sub_from_capsule(sub_capsule);
	if (!sub) return NULL;

	if (PyObject_GetBuffer(buf_obj, &view, PyBUF_WRITABLE | PyBUF_SIMPLE) < 0) {
		return NULL;
	}

	/* Create a temporary mirage_msg to receive data */
	mirage_msg msg;
	msg.data = (uint8_t *)view.buf;
	msg.length = 0;
	msg.cursor = 0;
	msg.max_length = (int64_t)view.len;

	int64_t rc;
	Py_BEGIN_ALLOW_THREADS;
	rc = p_rose_readlast(sub, &msg);
	Py_END_ALLOW_THREADS;

	PyBuffer_Release(&view);

	if (rc < 0) {
		/* No data available */
		Py_RETURN_NONE;
	}

	/* Return data_length */
	return PyLong_FromLongLong(msg.length);
}

static PyObject *
py_rose_remaining(PyObject *self, PyObject *arg) {
	rose_subscriber *sub = sub_from_capsule(arg);
	if (!sub) return NULL;

	return PyLong_FromUnsignedLongLong(p_rose_remaining(sub));
}

static PyObject *
py_rose_spin_once(PyObject *self, PyObject *arg) {
	rose_node *node = node_from_capsule(arg);
	if (!node) return NULL;

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
	if (!PyArg_ParseTuple(args, "d", &seconds)) {
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
	if (!PyArg_ParseTuple(args, "KK", &stop, &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_rose_seconds(stop, start));
}

static PyObject *
py_rose_nanoseconds(PyObject *self, PyObject *args) {
	uint64_t stop, start;
	if (!PyArg_ParseTuple(args, "KK", &stop, &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_rose_nanoseconds(stop, start));
}

static PyObject *
py_rose_elapsed_seconds(PyObject *self, PyObject *args) {
	uint64_t start;
	if (!PyArg_ParseTuple(args, "K", &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_rose_elapsed_seconds(start));
}

static PyObject *
py_rose_elapsed_nanoseconds(PyObject *self, PyObject *args) {
	uint64_t start;
	if (!PyArg_ParseTuple(args, "K", &start)) {
		return NULL;
	}
	return PyFloat_FromDouble(p_rose_elapsed_nanoseconds(start));
}

/* Sleep functions */

static PyObject *
py_rose_sleep(PyObject *self, PyObject *args) {
	double seconds;
	if (!PyArg_ParseTuple(args, "d", &seconds)) {
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
	if (!PyArg_ParseTuple(args, "d", &nanoseconds)) {
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
	if (!PyArg_ParseTuple(args, "d", &hz)) {
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
	if (!PyArg_ParseTuple(args, "s", &topic)) {
		return NULL;
	}

	char shm[256];
	p_rose_topic_to_shm(shm, sizeof(shm), topic);

	return PyUnicode_FromString(shm);
}

/* -------------------- 5.  Module method table ------------------------------ */

static PyMethodDef Ros3Methods[] = {
    /* Node management */
    {"create_node", (PyCFunction)py_rose_create_node, METH_VARARGS | METH_KEYWORDS,
     "Create a ROS3 node."},
    {"shutdown", py_rose_shutdown, METH_O, "Shutdown a node."},
    {"ok", py_rose_ok, METH_O, "Check if node is OK and send heartbeat."},

    /* Publisher/Subscriber */
    {"create_pub", py_rose_create_pub, METH_VARARGS, "Create a publisher."},
    {"create_sub", py_rose_create_sub, METH_VARARGS, "Create a subscriber."},
    {"delete_sub", py_rose_delete_subscriber, METH_VARARGS, "Delete a subscriber."},
    {"publish", py_rose_publish, METH_VARARGS, "Publish a message."},
    {"read", py_rose_read, METH_VARARGS, "Read a message from subscriber."},
    {"readlast", py_rose_readlast, METH_VARARGS, "Read latest message from subscriber."},
    {"remaining", py_rose_remaining, METH_O, "Get remaining message count."},
    {"spin_once", py_rose_spin_once, METH_O, "Process callbacks once."},

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
    "Thin C wrapper for libros3.so",
    -1,
    Ros3Methods,
    NULL,
    NULL,
    NULL,
    NULL};

/* ----------------------------- 7.  Module init ----------------------------- */

PyMODINIT_FUNC
PyInit__ros3(void) {
	PyObject *m = PyModule_Create(&ros3module);
	if (!m) {
		return NULL;
	}

	/*
	 * IMPORTANT: Load libraries with RTLD_GLOBAL so that when ros3 calls
	 * quicksand functions internally, those symbols are visible.
	 * Load in dependency order: mirage, quicksand, then ros3.
	 */

	/* Load mirage first (no dependencies) */
	void *mirage_handle = dlopen("libmirage.so", RTLD_NOW | RTLD_GLOBAL);
	if (!mirage_handle) {
		PyErr_Format(PyExc_RuntimeError,
		             "Failed to load shared library libmirage.so: %s",
		             dlerror());
		return NULL;
	}

	/* Load quicksand second (depends on mirage) */
	void *quicksand_handle = dlopen("libquicksand.so", RTLD_NOW | RTLD_GLOBAL);
	if (!quicksand_handle) {
		PyErr_Format(PyExc_RuntimeError,
		             "Failed to load shared library libquicksand.so: %s",
		             dlerror());
		return NULL;
	}
	(void)quicksand_handle;  /* We just need it loaded globally */

	/* Load ros3 last (depends on mirage and quicksand) */
	lib_handle = dlopen("libros3.so", RTLD_NOW | RTLD_GLOBAL);
	if (!lib_handle) {
		PyErr_Format(PyExc_RuntimeError,
		             "Failed to load shared library libros3.so: %s",
		             dlerror());
		return NULL;
	}

	/* Load all ros3 symbols */
	if (load_symbol(lib_handle, "rose_create_node", (void **)&p_rose_create_node) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_shutdown", (void **)&p_rose_shutdown) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_ok", (void **)&p_rose_ok) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_create_pub", (void **)&p_rose_create_pub) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_create_sub", (void **)&p_rose_create_sub) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_delete_subscriber", (void **)&p_rose_delete_subscriber) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_publish", (void **)&p_rose_publish) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_read", (void **)&p_rose_read) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_readlast", (void **)&p_rose_readlast) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_remaining", (void **)&p_rose_remaining) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_spin_once", (void **)&p_rose_spin_once) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_spin_forever", (void **)&p_rose_spin_forever) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_now", (void **)&p_rose_now) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_timer_calibrate_seconds", (void **)&p_rose_timer_calibrate_seconds) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_seconds", (void **)&p_rose_seconds) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_nanoseconds", (void **)&p_rose_nanoseconds) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_elapsed_seconds", (void **)&p_rose_elapsed_seconds) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_elapsed_nanoseconds", (void **)&p_rose_elapsed_nanoseconds) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_sleep", (void **)&p_rose_sleep) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_nanosleep", (void **)&p_rose_nanosleep) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_sleep_rate", (void **)&p_rose_sleep_rate) < 0) return NULL;
	if (load_symbol(lib_handle, "rose_topic_to_shm", (void **)&p_rose_topic_to_shm) < 0) return NULL;

	/* Load mirage symbols */
	if (load_symbol(mirage_handle, "mirage_create", (void **)&p_mirage_create) < 0) return NULL;
	if (load_symbol(mirage_handle, "mirage_destroy", (void **)&p_mirage_destroy) < 0) return NULL;

	/* Add constants */
	PyModule_AddIntConstant(m, "ROSE_OK", 0);
	PyModule_AddIntConstant(m, "ROSE_NO_DATA", -1);
	PyModule_AddIntConstant(m, "ROSE_ERR_BADDATA", -2);
	PyModule_AddIntConstant(m, "ROSE_ERR_BUFFERSIZE", -4);
	PyModule_AddIntConstant(m, "ROSE_ERR_BADINPUT", -8);

	return m;
}
