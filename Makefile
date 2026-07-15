PROJECT := kitty-keyboard
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
AR ?= ar
INSTALL ?= install

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -fPIC $(WARNINGS)

LIB_OBJS := \
	$(BUILD_DIR)/kitty_keyboard.o \
	$(BUILD_DIR)/kitty_keyboard_posix.o
STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so
TEST_BIN := $(BUILD_DIR)/test-keyboard
EXAMPLE_BIN := $(BUILD_DIR)/held-keys

.PHONY: all clean install sanitize test

all: $(STATIC_LIB) $(SHARED_LIB) $(EXAMPLE_BIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/kitty_keyboard.o: src/kitty_keyboard.c include/kitty_keyboard.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kitty_keyboard_posix.o: src/kitty_keyboard_posix.c include/kitty_keyboard.h include/kitty_keyboard_posix.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS)
	$(CC) -shared $(LDFLAGS) $^ -o $@

$(TEST_BIN): tests/test_keyboard.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -lutil -o $@

$(EXAMPLE_BIN): examples/held_keys.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

sanitize: | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g3 $(WARNINGS) \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/kitty_keyboard.c src/kitty_keyboard_posix.c tests/test_keyboard.c \
		-lutil -fsanitize=address,undefined -o $(BUILD_DIR)/test-keyboard-sanitize
	ASAN_OPTIONS=detect_leaks=1 $(BUILD_DIR)/test-keyboard-sanitize

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 0644 include/kitty_keyboard.h include/kitty_keyboard_posix.h \
		$(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -m 0644 $(STATIC_LIB) $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf $(BUILD_DIR)
