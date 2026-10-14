## roseserial

`roseserial` bridges serial frames and ROS3 topics.

### Start bridge

```bash
roseserial /dev/ttyUSB0 /mcu/rx /mcu/tx 115200 1024 256
```

If `tx_topic` does not exist yet, `roseserial` keeps running and retries that
subscription once per second. The serial -> `rx_topic` path continues working
immediately.

### Frame format

Serial protocol v1 is incompatible with the old unversioned two-byte length
prefix. Upgrade both bridge and firmware together; legacy frames are not
automatically accepted. A frame is:

- opening byte 0x7e;
- version byte 0x01, two-byte little-endian payload length, payload bytes;
- four-byte little-endian CRC-32/ISO-HDLC of version, length and payload;
- closing byte 0x7e.

Between delimiters, encode every 0x7e or 0x7d byte as 0x7d followed by that
byte XOR 0x20. The length counts unescaped payload bytes (1..65535). CRC uses
reflected polynomial 0xedb88320, initial value 0xffffffff, final XOR
0xffffffff; the check value for ASCII "123456789" is 0xcbf43926.

The configured msg_size is an exact maximum, including Mirage encoding.
Partial RX state survives idle reads. A frame has a one-second deadline
from its first encoded body byte; stale, malformed, oversized or CRC-invalid
input is dropped through an explicit delimiter before parsing resumes.
RX and TX run incrementally with bounded per-turn work. An outbound frame
that cannot finish within one second causes failure and closes the device;
the peer must likewise discard incomplete frames through a delimiter.
For slow baud rates choose payload sizes that fit this deadline, allowing
for escaping and ten serial bits per byte. Samples above msg_size are
consumed from the source topic, counted and dropped before transmission.
Source slots larger than 16MiB plus alignment are rejected at attachment.
The rounded publisher ring is limited to 64MiB. This does not reserve free
shared memory; creation can still fail when the system is full.

SIGINT/SIGTERM close resources normally. Other I/O, setup, allocation and
publication failures return nonzero. Exit statistics count only completed
transmissions/publications and explicitly rejected frames or samples.
CRC detects accidental corruption; it provides neither authentication nor
replay protection, acknowledgements or exactly-once delivery.


### Embedded API

`ros3_embedded.h` is a C99/C++11 header with no allocation. Include it and
link Mirage's `src/mirage_embedded.c`. `make install-embedded` installs the
header without building the desktop library.

Call `ros3_read_init(&frame, buffer, capacity)` once to assign receive storage.
There is no separate reset function or read timeout: delimiters start and end
frames, and recover after malformed input. `ros3_read_byte(&frame, byte)` returns
0 only when a complete frame passes CRC. States are `ROS3_BEGIN`,
`ROS3_READING`, `ROS3_VALID`, and `ROS3_INVALID`.

`ros3_read_message(&frame, &message)` borrows the decoded payload without copying.
Decode it before feeding the next byte. Call `mirage_read_start`, then the
Mirage readers; CRC does not validate payload syntax. The wire supports
1..65535 bytes; the Mirage adapter rejects lengths above 32767.

For output, initialize the writer and consume bytes while status is true:

```cpp
if (ros3_write_init(&writer, message.data, message.length) == 0)
    while (ros3_write_status(&writer))
        Serial.write(ros3_write_byte(&writer));
```

Keep the source buffer unchanged until output finishes. Zero is a valid byte.
Each byte call advances the writer. Wait until output can accept a byte before
calling it, or retain that byte yourself until it is accepted.

### Arduino example

Send `List["Hello world", count]` once a second. Receiving `reset[]` restarts
the counter at zero. The timer schedules output only; receive framing has no
timeout. This small example uses blocking serial output.

```cpp
#include <Arduino.h>
#include <ros3_embedded.h>
#include <string.h>

uint8_t tx[128], rx[128];
mirage_msg message;
ros3_writer writer;
ros3_frame frame;
int16_t count = 0;
uint32_t last;

void setup() {
    Serial.begin(115200);
    mirage_init(&message, tx, sizeof(tx));
    ros3_read_init(&frame, rx, sizeof(rx));
    last = millis() - 1000;
}

void loop() {
    if (Serial.available() && ros3_read_byte(&frame, Serial.read()) == 0) {
        mirage_msg incoming;
        char name[5];
        int16_t length, argc;
        if (ros3_read_message(&frame, &incoming) == 0 &&
            mirage_read_start(&incoming) >= 0 &&
            mirage_read_fn(&incoming, name, &length, sizeof(name), &argc) >= 0 &&
            length == 5 && argc == 0 && !memcmp(name, "reset", 5))
            count = 0;
    }
    if ((uint32_t)(millis() - last) >= 1000) {
        last = millis();
        mirage_write_start(&message);
        mirage_write_fn(&message, "List", 4, 2);
        mirage_write_string(&message, "Hello world", 11);
        mirage_write_i16(&message, count);
        if (ros3_write_init(&writer, message.data, message.length) == 0)
            while (ros3_write_status(&writer))
                Serial.write(ros3_write_byte(&writer));
        count = count == INT16_MAX ? 0 : count + 1;
    }
}
```

The fixed greeting fits the 128-byte buffer. Match the bridge limit:

```sh
roseserial /dev/ttyUSB0 /mcu/rx /mcu/tx 115200 128 16
```

### Validation

From the ROS3 root with Mirage at `../mirage`:

```sh
make BUILD_DIR=build-embedded-serial SANITIZE=1 check-embedded
make BUILD_DIR=build-embedded-serial check-embedded-avr
```

Host checks cover wire compatibility, malformed frames, buffer bounds,
and C++/Mirage integration. The AVR target compiles the equivalent
plain-C example in `test/avr/roseserial_uno.c`, then runs it in simavr to check
counting, reset, CRC rejection, delayed input, and delimiter recovery.
It needs `gcc-avr`, `avr-libc`, `binutils-avr`, and `libsimavr-dev`.
`make embedded-avr` builds ELF/HEX files without running the simulator.
These checks do not replace physical-board testing.
