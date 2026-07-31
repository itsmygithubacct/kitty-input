#include "kitty_keyboard.h"
#include "kitty_keyboard_posix.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool
is_quit_event(const kittykb_event *event)
{
    if (event->action != KITTYKB_ACTION_PRESS) {
        return false;
    }
    return kittykb_event_matches_key(event, (uint32_t)'q') ||
           (kittykb_event_matches_key(event, (uint32_t)'c') &&
            (event->modifiers & KITTYKB_MOD_CTRL) != 0u);
}

int
main(void)
{
    kittykb_terminal terminal;
    kittykb_terminal_options options;
    bool quit = false;

    kittykb_terminal_init(&terminal);
    kittykb_terminal_options_init(&options);
    if (kittykb_terminal_start(
            &terminal,
            STDIN_FILENO,
            STDOUT_FILENO,
            &options) != 0) {
        (void)fprintf(stderr, "keyboard setup: %s\n", strerror(errno));
        return 1;
    }

    while (!quit) {
        struct pollfd descriptor;
        kittykb_event event;
        int horizontal;
        int vertical;
        int poll_result;

        descriptor.fd = STDIN_FILENO;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        do {
            poll_result = poll(&descriptor, 1u, 16);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0 || kittykb_terminal_read(&terminal) < 0) {
            (void)fprintf(stderr, "\rkeyboard read: %s\n", strerror(errno));
            (void)kittykb_terminal_stop(&terminal);
            return 1;
        }

        while (kittykb_input_next(&terminal.input, &event)) {
            if (is_quit_event(&event)) {
                quit = true;
            }
        }

        horizontal =
            (kittykb_input_key_down(&terminal.input, (uint32_t)'d') ? 1 : 0) -
            (kittykb_input_key_down(&terminal.input, (uint32_t)'a') ? 1 : 0);
        vertical =
            (kittykb_input_key_down(&terminal.input, (uint32_t)'s') ? 1 : 0) -
            (kittykb_input_key_down(&terminal.input, (uint32_t)'w') ? 1 : 0);

        (void)printf(
            "\rKitty protocol: %-11s  held=%zu  axis=(%2d,%2d)  "
            "hold WASD together; Q quits",
            kittykb_input_has_release_events(&terminal.input) ?
                "release-cap" : "legacy",
            kittykb_input_held_count(&terminal.input),
            horizontal,
            vertical);
        (void)fflush(stdout);
    }

    if (kittykb_terminal_stop(&terminal) != 0) {
        (void)fprintf(stderr, "\rkeyboard restore: %s\n", strerror(errno));
        return 1;
    }
    (void)printf("\n");
    return 0;
}
