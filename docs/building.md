# Building and testing kitty-input

Build, test, sanitizer, benchmark and fuzz instructions, including the bundled keyboard component's own targets, moved here from the README.

## Build and test

```sh
git clone https://github.com/itsmygithubacct/kitty-input.git
cd kitty-input
make test
make sanitize
make benchmark
make fuzz FUZZ_TIME=30
```

`make` builds `build/libkitty-input.a`, `build/libkitty-input.so`, and the
`build/events` example. Run the example in a terminal and press `q` to exit.

The imported keyboard component retains its standalone build and PTY test
suite:

```sh
make keyboard-all
make keyboard-test
make keyboard-sanitize
```

The benchmark reports median parser and action-map costs. The Clang fuzz
target differentially checks bulk and byte-fragmented feeds and verifies
semantic-action state/source invariants under ASan and UBSan; `FUZZ_TIME` is
the run length in seconds.
