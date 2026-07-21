#include "kitty_input_posix.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;
static kittyin_terminal terminal;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
    kittyin_terminal_emergency_restore(&terminal);
}

static void print_event(const kittyin_event *event)
{
    if (event->kind == KITTYIN_EVENT_KEY) {
        (void)printf("key value=%" PRIu32 " action=%u modifiers=%" PRIu32 "\n",
                     event->data.key.key, (unsigned)event->data.key.action,
                     event->data.key.modifiers);
    } else if (event->kind == KITTYIN_EVENT_MOUSE) {
        (void)printf("mouse action=%u button=%u x=%" PRId32 " y=%" PRId32
                     " wheel=(%d,%d) modifiers=%" PRIu32 "\n",
                     (unsigned)event->data.mouse.action,
                     (unsigned)event->data.mouse.button,
                     event->data.mouse.x, event->data.mouse.y,
                     (int)event->data.mouse.wheel_x,
                     (int)event->data.mouse.wheel_y,
                     event->data.mouse.modifiers);
    } else if (event->kind == KITTYIN_EVENT_FOCUS) {
        (void)printf("focus %s\n",
                     event->data.focus.focused ? "in" : "out");
    } else if (event->kind == KITTYIN_EVENT_PASTE) {
        (void)printf("paste bytes=%zu final=%s\n",
                     event->data.paste.length,
                     event->data.paste.final ? "yes" : "no");
    }
}

int main(void)
{
    kittyin_terminal_options options;

    kittyin_terminal_init(&terminal);
    kittyin_terminal_options_init(&options);
    options.mouse_tracking = KITTYIN_MOUSE_TRACKING_MOTION;
    if (kittyin_terminal_start(&terminal, STDIN_FILENO, STDOUT_FILENO,
                               &options) != 0) {
        perror("kittyin_terminal_start");
        return 1;
    }
    (void)signal(SIGINT, request_stop);
    (void)signal(SIGTERM, request_stop);
    while (!stopping) {
        struct pollfd descriptor;
        kittyin_event event;
        int result;

        descriptor.fd = STDIN_FILENO;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        do {
            result = poll(&descriptor, 1u, 50);
        } while (result < 0 && errno == EINTR && !stopping);
        if (result < 0) {
            perror("poll");
            break;
        }
        if (kittyin_terminal_read(&terminal) < 0) {
            perror("kittyin_terminal_read");
            break;
        }
        while (kittyin_input_next(&terminal.input, &event)) {
            print_event(&event);
            if (event.kind == KITTYIN_EVENT_KEY &&
                event.data.key.key == (uint32_t)'q') stopping = 1;
        }
    }
    if (kittyin_terminal_stop(&terminal) != 0) {
        perror("kittyin_terminal_stop");
        return 1;
    }
    return 0;
}
