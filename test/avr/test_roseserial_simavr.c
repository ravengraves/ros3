/* Host harness: execute the real Uno ELF and exchange bytes over simulated
 * UART0. Requires libsimavr-dev. No physical serial device is involved. */
#include "../embedded_serial_message.h"
#include <assert.h>
#include <simavr/avr_uart.h>
#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/sim_io.h>
#include <stdio.h>
#include <string.h>

static uint8_t response[128], request[128], wire[276];
static ros3_frame frame;
static unsigned replies;
static int16_t last_value;
static uint16_t lowest_sp = UINT16_MAX;

static void uart_output(struct avr_irq_t *irq, uint32_t value, void *context) {
	(void) irq;
	(void) context;
	if(ros3_read_byte(&frame, (uint8_t) value) != 0) {
		return;
	}
	mirage_msg msg;
	char name[4], text[11];
	int16_t length, argc;
	assert(!ros3_read_message(&frame, &msg));
	assert(mirage_read_start(&msg) > 0);
	assert(mirage_read_fn(&msg, name, &length, sizeof(name), &argc) > 0);
	assert(length == 4 && argc == 2 && !memcmp(name, "List", 4));
	assert(mirage_read_string(&msg, text, &length, sizeof(text)) > 0);
	assert(length == 11 && !memcmp(text, "Hello world", 11));
	assert(mirage_read_i16(&msg, &last_value) > 0 && msg.cursor == msg.length);
	++replies;
}

static void run(avr_t *avr, avr_cycle_count_t cycles) {
	avr_cycle_count_t end = avr->cycle + cycles;
	while(avr->cycle < end) {
		int state = avr_run(avr);
		assert(state != cpu_Done && state != cpu_Crashed);
		uint16_t sp = avr->data[0x5d] | ((uint16_t) avr->data[0x5e] << 8);
		if(sp >= 0x100 && sp < lowest_sp) {
			lowest_sp = sp;
		}
	}
}

static size_t make_request(void) {
	mirage_msg msg;
	mirage_init(&msg, request, sizeof(request));
	assert(mirage_write_start(&msg) > 0 && mirage_write_fn(&msg, "reset", 5, 0) > 0);
	ros3_writer writer;
	assert(!ros3_write_init(&writer, msg.data, (uint16_t) msg.length));
	size_t n = 0;
	while(ros3_write_status(&writer)) {
		wire[n++] = ros3_write_byte(&writer);
	}
	return n;
}

static void inject(avr_t *avr, avr_irq_t *input, size_t start, size_t end) {
	for(size_t i = start; i < end; ++i) {
		avr_raise_irq(input, wire[i]);
		run(avr, 2400); /* 150 us/byte, enough for the emulated UART */
	}
	run(avr, 300000);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	elf_firmware_t firmware = {0};
	assert(elf_read_firmware(argv[1], &firmware) == 0);
	avr_t *avr = avr_make_mcu_by_name("atmega328p");
	assert(avr && avr_init(avr) == 0);
	avr->frequency = 16000000;
	avr_load_firmware(avr, &firmware);
	uint32_t flags = 0;
	avr_ioctl(avr, AVR_IOCTL_UART_SET_FLAGS('0'), &flags);
	avr_irq_t *input = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_INPUT);
	avr_irq_t *output = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT);
	assert(input && output && !ros3_read_init(&frame, response, sizeof(response)));
	avr_irq_register_notify(output, uart_output, NULL);
	run(avr, 200000);
	assert(replies == 1 && last_value == 0);
	/* Periodic greetings count up without a command. */
	run(avr, 16000000);
	assert(last_value == 1);
	size_t n = make_request();
	inject(avr, input, 0, n);
	run(avr, 16000000);
	assert(last_value == 0);
	wire[4] ^= 1;
	inject(avr, input, 0, n);
	run(avr, 16000000);
	assert(last_value == 1); /* corrupt reset ignored */
	wire[4] ^= 1;
	/* A long pause does not discard a partial frame. */
	inject(avr, input, 0, 5);
	run(avr, 17000000);
	inject(avr, input, 5, n);
	run(avr, 16000000);
	assert(last_value == 0);
	/* New opening delimiter recovers an abandoned partial frame. */
	inject(avr, input, 0, 5);
	inject(avr, input, 0, n);
	run(avr, 16000000);
	assert(last_value == 0);
	printf("simavr UART: %u greetings, reset, CRC rejection, delayed frame and delimiter recovery; stack peak=%u bytes\n",
	       replies, (unsigned) (avr->ramend - lowest_sp));
	avr_terminate(avr);
	return 0;
}
