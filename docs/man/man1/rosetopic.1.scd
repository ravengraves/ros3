ROSETOPIC(1)

# NAME

rosetopic - inspect and monitor ROS3 topics

# SYNOPSIS

*rosetopic* list

*rosetopic* info <topic>

*rosetopic* echo <topic>

*rosetopic* hz <topic>

*rosetopic* rm <topic>

# DESCRIPTION

Lists topics, prints publisher/subscriber details, streams message payloads, measures topic frequency, or unlinks a topic shared-memory segment.

Resolved topic names are limited to 245 bytes, including ROSE_BASE, namespace
prefixes and separators, excluding the terminating NUL. Longer names are
rejected. With the default base and no node namespace, the topic itself can
use up to 239 ASCII characters. ROSE_BASE is limited to 225 bytes.

# EXAMPLES

## List active topics

$ rosetopic list

## Show publishers and subscribers for a topic

$ rosetopic info /chatter

## Stream messages from a topic (Ctrl+C to stop)

$ rosetopic echo /chatter

## Measure topic rate (Ctrl+C to stop)

$ rosetopic hz /chatter

## Remove an old topic shared-memory segment

$ rosetopic rm /chatter

# ENVIRONMENT

- *ROSE_BASE*: Namespace prefix (default: _rose_).
- *ROSE_QUERY_TIMEOUT_MS*: Absolute query timeout in milliseconds (default 1000).
  Unrelated replies do not extend the deadline. An incomplete query exits with
  a nonzero status.
