# Arena allocator

A C++20 allocator backed by `mmap`. The single-threaded implementation uses
boundary tags and an intrusive first-fit free list. `ConcurrentAllocator`
adds a sharded concurrency layer without putting one lock around the entire
heap.

## Concurrent design

- Threads are assigned to one of 32 cache-line-aligned shards.
- Each shard owns an independent arena allocator and an `atomic_flag` lock.
- An 8-byte prefix records the owning shard for every allocation.
- A block can therefore be freed by a thread other than the one that
  allocated it.

The design is concurrent, not lock-free. An operation can wait for another
operation on the same shard, but unrelated shards do not contend. The prefix
costs 8 bytes per allocation and the default configuration maps one 1 MiB
arena per shard.

## Build and test

```sh
make test
make sanitize
make tsan
```

The tests cover splitting, coalescing, arena growth, `realloc`, alignment,
and concurrent allocation with cross-thread frees. AddressSanitizer,
UndefinedBehaviorSanitizer, and ThreadSanitizer pass locally.

## Benchmark

Workload: 16 threads, 500,000 fixed-size 64-byte allocate/free pairs per
thread, 10-core Apple M5, Apple Clang 21, `-O2`. Each thread warms its shard
before the timed region.

| Measurement | Result |
| --- | ---: |
| Median mean-thread latency (10 runs) | **19.7 ns/pair** |
| Range | 14.1–30.1 ns/pair |

Latency is the sum of each thread's timed duration divided by the total
number of pairs. The test also prints aggregate wall-clock throughput, which
is a different number.

## Limits

- First-fit search is linear in the number of free blocks.
- Arenas are released at allocator destruction, not returned piecemeal.
- Shard assignment is optimized for a long-lived shared allocator. More than
  32 active threads can share shards and contend.
