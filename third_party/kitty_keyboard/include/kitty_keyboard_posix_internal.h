/* Shared POSIX helpers for the kitty-keyboard library layers.
 * Internal: not installed; include only from library translation units. */
#ifndef KITTY_KEYBOARD_POSIX_INTERNAL_H
#define KITTY_KEYBOARD_POSIX_INTERNAL_H

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static inline int64_t
monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static inline int
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

static inline bool
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

#endif
