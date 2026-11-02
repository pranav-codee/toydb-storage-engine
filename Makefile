# Top-level Makefile for toyDB.
#
#   make            builds pflayer/pflayer.o and amlayer/amlayer.o
#   make tests      builds the test programs
#   make test       runs them in a scratch dir and checks each one finished
#   make stress     builds tests/stress_pool and runs it for STRESS_SEEDS
#                   (slow, not part of make test)
#   make bench      runs the buffer manager benchmark (bench/bench_buffer)
#                   with toydb.conf into results/bench_buffer.csv
#   make clean      removes build outputs and stray test files
#
# Each .c is compiled from its own directory. Provided PF code gets no -I
# flags except -Icommon, for the config.h include in pf.c. AM sources get
# -Ipflayer, so "pf.h" resolves to the single shared header. New code
# (common/, tests/) is built with NEWFLAGS and -Icommon and must compile with
# zero warnings; tests/ also gets -Ipflayer because the PF tests include pf.h.

CC     = gcc
CFLAGS = -std=gnu89 -g -O2 -w
NEWFLAGS = -std=gnu89 -g -O2 -Wall -Wextra
NEWINC = -Icommon
LD     = ld

PF  = pflayer
AM  = amlayer

PF_OBJS = $(PF)/buf.o $(PF)/hash.o $(PF)/pf.o
PF_HDRS = $(PF)/pf.h $(PF)/pftypes.h

AM_OBJS = $(AM)/am.o $(AM)/amfns.o $(AM)/amsearch.o $(AM)/aminsert.o \
	  $(AM)/amstack.o $(AM)/amglobals.o $(AM)/amscan.o $(AM)/amprint.o
AM_HDRS = $(AM)/am.h $(PF)/pf.h $(AM)/testam.h

COMMON_OBJS = common/config.o
COMMON_HDRS = common/config.h

AM_TESTS  = $(AM)/test1 $(AM)/test2 $(AM)/test3
TEST_BINS = $(AM_TESTS) $(AM)/amtest $(PF)/testpf tests/test_config tests/test_pool \
	    tests/test_policy tests/test_dirty tests/test_stats tests/test_workload \
	    tests/test_hfpage tests/test_hf tests/test_hf_load bench/bench_buffer

# seconds each test program may run in make test (make test TEST_TIMEOUT=300)
TEST_TIMEOUT = 60

# present_only (the default) only checks that each key in toydb.conf is present and parses,
# so editing the values after submission cannot make make test fail.
# make test CONF_MODE=exact compares every key to its documented default.
CONF_MODE = present_only

STRAY = testrel.* file1 file2

# make stress: randomized model-based test of the buffer pool, one run per seed
# with the default op count (make stress STRESS_SEEDS="1 2 3")
STRESS_BIN     = tests/stress_pool
STRESS_SEEDS   = 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
STRESS_TIMEOUT = 600

.PHONY: all tests test stress bench clean

all: $(PF)/pflayer.o $(AM)/amlayer.o $(COMMON_OBJS)

# config.o is linked in because PF_Init reads buffer_pool_size (P1)
$(PF)/pflayer.o: $(PF_OBJS) $(COMMON_OBJS)
	$(LD) -r -o $@ $(PF_OBJS) $(COMMON_OBJS)

$(AM)/amlayer.o: $(AM_OBJS)
	$(LD) -r -o $@ $(AM_OBJS)

$(PF)/%.o: $(PF)/%.c $(PF_HDRS)
	$(CC) $(CFLAGS) $(NEWINC) -c $< -o $@

$(PF)/pf.o: $(COMMON_HDRS)

$(AM)/%.o: $(AM)/%.c $(AM_HDRS)
	$(CC) $(CFLAGS) -Ipflayer -c $< -o $@

common/%.o: common/%.c $(COMMON_HDRS)
	$(CC) $(NEWFLAGS) $(NEWINC) -c $< -o $@

hf/hfpage.o: hf/hfpage.c hf/hfpage.h hf/hf.h $(PF)/pf.h
	$(CC) $(NEWFLAGS) $(NEWINC) -Ihf -I$(PF) -c $< -o $@

hf/hf.o: hf/hf.c hf/hf.h hf/hfinternal.h $(PF)/pf.h
	$(CC) $(NEWFLAGS) $(NEWINC) -Ihf -I$(PF) -c $< -o $@

tests/%.o: tests/%.c $(COMMON_HDRS)
	$(CC) $(NEWFLAGS) $(NEWINC) -I$(PF) -c $< -o $@

tests/test_hfpage.o: tests/test_hfpage.c hf/hfpage.h hf/hf.h $(PF)/pf.h
	$(CC) $(NEWFLAGS) $(NEWINC) -Ihf -I$(PF) -c $< -o $@
tests/test_hf.o: tests/test_hf.c hf/hf.h hf/hfinternal.h $(PF)/pf.h
	$(CC) $(NEWFLAGS) $(NEWINC) -Ihf -I$(PF) -c $< -o $@
tests/test_hf_load.o: tests/test_hf_load.c hf/hf.h $(COMMON_HDRS)
	$(CC) $(NEWFLAGS) $(NEWINC) -Ihf -I$(PF) -c $< -o $@
tests/test_pool.o: $(PF_HDRS)
tests/test_policy.o: $(PF_HDRS)
tests/test_dirty.o: $(PF_HDRS)
tests/test_stats.o: $(PF_HDRS)
tests/test_workload.o: $(PF_HDRS)
tests/stress_pool.o: $(PF_HDRS)

bench/bench_buffer.o: bench/bench_buffer.c $(COMMON_HDRS) $(PF_HDRS)
	$(CC) $(NEWFLAGS) $(NEWINC) -I$(PF) -c $< -o $@

tests: $(TEST_BINS)

$(AM_TESTS): %: %.o $(AM)/misc.o $(AM)/amlayer.o $(PF)/pflayer.o
	$(CC) $(CFLAGS) -o $@ $^

$(AM)/amtest: $(AM)/main.o $(AM)/amlayer.o $(PF)/pflayer.o
	$(CC) $(CFLAGS) -o $@ $^

$(PF)/testpf: $(PF)/testpf.o $(PF)/pflayer.o
	$(CC) $(CFLAGS) -o $@ $^

tests/test_config: tests/test_config.o common/config.o
	$(CC) $(NEWFLAGS) -o $@ $^

# test_pool gets config.o through pflayer.o, so it must not be linked twice
tests/test_pool: tests/test_pool.o $(PF)/pflayer.o
	$(CC) $(NEWFLAGS) -o $@ $^

tests/test_policy: tests/test_policy.o $(PF)/pflayer.o
	$(CC) $(NEWFLAGS) -o $@ $^

tests/test_dirty: tests/test_dirty.o $(PF)/pflayer.o
	$(CC) $(NEWFLAGS) -o $@ $^

tests/test_stats: tests/test_stats.o $(PF)/pflayer.o
	$(CC) $(NEWFLAGS) -o $@ $^

tests/test_workload: tests/test_workload.o $(PF)/pflayer.o
	$(CC) $(NEWFLAGS) -o $@ $^

tests/test_hfpage: tests/test_hfpage.o hf/hfpage.o
	$(CC) $(NEWFLAGS) -Ihf -I$(PF) -o $@ $^

tests/test_hf: tests/test_hf.o hf/hf.o hf/hfpage.o $(PF)/pflayer.o
	$(CC) $(NEWFLAGS) -Ihf -I$(PF) -o $@ $^

tests/test_hf_load: tests/test_hf_load.o hf/hf.o hf/hfpage.o $(PF)/pflayer.o
	$(CC) $(NEWFLAGS) -Ihf -I$(PF) -o $@ $^

$(STRESS_BIN): tests/stress_pool.o $(PF)/pflayer.o
	$(CC) $(NEWFLAGS) -o $@ $^

# config.o comes in through pflayer.o, like test_pool
bench/bench_buffer: bench/bench_buffer.o $(PF)/pflayer.o
	$(CC) $(NEWFLAGS) -o $@ $^

# The provided test mains have no return statement, so exit codes are garbage.
# A program passes only if its last output line is the expected "done!" line.
# testpf prints no "done!" line; "hash table:" is printed by the last statements
# of its main, after every exit(1) check, so that line is used instead.
# check <prog> <last|line> <expected line> [args for prog...]
# Each program is killed after TEST_TIMEOUT seconds, so it misses its last line
# and fails. Exit code 124 is only used to say "timed out" in the FAIL message,
# since a provided main can return 124 by chance.
# The scratch dir is kept on failure so the logs can be read.
# TOYDB_ROOT is exported so a test can resolve relative toydb.conf paths
# (like data_file) against the repo root instead of the scratch dir.
test: tests
	@dir=`mktemp -d "$${TMPDIR:-/tmp}/toydb-test.XXXXXX"` || exit 1; \
	fail=0; \
	check() { \
		prog=$$1; mode=$$2; want=$$3; shift 3; name=`basename $$prog`; \
		log="$$dir/$$name.log"; \
		( cd "$$dir" && rm -f $(STRAY) && export TOYDB_ROOT="$(CURDIR)" && \
		  timeout $(TEST_TIMEOUT) "$(CURDIR)/$$prog" "$$@" ) > "$$log" 2>&1; \
		rc=$$?; note=""; \
		if [ "$$mode" = last ]; then \
			ok=`tail -n 1 "$$log" | grep -cxF "$$want"`; \
		else \
			ok=`grep -cxF "$$want" "$$log"`; \
		fi; \
		if [ "$$ok" -eq 0 ] && [ $$rc -eq 124 ]; then \
			note=", probably timed out after $(TEST_TIMEOUT)s"; \
		fi; \
		if [ "$$ok" -gt 0 ]; then \
			echo "PASS $$prog"; \
			if grep -q SKIPPED "$$log"; then \
				echo "NOTE $$prog: skipped, see its README section"; \
			fi; \
		else \
			echo "FAIL $$prog (expected \"$$want\"$$note), last lines of $$log:"; \
			tail -n 5 "$$log" | sed 's/^/    /'; \
			fail=1; \
		fi; \
	}; \
	check $(AM)/test1  last "test1 done!"; \
	check $(AM)/test2  last "test2 done!"; \
	check $(AM)/test3  last "test3 done!"; \
	check $(AM)/amtest last "test3 done!"; \
	check $(PF)/testpf line "hash table:"; \
	check tests/test_config last "test_config done!" "$(CURDIR)/toydb.conf" $(CONF_MODE); \
	check tests/test_pool last "test_pool done!"; \
	check tests/test_policy last "test_policy done!"; \
	check tests/test_dirty last "test_dirty done!"; \
	check tests/test_stats last "test_stats done!"; \
	check tests/test_workload last "test_workload done!"; \
	check tests/test_hfpage last "test_hfpage done!"; \
	check tests/test_hf last "test_hf done!"; \
	check tests/test_hf_load last "test_hf_load done!" "conf=$(CURDIR)/toydb.conf"; \
	check bench/bench_buffer last "bench_buffer check done!" "conf=$(CURDIR)/toydb.conf" check; \
	if [ $$fail -ne 0 ]; then \
		echo "make test: FAILED, logs kept in $$dir"; exit 1; \
	fi; \
	rm -rf "$$dir"; \
	echo "make test: all passed"

# One stress_pool run per seed in a scratch dir, stopping at the first seed
# whose last line is not "stress_pool done!" (the dir is kept then).
stress: $(STRESS_BIN)
	@dir=`mktemp -d "$${TMPDIR:-/tmp}/toydb-stress.XXXXXX"` || exit 1; \
	for s in $(STRESS_SEEDS); do \
		log="$$dir/seed$$s.log"; \
		( cd "$$dir" && export TOYDB_ROOT="$(CURDIR)" && timeout $(STRESS_TIMEOUT) \
		  "$(CURDIR)/$(STRESS_BIN)" $$s ) > "$$log" 2>&1; \
		rc=$$?; \
		if tail -n 1 "$$log" | grep -qxF "stress_pool done!"; then \
			echo "PASS seed $$s: `head -n 1 "$$log"`"; \
		else \
			note=""; [ $$rc -eq 124 ] && note=", timed out after $(STRESS_TIMEOUT)s"; \
			echo "FAIL seed $$s (exit $$rc$$note), log $$log:"; \
			tail -n 40 "$$log" | sed 's/^/    /'; \
			echo "make stress: FAILED, logs kept in $$dir"; exit 1; \
		fi; \
	done; \
	rm -rf "$$dir"; \
	echo "make stress: all passed"

# Full buffer benchmark. The CSV goes to results/ in the repo, the scratch
# paged file to work_dir from toydb.conf (relative paths from TOYDB_ROOT).
# The CSV is written to a temp name first, so a failed run leaves no
# half-written results file behind.
BENCH_CSV = results/bench_buffer.csv

bench: bench/bench_buffer
	@mkdir -p results; start=`date +%s`; \
	TOYDB_ROOT="$(CURDIR)" bench/bench_buffer "conf=$(CURDIR)/toydb.conf" \
		> $(BENCH_CSV).tmp || { rm -f $(BENCH_CSV).tmp; \
		echo "make bench: FAILED"; exit 1; }; \
	mv $(BENCH_CSV).tmp $(BENCH_CSV); end=`date +%s`; \
	echo "make bench: $(BENCH_CSV), `wc -l < $(BENCH_CSV)` lines," \
		"`expr $$end - $$start`s"

clean:
	rm -f $(PF)/*.o $(AM)/*.o common/*.o hf/*.o tests/*.o bench/*.o $(TEST_BINS) $(STRESS_BIN) \
		$(PF)/testhash a.out $(AM)/a.out
	rm -f $(STRAY) $(addprefix $(PF)/,$(STRAY)) $(addprefix $(AM)/,$(STRAY))
