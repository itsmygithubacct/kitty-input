#include "kitty_input_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static int write_all(int fd, const char *bytes, size_t size)
{
    size_t offset = 0u;

    while (offset < size) {
        const ssize_t result = write(fd, bytes + offset, size - offset);

        if (result > 0) {
            offset += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor;
            int poll_result;

            descriptor.fd = fd;
            descriptor.events = POLLOUT;
            descriptor.revents = 0;
            do {
                poll_result = poll(&descriptor, 1u, 1000);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result > 0) continue;
            if (poll_result == 0) errno = ETIMEDOUT;
        } else if (result == 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

static int write_literal(int fd, const char *control)
{
    return write_all(fd, control, strlen(control));
}

static int enable_modes(kittyin_terminal *terminal)
{
    const int fd = terminal->keyboard.output_fd;

    if (terminal->mouse_tracking != KITTYIN_MOUSE_TRACKING_OFF) {
        const char *tracking = "";

        if (terminal->mouse_tracking == KITTYIN_MOUSE_TRACKING_BUTTON)
            tracking = "\x1b[?1000h";
        else if (terminal->mouse_tracking == KITTYIN_MOUSE_TRACKING_DRAG)
            tracking = "\x1b[?1002h";
        else
            tracking = "\x1b[?1003h";
        if (write_literal(fd, tracking) != 0 ||
            write_literal(fd, "\x1b[?1006h") != 0 ||
            (terminal->pixel_mouse &&
             write_literal(fd, "\x1b[?1016h") != 0)) return -1;
    }
    if (terminal->focus_events &&
        write_literal(fd, "\x1b[?1004h") != 0) return -1;
    if (terminal->bracketed_paste &&
        write_literal(fd, "\x1b[?2004h") != 0) return -1;
    terminal->modes_enabled = true;
    return 0;
}

static int disable_modes(kittyin_terminal *terminal)
{
    static const char controls[] =
        "\x1b[?2004l\x1b[?1004l\x1b[?1016l\x1b[?1006l"
        "\x1b[?1003l\x1b[?1002l\x1b[?1000l";
    int result = 0;

    if (terminal->keyboard.output_fd >= 0)
        result = write_all(terminal->keyboard.output_fd, controls,
                           sizeof controls - 1u);
    terminal->modes_enabled = false;
    return result;
}

void kittyin_terminal_options_init(kittyin_terminal_options *options)
{
    if (options == NULL) return;
    kittykb_terminal_options_init(&options->keyboard);
    options->mouse_tracking = KITTYIN_MOUSE_TRACKING_BUTTON;
    options->pixel_mouse = true;
    options->focus_events = true;
    options->bracketed_paste = true;
}

void kittyin_terminal_init(kittyin_terminal *terminal)
{
    if (terminal == NULL) return;
    (void)memset(terminal, 0, sizeof *terminal);
    kittykb_terminal_init(&terminal->keyboard);
    kittyin_input_init(&terminal->input);
    kittyin_input_bind_keyboard(&terminal->input,
                                &terminal->keyboard.input);
    terminal->escape_pending_since_ms = -1;
}

int kittyin_terminal_start(kittyin_terminal *terminal, int input_fd,
                           int output_fd,
                           const kittyin_terminal_options *options)
{
    kittyin_terminal_options defaults;
    const kittyin_terminal_options *selected = options;
    int failure;

    if (terminal == NULL || input_fd < 0 || output_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (terminal->active) {
        errno = EBUSY;
        return -1;
    }
    if (selected == NULL) {
        kittyin_terminal_options_init(&defaults);
        selected = &defaults;
    }
    if (selected->mouse_tracking < KITTYIN_MOUSE_TRACKING_OFF ||
        selected->mouse_tracking > KITTYIN_MOUSE_TRACKING_MOTION) {
        errno = EINVAL;
        return -1;
    }
    if (kittykb_terminal_start(&terminal->keyboard, input_fd, output_fd,
                               &selected->keyboard) != 0) return -1;

    kittyin_input_init(&terminal->input);
    kittyin_input_bind_keyboard(&terminal->input,
                                &terminal->keyboard.input);
    kittyin_input_set_pixel_coordinates(&terminal->input,
                                        selected->pixel_mouse);
    terminal->escape_pending_since_ms = -1;
    terminal->escape_timeout_ms = selected->keyboard.escape_timeout_ms;
    terminal->mouse_tracking = selected->mouse_tracking;
    terminal->pixel_mouse = selected->pixel_mouse;
    terminal->focus_events = selected->focus_events;
    terminal->bracketed_paste = selected->bracketed_paste;
    terminal->active = true;
    if (enable_modes(terminal) == 0) return 0;

    failure = errno;
    (void)disable_modes(terminal);
    (void)kittykb_terminal_stop(&terminal->keyboard);
    terminal->active = false;
    errno = failure;
    return -1;
}

static bool input_is_immediately_readable(int fd)
{
    struct pollfd descriptor;
    int result;

    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    do {
        result = poll(&descriptor, 1u, 0);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0;
}

int kittyin_terminal_read(kittyin_terminal *terminal)
{
    unsigned char buffer[4096];
    int total = 0;
    bool read_any = false;

    if (terminal == NULL || !terminal->active) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        const ssize_t count = read(terminal->keyboard.input_fd, buffer,
                                   sizeof buffer);

        if (count > 0) {
            read_any = true;
            kittyin_input_feed(&terminal->input, buffer, (size_t)count);
            if (count > INT_MAX - total) total = INT_MAX;
            else total += (int)count;
            if (!input_is_immediately_readable(terminal->keyboard.input_fd))
                break;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        break;
    }

    if (kittyin_input_has_pending_escape(&terminal->input)) {
        const int64_t now = monotonic_milliseconds();

        if (read_any || terminal->escape_pending_since_ms < 0)
            terminal->escape_pending_since_ms = now;
        else if (now - terminal->escape_pending_since_ms >=
                 terminal->escape_timeout_ms) {
            kittyin_input_flush_escape(&terminal->input);
            terminal->escape_pending_since_ms = -1;
        }
    } else {
        terminal->escape_pending_since_ms = -1;
    }
    return total;
}

int kittyin_terminal_stop(kittyin_terminal *terminal)
{
    int first_error = 0;

    if (terminal == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!terminal->active) return 0;
    if (disable_modes(terminal) != 0) first_error = errno;
    if (kittykb_terminal_stop(&terminal->keyboard) != 0 && first_error == 0)
        first_error = errno;
    kittyin_input_release_all(&terminal->input);
    terminal->active = false;
    terminal->escape_pending_since_ms = -1;
    if (first_error != 0) {
        errno = first_error;
        return -1;
    }
    return 0;
}

void kittyin_terminal_emergency_restore(const kittyin_terminal *terminal)
{
    static const char controls[] =
        "\x1b[?2004l\x1b[?1004l\x1b[?1016l\x1b[?1006l"
        "\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[<u";

    if (terminal != NULL && terminal->active &&
        terminal->keyboard.output_fd >= 0)
        (void)write(terminal->keyboard.output_fd, controls,
                    sizeof controls - 1u);
}
