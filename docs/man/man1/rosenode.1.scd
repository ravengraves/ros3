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

Node identities are _namespace/name-PID_, or _name-PID_ in the root namespace.
Name arguments match prefixes of that complete identity; for example,
*rosenode info robot/camera*. PID lookup includes every node in that process.
Signals affect the entire process, including other nodes it owns.

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
- *ROSE_QUERY_TIMEOUT_MS*: Absolute query timeout in milliseconds (default 1000).
  Unrelated replies do not extend the deadline. An incomplete query exits with
  a nonzero status.
