# Design decisions and requirement coverage

This file records every significant decision taken in this project: what was decided, why, and what would happen if it had not been done. Changes to the provided PF and AM code are listed in full, with their reasons and consequences, in the "Changes to provided code" table of README.md (rows B1, B2, P1 to P7, A1 to A3; A2 is still pending). This file references those rows rather than repeating them, and adds the decisions that are not code changes: build and test infrastructure, configuration, experiment design and measurement definitions.

Each entry gives the decision, the reason, the consequence if it had not been taken, and where it lives (commit hash and file:line). Entries marked **(pending)** are not merged yet. Pull requests are written PR n. Line numbers refer to the tree at merge commit 48b591d (PR 7).

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
| R14 | Index on Student, key roll-no, built from an existing file in one operation | not implemented yet | | **(pending)** |
| R15 | Index built incrementally through inserts | not implemented yet | | **(pending)** |
| R16 | Bulk-loading method for a pre-sorted file | `amlayer/ambulk.c` (A2), not implemented yet | | **(pending)** |
| R17 | Compare the three: query completion time, pages accessed | not implemented yet | | **(pending)** |
| S1 | Code follows programming etiquette, commented | all new code; `CHANGED [Xn]` markers in provided code | review | Ongoing |
| S2 | README: description, detailed compile and run steps | README.md | | **(pending)** Build, Run and Configuration sections are still empty headings |
| S3 | Submission as rollno1-rollno2.tar.gz | final packaging | | **(pending)** |
| S4 | No code changes after submission, only the config file | all tunables in toydb.conf (D3), tests independent of config values (D4) | test_config present_only, check modes | Done, keep checking |

## 2. Build, test and configuration infrastructure

**D1. Top-level Makefile; provided code compiled as gnu89 (README B1).**
Reason: the provided PF and AM code is K&R C. Current compilers default to C23, where it does not compile (gcc 15.2 reports 32 errors in the shipped buf.c, 47 in pf.c and 19 in hash.c).
If not done: the code does not build with a current gcc, so nothing can be graded. New code is compiled separately with `-std=gnu89 -Wall -Wextra` and kept warning-free; provided code keeps `-w` so its original warnings do not hide new ones.
Where: 43ecc25; `Makefile:22` (NEWFLAGS).

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
Where: b089ed3 (CONF_MODE), 28a1956 (present_only default); `Makefile:52`.

**D5. Test harness: each program runs in a scratch directory with a timeout, and passes only if its last output line is the expected "done!" line.**
Reason: several provided test mains have no return statement, so their exit codes are meaningless; running in a scratch directory keeps stray files out of the repo; the timeout turns a hang into a failure.
If not done: tests could "pass" with garbage exit codes, leave files behind, or hang `make test`.
Where: 43ecc25 (scratch directory, last-line check), b089ed3 (timeout); `Makefile:200`, `Makefile:207`.

**D6. Data-dependent tests resolve `data_file` against the repo root (`TOYDB_ROOT`), and report SKIPPED (with a visible NOTE) if the file is missing.**
Reason: tests run in a scratch directory, where the relative default `../data/student.txt` does not resolve. The data file is not part of the repo.
If not done: `make test` failed on any fresh checkout; and a silent skip could hide a missing data file during final checks.
Where: 2671cf5; `Makefile:219`, `tests/test_hf_load.c:152`.

**D7. Hand-computed expected values in every test, plus randomized model-based tests.**
Reason: a test that compares a program with its own earlier output proves nothing. Scenario tests use values derived by hand; `tests/stress_pool.c` checks every PF call against an independent model of the documented buffer rules. `make stress` runs 20 seeds of 200,000 operations; all 20 seeds also pass at 2,000,000 operations, and seeds 1 and 2 at 2,000,000 operations are clean under ASan and UBSan. A deliberately broken model rule (LRU victim taken from the head of the list) was caught at steps 70, 4000 and 60 for seeds 1, 2 and 3, showing the test can fail.
If not done: subtle errors in replacement order or counter attribution would go unnoticed.
Where: a009fba; `tests/stress_pool.c:360` (model victim rule), `Makefile:59`.

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
Reason: with pools larger than the file there are no evictions, so a write-heavy run would otherwise show zero physical writes, as if writes were free (pool 1200: `physical_writes` 0, `total_physical_writes` 1000 at 0% reads).
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

**D24. Index key: roll-no, field 2 of student.txt, as char(8).** An int key would reject the alphanumeric roll numbers and lose leading zeros. The index benchmark is not implemented yet. **(pending)**

**D25. `GetLeftPageNum` descends the leftmost child pointers from the root (README A1).**
Reason and consequence: in the README table. It is a prerequisite for bulk loading, whose leaves start at page 1.
Where: b021766; `amlayer/amscan.c:505`.

**D26. NOT_EQUAL scan fix (README A3).** Found by the A1 test (`tests/test_leftpage`); affects insert-built trees too. The case of a skip into a leaf emptied by deletes is pre-existing and not reachable in this assignment's workloads (no deletes from indexes), so it is documented in the README A3 row, not changed.
Where: 1edfe14; `amlayer/amscan.c:280` (`AM_FindNextEntry`).

**D27. Bulk loader constraints (A2):** the root must be page 0, because the AM layer hardcodes the root page; leaves are written at pages 1..k and chained; pages are built with the AM's own page functions; duplicate keys are one entry with a list of record ids. **(pending)**

**D28. Provided AM behaviour deliberately left unchanged:** no node merge on delete, 50/50 leaf split, a single global scan stack, the `bcopy` redefinition, and the unused debug helper `AM_DumpLeafPages`, which reads `pageNum` before setting it.
Reason: none affects correctness here, and changing them would alter the baseline that bulk loading is compared against.
Where: `amlayer/amprint.c:74`.

**D29. Index build methods and measurements.** **(pending)**

## 6. Process

**D30. The setup commits (a624959 to 13e8988: import, Makefile, config parser, B2, frozen interfaces) were made directly on main. Every later change was made on its own branch and merged into main through a pull request with a merge commit (PR 1 to PR 5, PR 7); the static layout (4ea9e1b, f181c68) had no pull request of its own and was merged together with its review fixes in PR 7. Changes are verified by an independent check before merge (fresh clone, both config modes, sanitizers, adversarial review).**
Reason: a reviewable history, and no unverified code on main.
Where: `git log --merges` (dc3ae7d, ba5870b, 5570c09, 92ceff1, 8e32c2a, 48b591d).

**D31. `bench_space` and `test_hf_load` validate their input before producing output, and a failed benchmark run keeps the previous CSV.** `bench_space` checks every `static_lengths` value (1 to `HF_MAX_RECLEN`), an empty list and `churn_fraction` (0 to 1) before it loads data, prints anything or creates a file, and names the bad value. Both programs stop with the line number on a line that does not fit the read buffer or a record longer than `HF_MAX_RECLEN`, before any results are printed. `make bench-space` writes to `results/bench_space.csv.tmp` and renames it only on success.
Reason: a partly written or silently wrong results file looks like a valid result.
If not done: an invalid `static_lengths` value printed half a CSV and failed with "static create failed", and the previous good CSV was deleted.
Where: cd0ef0c, ae11a49; `bench/bench_space.c:434`, `Makefile:180`, `Makefile:183`.
