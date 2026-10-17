ROSEMASTER(1)

# NAME

rosecore - ROS3 node registry and discovery daemon

# SYNOPSIS

*rosecore*

# DESCRIPTION

Starts the ROS3 master process used for node, publisher, and subscriber
registration and centralized node management. Nodes may start before rosecore
and continue publishing while it is absent. Regular calls to rose_ok
automatically restore registrations after a late start or restart of rosecore.
rose_spin_forever calls rose_ok internally; loops using rose_spin_once should
call rose_ok themselves. Introspection tools require a running core and may
briefly show partial results while registrations recover. Subscribers still
require an existing publisher topic.

The core creates its heartbeat topic if needed and publishes a timestamp every
100 ms. Registrations replay in bounded batches only on first detection or a
change in core PID or start timestamp. Node and endpoint constructors only
establish data channels. Pending registrations and node heartbeats are sent by
rose_ok after a fresh READY core heartbeat. Node heartbeats are also limited to
10 Hz. Endpoints added during an outage are registered when the core returns.
There is no periodic refresh or replay after a temporary gap from the same
core. During the 300 ms startup election, a STARTING core yields to any fresh
READY core, or to a STARTING core with a lower PID. Heartbeat election depends
on timely process scheduling; it is not an exclusive operating-system lock.

Core shared-memory topics persist after graceful shutdown and crashes. New core
instances reuse them, discard old registrations and requests, and publish READY
after initialization. Nodes retain their mappings across restarts. Stop all
participating processes before manually removing internal topics with rosetopic
rm. Upgrade the library, bindings, and core together; older cores unlink these
topics. Registration delivery is best effort: failed writes are retried, but
overwritten announcements have no acknowledgment or periodic refresh.

# EXAMPLES

## Start the master in the default namespace

$ rosecore

## Start the master in a custom namespace

$ export ROSE_BASE=robot1
$ rosecore

## Local startup workflow (default namespace)

Terminal 1 (start master):

$ rosecore

Terminal 2 (start a node):

$ ./build/test

Terminal 3 (verify discovery):

$ rosenode list
$ rosetopic list
$ rosetopic info /chatter

## Isolated startup workflow (custom namespace)

Terminal 1:

$ export ROSE_BASE=robot1
$ rosecore

Terminal 2:

$ export ROSE_BASE=robot1
$ ./build/test

Terminal 3:

$ export ROSE_BASE=robot1
$ rosenode list
$ rosetopic list

## Shutdown workflow

Graceful stop of all demo nodes:

$ rosenode kill demo

Then stop rosecore with Ctrl+C.

# ENVIRONMENT

- *ROSE_BASE*: Namespace prefix (default: _rose_).
- *ROSE_QUERY_TIMEOUT_MS*: Query timeout in milliseconds.
