ROSETOPIC(1)

# NAME

rosetopic - inspect and monitor ROS3 topics

# SYNOPSIS

*rosetopic* list

*rosetopic* info <topic>

*rosetopic* echo <topic>

# DESCRIPTION

Lists topics, prints publisher/subscriber details, or streams message payloads.

# EXAMPLES

## List active topics

$ rosetopic list

## Show publishers and subscribers for a topic

$ rosetopic info /chatter

## Stream messages from a topic (Ctrl+C to stop)

$ rosetopic echo /chatter

# ENVIRONMENT

- *ROSE_BASE*: Namespace prefix (default: _rose_).
- *ROSE_QUERY_TIMEOUT_MS*: Query timeout in milliseconds.
