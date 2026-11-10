/* Print every event the C decoder produces for stdin, one per line, in the
 * format python/tests/test_differential.py prints for the Python decoder.
 * argv[1] = split offset (feed [0,cut) then [cut,n)); argv[2] = "pixels". */
#include "kitty_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_event(const kittyin_event *e)
{
    size_t i;
    switch (e->kind) {
    case KITTYIN_EVENT_KEY:
        printf("key %u %u %u %u %u %u %u", e->data.key.key,
               e->data.key.modifiers, (unsigned)e->data.key.action,
               e->data.key.shifted_key, e->data.key.base_layout_key,
               (unsigned)e->data.key.source,
               (unsigned)e->data.key.event_type_explicit);
        for (i = 0; i < e->data.key.text_length; ++i)
            printf(" %u", e->data.key.text[i]);
        break;
    case KITTYIN_EVENT_MOUSE:
        printf("mouse %d %d %u %u %u %d %d %u %u", e->data.mouse.x,
               e->data.mouse.y, e->data.mouse.button, e->data.mouse.action,
               e->data.mouse.modifiers, e->data.mouse.wheel_x,
               e->data.mouse.wheel_y, e->data.mouse.raw_button,
               (unsigned)e->data.mouse.pixel_coordinates);
        break;
    case KITTYIN_EVENT_FOCUS:
        printf("focus %u", (unsigned)e->data.focus.focused);
        break;
    case KITTYIN_EVENT_PASTE:
        printf("paste %u", (unsigned)e->data.paste.final);
        for (i = 0; i < e->data.paste.length; ++i)
            printf(" %u", e->data.paste.bytes[i]);
        break;
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    static unsigned char buf[65536];
    size_t n = fread(buf, 1, sizeof buf, stdin);
    size_t cut = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : n;
    kittyin_input input;
    kittyin_event event;
    if (cut > n) cut = n;
    kittyin_input_init(&input);
    kittyin_input_set_pixel_coordinates(&input, argc > 2 && !strcmp(argv[2], "pixels"));
    kittyin_input_feed(&input, buf, cut);
    while (kittyin_input_next(&input, &event)) print_event(&event);
    kittyin_input_feed(&input, buf + cut, n - cut);
    while (kittyin_input_next(&input, &event)) print_event(&event);
    kittyin_input_flush_escape(&input);
    while (kittyin_input_next(&input, &event)) print_event(&event);
    return 0;
}
