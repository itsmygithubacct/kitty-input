#include "kitty_input_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/joystick.h>
#endif

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

static bool append_control(kittyin_terminal *terminal, const char *control)
{
    const size_t length = strlen(control);

    if (length > sizeof terminal->mode_restore - terminal->mode_restore_length)
        return false;
    (void)memcpy(terminal->mode_restore + terminal->mode_restore_length,
                 control, length);
    terminal->mode_restore_length += length;
    return true;
}

static bool build_mode_restore(kittyin_terminal *terminal)
{
    terminal->mode_restore_length = 0u;
    if (terminal->bracketed_paste &&
        !append_control(terminal, "\x1b[?2004l")) return false;
    if (terminal->focus_events &&
        !append_control(terminal, "\x1b[?1004l")) return false;
    if (terminal->mouse_tracking != KITTYIN_MOUSE_TRACKING_OFF) {
        if (terminal->pixel_mouse &&
            !append_control(terminal, "\x1b[?1016l")) return false;
        if (!append_control(terminal, "\x1b[?1006l")) return false;
        if (terminal->mouse_tracking == KITTYIN_MOUSE_TRACKING_BUTTON)
            return append_control(terminal, "\x1b[?1000l");
        if (terminal->mouse_tracking == KITTYIN_MOUSE_TRACKING_DRAG)
            return append_control(terminal, "\x1b[?1002l");
        return append_control(terminal, "\x1b[?1003l");
    }
    return true;
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
    int result = 0;

    if (terminal->keyboard.output_fd >= 0 &&
        terminal->mode_restore_length > 0u)
        result = write_all(terminal->keyboard.output_fd,
                           terminal->mode_restore,
                           terminal->mode_restore_length);
    terminal->modes_enabled = false;
    return result;
}

static bool claim_restore(const kittyin_terminal *terminal)
{
    return !__sync_lock_test_and_set(
        (volatile sig_atomic_t *)&terminal->restore_claimed, 1);
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
    terminal->restore_claimed = 0;
    if (!build_mode_restore(terminal)) {
        failure = EOVERFLOW;
        (void)kittykb_terminal_stop(&terminal->keyboard);
        errno = failure;
        return -1;
    }
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
    bool restore_output;

    if (terminal == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!terminal->active) return 0;
    restore_output = claim_restore(terminal);
    if (restore_output) {
        if (disable_modes(terminal) != 0) first_error = errno;
    } else {
        /* The emergency path already emitted both restore controls.  Suppress
         * a second keyboard-stack pop while retaining normal fd/termios
         * cleanup in kittykb_terminal_stop(). */
        terminal->modes_enabled = false;
        terminal->keyboard.keyboard_mode_pushed = false;
    }
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
    static const char keyboard_pop[] = "\x1b[<u";

    if (terminal != NULL && terminal->active &&
        terminal->keyboard.output_fd >= 0 && claim_restore(terminal)) {
        if (terminal->mode_restore_length > 0u)
            (void)write(terminal->keyboard.output_fd,
                        terminal->mode_restore,
                        terminal->mode_restore_length);
        if (terminal->keyboard.keyboard_mode_pushed)
            (void)write(terminal->keyboard.output_fd, keyboard_pop,
                        sizeof keyboard_pop - 1u);
    }
}

static void emit_gamepad_event(kittyin_gamepads *gamepads,
                               const kittyin_gamepad_event *event)
{
    size_t tail;

    if (gamepads == NULL || event == NULL) return;
    if (gamepads->event_count >=
        KITTYIN_GAMEPAD_EVENT_QUEUE_CAPACITY) {
        ++gamepads->dropped_events;
        if (event->kind != KITTYIN_GAMEPAD_DISCONNECTED) return;

        /* Losing a motion/button sample is recoverable; losing disconnect
         * can leave every downstream action from that device stuck.  Retain
         * the newest lifecycle truth by evicting the oldest queued event. */
        gamepads->event_head =
            (gamepads->event_head + 1u) %
            KITTYIN_GAMEPAD_EVENT_QUEUE_CAPACITY;
        --gamepads->event_count;
    }
    tail = (gamepads->event_head + gamepads->event_count) %
           KITTYIN_GAMEPAD_EVENT_QUEUE_CAPACITY;
    gamepads->events[tail] = *event;
    ++gamepads->event_count;
}

void kittyin_gamepads_init(kittyin_gamepads *gamepads)
{
    uint8_t index;

    if (gamepads == NULL) return;
    (void)memset(gamepads, 0, sizeof *gamepads);
    for (index = 0u; index < KITTYIN_GAMEPAD_CAPACITY; ++index)
        gamepads->devices[index].fd = -1;
    (void)snprintf(gamepads->directory, sizeof gamepads->directory,
                   "%s", "/dev/input");
    gamepads->scan_interval_ms = 500u;
}

bool kittyin_gamepads_set_directory(kittyin_gamepads *gamepads,
                                    const char *directory)
{
    const size_t length = directory != NULL ? strlen(directory) : 0u;

    if (gamepads == NULL || length == 0u ||
        length >= sizeof gamepads->directory) return false;
    (void)memcpy(gamepads->directory, directory, length + 1u);
    gamepads->next_scan_ms = 0;
    return true;
}

bool kittyin_gamepads_set_scan_interval(kittyin_gamepads *gamepads,
                                        uint32_t interval_ms)
{
    if (gamepads == NULL || interval_ms < 10u ||
        interval_ms > 60000u) return false;
    gamepads->scan_interval_ms = interval_ms;
    return true;
}

bool kittyin_gamepads_attach_fd(kittyin_gamepads *gamepads, uint8_t gamepad,
                                int fd, const char *name, uint8_t axes,
                                uint8_t buttons)
{
    kittyin_gamepad_device *device;
    int flags;
    int descriptor_flags;
    int failure;

    if (gamepads == NULL || gamepad >= KITTYIN_GAMEPAD_CAPACITY ||
        fd < 0) {
        errno = EINVAL;
        return false;
    }
    device = &gamepads->devices[gamepad];
    if (device->connected) {
        errno = EBUSY;
        return false;
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0) return false;
    descriptor_flags = fcntl(fd, F_GETFD);
    if (descriptor_flags < 0) return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        return false;
    if (fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        failure = errno;
        (void)fcntl(fd, F_SETFL, flags);
        errno = failure;
        return false;
    }
    device->fd = fd;
    device->axes = axes;
    device->buttons = buttons;
    device->connected = true;
    (void)snprintf(device->name, sizeof device->name, "%s",
                   name != NULL && name[0] != '\0' ?
                   name : "Linux joystick");
    emit_gamepad_event(
        gamepads,
        &(kittyin_gamepad_event){
            KITTYIN_GAMEPAD_CONNECTED, gamepad, 0u, 0, false
        });
    return true;
}

void kittyin_gamepads_detach(kittyin_gamepads *gamepads, uint8_t gamepad)
{
    kittyin_gamepad_device *device;

    if (gamepads == NULL || gamepad >= KITTYIN_GAMEPAD_CAPACITY)
        return;
    device = &gamepads->devices[gamepad];
    if (!device->connected) return;
    if (device->fd >= 0) (void)close(device->fd);
    *device = (kittyin_gamepad_device){.fd = -1};
    emit_gamepad_event(
        gamepads,
        &(kittyin_gamepad_event){
            KITTYIN_GAMEPAD_DISCONNECTED, gamepad, 0u, 0, false
        });
}

#ifdef __linux__
static void scan_gamepads(kittyin_gamepads *gamepads)
{
    uint8_t gamepad;

    for (gamepad = 0u; gamepad < KITTYIN_GAMEPAD_CAPACITY; ++gamepad) {
        kittyin_gamepad_device *device = &gamepads->devices[gamepad];
        char path[KITTYIN_GAMEPAD_PATH_CAPACITY];
        char name[KITTYIN_GAMEPAD_NAME_CAPACITY] = {0};
        unsigned char axes = 0u;
        unsigned char buttons = 0u;
        int fd;
        int length;

        if (device->connected) continue;
        length = snprintf(path, sizeof path, "%s/js%u",
                          gamepads->directory, (unsigned int)gamepad);
        if (length < 0 || (size_t)length >= sizeof path) continue;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, JSIOCGAXES, &axes) != 0 ||
            ioctl(fd, JSIOCGBUTTONS, &buttons) != 0) {
            (void)close(fd);
            continue;
        }
        if (ioctl(fd, JSIOCGNAME(sizeof name), name) < 0)
            (void)snprintf(name, sizeof name, "%s", "Linux joystick");
        if (!kittyin_gamepads_attach_fd(
                gamepads, gamepad, fd, name,
                (uint8_t)axes, (uint8_t)buttons)) {
            (void)close(fd);
            continue;
        }
        (void)snprintf(device->path, sizeof device->path, "%s", path);
    }
}

static void read_gamepad(kittyin_gamepads *gamepads, uint8_t gamepad)
{
    kittyin_gamepad_device *device = &gamepads->devices[gamepad];

    while (device->connected) {
        struct js_event source;
        ssize_t count = read(device->fd, &source, sizeof source);

        if (count == (ssize_t)sizeof source) {
            const unsigned int type =
                (unsigned int)source.type &
                (unsigned int)~JS_EVENT_INIT;
            kittyin_gamepad_event event = {
                type == JS_EVENT_BUTTON ?
                    KITTYIN_GAMEPAD_BUTTON : KITTYIN_GAMEPAD_AXIS,
                gamepad, source.number, source.value,
                ((unsigned int)source.type & JS_EVENT_INIT) != 0u
            };

            if (type == JS_EVENT_BUTTON || type == JS_EVENT_AXIS)
                emit_gamepad_event(gamepads, &event);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        kittyin_gamepads_detach(gamepads, gamepad);
    }
}
#endif

void kittyin_gamepads_poll(kittyin_gamepads *gamepads, int64_t now_ms)
{
    if (gamepads == NULL || now_ms < 0) return;
    if (now_ms >= gamepads->next_scan_ms) {
#ifdef __linux__
        scan_gamepads(gamepads);
#endif
        gamepads->next_scan_ms =
            now_ms > INT64_MAX - (int64_t)gamepads->scan_interval_ms ?
            INT64_MAX :
            now_ms + (int64_t)gamepads->scan_interval_ms;
    }
#ifdef __linux__
    {
        uint8_t gamepad;

        for (gamepad = 0u;
             gamepad < KITTYIN_GAMEPAD_CAPACITY; ++gamepad)
            if (gamepads->devices[gamepad].connected)
                read_gamepad(gamepads, gamepad);
    }
#endif
}

bool kittyin_gamepads_next(kittyin_gamepads *gamepads,
                           kittyin_gamepad_event *event)
{
    if (gamepads == NULL || event == NULL ||
        gamepads->event_count == 0u) return false;
    *event = gamepads->events[gamepads->event_head];
    gamepads->event_head =
        (gamepads->event_head + 1u) %
        KITTYIN_GAMEPAD_EVENT_QUEUE_CAPACITY;
    --gamepads->event_count;
    return true;
}

void kittyin_gamepads_close(kittyin_gamepads *gamepads)
{
    uint8_t gamepad;

    if (gamepads == NULL) return;
    for (gamepad = 0u; gamepad < KITTYIN_GAMEPAD_CAPACITY; ++gamepad) {
        kittyin_gamepad_device *device = &gamepads->devices[gamepad];

        if (device->connected && device->fd >= 0)
            (void)close(device->fd);
        *device = (kittyin_gamepad_device){.fd = -1};
    }
    gamepads->event_head = 0u;
    gamepads->event_count = 0u;
}

uint64_t kittyin_gamepads_dropped_events(const kittyin_gamepads *gamepads)
{
    return gamepads != NULL ? gamepads->dropped_events : 0u;
}
