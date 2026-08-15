PROJECT := kitty-input
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
AR ?= ar
INSTALL ?= install
FUZZ_CC ?= clang
FUZZ_TIME ?= 15

KITTY_KEYBOARD_DIR := third_party/kitty_keyboard
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	-Iinclude -I$(KITTY_KEYBOARD_DIR)/include
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -fPIC $(WARNINGS)
DEPFLAGS := -MMD -MP

LIB_OBJS := \
	$(BUILD_DIR)/kitty_input.o \
	$(BUILD_DIR)/kitty_input_posix.o \
	$(BUILD_DIR)/kitty_keyboard.o \
	$(BUILD_DIR)/kitty_keyboard_posix.o
STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so
TEST_BIN := $(BUILD_DIR)/test-input
EXAMPLE_BIN := $(BUILD_DIR)/events
BENCHMARK_BIN := $(BUILD_DIR)/benchmark-input
FUZZ_BIN := $(BUILD_DIR)/fuzz-input
DEPS := $(LIB_OBJS:.o=.d)

.PHONY: all benchmark clean fuzz install keyboard-all keyboard-clean \
	keyboard-sanitize keyboard-test sanitize test

all: $(STATIC_LIB) $(SHARED_LIB) $(EXAMPLE_BIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/kitty_input.o: src/kitty_input.c include/kitty_input.h $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/kitty_input_posix.o: src/kitty_input_posix.c include/kitty_input.h include/kitty_input_posix.h $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/kitty_keyboard.o: $(KITTY_KEYBOARD_DIR)/src/kitty_keyboard.c $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/kitty_keyboard_posix.o: $(KITTY_KEYBOARD_DIR)/src/kitty_keyboard_posix.c $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix.h $(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS)
	$(CC) -shared $(LDFLAGS) $^ -o $@

$(TEST_BIN): tests/test_input.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -lutil -o $@

$(EXAMPLE_BIN): examples/events.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

$(BENCHMARK_BIN): benchmarks/benchmark_input.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

benchmark: $(BENCHMARK_BIN)
	$(BENCHMARK_BIN)

$(FUZZ_BIN): tests/fuzz_input.c src/kitty_input.c include/kitty_input.h \
		$(KITTY_KEYBOARD_DIR)/src/kitty_keyboard.c \
		$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h \
		$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_internal.h | $(BUILD_DIR)
	$(FUZZ_CC) $(CPPFLAGS) -std=c11 -O1 -g3 $(WARNINGS) \
		-fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined \
		tests/fuzz_input.c src/kitty_input.c \
		$(KITTY_KEYBOARD_DIR)/src/kitty_keyboard.c \
		-fsanitize=fuzzer,address,undefined -o $@

fuzz: $(FUZZ_BIN)
	ASAN_OPTIONS=detect_leaks=1 $(FUZZ_BIN) \
		-dict=tests/kitty_input.dict -max_len=4096 \
		-max_total_time=$(FUZZ_TIME) -print_final_stats=1 -verbosity=0

test: $(TEST_BIN) $(BENCHMARK_BIN)
	$(TEST_BIN)
	$(MAKE) keyboard-test
	$(BENCHMARK_BIN) --assert

keyboard-all:
	$(MAKE) -C $(KITTY_KEYBOARD_DIR) all

keyboard-test:
	$(MAKE) -C $(KITTY_KEYBOARD_DIR) test

keyboard-sanitize:
	$(MAKE) -C $(KITTY_KEYBOARD_DIR) sanitize

keyboard-clean:
	$(MAKE) -C $(KITTY_KEYBOARD_DIR) clean

sanitize: | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g3 $(WARNINGS) \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/kitty_input.c src/kitty_input_posix.c \
		$(KITTY_KEYBOARD_DIR)/src/kitty_keyboard.c \
		$(KITTY_KEYBOARD_DIR)/src/kitty_keyboard_posix.c \
		tests/test_input.c -lutil -fsanitize=address,undefined \
		-o $(BUILD_DIR)/test-input-sanitize
	ASAN_OPTIONS=detect_leaks=1 $(BUILD_DIR)/test-input-sanitize

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 0644 include/kitty_input.h include/kitty_input_posix.h \
		$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard.h \
		$(KITTY_KEYBOARD_DIR)/include/kitty_keyboard_posix.h \
		$(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -m 0644 $(STATIC_LIB) $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
