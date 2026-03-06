ROSENODE(1)

# NAME

rosenode - inspect and control ROS3 nodes

# SYNOPSIS

*rosenode* list

*rosenode* info <name>

*rosenode* info --pid <pid>

*rosenode* kill [-9] <name>

# DESCRIPTION

Lists nodes, prints node info, and sends signals to matching nodes.

# EXAMPLES

## List active nodes

$ rosenode list

## Inspect a specific node

$ rosenode info demo

## Inspect by PID

$ rosenode info --pid 12345

## Request graceful shutdown for matching nodes

$ rosenode kill demo

## Force kill matching nodes

$ rosenode kill -9 demo

# ENVIRONMENT

- *ROSE_BASE*: Namespace prefix (default: _rose_).
- *ROSE_QUERY_TIMEOUT_MS*: Query timeout in milliseconds.
