# Portable codec tests and a real ATmega328P firmware build. No desktop ROS3
# library is linked with Mirage embedded, whose API uses a different msg ABI.
EMBEDDED_HEADER := include/ros3_embedded.h
EMBEDDED_MIRAGE_HEADER := $(MIRAGE_DIR)/include/mirage_embedded.h
EMBEDDED_MIRAGE_SOURCE := $(MIRAGE_DIR)/src/mirage_embedded.c
EMBEDDED_EXAMPLE_HEADER := test/embedded_serial_message.h
AVR_CC ?= avr-gcc
AVR_OBJCOPY ?= avr-objcopy
AVR_SIZE ?= avr-size
AVR_CFLAGS ?= -mmcu=atmega328p -DF_CPU=16000000UL -Os -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -fstack-usage
SIMAVR_CFLAGS ?= -I/usr/include/simavr
SIMAVR_LIBS ?= -lsimavr

$(BUILD_DIR)/test_embedded_serial: test/test_embedded_serial.c $(EMBEDDED_HEADER) cmd/roseserial/frame.h $(HEADER)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) $< -o $@

$(BUILD_DIR)/embedded_serial_demo: test/embedded_serial_demo.c $(EMBEDDED_EXAMPLE_HEADER) $(EMBEDDED_HEADER) $(EMBEDDED_MIRAGE_SOURCE) $(EMBEDDED_MIRAGE_HEADER)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) $< $(EMBEDDED_MIRAGE_SOURCE) -o $@

$(BUILD_DIR)/mirage_embedded_test.o: $(EMBEDDED_MIRAGE_SOURCE) $(EMBEDDED_MIRAGE_HEADER)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) -c $< -o $@

$(BUILD_DIR)/test_embedded_cpp: test/test_embedded_cpp.cpp $(EMBEDDED_HEADER) $(EMBEDDED_MIRAGE_HEADER) $(BUILD_DIR)/mirage_embedded_test.o
	$(CXX) $(CXXFLAGS) $< $(BUILD_DIR)/mirage_embedded_test.o -o $@

check-embedded: $(BUILD_DIR)/test_embedded_serial $(BUILD_DIR)/embedded_serial_demo $(BUILD_DIR)/test_embedded_cpp
	$(BUILD_DIR)/test_embedded_serial
	$(BUILD_DIR)/embedded_serial_demo
	$(BUILD_DIR)/test_embedded_cpp

$(BUILD_DIR)/roseserial_uno.elf: test/avr/roseserial_uno.c $(EMBEDDED_EXAMPLE_HEADER) $(EMBEDDED_HEADER) $(EMBEDDED_MIRAGE_SOURCE) $(EMBEDDED_MIRAGE_HEADER)
	mkdir -p $(BUILD_DIR)
	$(AVR_CC) $(AVR_CFLAGS) -Iinclude -I$(MIRAGE_DIR)/include $< $(EMBEDDED_MIRAGE_SOURCE) -Wl,--gc-sections -o $@

$(BUILD_DIR)/roseserial_uno.hex: $(BUILD_DIR)/roseserial_uno.elf
	$(AVR_OBJCOPY) -O ihex -R .eeprom $< $@

embedded-avr: $(BUILD_DIR)/roseserial_uno.elf $(BUILD_DIR)/roseserial_uno.hex
	$(AVR_SIZE) --format=avr --mcu=atmega328p $(BUILD_DIR)/roseserial_uno.elf

$(BUILD_DIR)/test_roseserial_simavr: test/avr/test_roseserial_simavr.c $(EMBEDDED_EXAMPLE_HEADER) $(EMBEDDED_HEADER) $(EMBEDDED_MIRAGE_SOURCE) $(EMBEDDED_MIRAGE_HEADER)
	mkdir -p $(BUILD_DIR)
	$(CC) $(filter-out -Wpedantic -Werror,$(CFLAGS_BASE)) $(SIMAVR_CFLAGS) $< $(EMBEDDED_MIRAGE_SOURCE) $(SIMAVR_LIBS) -o $@

# Keep this separate from check-embedded; simavr and avr-gcc are optional tools.
check-embedded-avr: embedded-avr $(BUILD_DIR)/test_roseserial_simavr
	$(BUILD_DIR)/test_roseserial_simavr $(BUILD_DIR)/roseserial_uno.elf

install-embedded: $(EMBEDDED_HEADER)
	mkdir -p $(INCLUDE_INSTALL_DIR)
	install -m 0644 $(EMBEDDED_HEADER) $(INCLUDE_INSTALL_DIR)/ros3_embedded.h

.PHONY: check-embedded embedded-avr check-embedded-avr install-embedded
