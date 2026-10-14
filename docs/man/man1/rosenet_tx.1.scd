ROSENET_TX(1)

# NAME

rosenet_tx - forward ROS3 topic traffic to TCP or UDP

# SYNOPSIS

*rosenet_tx* <topic> <host> <port> [--udp]

*rosenet_tx* --topic <topic> --host <host> --port <port> [--udp]++
  [--message_size N] [--read_last 0|1]

# DESCRIPTION

Subscribes to a local ROS3 topic and forwards frames over the network.


# LIMITS AND FAILURE POLICY

Message size is an exact maximum payload in bytes, including Mirage encoding,
from 1 to 16777216 (16MiB); the default is 65536. Older versions inflated
--message_size to at least 4MiB for benchmark traffic; callers must now supply
the actual encoded size. RX message_rate (alias queue_depth) is the requested
ring slot count, rounded up by Quicksand to a power of two, at least two.
Its default is 64. The actual aligned ring allocation is limited to 64MiB;
this is a limit, not a reservation of available shared memory. Named and
positional size/rate arguments have the same units. Invalid values fail.

UDP carries one payload per datagram and accepts at most 65507 bytes.
Empty, oversized or truncated incoming datagrams are discarded and counted;
no truncated prefix is published. TX consumes samples using the validated
source slot capacity, then drops/counts empty or over-limit samples before
sending, so one oversized sample cannot stall later valid samples. Source
slots larger than 16MiB plus alignment are rejected at attachment.

TCP retains the four-byte little-endian length prefix followed by payload.
A frame read/write has a one-second whole-frame deadline. Invalid sizes,
partial-frame timeouts and disconnects fail the connection. TCP connection
establishment has a one-second attempt deadline; retry by restarting the
bridge if the receiver is unavailable.

Socket, allocation, source-read and publication failures return nonzero.
SIGINT/SIGTERM clean up normally. Exit counters include only successful
socket sends or local publications; they do not acknowledge remote receipt.
These bridges provide no authentication, replay protection or exactly-once
delivery. Use them only across an independently trusted/protected link.

# EXAMPLES

## Forward a topic over TCP

$ rosenet_tx /camera 127.0.0.1 9000

## Forward a topic over UDP

$ rosenet_tx --topic /camera --host 192.168.1.40 --port 9000 --udp

## Forward only the latest message with explicit payload sizing

$ rosenet_tx --topic /camera --host 127.0.0.1 --port 9000 --message_size 1048576 --read_last 1
