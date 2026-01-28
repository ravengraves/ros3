#!/usr/bin/env python3
"""Simple Rose subscriber - receives messages from a topic."""

import ros3 as rose

node = rose.Node("subscriber")
sub = node.subscriber("chatter")

while node.ok():
    for msg in sub:
        print(f"Received: {msg}")
    rose.sleep(0.01)  # Avoid busy-waiting
