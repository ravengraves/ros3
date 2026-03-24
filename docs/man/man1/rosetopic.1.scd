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
- *ROSE_QUERY_TIMEOUT_MS*: Query timeout in milliseconds.
