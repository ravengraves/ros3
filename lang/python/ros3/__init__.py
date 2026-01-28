# Rose - High-performance robotics middleware
#
# Usage: import ros3 as rose
#
# Core classes:
#   rose.Node       - Create publishers and subscribers
#   rose.Publisher  - Send messages to a topic
#   rose.Subscriber - Receive messages from a topic
#   rose.Rate       - Maintain constant loop rate
#
# Functions:
#   rose.sleep(seconds)  - Sleep for a duration
#   rose.now()           - Get monotonic timestamp
#   rose.elapsed(start)  - Seconds since timestamp

from .ros3 import (
    # Core classes
    Node,
    Publisher,
    Subscriber,
    Rate,

    # Essential functions
    sleep,
    sleep_rate,
    now,
    elapsed,

    # Advanced (less common)
    nanosleep,
    seconds,
    nanoseconds,
    elapsed_nanoseconds,
    timer_calibrate,
    topic_to_shm,
)

# Keep elapsed_seconds as alias for compatibility
elapsed_seconds = elapsed

__version__ = "1.0.0"
__all__ = [
    "Node", "Publisher", "Subscriber", "Rate",
    "sleep", "now", "elapsed",
]
