#ifndef KITTY_INPUT_POSIX_H
#define KITTY_INPUT_POSIX_H

#include "kitty_input.h"
#include "kitty_keyboard_posix.h"

#include <stdbool.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum kittyin_mouse_tracking {
    KITTYIN_MOUSE_TRACKING_OFF = 0,
    KITTYIN_MOUSE_TRACKING_BUTTON = 1,
    KITTYIN_MOUSE_TRACKING_DRAG = 2,
    KITTYIN_MOUSE_TRACKING_MOTION = 3
} kittyin_mouse_tracking;

typedef struct kittyin_terminal_options {
    kittykb_terminal_options keyboard;
    kittyin_mouse_tracking mouse_tracking;
    bool pixel_mouse;
    bool focus_events;
    bool bracketed_paste;
} kittyin_terminal_options;

/* keyboard is first so lifecycle aggregators can retain a legacy alias. */
typedef struct kittyin_terminal {
    kittykb_terminal keyboard;
    kittyin_input input;
    int64_t escape_pending_since_ms;
    int escape_timeout_ms;
    kittyin_mouse_tracking mouse_tracking;
    bool pixel_mouse;
    bool focus_events;
    bool bracketed_paste;
    bool modes_enabled;
    bool active;
    volatile sig_atomic_t restore_claimed;
    char mode_restore[64];
    size_t mode_restore_length;
} kittyin_terminal;

#define KITTYIN_GAMEPAD_EVENT_QUEUE_CAPACITY 64u
#define KITTYIN_GAMEPAD_PATH_CAPACITY 128u
#define KITTYIN_GAMEPAD_NAME_CAPACITY 64u

typedef struct kittyin_gamepad_device {
    int fd;
    char path[KITTYIN_GAMEPAD_PATH_CAPACITY];
    char name[KITTYIN_GAMEPAD_NAME_CAPACITY];
    uint8_t axes;
    uint8_t buttons;
    bool connected;
} kittyin_gamepad_device;

typedef struct kittyin_gamepads {
    kittyin_gamepad_device devices[KITTYIN_GAMEPAD_CAPACITY];
    kittyin_gamepad_event events[KITTYIN_GAMEPAD_EVENT_QUEUE_CAPACITY];
    char directory[KITTYIN_GAMEPAD_PATH_CAPACITY];
    size_t event_head;
    size_t event_count;
    uint64_t dropped_events;
    int64_t next_scan_ms;
    uint32_t scan_interval_ms;
} kittyin_gamepads;

void kittyin_terminal_options_init(kittyin_terminal_options *options);
void kittyin_terminal_init(kittyin_terminal *terminal);

int kittyin_terminal_start(kittyin_terminal *terminal, int input_fd,
                           int output_fd,
                           const kittyin_terminal_options *options);
int kittyin_terminal_read(kittyin_terminal *terminal);
int kittyin_terminal_stop(kittyin_terminal *terminal);

/* Async-signal-safe best effort: restores only modes selected at start and
 * pops keyboard mode.  A later stop still restores descriptor/termios state. */
void kittyin_terminal_emergency_restore(const kittyin_terminal *terminal);

/*
 * Allocation-free Linux joystick discovery. poll() scans directory/js0..js3,
 * reads every available event, and reports connect/disconnect transitions.
 * Other POSIX systems retain the same API and simply discover no devices.
 */
void kittyin_gamepads_init(kittyin_gamepads *gamepads);
bool kittyin_gamepads_set_directory(kittyin_gamepads *gamepads,
                                    const char *directory);
bool kittyin_gamepads_set_scan_interval(kittyin_gamepads *gamepads,
                                        uint32_t interval_ms);
bool kittyin_gamepads_attach_fd(kittyin_gamepads *gamepads, uint8_t gamepad,
                                int fd, const char *name, uint8_t axes,
                                uint8_t buttons);
void kittyin_gamepads_detach(kittyin_gamepads *gamepads, uint8_t gamepad);
void kittyin_gamepads_poll(kittyin_gamepads *gamepads, int64_t now_ms);
bool kittyin_gamepads_next(kittyin_gamepads *gamepads,
                           kittyin_gamepad_event *event);
void kittyin_gamepads_close(kittyin_gamepads *gamepads);
uint64_t kittyin_gamepads_dropped_events(const kittyin_gamepads *gamepads);

#ifdef __cplusplus
}
#endif

#endif
