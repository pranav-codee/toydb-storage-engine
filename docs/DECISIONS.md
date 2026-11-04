# Design decisions and requirement coverage

This file records every significant decision taken in this project: what was decided, why, and what would happen if it had not been done. Changes to the provided PF and AM code are listed in full, with their reasons and consequences, in the "Changes to provided code" table of README.md (rows B1, B2, P1 to P7, A1 to A7). This file references those rows rather than repeating them, and adds the decisions that are not code changes: build and test infrastructure, configuration, experiment design and measurement definitions.

Each entry gives the decision, the reason, the consequence if it had not been taken, and where it lives (commit hash and file:line). Entries marked **(pending)** are not done yet. Pull requests are written PR n. Line numbers refer to the tree at commit 59153ad; the code has not changed since merge commit a7478f2 (PR 11).

Commit messages also use review-finding labels: F1 to F13 (Part 2 review), B1 to B10 and K1, K3 (bulk-loader review), and P1 to P4 (the scan bugs that review found, named after the probe groups of `tests/test_amscan`). These are not change IDs: the finding B1 is not README row B1, and the probe group P1 is not README row P1.

## 1. Requirement coverage

| # | Requirement (assignment text) | Where it is met | Evidence | Status |
|---|---|---|---|---|
| R1 | Page buffering in the PF layer | `pflayer/buf.c`, P1 (4e59e5c) | test_pool, stress_pool | Done |
| R2 | LRU and MRU replacement | `pflayer/buf.c`, P3 (7bf4b73) | test_policy, stress_pool | Done |
| R3 | Strategy selectable when opening a file | `PF_OpenFileEx(fname, policy)`, P3, `pflayer/pf.c:361` | test_policy | Done |
| R4 | Dirty flag per page, explicit call to mark a page updated | `PF_MarkDirty`, P5 (2fdfbc0), `pflayer/pf.c:884` | test_dirty | Done |
| R5 | Buffer pool size configurable as a parameter | `buffer_pool_size` in toydb.conf, `PF_InitBuffer` (`pflayer/pf.c:255`), P1 | test_pool, test_config | Done |
| R6 | Statistics: pages accessed (read/write), logical and physical I/O | `PF_GetStats` (`pflayer/pf.c:929`), P6 (3b468a6) | test_stats, stress_pool | Done |
| R7 | Statistics for different mixtures of read and write queries | `bench/bench_buffer` (7 read percentages, `buf_read_pcts`), 4e99d76 | results/bench_buffer.csv | Done |
| R8 | Graph: x = read/write mixture, y = statistics | `scripts/plot_buffer.py`: buffer_io_vs_mix, buffer_io_breakdown (6c57c0a) | results/*.pdf | Done |
| R9 | Slotted-page structure on the PF layer for variable-length records | `hf/hf.c`, `hf/hfpage.c` (0d377e0, 4caa11a) | test_hfpage, test_hf | Done |
| R10 | Insertion, deletion, sequential scan | `hf/hf.c` (92a850c, 8703036, fff45ec) | test_hf | Done |
| R11 | Used to manage the student data | test_hf_load, bench_space (e3c538f, cd0ef0c) | test_hf_load output, results/bench_space.csv | Done |
| R12 | Space utilization compared with static record management | `hf/hfstatic.c`, `bench/bench_space` (4ea9e1b, f181c68, fixed in cd0ef0c, 2ae05a9, ae11a49; PR 7) | results/bench_space.csv, README Part 2 table | Done |
| R13 | Results table over different maximum record lengths (static) | `static_lengths` in toydb.conf (`toydb.conf:28`), README Part 2 table (ae11a49) | results/bench_space.csv | Done |
| R14 | Index on Student, key roll-no, built from an existing file in one operation | M1: `build_m1` (`bench/bench_index.c:745`), one heap scan with `AM_InsertEntry` per record (d7e4ea2; PR 11) | bench_index check, results/bench_index.csv | Done |
| R15 | Index built incrementally through inserts | M2: `build_m2` (`bench/bench_index.c:679`), `HF_Insert` then `AM_InsertEntry` for each record (d7e4ea2) | bench_index check, results/bench_index.csv | Done |
| R16 | Bulk-loading method for a pre-sorted file | `AM_BulkLoad` (`amlayer/ambulk.c:445`, A2; 7ba6de0, fixed in ef31977; PR 9), used by M3 `build_m3` (`bench/bench_index.c:812`), which sorts the pairs first and times the load alone | test_ambulk, test_bulk, test_bulk_data, bench_index check | Done |
| R17 | Compare the three: query completion time, pages accessed | `run_batch` (`bench/bench_index.c:1192`): time and index logical and physical reads per query for point, missing-key, range and full-scan queries, plus build cost and tree size (d7e4ea2); `scripts/plot_index.py` (c25df58) | results/bench_index.csv, results/index_*, README Part 3 results | Done |
| S1 | Code follows programming etiquette, commented | all new code; `CHANGED [Xn]` markers in provided code | review | Ongoing |
| S2 | README: description, detailed compile and run steps | README.md: Overview, Build, Run, Configuration, Results and layout (d4dc6b6), Part 1 results (59153ad) | review | Done |
| S3 | Submission as rollno1-rollno2.tar.gz | final packaging | | **(pending)** |
| S4 | No code changes after submission, only the config file | all tunables in toydb.conf (D3), tests independent of config values (D4) | test_config present_only, check modes | Done, keep checking |

## 2. Build, test and configuration infrastructure

**D1. Top-level Makefile; provided code compiled as gnu89 (README B1).**
Reason: the provided PF and AM code is K&R C. Current compilers default to C23, where it does not compile (gcc 15.2 reports 32 errors in the shipped buf.c, 47 in pf.c and 19 in hash.c).
If not done: the code does not build with a current gcc, so nothing can be graded. New code is compiled separately with `-std=gnu89 -Wall -Wextra` and kept warning-free; provided code keeps `-w` so its original warnings do not hide new ones.
Where: 43ecc25; `Makefile:27` (NEWFLAGS).

**D2. One `pf.h` shared by PF and AM, page size overridable (README B2).**
Reason: the PF and AM layers had separate copies of `pf.h`, and they had already drifted: the AM copy used a 1020-byte page, the PF copy 4096 bytes.
If not done: the B+ tree would be built for a different page size than the PF layer provides, and any later prototype change in one layer would silently mismatch the other.
Where: b9c588e; `pflayer/pf.h`.

**D3. All tunables in one key=value config file, `toydb.conf`.**
Reason: the submission rules allow changes only to the config file after submission. Every parameter an evaluator might want to vary (pool size, default policy, data file, benchmark sizes, record lengths, seeds, repeats, fill factor) is therefore a config key, read through `common/config.c`. The file is found from a `conf=` argument, then `$TOYDB_CONF`, then `./toydb.conf`; individual keys can also be overridden as `key=value` arguments. Everything from the first `#` on a line is a comment.
If not done: changing an experiment parameter would require a code change, which the rules penalise.
Where: b089ed3; `common/config.h:46`, `common/config.h:60`, `common/config.c:70`.

**D4. `make test` must pass whatever valid values the config holds.**
Reason: an evaluator may edit toydb.conf and then run `make test`. `test_config` checks only that each key is present and parses (`CONF_MODE=present_only`, the default); `make test CONF_MODE=exact` additionally compares every key to its documented default, for us. Benchmark check modes use fixed, hand-computed fixtures instead of config values.
If not done: a legitimate config edit would make `make test` fail and look like broken code.
Where: b089ed3 (CONF_MODE), 28a1956 (present_only default); `Makefile:59`.

**D5. Test harness: each program runs in a scratch directory with a timeout, and passes only if its last output line is the expected "done!" line.**
Reason: several provided test mains have no return statement, so their exit codes are meaningless; running in a scratch directory keeps stray files out of the repo; the timeout turns a hang into a failure.
If not done: tests could "pass" with garbage exit codes, leave files behind, or hang `make test`.
Where: 43ecc25 (scratch directory, last-line check), b089ed3 (timeout); `Makefile:259`, `Makefile:266`.

**D6. Data-dependent tests resolve `data_file` against the repo root (`TOYDB_ROOT`), and report SKIPPED (with a visible NOTE) if the file is missing.**
Reason: tests run in a scratch directory, where the relative default `../data/student.txt` does not resolve. The data file is not part of the repo.
If not done: `make test` failed on any fresh checkout; and a silent skip could hide a missing data file during final checks.
Where: 2671cf5; `Makefile:278`, `tests/test_hf_load.c:152`.

**D7. Hand-computed expected values in every test, plus randomized model-based tests.**
Reason: a test that compares a program with its own earlier output proves nothing. Scenario tests use values derived by hand; `tests/stress_pool.c` checks every PF call against an independent model of the documented buffer rules. `make stress` runs 20 seeds of 200,000 operations; all 20 seeds also pass at 2,000,000 operations, and seeds 1 and 2 at 2,000,000 operations are clean under ASan and UBSan. A deliberately broken model rule (LRU victim taken from the head of the list) was caught at steps 70, 4000 and 60 for seeds 1, 2 and 3, showing the test can fail.
If not done: subtle errors in replacement order or counter attribution would go unnoticed.
Where: a009fba; `tests/stress_pool.c:360` (model victim rule), `Makefile:66`.

## 3. Part 1: buffer manager

Code changes P1 to P7 are documented in the README table. The decisions behind their semantics:

**D8. Replacement victim is chosen by the policy of the file whose page is being brought in; the victim may belong to any file.**
Reason: the pool is shared, and the requirement is that the strategy is chosen per file at open.
If not done: a per-file policy would have no defined meaning when two files with different policies share the pool.
Where: 7bf4b73; `pflayer/buf.c:121`.

**D9. Recency: a page moves to the head of the list when it is loaded and when it is unfixed; a hit does not move it.** LRU evicts the first unfixed page from the tail, MRU the first unfixed page from the head.
Reason: this matches the provided code's list discipline, so the change is minimal, and it gives a well-defined order that the tests and the model can check exactly.
If not done: LRU and MRU would have no precise definition to test against.
Where: `pflayer/buf.c:221` (load), `pflayer/buf.c:377` (hit, no move), `pflayer/buf.c:443` (unfix).

**D10. The dirty bit is set only by an unfix with dirty=TRUE, by `PF_MarkDirty`, or by page allocation; an unfix with dirty=FALSE never clears it.**
Reason: the requirement asks for an explicit mark-dirty call; clearing the bit on a clean unfix would silently lose an earlier modification.
If not done: a page marked dirty and later unfixed clean would never be written back.
Where: 2fdfbc0; `pflayer/pf.c:901`.

**D11. Statistics definitions and attribution.** Reads, hits, misses and logical writes are counted against the file of the requested page; evictions, dirty evictions and physical writes against the victim's file. A logical write is counted at most once per fix period. Every page read from disk is a physical read. File-header I/O is not counted. `PF_FlushAll` writes all dirty pages and keeps them resident, in the same place in the list. Per-file counters reset at open; a global total keeps the history of closed files.
Reason: each counter then has one unambiguous meaning, and hits + misses = logical reads holds exactly (checked in every test and every benchmark run).
If not done: counters could double-count or be charged to the wrong file, making the LRU/MRU comparison unreliable.
Where: 3b468a6; `pflayer/pf.c:929` (definitions), `pflayer/pf.c:1053` (PF_FlushAll), `pflayer/pf.c:442` (reset at open).

**D12. A failed `PF_CloseFile` (file still has a fixed page) is left as in the original code.**
Finding: it returns PFE_PAGEFIXED after it has already written back (if dirty) and released every unfixed page of that file that comes before the first fixed one in the used list. This is provided behaviour, found by the stress test and modelled there.
Reason: graders do not rely on it, and changing provided behaviour without a requirement adds risk.
If not done (if changed): an extra, unrequired change to provided code.
Where: a009fba; `tests/stress_pool.c:37`.

**D13. Benchmark design (`bench/bench_buffer`).** A 1000-page file; pool sizes 20, 50, 100, 900 (close to the file size) and 1200 (larger than the file); three access patterns: uniform (control, where LRU and MRU should match), hot/cold with 80% of accesses to 20% of pages (favours LRU), and a looping sequential scan (LRU's worst case, favours MRU); read percentages 100, 90, 75, 50, 25, 10, 0; 5 repeats (the `repeats` key, which Part 3 also uses); 10,000 operations per run.
Reason: the three patterns separate the two policies in both directions, and the control shows the measurement is fair.
If not done: a single pattern would show only one side of the comparison.
Where: 4e99d76; `bench/bench_buffer.c:578` (config keys), toydb.conf Part 1 section.

**D14. LRU and MRU, and every pool size, see exactly the same operation stream; the page sequence is also independent of the read percentage.** The page stream is seeded from (seed, repeat, pattern); the read/write choice from a second generator that also takes the read percentage.
Reason: replacement ignores the dirty bit, so with the same page sequence hits, misses, physical reads and evictions are identical at every read mix, and the graph shows only the effect of the write mix (write-back cost). Verified on results/bench_buffer.csv: 0 of 150 (policy, pattern, pool, repeat) groups differ.
If not done: random differences between streams would appear as effects on the read-mix graph.
Where: 01452fe; `bench/bench_buffer.c:127`, `bench/bench_buffer.c:152`, `bench/bench_buffer.c:158`.

**D15. Physical writes are reported twice: before and after a final `PF_FlushAll`.**
Reason: with pools larger than the file there are no evictions, so a write-heavy run would otherwise show zero physical writes, as if writes were free (pool 1200 at 0% reads: `physical_writes` 0, `total_physical_writes` 1000 for the uniform and sequential patterns and 926 to 936 for hot/cold, which does not touch every page).
If not done: the write cost of large pools would be invisible.
Where: 4e99d76; `bench/bench_buffer.c:28`.

**D16. Figures sized for a single-column report (6.3 in wide); LRU and MRU always in the same colour-blind-safe colours plus distinct markers and line styles; min/max over repeats as error bars; PDFs reproducible byte for byte (no creation date in the metadata).**
Reason: readable when included at text width, and in greyscale print.
Where: 6c57c0a; `scripts/plot_buffer.py:22`, `scripts/plot_buffer.py:43`, `scripts/plot_buffer.py:223`.

## 4. Part 2: slotted pages and static management

**D17. Record ids are page × 4096 + slot (`HF_RID_STRIDE`), always ≥ 0; records longer than `HF_MAX_RECLEN` (1024) are rejected with HFE_TOOLONG, in both layouts.**
Reason: a single int rid that can be passed to the AM layer as the record id of an index entry.
Where: 13e8988; `hf/hf.h:18`, `hf/hf.h:29`, `hf/hf.c:212`; static layout since 2ae05a9.

**D18. Utilization = useful bytes / (data pages × page size), for both layouts, with the header page excluded and useful bytes = the bytes of record content actually stored.**
Reason: one definition for both layouts makes the comparison fair.
If not done: comparing slot capacity with stored data inflates static utilization (finding F1: the first version reported 0.94 to 0.99 for static, which reversed the real result).
Where: e473fd6 (definition), 2ae05a9 (static implementation); `hf/hf.h:42`, `hf/hfstatic.c:575`.

**D19. A record is the content of one line, without its line terminator. The data file has CRLF line endings; the loader strips a trailing "\n" and then one trailing "\r". Lines without 16 semicolon-separated fields (the title line) and empty lines (the last line, which holds only "\r\n") are skipped. A line too long for the read buffer is rejected with its line number instead of being split.**
Reason: the terminator is a file-format artifact, not data.
If not done: every record carries a stray "\r", inflating useful bytes by about 1% (1,776,824 instead of 1,759,011) and shifting truncation counts (found in review: longest record 109 instead of 108).
Where: cd0ef0c; `tests/test_hf_load.c:43`, `bench/bench_space.c:40`.

**D20. Tests and benchmarks assert invariants, not dataset-specific numbers.**
Reason: an evaluator may point `data_file` at another valid file. Inserted = scanned = stats records; every record reads back byte-equal by rid and in a full scan; useful bytes = sum of the lengths inserted (for static, the sum of min(length, L)); utilization = useful / (pages × page size). An empty or header-only file is valid input with 0 records.
If not done: any other data file made `make test` and the benchmark fail.
Where: cd0ef0c, 2ae05a9; `tests/test_hf_load.c:212`, `tests/test_hf_load.c:260`, `bench/bench_space.c:273`.

**D21. Static layout: each slot stores a 2-byte record length (top bit = truncated, 0xFFFF = empty slot) followed by L payload bytes; slots per page = floor((page size - 4-byte page header) / (L + 2)). The header page keeps the live record count, the live truncated count and the sum of stored lengths, updated by insert and delete.**
Reason: to support variable-length records in fixed slots, the length must be stored somewhere; without it a short record reads back as L bytes, stored bytes cannot be known, and delete cannot update the statistics (findings F1, F2, F4).
If not done: wrong utilization, a truncation count that grows across delete/reinsert, and records returned with padding.
Result: slotted 452 pages, utilization 0.950; static utilization from 0.957 at L=100 (5,749 records truncated) down to 0.361 at L=256; slots per page 40, 38, 36, 31, 25, 20 and 15 for L = 100, 104, 110, 128, 160, 200 and 256.
Where: 2ae05a9; `hf/hfstatic.c:23`, `hf/hfstatic.c:34`, `hf/hfstatic.c:452`.

**D22. Static record lengths compared: 100, 104, 110, 128, 160, 200, 256 (`static_lengths`).** The longest record is 108 bytes, so 108 is the smallest length that stores every record whole. 100 and 104 are below it and show truncation (5,749 and 159 records); 110 is the smallest configured length without truncation; larger values show the growing waste.
Where: `toydb.conf:28`.

**D23. Churn experiment: delete `churn_fraction` (0.3) of the records, chosen by a shuffle seeded with `seed`, and reinsert the same records, then measure again.**
Reason: shows whether each layout reuses freed space. With the same records reinserted, the churn rows must equal the full-load rows in records, useful bytes and truncation count; the benchmark asserts this.
Where: f181c68, ae11a49; `bench/bench_space.c:163`, `bench/bench_space.c:212`, `bench/bench_space.c:302`, `toydb.conf:29`.

## 5. Part 3: B+ tree index

**D24. Index key: roll-no, field 2 of student.txt, as char(8).**
Reason: every roll number in student.txt has 8 characters; 836 of the 17,813 contain a letter (for example 96D07001) and 3,110 start with 0. Char keys compare with `strncmp`, so the pairs are sorted with `memcmp`, which gives the same order on 8-byte keys.
If not done: an int key would reject the alphanumeric roll numbers and lose the leading zeros.
Where: d7e4ea2; `bench/bench_index.c:119` (KEYLEN), `bench/bench_index.c:310` (`roll_number`), `bench/bench_index.c:639` (`AM_CreateIndex(..., 'c', KEYLEN)`).

**D25. `GetLeftPageNum` descends the leftmost child pointers from the root (README A1).**
Reason and consequence: in the README table. It is a prerequisite for bulk loading, whose leaves start at page 1.
Where: b021766; `amlayer/amscan.c:539`.

**D26. NOT_EQUAL scan fix (README A3).** Found by the A1 test (`tests/test_leftpage`); affects insert-built trees too. The case of a skip into a leaf emptied by deletes is pre-existing and not reachable in this assignment's workloads (no deletes from indexes), so it is documented in the README A3 row, not changed.
Where: 1edfe14; `amlayer/amscan.c:301` (`AM_FindNextEntry`).

**D27. Bulk loader rules (README A2), as fixed after the bulk-loader review.**
Page 0: the AM layer always starts at page 0, so the root is written directly into page 0. Leaves are allocated at pages 1..k and chained in key order, the internal levels follow, and a tree of one leaf is just page 0. The file therefore has exactly 1 + leaves + internal pages, all reachable from page 0 (the first version left one page orphaned: 73 pages for the student index instead of 72).
Fill factor: f = `bulk_fill_factor`, in (0, 1]. A leaf takes floor(f × leaf capacity) keys, at least 2, where leaf capacity is the real byte capacity of a leaf (254 for char(8); the first version used the internal node's maxKeys). An internal node takes floor(f × (maxKeys + 1)) children, at least 3, at most 341 for char(8). The clamps make every f in (0, 1] valid; the student index has 71, 94 and 141 leaves at f = 1.0, 0.75 and 0.5.
Other rules: pages are built with the AM's own page functions; equal keys are one entry with a list of record ids that never spans two leaves (at most 677 rids per char(8) key, else `AME_INVALIDATTRLENGTH`); the input order is checked with `AM_Compare`, so int and float keys load in numeric order (`AME_INVALIDVALUE` otherwise); on any error no fixed page and no index file is left.
Reason: the existing AM scan, search and insert code must work unchanged on a bulk-loaded tree, and the size comparison in Part 3 must count only reachable pages.
If not done: an orphaned page inflates the M3 size; a leaf target taken from maxKeys does not fill leaves to f; an int key list loaded in byte order is not a valid B+ tree.
Where: 7ba6de0, ef31977 (fixes B1 to B5, B7), b01082f (tests for findings B8 to B10, K1, K3); PR 9. `amlayer/ambulk.c:152` (`bulk_target`), `amlayer/ambulk.c:173` (`bulk_plan_leaves`), `amlayer/ambulk.c:509` (order check), `amlayer/ambulk.c:523` (rid limit), `amlayer/ambulk.c:557` (targets), `tests/test_bulk.c`.

**D28. Provided AM behaviour deliberately left unchanged:** no node merge on delete, 50/50 leaf split, a single global scan stack, the `bcopy` redefinition, and the unused debug helper `AM_DumpLeafPages`, which reads `pageNum` before setting it.
Reason: none affects correctness here, and changing them would alter the baseline that bulk loading is compared against.
Where: `amlayer/amprint.c:74`.

**D29. Index build methods.** M1 (existing file): the heap file is already loaded and not measured; one heap scan inserts every (roll no, rid) with `AM_InsertEntry`. M2 (incremental): from an empty heap file and an empty index, each record goes into the heap file with `HF_Insert` and its key into the index right away. M3 (bulk): one heap scan collects the pairs, `qsort` sorts them and `AM_BulkLoad` builds the tree; the scan plus sort and the load are timed apart, and the load alone is the cost for a file that is already sorted. M2 runs first and its heap file is the one M1 and M3 index, so all three trees hold the same (key, rid) pairs.
Reason: the three methods the assignment names, on identical data.
If not done: the trees would index different rid sets and their query results could not be compared.
Where: d7e4ea2; `bench/bench_index.c:679` (M2), `bench/bench_index.c:745` (M1), `bench/bench_index.c:812` (M3), `bench/bench_index.c:1305` (`run_repeat`, order M2, M1, M3).

**D32. Index and heap I/O are measured separately, with a self-check against the global totals.** Only the heap file and the index file are ever open, and the PF counters charge every page access, including the write-back of an evicted dirty page, to the file that owns the page. While a file is open its own `PF_GetStats(fd)` counters are read after `PF_FlushAll`, so closing it writes nothing more. `AM_CreateIndex`, `HF_CreateFile` and `AM_BulkLoad` open and close their file themselves; each runs with no other file open, so the global delta around the call is that file's I/O. For every build the global delta must equal index + heap in logical and physical reads and writes (for M3, checked on the heap scan; the load is measured as a global delta with no other file open), and in every measurement hits + misses must equal logical reads. Any difference stops the run.
Reason: heap and index pages compete for one pool, and the comparison is about index cost.
If not done: M2's heap traffic (4.0 million heap page transfers) would be charged to its index, and a counting error would go unnoticed.
Where: d7e4ea2; `bench/bench_index.c:43` (method), `bench/bench_index.c:526` (`check_split`), `bench/bench_index.c:659` (`finish_open_build`), `bench/bench_index.c:855` (M3 heap scan check).

**D33. All three indexes answer identical query sets, and every result is checked.** One query set is made per repeat from `seed` and the repeat number: `idx_point_queries` EQ scans on existing keys, `idx_missing_queries` absent values (half in a gap between two leaves, half in a gap inside a leaf), `idx_range_queries` GE scans stopped after `idx_range_len` results, and one full scan. Every query's rids are compared with a sorted in-memory copy of the pairs.
Reason: the same queries make the per-query costs comparable, and the check proves the three indexes return the same sets.
If not done: differences between query sets would look like differences between the trees, and a wrong scan result could still look fast.
Where: d7e4ea2; `bench/bench_index.c:1036` (`make_queries`), `bench/bench_index.c:1136` (`verify`), `bench/bench_index.c:1324` (one set per repeat).

**D34. A cold pool for every build and every query batch.** All files are closed and `PF_InitBuffer(buffer_pool_size)` sets up an empty pool (50 frames in toydb.conf) before each build, each batch of one query kind on one index, and the separate record-fetch pass.
Reason: each measurement starts from the same state, whatever ran before it.
If not done: a batch would find pages left by the previous one, so the results would depend on the order in which methods and query kinds run.
Where: d7e4ea2; `bench/bench_index.c:554` (`cold_pool`), `bench/bench_index.c:1213` (query batch), `bench/bench_index.c:1271` (record fetch).

**D35. Scan bugs P1 to P4, found by the bulk-loader review, fixed as README A4 to A7.** Testing scans on bulk-loaded trees showed four pre-existing bugs in `amlayer/amscan.c` that affect insert-built trees too. `tests/test_amscan` checks every operator on every key, gap value and bound of insert-built and bulk-loaded trees and groups the failing probes: P1 (NE on an absent value, 4807 of 4807 probes failed) is fixed by A4, P2 (a scan on an absent value in the gap after a non-last leaf, 95 of 95) by A5, P3 (LE on the largest key, 7 of 7) by A6, and P4 (LT on the first key of a leaf other than the leftmost, 5 of 19) by A7. After the fix none fails; the 52,721 other probes passed before and after.
Reason: Part 3's missing-key queries probe exactly these gap values, and the comparison needs correct scans on both kinds of tree.
If not done: missing-key and boundary queries would fail with `AME_PF`, return wrong rids or crash.
Where: 0ade193; PR 10 (c13aa0c). `amlayer/amscan.c:257`, `amlayer/amscan.c:282` (A4), `amlayer/amscan.c:118` (A5), `amlayer/amscan.c:501` (A6), `amlayer/amscan.c:368` (A7), `tests/test_amscan.c:29`.

**D36. Every scan open computes the leftmost leaf (provided behaviour, left unchanged).** `AM_OpenIndexScan` calls `GetLeftPageNum` for every scan, also for EQ and GE scans that do not use it. On a tree of height 2 that is 2 page fixes, the search is 2 more, and `AM_FindNextEntry` fixes the current leaf once for every rid it returns, so a point query costs about 5 logical reads (5.00 to 5.01 measured) instead of 2.
Reason: it is provided behaviour and costs the same on all three trees, so it does not change the comparison.
If not done (if changed): an extra change to provided code, and logical reads that no longer match the provided AM layer.
Where: `amlayer/amscan.c:79` (line 78 in the shipped a624959); README Part 3 results, "Queries".

**D37. Physical reads mostly come from the OS file cache, so times differ less than page counts.** A PF physical read is a `read()` call, and the pages are usually in the operating system's file cache. M3 needs 41% fewer physical reads per point query than M1 (0.323 against 0.550) but only 23% less time (0.46 against 0.60 µs); a whole point query takes under 1 µs, less than one device read, and M2 moves 4.0 million heap pages in about 2 s. The page counts are therefore the main result, and the times are reported next to them.
Reason: the page counts do not depend on the machine or on its cache.
If not done: the small time differences would understate the effect of the smaller tree; on a real disk the page counts would matter much more than these times.
Where: results/index_summary.csv; README Part 3 results.

## 6. Process

**D30. Code changes are made on their own branch and merged into main through a pull request with a merge commit. Only the initial setup commits, documentation-only commits and the results commit (0a3e199, generated files only) are committed directly to main. Code changes are verified before merge by checks chosen for the change, such as a fresh clone, both config modes, sanitizers or an adversarial review.**
Reason: a reviewable history, and no unverified code on main.
Where: `git log --first-parent main`.

**D31. `bench_space` and `test_hf_load` validate their input before producing output, and a failed benchmark run keeps the previous CSV.** `bench_space` checks every `static_lengths` value (1 to `HF_MAX_RECLEN`), an empty list and `churn_fraction` (0 to 1) before it loads data, prints anything or creates a file, and names the bad value. Both programs stop with the line number on a line that does not fit the read buffer or a record longer than `HF_MAX_RECLEN`, before any results are printed. `make bench-space` writes to `results/bench_space.csv.tmp` and renames it only on success.
Reason: a partly written or silently wrong results file looks like a valid result.
If not done: an invalid `static_lengths` value printed half a CSV and failed with "static create failed", and the previous good CSV was deleted.
Where: cd0ef0c, ae11a49; `bench/bench_space.c:434`, `Makefile:227`, `Makefile:230`.
