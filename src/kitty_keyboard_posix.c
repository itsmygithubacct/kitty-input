#include "kitty_keyboard_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t
monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static int
write_all(int fd, const char *bytes, size_t size)
{
    size_t offset = 0u;

    while (offset < size) {
        const ssize_t result = write(fd, bytes + offset, size - offset);

        if (result > 0) {
            offset += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor;
            int poll_result;

            descriptor.fd = fd;
            descriptor.events = POLLOUT;
            descriptor.revents = 0;
            do {
                poll_result = poll(&descriptor, 1u, 1000);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result > 0) {
                continue;
            }
            if (poll_result == 0) {
                errno = ETIMEDOUT;
            }
        } else if (result == 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

int
kittykb_terminal_push_flags(int output_fd, uint32_t flags)
{
    char control[32];
    int length;

    if (output_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    length = snprintf(
        control,
        sizeof(control),
        "\x1b[>%" PRIu32 "u",
        flags);
    if (length < 0 || (size_t)length >= sizeof(control)) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_all(output_fd, control, (size_t)length);
}

int
kittykb_terminal_pop_flags(int output_fd)
{
    static const char control[] = "\x1b[<u";

    if (output_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return write_all(output_fd, control, sizeof(control) - 1u);
}

int
kittykb_terminal_query_flags(int output_fd)
{
    /* The DA1 response bounds capability detection on legacy terminals. */
    static const char control[] = "\x1b[?u\x1b[c";

    if (output_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return write_all(output_fd, control, sizeof(control) - 1u);
}

void
kittykb_terminal_options_init(kittykb_terminal_options *options)
{
    if (options == NULL) {
        return;
    }
    options->flags = KITTYKB_FLAGS_DEFAULT;
    options->probe_timeout_ms = 80;
    options->escape_timeout_ms = 25;
    options->make_raw = true;
    options->make_nonblocking = true;
    options->require_tty = true;
}

void
kittykb_terminal_init(kittykb_terminal *terminal)
{
    if (terminal == NULL) {
        return;
    }
    (void)memset(terminal, 0, sizeof(*terminal));
    kittykb_input_init(&terminal->input);
    terminal->input_fd = -1;
    terminal->output_fd = -1;
    terminal->saved_fd_flags = -1;
    terminal->escape_pending_since_ms = -1;
}

static int
restore_terminal(kittykb_terminal *terminal)
{
    int first_error = 0;

    if (terminal->keyboard_mode_pushed) {
        if (kittykb_terminal_pop_flags(terminal->output_fd) != 0) {
            first_error = errno;
        }
        terminal->keyboard_mode_pushed = false;
    }
    if (terminal->has_saved_fd_flags) {
        if (fcntl(
                terminal->input_fd,
                F_SETFL,
                terminal->saved_fd_flags) < 0 && first_error == 0) {
            first_error = errno;
        }
        terminal->has_saved_fd_flags = false;
    }
    if (terminal->has_saved_termios) {
        if (tcsetattr(
                terminal->input_fd,
                TCSADRAIN,
                &terminal->saved_termios) != 0 && first_error == 0) {
            first_error = errno;
        }
        terminal->has_saved_termios = false;
    }

    terminal->input_fd = -1;
    terminal->output_fd = -1;
    terminal->saved_fd_flags = -1;
    terminal->escape_pending_since_ms = -1;
    terminal->active = false;
    kittykb_input_release_all(&terminal->input);
    if (first_error != 0) {
        errno = first_error;
        return -1;
    }
    return 0;
}

static int
make_terminal_raw(kittykb_terminal *terminal)
{
    struct termios raw;

    if (tcgetattr(terminal->input_fd, &terminal->saved_termios) != 0) {
        return -1;
    }
    terminal->has_saved_termios = true;
    raw = terminal->saved_termios;
    raw.c_iflag &= (tcflag_t)~(
        IGNBRK | BRKINT | PARMRK | INPCK | ISTRIP |
        INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(terminal->input_fd, TCSAFLUSH, &raw);
}

static int
make_input_nonblocking(kittykb_terminal *terminal)
{
    const int flags = fcntl(terminal->input_fd, F_GETFL);

    if (flags < 0) {
        return -1;
    }
    terminal->saved_fd_flags = flags;
    terminal->has_saved_fd_flags = true;
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    return fcntl(terminal->input_fd, F_SETFL, flags | O_NONBLOCK);
}

static int
wait_for_protocol_response(kittykb_terminal *terminal, int timeout_ms)
{
    const int64_t deadline = monotonic_milliseconds() + timeout_ms;

    while (kittykb_input_protocol_support(&terminal->input) ==
           KITTYKB_SUPPORT_UNKNOWN) {
        struct pollfd descriptor;
        const int64_t now = monotonic_milliseconds();
        int remaining;
        int result;

        if (now >= deadline) {
            break;
        }
        remaining = (int)(deadline - now);
        descriptor.fd = terminal->input_fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        do {
            result = poll(&descriptor, 1u, remaining);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            return -1;
        }
        if (result == 0) {
            break;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            errno = EIO;
            return -1;
        }
        if ((descriptor.revents & POLLHUP) != 0 &&
            (descriptor.revents & POLLIN) == 0) {
            break;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP)) != 0 &&
            kittykb_terminal_read(terminal) < 0) {
            return -1;
        }
    }
    kittykb_input_finish_probe(&terminal->input);
    return 0;
}

int
kittykb_terminal_start(
    kittykb_terminal *terminal,
    int input_fd,
    int output_fd,
    const kittykb_terminal_options *options)
{
    kittykb_terminal_options defaults;
    const kittykb_terminal_options *selected = options;
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
        kittykb_terminal_options_init(&defaults);
        selected = &defaults;
    }
    if (selected->probe_timeout_ms < 0 || selected->escape_timeout_ms < 0) {
        errno = EINVAL;
        return -1;
    }
    if (selected->require_tty &&
        (!isatty(input_fd) || !isatty(output_fd))) {
        errno = ENOTTY;
        return -1;
    }

    kittykb_input_init(&terminal->input);
    kittykb_input_set_requested_flags(&terminal->input, selected->flags);
    terminal->input_fd = input_fd;
    terminal->output_fd = output_fd;
    terminal->escape_timeout_ms = selected->escape_timeout_ms;
    terminal->escape_pending_since_ms = -1;
    terminal->active = true;

    if (selected->make_raw && make_terminal_raw(terminal) != 0) {
        goto fail;
    }
    if (selected->make_nonblocking && make_input_nonblocking(terminal) != 0) {
        goto fail;
    }
    if (kittykb_terminal_push_flags(output_fd, selected->flags) != 0) {
        goto fail;
    }
    terminal->keyboard_mode_pushed = true;
    if (kittykb_terminal_query_flags(output_fd) != 0) {
        goto fail;
    }
    if (selected->probe_timeout_ms > 0 &&
        wait_for_protocol_response(
            terminal,
            selected->probe_timeout_ms) != 0) {
        goto fail;
    }
    if (selected->probe_timeout_ms == 0) {
        kittykb_input_finish_probe(&terminal->input);
    }
    return 0;

fail:
    failure = errno;
    (void)restore_terminal(terminal);
    errno = failure;
    return -1;
}

static bool
input_is_immediately_readable(int fd)
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

int
kittykb_terminal_read(kittykb_terminal *terminal)
{
    unsigned char buffer[4096];
    int total = 0;
    bool read_any = false;

    if (terminal == NULL || !terminal->active) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        const ssize_t count = read(
            terminal->input_fd,
            buffer,
            sizeof(buffer));

        if (count > 0) {
            read_any = true;
            kittykb_input_feed(&terminal->input, buffer, (size_t)count);
            if (count > INT_MAX - total) {
                total = INT_MAX;
            } else {
                total += (int)count;
            }
            if (!input_is_immediately_readable(terminal->input_fd)) {
                break;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        break;
    }

    if (kittykb_input_has_pending_escape(&terminal->input)) {
        const int64_t now = monotonic_milliseconds();

        if (read_any || terminal->escape_pending_since_ms < 0) {
            terminal->escape_pending_since_ms = now;
        } else if (now - terminal->escape_pending_since_ms >=
                   terminal->escape_timeout_ms) {
            kittykb_input_flush_escape(&terminal->input);
            terminal->escape_pending_since_ms = -1;
        }
    } else {
        terminal->escape_pending_since_ms = -1;
    }
    return total;
}

int
kittykb_terminal_stop(kittykb_terminal *terminal)
{
    if (terminal == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!terminal->active) {
        return 0;
    }
    return restore_terminal(terminal);
}
