#ifndef KITTY_INPUT_POSIX_H
#define KITTY_INPUT_POSIX_H

#include "kitty_input.h"
#include "kitty_keyboard_posix.h"

#include <stdbool.h>
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
} kittyin_terminal;

void kittyin_terminal_options_init(kittyin_terminal_options *options);
void kittyin_terminal_init(kittyin_terminal *terminal);

int kittyin_terminal_start(kittyin_terminal *terminal, int input_fd,
                           int output_fd,
                           const kittyin_terminal_options *options);
int kittyin_terminal_read(kittyin_terminal *terminal);
int kittyin_terminal_stop(kittyin_terminal *terminal);

/* Async-signal-safe best effort: disables input modes and pops keyboard mode. */
void kittyin_terminal_emergency_restore(const kittyin_terminal *terminal);

#ifdef __cplusplus
}
#endif

#endif
