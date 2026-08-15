#include "kitty_input.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SAMPLE_COUNT 7u

typedef uint64_t (*benchmark_function)(size_t iterations);

static volatile uint64_t benchmark_sink;
static bool assert_budgets;
static bool budget_exceeded;

static uint64_t nanoseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static uint64_t drain_input(kittyin_input *input)
{
    kittyin_event event;
    uint64_t checksum = 0u;

    while (kittyin_input_next(input, &event)) {
        checksum += (uint64_t)event.kind;
        if (event.kind == KITTYIN_EVENT_KEY)
            checksum += event.data.key.key;
        else if (event.kind == KITTYIN_EVENT_PASTE)
            checksum += event.data.paste.length;
    }
    return checksum;
}

static uint64_t benchmark_ascii_bulk(size_t iterations)
{
    unsigned char bytes[KITTYKB_EVENT_QUEUE_CAPACITY];
    kittyin_input input;
    uint64_t checksum = 0u;
    size_t iteration;

    (void)memset(bytes, 'a', sizeof bytes);
    kittyin_input_init(&input);
    for (iteration = 0u; iteration < iterations; ++iteration) {
        kittyin_input_feed(&input, bytes, sizeof bytes);
        checksum += drain_input(&input);
    }
    return checksum;
}

static uint64_t benchmark_ascii_fragmented(size_t iterations)
{
    static const unsigned char byte = 'a';
    kittyin_input input;
    uint64_t checksum = 0u;
    size_t iteration;

    kittyin_input_init(&input);
    for (iteration = 0u; iteration < iterations; ++iteration) {
        kittyin_input_feed(&input, &byte, 1u);
        checksum += drain_input(&input);
    }
    return checksum;
}

static uint64_t benchmark_mixed_wire(size_t iterations)
{
    static const char bytes[] =
        "a"
        "\x1b[<20;123;456M"
        "\x1b[97;1:3u"
        "\x1b[I"
        "\x1b[200~paste payload\x1b[201~";
    kittyin_input input;
    uint64_t checksum = 0u;
    size_t iteration;

    kittyin_input_init(&input);
    for (iteration = 0u; iteration < iterations; ++iteration) {
        kittyin_input_feed(&input, bytes, sizeof bytes - 1u);
        checksum += drain_input(&input);
    }
    return checksum;
}

static uint64_t benchmark_paste(size_t iterations)
{
    unsigned char bytes[6u + 4096u + 6u];
    kittyin_input input;
    uint64_t checksum = 0u;
    size_t iteration;

    (void)memcpy(bytes, "\x1b[200~", 6u);
    (void)memset(bytes + 6u, 'p', 4096u);
    (void)memcpy(bytes + 6u + 4096u, "\x1b[201~", 6u);
    kittyin_input_init(&input);
    for (iteration = 0u; iteration < iterations; ++iteration) {
        kittyin_input_feed(&input, bytes, sizeof bytes);
        checksum += drain_input(&input);
    }
    return checksum;
}

static void fill_key_map(kittyin_action_map *map)
{
    size_t binding;

    kittyin_action_map_init(map);
    for (binding = 0u;
         binding < KITTYIN_ACTION_BINDING_CAPACITY; ++binding)
        (void)kittyin_action_map_bind_key(
            map, (uint16_t)(binding % KITTYIN_ACTION_CAPACITY),
            (uint32_t)(0x100u + binding), 0u);
}

static uint64_t benchmark_key_map(size_t iterations)
{
    kittyin_action_event output[1];
    kittyin_action_map map;
    kittykb_event key = {0};
    uint64_t checksum = 0u;
    size_t iteration;

    fill_key_map(&map);
    key.key = 0x100u + KITTYIN_ACTION_BINDING_CAPACITY - 1u;
    key.action = KITTYKB_ACTION_PRESS;
    for (iteration = 0u; iteration < iterations; ++iteration)
        checksum += kittyin_action_map_key(&map, &key, output, 1u);
    return checksum;
}

static void fill_gamepad_map(kittyin_action_map *map)
{
    size_t binding;

    kittyin_action_map_init(map);
    for (binding = 0u;
         binding < KITTYIN_ACTION_BINDING_CAPACITY; ++binding)
        (void)kittyin_action_map_bind_gamepad_button(
            map, (uint16_t)(binding % KITTYIN_ACTION_CAPACITY),
            (uint8_t)(binding % KITTYIN_GAMEPAD_CAPACITY),
            (uint8_t)binding);
}

static uint64_t benchmark_gamepad_map(size_t iterations)
{
    kittyin_action_event output[1];
    kittyin_gamepad_event event = {
        KITTYIN_GAMEPAD_BUTTON, 3u,
        KITTYIN_ACTION_BINDING_CAPACITY - 1u, 0, false
    };
    kittyin_action_map map;
    uint64_t checksum = 0u;
    size_t iteration;

    fill_gamepad_map(&map);
    for (iteration = 0u; iteration < iterations; ++iteration) {
        event.value = (int16_t)((iteration & 1u) == 0u ? 1 : 0);
        checksum += kittyin_action_map_gamepad(&map, &event, output, 1u);
    }
    return checksum;
}

static uint64_t benchmark_maximum_advance(size_t iterations)
{
    kittyin_action_event output[1];
    kittyin_gamepad_event event = {
        KITTYIN_GAMEPAD_BUTTON, 0u, 0u, 1, false
    };
    uint64_t checksum = 0u;
    size_t iteration;

    for (iteration = 0u; iteration < iterations; ++iteration) {
        kittyin_action_map map;

        kittyin_action_map_init(&map);
        (void)kittyin_action_map_set_repeat(&map, 50u, 16u);
        (void)kittyin_action_map_bind_gamepad_button(
            &map, 0u, KITTYIN_GAMEPAD_ANY, 0u);
        checksum += kittyin_action_map_gamepad(
            &map, &event, output, 1u);
        checksum += kittyin_action_map_advance(
            &map, UINT32_MAX, output, 1u);
    }
    return checksum;
}

static void sort_samples(uint64_t *samples)
{
    size_t index;

    for (index = 1u; index < SAMPLE_COUNT; ++index) {
        const uint64_t value = samples[index];
        size_t position = index;

        while (position > 0u && samples[position - 1u] > value) {
            samples[position] = samples[position - 1u];
            --position;
        }
        samples[position] = value;
    }
}

static void run_benchmark(const char *name, benchmark_function function,
                          size_t iterations, size_t units_per_iteration,
                          double budget_ns)
{
    uint64_t samples[SAMPLE_COUNT];
    double median;
    size_t sample;

    for (sample = 0u; sample < SAMPLE_COUNT; ++sample) {
        const uint64_t begin = nanoseconds();
        const uint64_t checksum = function(iterations);
        const uint64_t end = nanoseconds();

        benchmark_sink += checksum;
        samples[sample] = end - begin;
    }
    sort_samples(samples);
    median = (double)samples[SAMPLE_COUNT / 2u] /
             ((double)iterations * (double)units_per_iteration);
    (void)printf("%-24s %10.2f ns/unit\n", name, median);

    /* Budgets are deliberately far above recorded medians: they must
     * tolerate host-to-host variance and background load while still
     * failing on algorithmic cliffs, which are orders of magnitude. */
    if (assert_budgets && median > budget_ns) {
        (void)fprintf(stderr,
                      "budget exceeded: %s %.2f > %.2f ns/unit\n",
                      name, median, budget_ns);
        budget_exceeded = true;
    }
}

static void run_parser_benchmarks(void)
{
    run_benchmark("ASCII bulk", benchmark_ascii_bulk, 20000u,
                  KITTYKB_EVENT_QUEUE_CAPACITY, 800.0);
    run_benchmark("ASCII fragmented", benchmark_ascii_fragmented,
                  1000000u, 1u, 1000.0);
    run_benchmark("mixed wire", benchmark_mixed_wire, 200000u, 1u,
                  6000.0);
    run_benchmark("paste", benchmark_paste, 20000u, 4096u, 10.0);
}

int main(int argc, char **argv)
{
    bool actions_only = false;
    bool parser_only = false;
    int argument;

    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "actions") == 0)
            actions_only = true;
        else if (strcmp(argv[argument], "parser") == 0)
            parser_only = true;
        else if (strcmp(argv[argument], "--assert") == 0)
            assert_budgets = true;
    }
    if (!actions_only) run_parser_benchmarks();
    if (parser_only) {
        (void)printf("checksum: %" PRIu64 "\n", benchmark_sink);
        return budget_exceeded ? 1 : 0;
    }
    run_benchmark("key map (96 bindings)", benchmark_key_map,
                  2000000u, 1u, 5000.0);
    run_benchmark("gamepad (96 bindings)", benchmark_gamepad_map,
                  2000000u, 1u, 2000.0);
    run_benchmark("maximum repeat step", benchmark_maximum_advance,
                  1u, 1u, 10000.0);
    (void)printf("checksum: %" PRIu64 "\n", benchmark_sink);
    return budget_exceeded ? 1 : 0;
}
