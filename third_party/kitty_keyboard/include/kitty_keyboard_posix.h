#ifndef KITTY_KEYBOARD_POSIX_H
#define KITTY_KEYBOARD_POSIX_H

#include "kitty_keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kittykb_terminal_options {
    uint32_t flags;
    int probe_timeout_ms;
    int escape_timeout_ms;
    bool make_raw;
    bool make_nonblocking;
    bool require_tty;
} kittykb_terminal_options;

typedef struct kittykb_terminal {
    kittykb_input input;
    struct termios saved_termios;
    int input_fd;
    int output_fd;
    int saved_fd_flags;
    int64_t escape_pending_since_ms;
    int escape_timeout_ms;
    bool has_saved_termios;
    bool has_saved_fd_flags;
    bool keyboard_mode_pushed;
    bool active;
} kittykb_terminal;

void kittykb_terminal_options_init(kittykb_terminal_options *options);
void kittykb_terminal_init(kittykb_terminal *terminal);

/*
 * Call after entering the screen (main or alternate) whose keyboard-mode
 * stack should be changed.  Returns 0 or -1 with errno set.
 */
int kittykb_terminal_start(
    kittykb_terminal *terminal,
    int input_fd,
    int output_fd,
    const kittykb_terminal_options *options);

/* Read all currently available bytes, update state, and queue events. */
int kittykb_terminal_read(kittykb_terminal *terminal);

/* Pop the mode before leaving that screen, then restore fd/termios state. */
int kittykb_terminal_stop(kittykb_terminal *terminal);

/* Low-level helpers for applications that already own input and termios. */
int kittykb_terminal_push_flags(int output_fd, uint32_t flags);
int kittykb_terminal_pop_flags(int output_fd);
int kittykb_terminal_query_flags(int output_fd);

#ifdef __cplusplus
}
#endif

#endif
