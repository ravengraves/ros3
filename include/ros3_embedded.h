#ifndef ROS3_EMBEDDED_H
#define ROS3_EMBEDDED_H

/* Header-only serial protocol v1, usable from C99 and C++11. No allocation,
 * POSIX dependency, frame-sized temporary storage, or platform I/O. Define
 * ROS3_EMBEDDED_NO_MIRAGE before inclusion for the standalone byte codec. */
#include <stddef.h>
#include <stdint.h>
#ifndef ROS3_EMBEDDED_NO_MIRAGE
#include "mirage_embedded.h"
#endif

#define ROS3_SERIAL_VERSION 1U
#define ROS3_SERIAL_DELIMITER 0x7eU
#define ROS3_SERIAL_ESCAPE 0x7dU
#define ROS3_SERIAL_MAX_PAYLOAD UINT16_MAX

/* Frame: 0x7e | version:u8 | payload_length:u16 LE | payload | CRC32:u32 LE | 0x7e
 * Between delimiters, encode 0x7e/0x7d as 0x7d followed by byte XOR 0x20.
 * CRC covers the unescaped version, length and payload; not delimiters or CRC.
 * Length counts payload bytes only. */

/* CRC-32/ISO-HDLC: start with UINT32_MAX, update version/length/payload,
 * complement once at the end. Check("123456789") = 0xcbf43926. */
static inline uint32_t ros3_crc_byte(uint32_t crc, uint8_t byte) {
	crc ^= byte;
	for(uint8_t i = 0; i < 8; ++i) {
		crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & ((uint32_t) 0 - (crc & 1U)));
	}
	return crc;
}

typedef struct ros3_writer {
	const uint8_t *data;
	uint32_t crc;
	uint16_t length, index;
	/* Next field: 0 opening delimiter, 1 version, 2/3 length low/high,
	 * 4 payload (repeat length times), 5..8 CRC least to most significant,
	 * 9 closing delimiter, 10 done. An escape suffix is emitted before
	 * processing the next phase; it does not advance phase or CRC again. */
	uint8_t phase, escaped, escape_byte;
	int8_t error;
} ros3_writer;

/* Returns 0 on success, -1 for NULL storage or length outside 1..65535.
 * Invalid initialization leaves a non-pending writer. The payload is borrowed
 * and must remain unchanged until output finishes. */
static inline int8_t ros3_write_init(ros3_writer *writer, const uint8_t *data, size_t length) {
	if(!writer) {
		return -1;
	}
	writer->data = data;
	writer->crc = UINT32_MAX;
	writer->length = 0;
	writer->index = 0;
	writer->phase = 10; /* done */
	writer->escaped = writer->escape_byte = 0;
	writer->error = -1;
	if(!data || !length || length > UINT16_MAX) {
		return -1;
	}
	writer->length = (uint16_t) length;
	writer->phase = 0;
	writer->error = 0;
	return 0;
}

/* Boolean pending status; errors are reported by init, never as a
 * truthy error code in the output loop. Check status BEFORE taking each byte. */
static inline uint8_t ros3_write_status(const ros3_writer *writer) {
	return writer && !writer->error
			&& (writer->escaped || writer->phase != 10);
}

/* Returns an actual wire byte, including zero. Calling when status is false
 * returns zero without emitting anything; zero is NOT an end-of-frame marker.
 * Each call consumes a byte, so direct callers must arrange reliable output. */
static inline uint8_t ros3_write_byte(ros3_writer *writer) {
	uint8_t byte;
	if(!ros3_write_status(writer)) {
		return 0;
	}
	if(writer->escaped) {
		writer->escaped = 0;
		return writer->escape_byte;
	}
	uint8_t checksummed = writer->phase >= 1 && writer->phase <= 4;
	switch(writer->phase) {
	case 0:
		writer->phase = 1;
		return ROS3_SERIAL_DELIMITER;
	case 1:
		byte = ROS3_SERIAL_VERSION;
		++writer->phase;
		break;
	case 2:
		byte = (uint8_t) writer->length;
		++writer->phase;
		break;
	case 3:
		byte = (uint8_t) (writer->length >> 8);
		++writer->phase;
		break;
	case 4:
		byte = writer->data[writer->index++];
		if(writer->index == writer->length) {
			++writer->phase;
		}
		break;
	case 9:
		writer->phase = 10;
		return ROS3_SERIAL_DELIMITER;
	default: /* checksum phases 5..8, little endian */
		byte = (uint8_t) (~writer->crc >> (8U * (writer->phase - 5U)));
		++writer->phase;
		break;
	}
	if(checksummed) {
		writer->crc = ros3_crc_byte(writer->crc, byte);
	}
	if(byte == ROS3_SERIAL_DELIMITER || byte == ROS3_SERIAL_ESCAPE) {
		writer->escaped = 1;
		writer->escape_byte = byte ^ 0x20U;
		return ROS3_SERIAL_ESCAPE;
	}
	return byte;
}

enum {
	ROS3_INVALID = -1,
	ROS3_VALID = 0,
	ROS3_BEGIN = 1,
	ROS3_READING = 2
};

typedef struct ros3_frame {
	uint8_t *buffer;
	uint32_t crc, checksum;
	uint16_t capacity, length, index;
	int8_t status;
	/* Next unescaped field: 0 version, 1/2 length low/high, 3 payload
	 * (repeat length times), 4..7 CRC least to most significant, 8 wait
	 * for closing delimiter. Delimiters are handled outside these phases;
	 * active is false until the first delimiter. Escape prefixes consume
	 * no field. Extra bytes in phase 8 invalidate the frame. */
	uint8_t phase, active, escaped, dropping;
} ros3_frame;

/* Assign caller-owned storage and wait for a delimiter. No buffer clearing.
 * Returns 0 on success, -1 for NULL storage or capacity outside 1..65535. */
static inline int8_t ros3_read_init(ros3_frame *frame, uint8_t *buffer, size_t capacity) {
	if(!frame) {
		return -1;
	}
	frame->buffer = buffer;
	frame->capacity = buffer && capacity <= UINT16_MAX ? (uint16_t) capacity : 0;
	frame->crc = UINT32_MAX;
	frame->checksum = 0;
	frame->length = frame->index = 0;
	frame->phase = frame->active = frame->escaped = frame->dropping = 0;
	frame->status = frame->capacity ? ROS3_BEGIN : ROS3_INVALID;
	return frame->capacity ? 0 : -1;
}

/* Returns zero ONLY at the closing delimiter of a complete valid frame.
 * Inspect length/buffer or call read_message immediately, before feeding ANY
 * next byte (even a delimiter). No complete-message queue is maintained.
 * Malformed/oversized frames are ignored through the next delimiter, which
 * also opens a new frame. Leading garbage and repeated delimiters are safe.
 * This checks framing/CRC, not the application payload's Mirage syntax. */
static inline int8_t ros3_read_byte(ros3_frame *frame, uint8_t byte) {
	if(!frame || !frame->buffer || !frame->capacity) {
		return ROS3_INVALID;
	}
	if(byte == ROS3_SERIAL_DELIMITER) {
		/* Only phase 8 can complete a frame. Otherwise this delimiter
		 * discards any partial frame and opens the next one at phase 0. */
		uint8_t bad = frame->active && (frame->status == ROS3_READING || frame->escaped || frame->dropping);
		if(bad && !frame->dropping && !frame->escaped && frame->phase == 8
		   && frame->checksum == ~frame->crc) {
			frame->status = ROS3_VALID;
			return ROS3_VALID;
		}
		ros3_read_init(frame, frame->buffer, frame->capacity);
		frame->active = 1;
		frame->status = bad ? ROS3_INVALID : ROS3_BEGIN;
		return frame->status;
	}
	if(frame->dropping || !frame->active) {
		return frame->status;
	}
	if(frame->status != ROS3_READING) {
		ros3_read_init(frame, frame->buffer, frame->capacity);
		frame->active = 1;
		frame->status = ROS3_READING;
	}
	if(frame->escaped) {
		byte ^= 0x20U;
		frame->escaped = 0;
	} else if(byte == ROS3_SERIAL_ESCAPE) {
		frame->escaped = 1;
		return frame->status;
	}
	if(frame->phase <= 3) {
		frame->crc = ros3_crc_byte(frame->crc, byte);
	}
	switch(frame->phase) {
	case 0:
		if(byte != ROS3_SERIAL_VERSION) {
			frame->dropping = 1;
		}
		++frame->phase;
		break;
	case 1:
		frame->length = byte;
		++frame->phase;
		break;
	case 2:
		frame->length |= (uint16_t) byte << 8;
		if(!frame->length || frame->length > frame->capacity) {
			frame->dropping = 1;
		}
		++frame->phase;
		break;
	case 3:
		frame->buffer[frame->index++] = byte;
		if(frame->index == frame->length) {
			++frame->phase;
		}
		break;
	case 4:
	case 5:
	case 6:
	case 7:
		frame->checksum |= (uint32_t) byte << (8U * (frame->phase - 4U));
		++frame->phase;
		break;
	default: frame->dropping = 1; break;
	}
	if(frame->dropping) {
		frame->status = ROS3_INVALID;
	}
	return frame->status;
}

#ifndef ROS3_EMBEDDED_NO_MIRAGE
/* Borrow, do not copy: cursor=0, length=max_length=valid payload length.
 * Next call mirage_read_start(), then Mirage readers. This view expires at
 * the next read_byte/init call and must never be freed. Mirage embedded
 * has signed 16-bit lengths: wire payloads >32767 are rejected here, even
 * though the codec supports 65535. Failure leaves *message unchanged. */
static inline int8_t ros3_read_message(const ros3_frame *frame, mirage_msg *message) {
	if(!frame || !message || frame->status != ROS3_VALID || !frame->buffer
	   || !frame->length || frame->length > frame->capacity || frame->length > INT16_MAX) {
		return -1;
	}
	message->data = frame->buffer;
	message->max_length = message->length = (int16_t) frame->length;
	message->cursor = 0;
	return 0;
}
#endif
#endif
