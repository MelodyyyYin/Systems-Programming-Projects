# Proxy Lab Repository

This repository contains a full solution to the CS:APP Proxy Lab, including the
checkpoint proxy, the final concurrent cached proxy, and the test plan used
during implementation.

## What is in this repo

- `proxy.c`: the proxy implementation
- `csapp.c`, `csapp.h`: CS:APP helper library code used by the proxy
- `Makefile`: build rules for `proxy`
- `driver.sh`: checkpoint and final validation entry point
- `tests/`: Pxydrive test scripts used by the lab infrastructure
- `tiny/`: the Tiny web server from the CS:APP text
- `CHECKPOINT_PLAN.md`: the step-by-step checkpoint plan used during coding

## Current implementation

The proxy currently:

- accepts client connections
- parses and forwards HTTP `GET` requests
- normalizes outbound request headers
- handles malformed input and broken connections safely
- serves requests concurrently using one thread per connection
- caches origin responses in an in-memory LRU cache
- passes the full `driver.sh` test suite

## Build

From the repository root:

```bash
make clean
make proxy
```

To build the Tiny server too:

```bash
make all
```

## Test

Checkpoint tests:

```bash
./driver.sh check
```

Final tests:

```bash
./driver.sh
```

## Implementation notes

- The proxy ignores `SIGPIPE` so a closed client connection does not terminate
  the process.
- Each accepted connection is handled in a detached thread.
- The cache stores complete responses up to `MAX_OBJECT_SIZE`.
- A global mutex protects the cache metadata and data.
- The cache uses an LRU eviction policy when the total cache size exceeds
  `MAX_CACHE_SIZE`.
- The proxy preserves binary content by forwarding bytes rather than treating
  responses as C strings.

## Files you are most likely to inspect

- `proxy.c`
- `CHECKPOINT_PLAN.md`
- `tests/README.txt`
- `driver.sh`

## Notes

- `README` is the original handout-style file from the lab starter.
- `README.md` is the more detailed repo-oriented documentation.
- `demo.md` is intentionally ignored by git and can be used as a personal
  interview/demo script.

