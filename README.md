# nasdaq-feed-handler

A NASDAQ TotalView-ITCH 5.0 feed handler and limit order book in C++20. It memory-maps a full
day of exchange data, decodes every message in place and keeps a book for every symbol. There
are two books: a reference built on standard containers, and a fast one built on flat price
arrays, a preallocated order pool and an open addressing hash map. The tools check the fast book
against the reference after every order event, measure throughput and per-message latency with
the timestamp counter, and run a two-thread pipeline over a lock-free ring that computes order
flow imbalance.

Builds on Linux (GCC, Clang) and Windows (MSVC); CI runs the tests on both, plus AddressSanitizer,
UndefinedBehaviorSanitizer and ThreadSanitizer builds.

## Layout

| Path | Contents |
|---|---|
| `include/itch` | message views, framing and dispatch, memory-mapped file, file summary, message writer |
| `include/book` | reference book, fast book, order map, ITCH to book adapter |
| `include/util` | timestamp counter and thread pinning, latency histogram, SPSC ring, OS memory, allocation counter |
| `src` | platform code and the out-of-line parts of the books |
| `tools` | `itch_verify`, `itch_bench`, `itch_pipeline`, `itch_gen` |
| `tests` | unit tests, spec edge cases and a randomized fast versus reference comparison |

## Build

Linux, with GCC 11+ or Clang 15+ and CMake 3.20+:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Windows, with Visual Studio 2022 or newer ("Desktop development with C++"), from a Developer
PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

WSL2 on Windows runs the Linux build. Keep data files in the Linux file system (for example
`~/itch`), because files under `/mnt/c` are read through a slow file sharing layer. WSL2 gets half
of the machine's RAM by default; on a 32 GB machine, raise it to 24 GB in
`%UserProfile%\.wslconfig` and run `wsl --shutdown`:

```ini
[wsl2]
memory=24GB
```

Binaries land in `build/` on Linux and `build\Release\` on Windows. The build tunes for the
machine it runs on (`-march=native`, or `/arch:AVX2` on MSVC); `-DFH_NATIVE=OFF` gives portable
binaries. `-DFH_SANITIZE=address,undefined` or `-DFH_SANITIZE=thread` enables sanitizers on GCC
and Clang.

The book preallocates its buffers on large pages when it can, to cut TLB misses on the roughly
2 GB of tables. Linux uses transparent huge pages with no setup. A native Windows build needs the
"Lock pages in memory" user right: open `secpol.msc`, go to Local Policies, User Rights
Assignment, add your account to "Lock pages in memory", then sign out and back in. Without it the
tools fall back to normal pages and the `memory` line reports `large pages: no`. WSL2 runs the
Linux build, so it needs none of this.

The fast book preallocates about 2 GB for a full day, and `itch_verify` also builds the reference
book. The Data section covers how much memory the file itself needs.

## Data

NASDAQ publishes sample days at <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>. The older files
are named by trading day (`01302020.NASDAQ_ITCH50.gz`); the 2026 uploads use names like
`itch50_05_15.gz` (15 May 2026: 13 GB compressed, 30 GB unpacked, 961 million messages). The
tools read the unpacked file:

```sh
FILE=01302020.NASDAQ_ITCH50.gz   # any file in the listing
curl -fL --retry 5 -C - -O "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${FILE}"
gunzip -k "${FILE}"
```

On Windows, run the same command with `curl.exe` or use a browser, and extract with 7-Zip.

`itch_bench` and `itch_pipeline` load the whole file into memory before timing, so the file and
about 2 GB for the book have to fit in RAM, or the timed run ends up waiting on the disk. A file
unpacks to a bit over twice its download size. On a 32 GB machine, use one of the 2019 or 2020
days; the 2026 day needs 48 GB or more. `itch_verify` reads the file front to back and works with
any day. With enough RAM the OS keeps the file cached between runs, so only the first run waits
on the disk.

To try the tools without the download, `itch_gen` writes a synthetic day with the same framing
and message types (`--messages`, `--symbols` and `--seed` control it):

```sh
./build/itch_gen sample.itch --messages 20000000 --symbols 500
```

Synthetic files are for testing. Benchmark numbers only mean something on a real day.

## Tools

### itch_verify

```sh
./build/itch_verify 01302020.NASDAQ_ITCH50
```

Replays the day into both books and compares the best bid and ask (price, size and order count)
after every add, execution, cancel, delete and replace. It also reports:

- feed anomalies in each book: unknown order references, duplicate adds, zero share orders and
  executions larger than the order. Real data should produce none, so any count points at a
  parsing bug;
- order events that leave a trading symbol locked or crossed during market hours;
- heap allocations made inside the fast book, which must be zero;
- live orders at the end of the day and the peak live count.

The exit code is 0 only if every check passes.

### itch_bench

```sh
./build/itch_bench 01302020.NASDAQ_ITCH50 --cpu 2
./build/itch_bench 01302020.NASDAQ_ITCH50 --book reference
./build/itch_bench 01302020.NASDAQ_ITCH50 --book decode
```

Runs two passes over the day:

1. Throughput: the whole replay with no timing inside the loop, in messages per second.
2. Latency: every message timed on its own with `rdtsc`, collected in a histogram and reported as
   p50, p90, p99, p99.9, p99.99 and max, overall and per message type.

`--book decode` reads every field without updating a book, which separates parsing cost from book
cost. `--cpu N` pins the thread, `--no-latency` skips the second pass and `--max-orders N` sets the
fast book's order capacity (by default the number of adds and replaces in the file, capped at
8,388,608).

### itch_pipeline

```sh
./build/itch_pipeline 01302020.NASDAQ_ITCH50 --feed-cpu 2 --strategy-cpu 4 --out ofi.csv
```

The feed thread applies messages to the fast book and pushes the top of book into a lock-free
ring whenever it changes. The strategy thread pops the updates and computes order flow imbalance
per symbol per minute. The tool reports the handoff latency between the threads and writes
`symbol,minute,ofi,events,mid_open,mid_close` rows to the CSV.

## Design

### Parsing

The file is memory-mapped (`mmap` or `MapViewOfFile`) and every page is touched before the timed
run. Each message is a two byte big-endian length followed by the payload. Message types are views
over the mapped bytes: a field accessor is a `memcpy` into an integer plus a byte swap, which
compiles to one load and one `bswap`. Casting the buffer to packed structs would be undefined
behavior. `dispatch` switches on the type byte and calls the handler's `on()` overload for that
message type. Handlers only implement the types they care about, resolved at compile time with no
virtual calls. Messages shorter than the spec length are reported instead of decoded.

### Book rules that are easy to get wrong

- An execution with price (`C`) takes shares from the order at its resting price, not at the
  execution price.
- A replace (`U`) carries no side or symbol. The new order inherits both from the original and
  loses its place in the queue.
- Trades (`P`), crosses (`Q`) and broken trades (`B`) never change the displayed book. `P` reports
  executions against hidden orders and `Q` is an auction print.

### Reference book

`std::map` price levels per side (bids ordered with `std::greater`) and a `std::unordered_map` from
order reference to order. Simple enough to trust, and it allocates on almost every add.

### Fast book

- **Price ladder.** Each side of each symbol has a window of 4096 ticks, stored as a flat array of
  16 byte levels (aggregate shares and order count). Prices map to ticks of $0.0001 below $1 and
  one cent above (Reg NMS Rule 612), so ticks stay dense across the $1 boundary.
- **Bitmap.** A 64 word occupancy bitmap with a one word summary on top. When the best level
  empties, the next best is two count-leading-zeros instructions away.
- **Far levels.** Levels outside the window, and prices off the cent grid, go to a small ordered
  map per side (`std::pmr::map`) whose nodes come from a pool over a preallocated arena. The top of
  book is the better of the window's best and the far map's best, so correctness never depends on
  where a level lives.
- **Recentering.** An order that would become the new best from outside the window, or the first
  order on an empty side, moves the window so it is centered on that price, swapping levels with
  the far map. This keeps the busy part of the book in the array as prices move during the day.
  The tools report how often this slow path runs.
- **Orders.** A fixed pool of 12 byte records with a LIFO free list, so a new order reuses the slot
  freed most recently while it is still in cache. An open addressing map goes from order reference
  to pool slot: Fibonacci hashing, linear probing, a load factor of at most one half, and
  backward-shift deletion instead of tombstones, so lookups stay short through a full day of
  deletes.
- **Memory.** Every buffer comes from the OS at construction, zeroed and touched so the replay
  never takes a page fault. On Linux the buffers are 2 MB aligned and marked for transparent huge
  pages, which keeps lookups in the large tables from missing the TLB. A day with about 8,900
  symbols needs roughly 1.8 GB.
- **No heap allocation after construction.** The tools replace global `operator new` with a
  counting version, and `itch_verify` counts every call made from inside the fast book.

### Timing

- Each message is timed between `lfence; rdtsc; lfence` and `rdtscp; lfence`, so the timed work
  cannot drift outside the two reads. The timer's own cost is measured and printed, and the
  latency numbers include it.
- The latency is the time to decode and apply one message from memory, not network to book.
- The fences also stop the CPU from overlapping cache misses across messages, which is why p50
  latency comes out higher than one over throughput. The throughput pass has no fences.
- The histogram is exact below 1024 ticks and has 64 buckets per power of two above that (under
  1.6% error), so a full day needs no per-message storage. The counter frequency is calibrated
  against `steady_clock` at startup.

### Ring and order flow imbalance

- The ring has a power of two capacity. The producer and consumer indices sit in separate 128 byte
  blocks (Intel's adjacent-line prefetcher pulls 64 byte lines in pairs), and each side caches the
  other side's index, reloading it only when the ring looks full or empty. Publishing is a release
  store and reading is an acquire load.
- The feed thread publishes the top of book only when it changes, plus market open and close and
  halt and resume events.
- Order flow imbalance follows Cont, Kukanov and Stoikov (2014), "The Price Impact of Order Book
  Events". For consecutive quotes n-1 and n, with bid price and size Pb and qb, and ask price and
  size Pa and qa:

  ```
  e(n) =   qb(n)    if Pb(n) >= Pb(n-1)
         - qb(n-1)  if Pb(n) <= Pb(n-1)
         - qa(n)    if Pa(n) <= Pa(n-1)
         + qa(n-1)  if Pa(n) >= Pa(n-1)
  ```

  It is summed per symbol per minute between the market open and close system events, skipping
  halted symbols. The CSV includes the mid at the start and end of each minute, so price changes
  can be regressed on OFI directly.
- Handoff latency subtracts a counter stamped on the feed core from one read on the strategy
  core. Invariant timestamp counters are usually synchronized across cores, but not guaranteed to
  be, so a sample where the strategy core reads behind is recorded as 0 rather than wrapping, and
  the tool reports how many samples that happened to.

## Profiling

On Linux, `perf` reads the hardware counters:

```sh
sudo apt install linux-tools-common linux-tools-$(uname -r)
sudo sysctl kernel.perf_event_paranoid=1
perf stat -e cycles,instructions,branch-misses,cache-references,cache-misses,LLC-load-misses,dTLB-load-misses \
  ./build/itch_bench FILE --cpu 2 --no-latency
perf record -g ./build/itch_bench FILE --cpu 2 --no-latency
perf report
```

`perf stat` covers the whole process, including loading the file, so compare `--book fast` with
`--book decode` to isolate the book. On Windows, Intel VTune's Memory Access analysis reads the
same counters.

WSL2 has no perf package for its kernel. Install the generic tools and call the binary directly:

```sh
sudo apt install linux-tools-generic
PERF=$(ls /usr/lib/linux-tools/*/perf | head -n 1)
$PERF stat -e cycles,instructions,cache-misses true
```

The last command prints counts if the host passes the hardware counters through to the VM, and
`<not supported>` if it does not. Thread pinning under WSL2 applies to virtual CPUs, so latency
tails pick up some noise from the host.

For stable numbers, set the CPU governor to performance (`sudo cpupower frequency-set -g
performance`; on Windows and WSL2, the Best performance power mode), pin to a core nothing else
uses, keep the file in the page cache and repeat each measurement a few times.

## Next experiments

- The order map is the likely hotspot on a full day. Fibonacci hashing scatters consecutive order
  references, so most lookups miss cache. References are assigned in sequence, so a hash that
  keeps neighbours in the same cache line, or prefetching the next message's slot, should cut the
  misses.
- Once the Rule 612 amendments take effect (currently set for November 2027), some stocks will
  quote in half cents. Those prices go through the far map today; if a file has many of them, a
  half-cent grid in `tick_of` keeps them in the array.
