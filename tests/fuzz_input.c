#include "kitty_input.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void require_equal(bool condition)
{
    if (!condition) abort();
}

static unsigned int popcount8(uint8_t value)
{
    unsigned int count = 0u;

    while (value != 0u) {
        count += (unsigned int)(value & UINT8_C(1));
        value = (uint8_t)(value >> 1u);
    }
    return count;
}

static bool bindings_equal(const kittyin_action_binding *left,
                           const kittyin_action_binding *right)
{
    return left->key == right->key &&
           left->modifiers == right->modifiers &&
           left->action == right->action &&
           left->kind == right->kind &&
           left->gamepad == right->gamepad &&
           left->control == right->control &&
           left->direction == right->direction &&
           left->down_gamepads == right->down_gamepads;
}

static void require_action_maps_equal(const kittyin_action_map *left,
                                      const kittyin_action_map *right)
{
    size_t index;

    require_equal(left->binding_count == right->binding_count);
    require_equal(left->repeat_delay_ms == right->repeat_delay_ms);
    require_equal(left->repeat_interval_ms == right->repeat_interval_ms);
    require_equal(left->axis_dead_zone == right->axis_dead_zone);
    require_equal(left->connected_gamepads == right->connected_gamepads);
    require_equal(left->active_gamepad == right->active_gamepad);
    require_equal(left->active_device == right->active_device);
    for (index = 0u; index < left->binding_count; ++index)
        require_equal(bindings_equal(&left->bindings[index],
                                     &right->bindings[index]));
    for (index = 0u; index < KITTYIN_ACTION_CAPACITY; ++index) {
        const kittyin_action_state *left_state = &left->actions[index];
        const kittyin_action_state *right_state = &right->actions[index];

        require_equal(left_state->held_ms == right_state->held_ms);
        require_equal(left_state->next_repeat_ms ==
                      right_state->next_repeat_ms);
        require_equal(left_state->sources == right_state->sources);
        require_equal(left_state->gamepad == right_state->gamepad);
    }
}

static void require_action_invariants(const kittyin_action_map *map)
{
    size_t action;

    for (action = 0u; action < KITTYIN_ACTION_CAPACITY; ++action) {
        const kittyin_action_state *state = &map->actions[action];
        unsigned int sources = 0u;
        bool gamepad_is_live = false;
        size_t binding;

        for (binding = 0u; binding < map->binding_count; ++binding) {
            const kittyin_action_binding *entry = &map->bindings[binding];

            if (entry->action != action ||
                entry->kind == (uint8_t)KITTYIN_BINDING_KEY) continue;
            sources += popcount8(entry->down_gamepads);
            if (state->gamepad < KITTYIN_GAMEPAD_CAPACITY &&
                (entry->down_gamepads &
                 (uint8_t)(UINT8_C(1) << state->gamepad)) != 0u)
                gamepad_is_live = true;
        }
        require_equal(state->sources == sources);
        if (sources == 0u) {
            require_equal(state->held_ms == 0u);
            require_equal(state->next_repeat_ms == 0u);
            require_equal(state->gamepad == 0u);
        } else {
            require_equal(gamepad_is_live);
        }
    }
}

static uint8_t fuzz_byte(const uint8_t *data, size_t size, size_t index)
{
    return index < size ? data[index] : 0u;
}

static void fuzz_actions(const uint8_t *data, size_t size)
{
    kittyin_action_event output[4];
    kittyin_action_map map;
    kittyin_action_map shadow;
    const size_t limit = size < 256u ? size : 256u;
    size_t action;
    size_t index;

    kittyin_action_map_init(&map);
    for (action = 0u; action < 16u; ++action) {
        (void)kittyin_action_map_bind_key(
            &map, (uint16_t)action, (uint32_t)'a' + (uint32_t)action, 0u);
        (void)kittyin_action_map_bind_gamepad_button(
            &map, (uint16_t)action, KITTYIN_GAMEPAD_ANY,
            (uint8_t)(action % 8u));
        (void)kittyin_action_map_bind_gamepad_axis(
            &map, (uint16_t)action,
            (uint8_t)(action % KITTYIN_GAMEPAD_CAPACITY),
            (uint8_t)(action % 4u), (action & 1u) == 0u ? -1 : 1);
    }
    shadow = map;

    for (index = 0u; index < limit; index += 8u) {
        const uint8_t operation = fuzz_byte(data, size, index) % 6u;
        const size_t capacity =
            (size_t)(fuzz_byte(data, size, index + 1u) % 5u);

        if (operation == 0u) {
            kittyin_gamepad_event event;

            event.kind = (kittyin_gamepad_event_kind)(
                1u + fuzz_byte(data, size, index + 2u) % 4u);
            event.gamepad =
                fuzz_byte(data, size, index + 3u) %
                KITTYIN_GAMEPAD_CAPACITY;
            event.control = fuzz_byte(data, size, index + 4u) % 8u;
            event.value = (int16_t)(
                (uint16_t)fuzz_byte(data, size, index + 5u) |
                ((uint16_t)fuzz_byte(data, size, index + 6u) << 8u));
            event.initial =
                (fuzz_byte(data, size, index + 7u) & 1u) != 0u;
            require_equal(kittyin_action_map_gamepad(
                              &map, &event, output, capacity) <= capacity);
            (void)kittyin_action_map_gamepad(
                &shadow, &event, NULL, 0u);
        } else if (operation == 1u) {
            const uint32_t elapsed =
                (uint32_t)fuzz_byte(data, size, index + 2u) |
                ((uint32_t)fuzz_byte(data, size, index + 3u) << 8u) |
                ((uint32_t)fuzz_byte(data, size, index + 4u) << 16u) |
                ((uint32_t)fuzz_byte(data, size, index + 5u) << 24u);

            require_equal(kittyin_action_map_advance(
                              &map, elapsed, output, capacity) <= capacity);
            (void)kittyin_action_map_advance(
                &shadow, elapsed, NULL, 0u);
        } else if (operation == 2u) {
            require_equal(kittyin_action_map_release_all(
                              &map, output, capacity) <= capacity);
            (void)kittyin_action_map_release_all(&shadow, NULL, 0u);
        } else if (operation == 3u) {
            kittykb_event key = {0};

            key.key = fuzz_byte(data, size, index + 2u);
            key.modifiers =
                (uint32_t)(fuzz_byte(data, size, index + 3u) & 0x3fu);
            key.action = (kittykb_action)(
                1u + fuzz_byte(data, size, index + 4u) % 3u);
            require_equal(kittyin_action_map_key(
                              &map, &key, output, capacity) <= capacity);
            (void)kittyin_action_map_key(&shadow, &key, NULL, 0u);
        } else if (operation == 4u) {
            const uint16_t selected = (uint16_t)(
                fuzz_byte(data, size, index + 2u) % 16u);
            const kittyin_device_kind device =
                (fuzz_byte(data, size, index + 3u) & 1u) == 0u ?
                KITTYIN_DEVICE_KEYBOARD : KITTYIN_DEVICE_GAMEPAD;

            require_equal(kittyin_action_map_unbind(
                              &map, selected, device) ==
                          kittyin_action_map_unbind(
                              &shadow, selected, device));
        } else {
            const uint32_t delay = UINT32_C(50) +
                fuzz_byte(data, size, index + 2u);
            const uint32_t interval = UINT32_C(16) +
                fuzz_byte(data, size, index + 3u);
            const int dead_zone = 1 +
                (int)(((unsigned int)fuzz_byte(data, size, index + 4u) << 7u) |
                      fuzz_byte(data, size, index + 5u));

            require_equal(kittyin_action_map_set_repeat(
                              &map, delay, interval) ==
                          kittyin_action_map_set_repeat(
                              &shadow, delay, interval));
            require_equal(kittyin_action_map_set_dead_zone(
                              &map, dead_zone) ==
                          kittyin_action_map_set_dead_zone(
                              &shadow, dead_zone));
        }
    }
    require_action_maps_equal(&map, &shadow);
    require_action_invariants(&map);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    kittyin_input bulk;
    kittyin_input fragmented;
    kittyin_event bulk_event;
    kittyin_event fragmented_event;
    bool bulk_has_event;
    bool fragmented_has_event;
    size_t index;

    kittyin_input_init(&bulk);
    kittyin_input_init(&fragmented);
    kittyin_input_feed(&bulk, data, size);
    for (index = 0u; index < size; ++index)
        kittyin_input_feed(&fragmented, data + index, 1u);

    do {
        bulk_has_event = kittyin_input_next(&bulk, &bulk_event);
        fragmented_has_event =
            kittyin_input_next(&fragmented, &fragmented_event);
        require_equal(bulk_has_event == fragmented_has_event);
        if (bulk_has_event)
            require_equal(memcmp(&bulk_event, &fragmented_event,
                                 sizeof bulk_event) == 0);
    } while (bulk_has_event);

    require_equal(memcmp(&bulk.stats, &fragmented.stats,
                         sizeof bulk.stats) == 0);
    require_equal(bulk.sequence_length == fragmented.sequence_length);
    require_equal(bulk.paste_length == fragmented.paste_length);
    require_equal(bulk.paste_match_length ==
                  fragmented.paste_match_length);
    require_equal(bulk.parser_state == fragmented.parser_state);
    require_equal(memcmp(bulk.sequence, fragmented.sequence,
                         sizeof bulk.sequence) == 0);
    require_equal(memcmp(bulk.paste, fragmented.paste,
                         sizeof bulk.paste) == 0);
    require_equal(memcmp(&bulk.keyboard_storage,
                         &fragmented.keyboard_storage,
                         sizeof bulk.keyboard_storage) == 0);
    fuzz_actions(data, size);
    return 0;
}
