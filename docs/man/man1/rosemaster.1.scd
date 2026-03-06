ROSEMASTER(1)

# NAME

rosemaster - ROS3 node registry and discovery daemon

# SYNOPSIS

*rosemaster*

# DESCRIPTION

Starts the ROS3 master process used for node, publisher, and subscriber
registration. Run this before other ROS3 tools.

# EXAMPLES

## Start the master in the default namespace

$ rosemaster

## Start the master in a custom namespace

$ export ROSE_BASE=robot1
$ rosemaster

## Local startup workflow (default namespace)

Terminal 1 (start master):

$ rosemaster

Terminal 2 (start a node):

$ ./build/test

Terminal 3 (verify discovery):

$ rosenode list
$ rosetopic list
$ rosetopic info /chatter

## Isolated startup workflow (custom namespace)

Terminal 1:

$ export ROSE_BASE=robot1
$ rosemaster

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

Then stop rosemaster with Ctrl+C.

# ENVIRONMENT

- *ROSE_BASE*: Namespace prefix (default: _rose_).
- *ROSE_QUERY_TIMEOUT_MS*: Query timeout in milliseconds.
