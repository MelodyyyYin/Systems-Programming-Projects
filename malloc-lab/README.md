# Malloc Lab

This repository contains a CS:APP malloc-lab allocator implementation in `mm.c` together with the local driver, traces, and support files needed to build and test it.

## Build

```bash
make
```

Useful cleanup target:

```bash
make clean
```

## Run

- `./mdriver -V` runs the full trace suite for correctness, utilization, and throughput.
- `./mdriver-dbg -d 2 -f traces/syn-array-short.rep` is useful for debugging.
- `./mdriver-emulate -V -f traces/syn-giantarray.rep` checks the 64-bit / sparse-emulation path.

## Current repo checks

These are reproducible in this checkout:

- `make` completes successfully.
- `./mdriver -V`
  - 26/26 traces pass
  - harmonic mean utilization: `74.0%`
  - harmonic mean throughput: `5608 Kops/sec`
  - perf index: `99.9/100`
- `./mdriver-emulate -V -f traces/syn-giantarray.rep`
  - trace passes
  - utilization: `97.0%`

## Layout

- `mm.c`: allocator submission target
- `mdriver.c`, `memlib.c`, `tracefile.c`: local driver support
- `traces/`: benchmark traces used by the driver
- `Makefile`: build rules for the normal, debug, and emulate drivers

## Notes

- The lab submission is typically `mm.c` only.
- Build outputs such as `mdriver`, `mdriver-dbg`, `mdriver-emulate`, `*.o`, `*.bc`, and `*.ll` are generated locally by `make`.
