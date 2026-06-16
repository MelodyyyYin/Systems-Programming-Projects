# Cache Lab

This repository contains a Cache Lab solution workspace for the 15-213 / 18-213 cache assignment.
The code is split into two parts:

- `csim.c`: a cache simulator that parses the standard `csim` command line, processes traces, and
  reports hit/miss/eviction statistics through `printSummary()`.
- `trans.c`: matrix-transpose implementations, including the submitted `transpose_submit()` and a
  few reference helpers for validation and comparison.

## Repository Layout

- `cachelab.c`, `cachelab.h`: shared helpers, statistics output, and transpose harness definitions.
- `test-csim.c`: correctness checker for the cache simulator.
- `test-trans.c`: performance and correctness checker for transpose implementations.
- `test-trans-simple.c`: sanitizer-friendly transpose checker.
- `driver.py`: end-to-end autograder driver used by the lab.
- `traces-driver.py`: validator for student-written trace files.
- `traces/csim/`: reference traces used to test `csim`.
- `traces/traces/`: student trace files for the trace-writing portion of the lab.

## Build

Use the provided makefile:

```bash
make
```

Useful targets:

- `make test-csim`
- `make test-trans`
- `make test-trans-simple`
- `make clean`

## Verification

Run the lab checks directly:

```bash
./test-csim
./test-trans -s -M 32 -N 32
./test-trans -s -M 1024 -N 1024 -l
./driver.py
./traces-driver.py -D
```

The autograder harness expects `printSummary()` to write `.csim_results`, and the transpose tests
look for `TEST_TRANS_RESULTS=...` in the output of `test-trans`.

Locally validated results in this tree:

- `./test-csim` reports `TEST_CSIM_RESULTS=60`.
- `./traces-driver.py -D` reports `TRACES_TOTAL: 10`.
- `./test-trans -s -M 32 -N 32` reports `TEST_TRANS_RESULTS=1:35456`.

## Implementation Notes

- `csim.c` currently supports `-h`, `-v`, `-s`, `-E`, `-b`, and `-t`, validates trace formatting,
  and simulates an LRU write-back cache with dirty-byte accounting.
- `transpose_submit()` in `trans.c` uses an 8x8 blocked strategy for the 32x32 case and a
  special-case 8x8 strategy for 1024x1024, then falls back to a straightforward transpose for all
  other sizes.
- The helper transpose functions in `trans.c` are kept around for comparison and debugging.
