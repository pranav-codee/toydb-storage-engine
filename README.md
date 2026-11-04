# Overview

TODO: Describe the toyDB assignment and its three parts.

# Build

TODO: Document the clean build and test commands.

# Run

TODO: Document the available programs and benchmark commands.

# Configuration (toydb.conf)

TODO: Describe configuration keys, defaults, and command-line overrides.

# Part 1 buffer manager

TODO: Describe the buffer-manager experiment and reported metrics.

## Buffer manager

The buffer pool size and the default replacement policy are read from
`toydb.conf` by `PF_Init()` (keys `buffer_pool_size` and `default_policy`). If
no configuration is loaded, the pool has 20 frames and the default policy is
LRU. The pool can also be resized at runtime with `PF_InitBuffer(n)` while no
page is in the buffer; the minimum is 8 frames.

The replacement policy is chosen per file when the file is opened.
`PF_OpenFileEx(fname, PF_LRU)` or `PF_OpenFileEx(fname, PF_MRU)` selects it
explicitly, and `PF_OpenFile(fname)` uses the configured default. The pool is
shared, so the victim is chosen by the policy of the file whose page is being
brought in, and it may belong to any open file.

Recency is defined by the last load or unfix of a page. A page moves to the
head of the used list when it is read into a frame and when it is unfixed. A
hit does not move the page. LRU evicts the unfixed page nearest the tail, and
MRU evicts the unfixed page nearest the head. `PF_MarkDirty` sets the dirty bit
without changing recency.

The layer keeps eight counters per open file and a global total, read with
`PF_GetStats(fd, &stats)` (`fd = -1` for the totals) and cleared with
`PF_ResetStats`. The exact definitions are in the comment above
`PF_GetStats` in `pflayer/pf.c`.

- `logical_reads`: requests to fix an existing page, whether hit or miss.
- `hits`: logical reads whose page was already in the pool.
- `misses`: logical reads whose page had to be read from the file.
- `physical_reads`: pages read from the file into a frame.
- `logical_writes`: requests to mark a page dirty, counted at most once per fix of the page.
- `physical_writes`: pages written to the file on eviction, on close, or by `PF_FlushAll`.
- `evictions`: frames taken from a resident page to make room, clean or dirty.
- `dirty_evictions`: evictions whose victim was dirty and had to be written back.

## Buffer benchmark

`make bench` runs `bench/bench_buffer` and writes `results/bench_buffer.csv`.
It builds one scratch file of `buf_file_pages` pages in `work_dir` (a relative
path is resolved against the repo root). Then, for every pool size in
`buf_pool_sizes`, policy (LRU, MRU), pattern in `buf_patterns`, read
percentage in `buf_read_pcts`, and each of `repeats` repeats, it opens the
file with a cold pool and runs `buf_queries` operations. An operation fixes a
page and unfixes it. A read unfixes it clean. A write changes one byte and
marks the page dirty, either with `PF_UnfixPage(..., TRUE)` or with
`PF_MarkDirty` followed by a clean unfix (the two forms alternate). The
patterns are `uniform` (random page), `hotcold` (`hot_prob` of the accesses go
to the first `hot_fraction` of the pages) and `sequential` (a looping scan over
all pages). Each run uses two fixed xorshift generators. The page stream is
seeded from `seed`, the repeat and the pattern. The read/write choice comes
from a second generator that is also seeded with the read percentage. So
every read percentage, policy and pool size sees the same page sequence on
every machine. Replacement ignores the dirty bit, so hits, misses, physical
reads and evictions do not change with the read percentage; only the write
counters do.

Each run is one CSV line:
`policy,pattern,pool,read_pct,repeat,ops,logical_reads,logical_writes,hits,misses,physical_reads,physical_writes,total_physical_writes,evictions,dirty_evictions,hit_ratio,ms`.
The counters are read right after the operations, so `physical_writes` covers
only dirty evictions. The program then calls `PF_FlushAll`, and
`total_physical_writes` is the write count after that flush, i.e. dirty
evictions plus the final write-back. `hit_ratio` is `hits / logical_reads`.
`ms` is the wall time of the operations only, without the flush.

`bench/bench_buffer check` is the fast self-test run by `make test`. On a
10-page file it checks that `hits + misses = logical_reads = ops`. It checks
that a pool larger than the file has exactly one miss per page and no
evictions, and that a read-only run writes nothing. It checks the
hand-computed counts for a looping scan with an 8-frame pool (LRU 0 hits,
MRU 16 hits out of 30), and that the same seed gives the same counters. A
write-only run must have only dirty evictions and
`total_physical_writes = dirty_evictions + pages still dirty at the end`,
and every written byte must be read back from disk after the file is closed.
Finally, an all-read and an all-write run over the same page stream must give
the same hits, misses, physical reads and evictions.

## Buffer plots

`make plots` runs `scripts/plot_buffer.py` (Python 3 and matplotlib) on
`results/bench_buffer.csv` and writes these files to `results/`, each figure
as PDF and as 150 dpi PNG. Points are means over the repeats, with min/max
error bars.

- `buffer_hit_ratio`: hit ratio vs pool size, one panel per pattern, all reads.
- `buffer_io_vs_mix`: disk I/O per operation,
  `(physical_reads + total_physical_writes) / ops`, vs read percentage, at
  pool `plot_pool`.
- `buffer_io_breakdown`: the same disk I/O split into physical reads per
  operation (solid lines) and total physical writes per operation (dotted
  lines) vs read percentage, at the same pool.
- `buffer_summary.csv` and `buffer_summary.tex` (a booktabs tabular): hit
  ratio, physical reads, total physical writes, evictions and dirty evictions
  per pattern and policy, at pool `plot_pool` and `plot_read_pct` reads.

`plot_pool` and `plot_read_pct` are in `toydb.conf`. Run the script directly
with `--pool N` to plot another pool size.

# Part 2 slotted pages

## Slotted pages

The heap file reserves page 0 for its metadata header. Each data page uses a
four-byte header (`slot_count`, `data_start`), a four-byte slot directory
growing from the front, and variable-length record bytes growing backward from
the end. A deleted slot is empty but keeps its slot number; live records may
be compacted within the page, so existing RIDs remain stable.

RIDs use `page * HF_RID_STRIDE + slot`, with `HF_RID_STRIDE = 4096`. The
space statistics exclude the metadata page:

- `pages` is the number of data pages;
- `records` is the number of live records;
- `useful_bytes` is the sum of record lengths;
- `records_per_page` is `records / pages`;
- `utilization` is `useful_bytes / (pages * PF_PAGE_SIZE)`.

`student.txt` has CRLF line endings; the loader strips the `"\r\n"` (or
`"\n"`), so a record is exactly the line content. A line too long for the
loader's read buffer is reported with its line number instead of being
split. Loading the configured student dataset produced 17,813 records,
1,759,011 useful bytes, and a longest record of 108 bytes. The slotted file
used 452 data pages, with 39 records per page (integer division) and 95.010%
space utilization (`1,759,011 / (452 * 4,096)`). `test_hf_load` checks
invariants rather than fixed numbers: every inserted record reads back
byte-equal by its RID and in a full scan, the scan count equals the inserted
count and the `records` statistic, `useful_bytes` equals the sum of the
inserted lengths, and the utilization formula holds.

`tests/test_hf_load` needs the data file at the path given by `data_file` in
`toydb.conf`; under `make test` a relative path is resolved against the repo
root. If the file is missing, the test prints SKIPPED and still counts as a
pass.

## Static layout and comparison

The static baseline gives every record a fixed-size slot. A data page has a
four-byte slot-count header followed by the slots; each slot is a two-byte
length word and then `L` payload bytes. Bits 0 to 14 of the length word hold
the stored length, bit 15 marks a record that was truncated at insert, and
`0xFFFF` marks an empty slot. Thus

```
slots_per_page = floor((PF_PAGE_SIZE - 4) / (L + 2))
```

Records longer than `L` (up to `HF_MAX_RECLEN`) are truncated to `L` and
flagged; longer ones are rejected with `HFE_TOOLONG`. `HF_StaticGet` and the
scan return the stored length. The header page keeps the live record count,
the number of live truncated records and the sum of stored lengths; insert
and delete update them, so a delete lowers both `useful_bytes` and
`truncated`. Closing a static file invalidates its open scans. Static RIDs
use the same `page * HF_RID_STRIDE + slot` encoding as the slotted file.

Both layouts use the same statistics: the header page is excluded,
`useful_bytes` is the bytes actually stored, and
`utilization = useful_bytes / (data_pages * PF_PAGE_SIZE)`. Run the full
comparison with:

```
make bench-space
```

This writes `results/bench_space.csv` (through a temporary file, so a failed
run leaves the previous CSV in place). Every value in `static_lengths` must be
in `1..HF_MAX_RECLEN` and `churn_fraction` in `0..1`; otherwise the program
names the bad value and exits with status 1 before it prints or creates
anything. `student.txt` has CRLF line endings, and `bench_space` strips them
like `test_hf_load`, so each record is exactly the line content. The CSV
contains full-load and delete/reinsert churn rows for the slotted layout and
every configured static length. The churn experiment deletes
`churn_fraction` of the records, chosen with the configured `seed`, and
inserts the same records again. With the supplied 17,813-record data file,
the full-load rows were:

| layout | L | records | truncated | data pages | useful bytes | records/page | utilization |
|---|---:|---:|---:|---:|---:|---:|---:|
| slotted | 0 | 17813 | 0 | 452 | 1759011 | 39 | 0.950102 |
| static | 100 | 17813 | 5749 | 446 | 1748550 | 39 | 0.957157 |
| static | 104 | 17813 | 159 | 469 | 1758761 | 37 | 0.915533 |
| static | 110 | 17813 | 0 | 495 | 1759011 | 35 | 0.867568 |
| static | 128 | 17813 | 0 | 575 | 1759011 | 30 | 0.746863 |
| static | 160 | 17813 | 0 | 713 | 1759011 | 24 | 0.602309 |
| static | 200 | 17813 | 0 | 891 | 1759011 | 19 | 0.481982 |
| static | 256 | 17813 | 0 | 1188 | 1759011 | 14 | 0.361487 |

The slot formula gives 40, 38, 36, 31, 25, 20, and 15 slots per page for
lengths 100, 104, 110, 128, 160, 200, and 256, which produces the measured
page counts (`ceil(17813 / slots)`). `records/page` is
`floor(records / pages)`, an average over the file, not the slot capacity of
a page. The longest record is 108 bytes, so every static length of 108 or
more stores all bytes and only the page count grows; at 100 and 104 the
utilization is higher only because 5,749 and 159 records lost bytes. Every
churn row equals its full-load row: the same records go back in, so records,
stored bytes and the truncation count are unchanged (the benchmark asserts
this), and the freed slots or page space are reused without new pages.

# Part 3 B+ tree

## Three ways to build the index

The index is a B+ tree on roll no (field 2, `char(8)`), with the record's
heap-file RID as the record id. `bench/bench_index` builds it three ways
from the same 17,813 records:

- **M1 existing.** The heap file is already loaded (not measured). One scan
  of it (`HF_ScanNext`) inserts every `(roll no, rid)` with
  `AM_InsertEntry`.
- **M2 incremental.** Starts from an empty heap file and an empty index.
  Each record goes into the heap file with `HF_Insert` and its key goes into
  the index right away.
- **M3 bulk.** One heap scan collects the `(roll no, rid)` pairs, `qsort`
  puts them in AM order (`memcmp`, the same as the AM's `strncmp` on 8-byte
  keys), and `AM_BulkLoad` builds the tree bottom-up. The scan and the sort
  are timed apart from the load. The load alone is the cost when the file is
  already sorted.

M2 runs first, and its heap file is the one M1 and M3 index, so all three
trees hold the same `(key, rid)` pairs.

The bulk loader (`amlayer/ambulk.c`, change A2) fills each leaf with
floor(f × 254) keys at fill factor f (`bulk_fill_factor`, default 1.0).
254 is the char(8) leaf capacity, floor((4096 − 24) / 16). Internal nodes
take floor(f × 341) children. The rids of one key never span two leaves.
The root is written into page 0, and every page is reachable from it, so the
file has exactly 1 + leaves + internal pages.

## How it is measured

- **Index and heap I/O are counted apart.** Only the heap file and the index
  file are ever open, and the PF counters charge each page access to the file
  that owns the page, including the write-back of an evicted dirty page.
  While a file is open its own `PF_GetStats(fd)` counters are used, read
  after `PF_FlushAll` so that closing it writes nothing more.
  `AM_CreateIndex`, `HF_CreateFile` and `AM_BulkLoad` open and close their
  file themselves. Each runs with no other file open, so the delta of the
  global counters `PF_GetStats(-1)` around the call is that file's I/O.
- **Self-checks.** For every build the global delta must equal index plus
  heap (logical and physical reads and writes). For every measurement
  hits + misses must equal logical reads. Any difference stops the run.
- **Cold pool.** Every build and every query batch starts with all files
  closed and an empty pool of `buffer_pool_size` frames (50 in `toydb.conf`).
- **Identical query sets.** One query set is made per repeat from `seed` and
  the repeat number, and all three indexes answer it. It contains:
  - `idx_point_queries` EQ scans on existing keys;
  - `idx_missing_queries` absent values, half in the gap between two leaves
    and half in a gap inside a leaf;
  - `idx_range_queries` GE scans from an existing key, stopped after
    `idx_range_len` results;
  - one full scan.
- **Index cost vs record fetch.** Each query is timed alone, and the index
  columns hold only the index file's I/O. For the point queries, a separate
  cold-pool pass then fetches every record found with `HF_Get`, and reports
  the heap pages that needs.
- **Correctness.** Every query's record ids are compared with a sorted
  in-memory copy of the pairs: each rid once, keys in order, the right count.
  All three indexes must pass, so they return the same sets.

Run it with `make bench-index`, which writes `results/bench_index.csv`, one
row per method, repeat and phase. `make plots` then also writes
`results/index_build.pdf/.png`, `results/index_queries.pdf/.png` and
`results/index_summary.csv/.tex`. `make test` runs
`bench/bench_index check`: 604 synthetic records with a hand-computed M3
shape and result counts, independent of `toydb.conf` and `student.txt`.

## Results

These are means over 5 repeats on `student.txt`, with pool 50 and f = 1.0.
The build I/O and the tree shapes were identical in every repeat; only the
times varied. Logical reads count
page fix requests; physical reads and writes count actual page transfers.

| | M1 existing | M2 incremental | M3 bulk |
|---|---:|---:|---:|
| Index pages | 110 | 110 | 72 |
| Leaves | 109 | 109 | 71 |
| Height | 2 | 2 | 2 |
| Leaf fill | 64.3% | 64.3% | 98.8% |
| Build time (ms) | 8.3 | 2001.0 | 6.5 |
| &nbsp;&nbsp;index part (ms) | 4.2 | 23.1 | 1.2 |
| &nbsp;&nbsp;heap scan + sort (ms) | – | – | 5.2 |
| Build index logical reads / writes | 35,478 / 18,030 | 35,478 / 18,030 | 2 / 73 |
| Build index physical reads / writes | 30 / 140 | 31,905 / 16,168 | 2 / 73 |
| Build heap physical reads + writes | 453 | 4,010,110 | 453 |
| Point: logical / physical reads per query | 5.01 / 0.550 | 5.01 / 0.550 | 5.00 / 0.323 |
| Point: time per query (µs) | 0.58 | 0.59 | 0.46 |
| Point: heap physical reads per query (record fetch) | 0.898 | 0.898 | 0.898 |
| Missing: logical / physical reads per query | 4.31 / 0.648 | 4.30 / 0.644 | 4.19 / 0.494 |
| Missing: time per query (µs) | 0.67 | 0.67 | 0.57 |
| Range (100 rids): logical / physical reads per query | 104.62 / 1.000 | 104.62 / 1.000 | 104.38 / 0.572 |
| Range: time per query (µs) | 2.95 | 3.00 | 2.64 |
| Full scan: logical / physical reads | 17,924 / 110 | 17,924 / 110 | 17,886 / 72 |
| Full scan: time (µs) | 430 | 416 | 400 |

**Size.** The bulk-loaded tree has 72 pages against 110 (about 35% fewer), because
its leaves are full: 98.8% against 64.3%. Its fill is not 100% only because
the last of the 71 leaves is partly empty (17,810 distinct keys over
71 × 254 slots).

**Build cost.** M3 is the cheapest build by far in index I/O. It writes each
of its 72 pages once (73 writes, since page 0 is written once by
`AM_CreateIndex` and again as the root) and needs 2 logical reads. M1 needs
35,478 logical reads and 18,030 logical writes: a root-to-leaf search for
every insert, plus the splits. The load alone takes 1.2 ms. Most of M3's
6.5 ms is the heap scan (453 heap page reads, the same as M1's scan) and the
in-memory sort (5.2 ms together). The sort itself costs no PF I/O; a sort
too big for memory would add about two passes over the pairs.

**M2.** M2 does the same index work as M1: the same logical counts.
Its 2 s build, though, comes almost entirely from the heap. The first-fit
`HF_Insert` reads 4.0 million heap pages through the pool for 17,813
inserts. Those heap pages push index pages out of the 50 frames, so the
index needs 31,905 physical reads and 16,168 physical writes, against 30 and
140 in M1. That is why its index part takes 23.1 ms against 4.2 ms.

**M1 and M2 are not the same tree.** They have the same totals (110 pages,
109 leaves, 64.3% fill), but walking both leaf chains shows that 9 of the
109 leaves hold different key ranges (for example 239 against 241 keys in
leaf 32), and 5 leaves sit on different page numbers. The cause is insertion
order. M2 inserts in file order. M1 inserts in heap-scan order, which
differs wherever first-fit put a record on an earlier page with room left:
31 records have a smaller rid than the record before them in the file. Those
keys arrive at different times, so a few leaves split at different keys. The
number of splits is the same (both trees end with 109 leaves under one
root), and so are the logical page counts. This comparison was a one-off
check with a scratch copy of the benchmark, not part of `bench_index`.

**Queries.** All three trees have height 2, so the logical reads per query
are almost the same. They are higher than 2 because of how the provided scan
code works. Every `AM_OpenIndexScan` walks to the leftmost leaf
(`GetLeftPageNum`, 2 reads) besides the search (2 reads), and
`AM_FindNextEntry` fixes the current leaf for every rid it returns.
- **Point:** 5 logical reads.
- **Missing:** about 4. A value past the last key of the leaf it reaches
  costs one more fix, the next leaf.
- **Range:** 4 plus 100 rids plus the leaf crossings.
- **Full scan:** 3 + 17,813 + one per leaf crossed, so 17,924 for M1 and M2
  (108 crossings) and 17,886 for M3 (70 crossings). A full-scan open fixes
  the leftmost leaf once more after `GetLeftPageNum`.

The difference shows in the physical reads. With a cold 50-frame pool, a
smaller tree means fewer distinct pages to bring in. M3 needs 41% fewer
physical reads per point query (0.323 against 0.550), 43% fewer per range
query (0.572 against 1.000), and 72 instead of 110 for the full scan. The
times follow the same order but differ little (0.46 against 0.58 µs per
point query). The pages come from the operating system's file cache, so a
"physical" read is cheap here; on a real disk the page counts would matter
much more than these times suggest.

**Record fetch.** Fetching the records of the point queries costs 0.898 heap
physical reads per query for every method, since the heap file is the same.
That is more than the index lookup costs on any of the three trees. `HF_Get`
counts 2 logical reads per record (the header page and the data page), and
the header page stays in the pool.

# Changes to provided code

Every modification to the supplied PF and AM code is listed below. Each edit
site in the source carries a comment of the form `/* CHANGED [ID]: ... */`
with the same ID, so a row can be traced to the lines it describes.

| ID | File(s) | What changed | Reason | Consequence if not made |
|----|---------|--------------|--------|-------------------------|
| B1 | `Makefile` (new, top level) | One top-level Makefile replaces the per-folder builds. Provided sources are compiled with `-std=gnu89 -w`, new code with `-std=gnu89 -Wall -Wextra`. `make test` runs every test program in a scratch directory and checks its final line. `make clean` removes the shipped object files and binaries. | The provided sources are pre-ANSI C (implicit `int`, K&R function definitions, calls to undeclared functions), and the shipped objects were built by an old toolchain. | The shipped binaries and per-folder builds do not build on a current gcc. Under the default C23 mode of gcc 15.2, `pflayer/buf.c` fails with 32 errors, `pflayer/pf.c` with 47 and `pflayer/hash.c` with 19 (mostly implicit `int`, and calls to functions declared with an empty parameter list, which C23 reads as taking no arguments); with `-std=gnu89` they compile. |
| B2 | `amlayer/pf.h` (deleted), `pflayer/pf.h`, `Makefile` | The private copy of `pf.h` in `amlayer/` is removed, so the AM sources include the single `pflayer/pf.h`. `PF_PAGE_SIZE` is 4096 and can be overridden with `-DPF_PAGE_SIZE=`. | The two headers disagreed on the page size (1020 bytes in `amlayer/`, 4096 in `pflayer/`). | The stale amlayer copy had a 1020-byte page size, so B+ tree nodes used about a quarter of each 4096-byte page and Part 3 page counts would be roughly four times too high. |
| P1 | `pflayer/pftypes.h`, `pflayer/buf.c`, `pflayer/pf.c`, `pflayer/pf.h` | The compile-time constant `PF_MAX_BUFS` is replaced by a pool allocated at runtime. `PF_Init` sizes it from `buffer_pool_size` in `toydb.conf` (default 20), and the new `PF_InitBuffer(n)` resizes it while no page is in the buffer. New error codes `PFE_INVALIDBUFS` and `PFE_BUFINUSE`. | The assignment requires a configurable buffer pool size. | Every pool-size experiment needs a recompile, which the rule that only the configuration file may change after submission forbids. |
| P2 | `pflayer/pftypes.h` | `PF_HASH_TBL_SIZE` is raised from 20 to 1031, a prime. | Pools of 50 to 100 frames are used in the experiments. | With 50 to 100 frames, 20 buckets give long chains and add lookup cost to every measurement. |
| P3 | `pflayer/pftypes.h`, `pflayer/pf.c`, `pflayer/buf.c`, `pflayer/pf.h` | Each open-file entry records a replacement policy. The new `PF_OpenFileEx(fname, policy)` sets it, and victim selection searches from the tail for LRU and from the head for MRU, according to the policy of the file whose page is being loaded. New error code `PFE_INVALIDPOLICY`. | The assignment requires LRU and MRU to be selectable when a file is opened. | LRU and MRU cannot be chosen when a file is opened. |
| P4 | `pflayer/pf.c`, `pflayer/pf.h` | `PF_OpenFile` is kept as a wrapper around `PF_OpenFileEx` that uses `default_policy` from `toydb.conf` (default LRU). | Backward compatibility with existing callers. | Every AM caller and provided test would need editing. |
| P5 | `pflayer/buf.c`, `pflayer/pf.c`, `pflayer/pftypes.h`, `pflayer/pf.h` | New public call `PF_MarkDirty(fd, page)`, backed by `PFbufMarkDirty`, which sets the dirty bit of a resident page (fixed or unfixed) without changing its recency. `PFbufUsed` is unchanged. | The assignment requires an explicit mark-dirty call, and `PFbufUsed` also moves the page to the head of the list. | The dirty bit can only be set by the unfix call. |
| P6 | `pflayer/buf.c`, `pflayer/pf.c`, `pflayer/pftypes.h`, `pflayer/pf.h` | Per-file and global counters for logical reads and writes, hits, misses, physical reads and writes, evictions and dirty evictions, updated where each event occurs. New public calls `PF_GetStats`, `PF_ResetStats`, `PF_PrintStats` and `PF_FlushAll`. `PFbufGet` takes a new argument that says whether the fix is a logical read. | The assignment requires I/O statistics, and all three parts report them. | No logical or physical I/O can be measured. |
| P7 | `pflayer/buf.c`, `pflayer/hash.c` | The debug prints in `PFbufPrint` and the hash table print show the frame address with `%p` instead of `%d`. | Pointers are 64 bits wide on the target platform. | Pointers printed through `%d` are truncated on a 64-bit build. |
| A1 | `amlayer/amscan.c` | `GetLeftPageNum`, which `AM_OpenIndexScan` uses to start full, LT, LE and NE scans, now follows the leftmost child pointer `p0` from the root (page 0) down to a leaf, unfixing each page before fixing its child. Before, it returned page 0 if the root was a leaf and page 2 otherwise. | Page 2 is the leftmost leaf only in trees built by inserts, because the first root split puts the copy of the left half in page 2. A tree with any other page layout, such as the bulk-loaded tree of A2 (leaves in pages 1..k, root in page 0), has its leftmost leaf elsewhere. | `tests/test_leftpage` shows it. With leaves in pages 1..4, the full scan returns 75 of 100 keys and LT/LE/NE miss the same 25 keys of the first leaf, with no error. With internal nodes in pages 1 and 2, every full, LT, LE and NE scan reads internal page 2 as a leaf and fails with `AME_PF` before returning one record. EQ, GT and GE scans are not affected. |
| A2 | `amlayer/ambulk.c` | Added `AM_BulkLoad`, which reads a sorted callback stream of (key, rid) pairs, checks the order with the AM's own comparison (numeric for int and float, `strncmp` for char), and builds AM-format leaves and internal levels bottom-up. The root is written into page 0 and every page is reachable from it, so the file has exactly 1 + leaves + internal pages; a tree of one leaf is just page 0. No existing AM function was changed: the loader uses the existing AM page helpers and PF I/O. Fill factor f (`bulk_fill_factor`, in (0, 1]) applies to both levels: a leaf takes floor(f × leaf capacity) keys, at least 2, where leaf capacity = floor((4096 − 24) / (L + 8)) for key length L (254 for char(8)); an internal node takes floor(f × (maxKeys + 1)) children, at least 3 (341 children at most for char(8)). A key's rids never span two leaves, so one key can have at most floor((4096 − 24 − (L + 2)) / 6) rids (677 for char(8)); more is refused with `AME_INVALIDATTRLENGTH`. Unsorted input returns `AME_INVALIDVALUE`; on any error no index file is left behind. | The assignment requires a bulk loader without changing the existing AM implementation. | Without A2 there is no bulk-loading method to compare in Part 3. |
| A3 | `amlayer/amscan.c` | In `AM_FindNextEntry`, the step that makes a NOT_EQUAL scan skip its value now also copies the next key into `nextvalue`, both when the next key is in the same leaf and when it is in the next leaf. In the next-leaf case it also unfixes the page it fixed (`nextpageNum`) instead of `header->nextLeafPage`, which by then belonged to the following leaf. | The skip moved the cursor but left `nextvalue` holding the skipped key, so the deleted-record check right after it saw a different key, stepped back and returned the skipped key. The wrong unfix in the next-leaf case made the call fail. | `tests/test_leftpage` shows it on insert-built and bulk-layout trees alike. With the value in the middle or as the first key of a leaf, a NE scan returns it anyway: 100 instead of 99 records, or 102 instead of 99 when the key has 3 record ids. With the value as the last key of a leaf that has a next leaf, the scan stops with `AME_PF` after 49 of 99 records and leaves the next leaf fixed, so the index file cannot be closed. A value that is the smallest or largest key happened to work. A skip into a leaf emptied by deletes is still not handled (pre-existing; no workload in this assignment deletes keys). |
| A4 | `amlayer/amscan.c` | In `AM_OpenIndexScan`, a NOT_EQUAL scan now always sets its cursor to the start of the leftmost leaf. Before, it did so only when the value was in the index; for an absent value it only set `pageNum` to `AM_NULL_PAGE` (nothing to skip), which is kept. | For an absent value, `nextpageNum`, `nextIndex`, `actindex` and `nextRecIdPtr` kept whatever the scan table slot held: zeros (page 0, record pointer 0) or the cursor of an earlier scan. | `tests/test_amscan` shows it on all 7 trees: all 4807 NE scans on absent values fail. On a tree whose root is a leaf (100 keys), the scan returns 1 of 128 record ids and then a garbage negative return code. On larger trees it returns 1 or 3 wrong record ids (for example 3 of 1028), or crashes with a segmentation fault (197 of 801 values on the 800-key insert-built int tree, 505 of 601 on the 600-key bulk-loaded char(8) tree), depending on what the slot held. |
| A5 | `amlayer/amscan.c` | In `AM_OpenIndexScan`, when the value sorts after the last key of the leaf `AM_Search` returns and that leaf has a next leaf, the code moves to the next leaf and now unfixes that page (`pageNum`) instead of `header->nextLeafPage`. The LT, LE and NE cases now read the leftmost leaf from the buffer `AM_Search` returned (`searchBuf`) when that leaf is the leftmost one, instead of from `pageBuf`, which by then is the next leaf. | `header` already held the next leaf's header when the unfix ran, so the unfix named the leaf after it, which was not fixed. `AM_Search` is correct here: index `numKeys + 1` means "after the last key". | `tests/test_amscan` shows it: all 95 EQ, LT, LE, GT and GE scans on a gap value between two leaves fail with `AME_PF` and leave 2 pages fixed (the search leaf and the next leaf). The scan table slot is not freed either, so `PF_CloseFile` on the index fails. With only the unfix fixed, LT and LE on the gap after the leftmost leaf of the bulk-loaded char(8) tree return the right count (294) but the wrong record ids, because they read the first record pointer from the second leaf. |
| A6 | `amlayer/amscan.c` | In `AM_FindNextEntry`, the end-of-range check for LT and LE scans is skipped once the scan is `OVER`. | After the last record id of the last key in the last leaf, the scan is set to `OVER`. For an LE scan on the largest key, `lastpageNum`/`lastIndex` still matched the cursor, so the check set the state back to `LAST`, and the next call read on from record pointer 0. | `tests/test_amscan` shows it: LE on the largest key fails on all 7 trees. Four of them return about twice as many record ids (2066 instead of 1028, 1550 instead of 770, 2578 instead of 1284) and never reach `AME_EOF`; the other three return 1 extra garbage record id and then a garbage return code (for example 901 instead of 900). |
| A7 | `amlayer/amscan.c` | In `AM_FindNextEntry`, the early stop of an LT or LE scan whose stop position is index 0 of a leaf now needs `lastpageNum == nextpageNum` instead of `lastpageNum <= nextpageNum`. | Page numbers are not in leaf order. In a tree built by inserts the leftmost leaf is page 2 and the next leaf is often page 1. The check runs on every call, so the scan stopped at the first leaf whose page number was at least `lastpageNum`. The stop position is index 0 when the value is the first key of a leaf, or (after A5) a value in the gap before it. The `<=` was meant for a scan that skips empty leaves past its stop leaf, which needs deletes; no workload here deletes keys, and with page numbers out of leaf order `<=` does not detect that case either. | `tests/test_amscan` shows it on insert-built trees: 5 of the 9 LT scans on the first key of a leaf other than the leftmost fail. On the 800-key int tree, LT on 3980 returns 0 of 509 record ids and LT on 6040 returns 256 of 775. After A5, LT and LE on the gap value before those leaves fail the same way (12 of 95 gap scans). Bulk-loaded trees, with leaves in pages 1..k in chain order, were not affected (0 of 13). |

# Results

TODO: Add measured tables, plots, and discussion.

# Repository layout

TODO: Describe the purpose of each source, header, configuration, and results directory.
