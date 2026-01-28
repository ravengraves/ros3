#!/usr/bin/env python3
"""Simple Rose publisher - sends messages to a topic."""

import ros3 as rose

node = rose.Node("publisher")
pub = node.publisher("chatter", message_size=1024, rate=10)
rate = rose.Rate(1)  # 1 Hz

count = 0
while node.ok():
    pub.publish(["Hello", count])
    print(f"Published: ['Hello', {count}]")
    count += 1
    rate.sleep()
