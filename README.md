# DuraKV

A crash-safe, multi-client key–value store in C. **DuraKV never corrupts or
loses committed data even when the process is killed mid-write** (`kill -9` /
power loss), proven by crash-injection tests.

It is built in layers: the durable spine (storage + write-ahead log + crash
recovery), a buffer pool (paging with FIFO/LRU eviction), and concurrency
(thread pool, round-robin scheduler, a thread-safe store). The only dependency
so far is **pthreads** (in libc).

## Build & run

```bash
make            # build the durakv CLI
make tests      # build the unit tests
make test       # run unit tests
make crashtest  # the headline kill -9 durability demo
make clean
```

Requires only a C11 compiler (clang/gcc). Page size is compile-time
configurable: `make CFLAGS="... -DPAGE_SIZE=8192"`.

### CLI

```bash
./durakv data.db wal.log          # interactive command loop
```

| Command | Response |
|---------|----------|
| `set <key> <value...>` | `OK` |
| `get <key>` | `VALUE <value>` / `NOTFOUND` |
| `del <key>` | `OK` / `NOTFOUND` |
| `list` | one key per line, then `END` |
| `checkpoint` | `OK` (flush pool + fsync data + WAL checkpoint marker) |
| `stats` | buffer-pool counters (accesses/hits/faults/evictions/hit ratio) |
| `quit` | exit |

The buffer pool is configurable via environment variables:
`DURAKV_FRAMES=<n>` (default 64) and `DURAKV_POLICY=fifo|lru` (default `lru`).

```
$ printf 'set city kathmandu\nget city\nquit\n' | ./durakv data.db wal.log
OK
VALUE kathmandu
```

## How durability works

Three concepts carry Phase 1:

1. **Write-ahead logging** (`src/wal.c`). Every mutation is appended to
   `wal.log` as a length-prefixed, CRC32-protected record holding the full
   before- and after-image of each modified page. On `COMMIT` the WAL is
   `fsync`'d (macOS: `F_FULLFSYNC`) **before** the client is told `OK`. So any
   acknowledged write is on stable storage.

2. **The write-ahead invariant** (`src/storage.c`). A dirty data page is only
   written back to `data.db` *after* the WAL is fsync'd up to that page's last
   log record. `data.db` itself is only fsync'd at checkpoint/close — a crash
   that loses those unsynced pages is harmless, because the WAL can rebuild
   them.

3. **Idempotent recovery** (`src/recovery.c`). On startup: **analysis** (find
   committed vs in-flight transactions from the last checkpoint), **redo**
   (re-apply committed after-images, but only where `page.page_lsn <
   record.lsn` — this check makes redo safe to crash *during*), **undo** (roll
   back in-flight transactions with their before-images). Page-level images
   also transparently repair torn page writes.

## Buffer pool (Phase 2)

The live read/write path goes through an in-RAM page cache
(`src/bufferpool.c`) with pluggable replacement policies (`src/replacement.c`,
a function-pointer vtable — FIFO and LRU ship; CLOCK/LRU-K would just be more
entries). Pages are **write-back**: a modified page is only flushed to
`data.db` on eviction, checkpoint, or close.

This tightens the durability story: because dirty pages live only in this
process's RAM, a `kill -9` really does lose them, so recovery's redo pass is
exercised on every restart — not masked by the OS page cache.

Two demonstrations earn the Phase 2 marks:

- **Belady's anomaly** (`tests/test_belady.c`): the reference string
  `1 2 3 4 1 2 5 1 2 3 4 5` faults **more with 4 frames than with 3** under
  FIFO (9 → 10), while LRU is monotonic (10 → 8). Driven through the real
  buffer pool, not a model.
- **FIFO vs LRU hit ratios** (`tests/test_bufferpool.c`): a report across
  looping and skewed-locality workloads, e.g. LRU 82.8% vs FIFO 73.2% on an
  80/20 hot-set.

## Concurrency (Phase 3)

The store is multi-threaded:

- **Thread pool** (`src/threadpool.c`) — N workers over a bounded job queue
  guarded by one mutex and two condition variables (`not_empty`, `not_full`):
  the textbook producer–consumer. Every wait loops on its predicate, so
  spurious wakeups are harmless.
- **Round-robin scheduler** (`src/scheduler.c`) — per-client queues serviced
  in rotation, so a heavy client cannot starve the others (real fairness, not
  a simulation).
- **Thread-safe store** — a `pthread_rwlock_t` lets `GET` run as a shared
  reader while `SET`/`DEL` take it exclusively; the buffer pool has its own
  mutex so concurrent readers can fault pages safely. The locking is verified
  race-free under **ThreadSanitizer**.
- **Deadlock prevention** — page latches follow a strict lock hierarchy
  (ascending `page_id`), breaking the Coffman circular-wait condition.

Demos (`make test` runs them):

| Demo | Shows |
|------|-------|
| `tests/demo_race.c` | unsynchronised `count++` loses ~140k updates; mutex and C11 `_Atomic` are exact |
| `tests/demo_deadlock.c` | naive opposite-order locking deadlocks (a watchdog kills it); ascending-order locking completes |
| `tests/demo_scheduler.c` | round-robin service order — light clients finish without waiting for a heavy client to drain |
| `tests/loadtest.c` | 16 concurrent clients × 120 ops, every value verified |

Throughput is **fsync-bound**: each commit does a full `F_FULLFSYNC` under the
write lock, so commits serialise at a few ms each (group commit would be the
fix). That is the price of durability, paid deliberately.

## Tests

| Test | Proves |
|------|--------|
| `tests/test_storage.c` | record round-trips, update/delete, multi-page growth, persistence across reopen |
| `tests/test_wal_recovery.c` | **redo** rebuilds 500 keys from the WAL after `data.db` is rolled back to a pre-write snapshot (an honest power-loss model) |
| `tests/test_bufferpool.c` | page faults, eviction, dirty write-back; FIFO/LRU hit-ratio report |
| `tests/test_belady.c` | Belady's anomaly under FIFO; LRU stays monotonic |
| `tests/demo_*` / `loadtest` | concurrency (see table above) |
| `scripts/crashtest.sh` | committed keys survive repeated `kill -9`; **`make crashtest_concurrent`** does it with 4 writer threads |

The two test harnesses cover different failure modes on purpose:

- **`crashtest.sh`** kills the *process* (`kill -9`). The OS page cache keeps
  `data.db`'s writes across process death, so recovery mostly confirms/repairs
  the WAL tail. This is the spec's "kill -9 loop".
- **`test_wal_recovery.c`** simulates *power loss* by discarding `data.db`'s
  unsynced writes (restoring a snapshot) while keeping the fsync'd WAL, forcing
  recovery to reconstruct everything via redo.

```
$ make crashtest
iter  1: acked +25   total=25     missing=0
...
crashtest: PASS -- 1047 committed keys survived 12 kill -9 cycles
```

## Layout (Phases 1–3)

```
include/  storage.h wal.h recovery.h bufferpool.h replacement.h
          threadpool.h scheduler.h
src/      storage.c wal.c recovery.c bufferpool.c replacement.c
          threadpool.c scheduler.c durakv.c
tests/    test_storage.c test_wal_recovery.c test_bufferpool.c test_belady.c
          mem_demo.c demo_race.c demo_deadlock.c demo_scheduler.c loadtest.c
scripts/  crashtest.sh
```
