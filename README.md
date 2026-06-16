# Systems Programming Projects

Selected systems programming work from 18-613, grouped into one portfolio repository.

## Projects

| Project | Focus | Highlights |
| --- | --- | --- |
| [Cache Lab](cache-lab) | Caching and data locality | Cache simulator, trace parsing, blocked matrix transpose, performance-oriented tuning |
| [Proxy Lab](proxy-lab) | Networked systems | Concurrent HTTP proxy, request forwarding, response caching, LRU eviction |
| [Malloc Lab](malloc-lab) | Memory allocation | Explicit allocator, heap management, trace-based tuning, throughput/utilization work |
| [Shell Lab](shell-lab) | Process control | Tiny shell, job control, signal handling, trace-driven validation |

## What to look at

- `cache-lab/trans.c` for transpose strategy work
- `proxy-lab/proxy.c` for the concurrent cached proxy
- `malloc-lab/mm.c` for allocator logic
- `shell-lab/tsh.c` for shell and job-control behavior

## Notes

- The extra lab you remembered is `shell-lab`.
- Generated binaries, handin archives, and scratch outputs were removed so the repository stays readable as a portfolio piece.
- Each subdirectory keeps its own lab-specific documentation and build files.
