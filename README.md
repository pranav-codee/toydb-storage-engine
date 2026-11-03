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

This writes `results/bench_space.csv`. The CSV contains full-load and
delete/reinsert churn rows for the slotted layout and every configured static
length. The churn experiment selects `churn_fraction` of the records with the
configured `seed`. With the supplied 17,813-record data file, the full-load
rows were:

| layout | L | records | truncated | data pages | useful bytes | records/page | utilization |
|---|---:|---:|---:|---:|---:|---:|---:|
| slotted | 0 | 17813 | 0 | 458 | 1776824 | 38 | 0.947150 |
| static | 100 | 17813 | 7213 | 446 | 1781300 | 39 | 0.975085 |
| static | 104 | 17813 | 489 | 457 | 1852552 | 38 | 0.989679 |
| static | 110 | 17813 | 0 | 482 | 1959430 | 36 | 0.992482 |
| static | 128 | 17813 | 0 | 575 | 2280064 | 30 | 0.968098 |
| static | 160 | 17813 | 0 | 713 | 2850080 | 24 | 0.975905 |
| static | 200 | 17813 | 0 | 891 | 3562600 | 19 | 0.976179 |
| static | 256 | 17813 | 0 | 1188 | 4560128 | 14 | 0.937132 |

The slot arithmetic gives 40, 39, 37, 31, 25, 20, and 15 slots per page
for lengths 100, 104, 110, 128, 160, 200, and 256 respectively, producing
the measured page counts. The churn rows have the same page counts and
utilization; static truncation counts increase when truncated records are
reinserted.

# Part 3 B+ tree

TODO: Describe index construction, bulk loading, and query measurements.

# Changes to provided code

Every modification to the supplied PF and AM code is listed below. Each edit
site in the source carries a comment of the form `/* CHANGED [ID]: ... */`
with the same ID, so a row can be traced to the lines it describes.

| ID | File(s) | What changed | Reason | Consequence if not made |
|----|---------|--------------|--------|-------------------------|
| B1 | `Makefile` (new, top level) | One top-level Makefile replaces the per-folder builds. Provided sources are compiled with `-std=gnu89 -w`, new code with `-std=gnu89 -Wall -Wextra`. `make test` runs every test program in a scratch directory and checks its final line. `make clean` removes the shipped object files and binaries. | The provided sources are pre-ANSI C (implicit `int`, K&R function definitions, calls to undeclared functions), and the shipped objects were built by an old toolchain. | The shipped binaries and per-folder builds do not build on a current gcc. Under the default C23 mode of gcc 15.2, `pflayer/buf.c` alone fails with 65 errors (implicit `int` and implicit function declarations); with `-std=gnu89` it compiles. |
| B2 | `amlayer/pf.h` (deleted), `pflayer/pf.h`, `Makefile` | The private copy of `pf.h` in `amlayer/` is removed, so the AM sources include the single `pflayer/pf.h`. `PF_PAGE_SIZE` is 4096 and can be overridden with `-DPF_PAGE_SIZE=`. | The two headers disagreed on the page size (1020 bytes in `amlayer/`, 4096 in `pflayer/`). | The stale amlayer copy had a 1020-byte page size, so B+ tree nodes used about a quarter of each 4096-byte page and Part 3 page counts would be roughly four times too high. |
| P1 | `pflayer/pftypes.h`, `pflayer/buf.c`, `pflayer/pf.c`, `pflayer/pf.h` | The compile-time constant `PF_MAX_BUFS` is replaced by a pool allocated at runtime. `PF_Init` sizes it from `buffer_pool_size` in `toydb.conf` (default 20), and the new `PF_InitBuffer(n)` resizes it while no page is in the buffer. New error codes `PFE_INVALIDBUFS` and `PFE_BUFINUSE`. | The assignment requires a configurable buffer pool size. | Every pool-size experiment needs a recompile, which the rule that only the configuration file may change after submission forbids. |
| P2 | `pflayer/pftypes.h` | `PF_HASH_TBL_SIZE` is raised from 20 to 1031, a prime. | Pools of 50 to 100 frames are used in the experiments. | With 50 to 100 frames, 20 buckets give long chains and add lookup cost to every measurement. |
| P3 | `pflayer/pftypes.h`, `pflayer/pf.c`, `pflayer/buf.c`, `pflayer/pf.h` | Each open-file entry records a replacement policy. The new `PF_OpenFileEx(fname, policy)` sets it, and victim selection searches from the tail for LRU and from the head for MRU, according to the policy of the file whose page is being loaded. New error code `PFE_INVALIDPOLICY`. | The assignment requires LRU and MRU to be selectable when a file is opened. | LRU and MRU cannot be chosen when a file is opened. |
| P4 | `pflayer/pf.c`, `pflayer/pf.h` | `PF_OpenFile` is kept as a wrapper around `PF_OpenFileEx` that uses `default_policy` from `toydb.conf` (default LRU). | Backward compatibility with existing callers. | Every AM caller and provided test would need editing. |
| P5 | `pflayer/buf.c`, `pflayer/pf.c`, `pflayer/pftypes.h`, `pflayer/pf.h` | New public call `PF_MarkDirty(fd, page)`, backed by `PFbufMarkDirty`, which sets the dirty bit of a resident page (fixed or unfixed) without changing its recency. `PFbufUsed` is unchanged. | The assignment requires an explicit mark-dirty call, and `PFbufUsed` also moves the page to the head of the list. | The dirty bit can only be set by the unfix call. |
| P6 | `pflayer/buf.c`, `pflayer/pf.c`, `pflayer/pftypes.h`, `pflayer/pf.h` | Per-file and global counters for logical reads and writes, hits, misses, physical reads and writes, evictions and dirty evictions, updated where each event occurs. New public calls `PF_GetStats`, `PF_ResetStats`, `PF_PrintStats` and `PF_FlushAll`. `PFbufGet` takes a new argument that says whether the fix is a logical read. | The assignment requires I/O statistics, and all three parts report them. | No logical or physical I/O can be measured. |
| P7 | `pflayer/buf.c`, `pflayer/hash.c` | The debug prints in `PFbufPrint` and the hash table print show the frame address with `%p` instead of `%d`. | Pointers are 64 bits wide on the target platform. | Pointers printed through `%d` are truncated on a 64-bit build. |
| A1 | `amlayer/amscan.c` | `GetLeftPageNum`, which `AM_OpenIndexScan` uses to start full, LT, LE and NE scans, now follows the leftmost child pointer `p0` from the root (page 0) down to a leaf, unfixing each page before fixing its child. Before, it returned page 0 if the root was a leaf and page 2 otherwise. | Page 2 is the leftmost leaf only in trees built by inserts, because the first root split puts the copy of the left half in page 2. A tree with any other page layout, such as the bulk-loaded tree of A2 (leaves in pages 1..k, root in page 0), has its leftmost leaf elsewhere. | `tests/test_leftpage` shows it. With leaves in pages 1..4, the full scan returns 75 of 100 keys and LT/LE/NE miss the same 25 keys of the first leaf, with no error. With internal nodes in pages 1 and 2, every full, LT, LE and NE scan reads internal page 2 as a leaf and fails with `AME_PF` before returning one record. EQ, GT and GE scans are not affected. |
| A2 | `amlayer/ambulk.c`, `amlayer/am.h` | Pending. | | |
| A3 | `amlayer/amscan.c` | In `AM_FindNextEntry`, the step that makes a NOT_EQUAL scan skip its value now also copies the next key into `nextvalue`, both when the next key is in the same leaf and when it is in the next leaf. In the next-leaf case it also unfixes the page it fixed (`nextpageNum`) instead of `header->nextLeafPage`, which by then belonged to the following leaf. | The skip moved the cursor but left `nextvalue` holding the skipped key, so the deleted-record check right after it saw a different key, stepped back and returned the skipped key. The wrong unfix in the next-leaf case made the call fail. | `tests/test_leftpage` shows it on insert-built and bulk-layout trees alike. With the value in the middle or as the first key of a leaf, a NE scan returns it anyway: 100 instead of 99 records, or 102 instead of 99 when the key has 3 record ids. With the value as the last key of a leaf that has a next leaf, the scan stops with `AME_PF` after 49 of 99 records and leaves the next leaf fixed, so the index file cannot be closed. A value that is the smallest or largest key happened to work. A skip into a leaf emptied by deletes is still not handled (pre-existing; no workload in this assignment deletes keys). |

# Results

TODO: Add measured tables, plots, and discussion.

# Repository layout

TODO: Describe the purpose of each source, header, configuration, and results directory.
