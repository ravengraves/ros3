## Pub/Sub in Python

### Install bindings

```bash
cd lang/python
pip install -e .
```

### Publisher

```python
import ros3 as rose

node = rose.Node("py_publisher")
publisher = node.publisher("chatter", message_size=1024, rate=128)

while node.ok():
    publisher.publish(["hello from python"])
    rose.sleep(0.1)
```

### Subscriber

```python
import ros3 as rose

node = rose.Node("py_subscriber")
subscriber = node.subscriber("chatter")

while node.ok():
    for message in subscriber:
        print(f"Received: {message}")
    rose.sleep(0.01)
```

`publisher.publish(...)` encodes with Mirage automatically, and `subscriber` yields decoded Python objects.
