# Top-level Makefile for toyDB.
#
#   make            builds pflayer/pflayer.o and amlayer/amlayer.o
#   make tests      builds the test programs
#   make test       runs them in a scratch dir and checks each one finished
#   make clean      removes build outputs and stray test files
#
# Each .c is compiled from its own directory, so #include "pf.h" picks up the
# pf.h next to the source file (amlayer has its own copy until B2). No -I flags.

CC     = gcc
CFLAGS = -std=gnu89 -g -O2 -w
LD     = ld

PF  = pflayer
AM  = amlayer

PF_OBJS = $(PF)/buf.o $(PF)/hash.o $(PF)/pf.o
PF_HDRS = $(PF)/pf.h $(PF)/pftypes.h

AM_OBJS = $(AM)/am.o $(AM)/amfns.o $(AM)/amsearch.o $(AM)/aminsert.o \
	  $(AM)/amstack.o $(AM)/amglobals.o $(AM)/amscan.o $(AM)/amprint.o
AM_HDRS = $(AM)/am.h $(AM)/pf.h $(AM)/testam.h

AM_TESTS  = $(AM)/test1 $(AM)/test2 $(AM)/test3
TEST_BINS = $(AM_TESTS) $(AM)/amtest $(PF)/testpf

STRAY = testrel.* file1 file2

.PHONY: all tests test clean

all: $(PF)/pflayer.o $(AM)/amlayer.o

$(PF)/pflayer.o: $(PF_OBJS)
	$(LD) -r -o $@ $(PF_OBJS)

$(AM)/amlayer.o: $(AM_OBJS)
	$(LD) -r -o $@ $(AM_OBJS)

$(PF)/%.o: $(PF)/%.c $(PF_HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(AM)/%.o: $(AM)/%.c $(AM_HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

tests: $(TEST_BINS)

$(AM_TESTS): %: %.o $(AM)/misc.o $(AM)/amlayer.o $(PF)/pflayer.o
	$(CC) $(CFLAGS) -o $@ $^

$(AM)/amtest: $(AM)/main.o $(AM)/amlayer.o $(PF)/pflayer.o
	$(CC) $(CFLAGS) -o $@ $^

$(PF)/testpf: $(PF)/testpf.o $(PF)/pflayer.o
	$(CC) $(CFLAGS) -o $@ $^

# The provided test mains have no return statement, so exit codes are garbage.
# A program passes only if its last output line is the expected "done!" line.
# testpf prints no "done!" line; "hash table:" is printed by the last statements
# of its main, after every exit(1) check, so that line is used instead.
# The scratch dir is kept on failure so the logs can be read.
test: tests
	@dir=`mktemp -d "$${TMPDIR:-/tmp}/toydb-test.XXXXXX"` || exit 1; \
	fail=0; \
	check() { \
		prog=$$1; mode=$$2; want=$$3; name=`basename $$prog`; \
		log="$$dir/$$name.log"; \
		( cd "$$dir" && rm -f $(STRAY) && "$(CURDIR)/$$prog" ) > "$$log" 2>&1; \
		if [ "$$mode" = last ]; then \
			ok=`tail -n 1 "$$log" | grep -cxF "$$want"`; \
		else \
			ok=`grep -cxF "$$want" "$$log"`; \
		fi; \
		if [ "$$ok" -gt 0 ]; then \
			echo "PASS $$prog"; \
		else \
			echo "FAIL $$prog (expected \"$$want\"), last lines of $$log:"; \
			tail -n 5 "$$log" | sed 's/^/    /'; \
			fail=1; \
		fi; \
	}; \
	check $(AM)/test1  last "test1 done!"; \
	check $(AM)/test2  last "test2 done!"; \
	check $(AM)/test3  last "test3 done!"; \
	check $(AM)/amtest last "test3 done!"; \
	check $(PF)/testpf line "hash table:"; \
	if [ $$fail -ne 0 ]; then \
		echo "make test: FAILED, logs kept in $$dir"; exit 1; \
	fi; \
	rm -rf "$$dir"; \
	echo "make test: all passed"

clean:
	rm -f $(PF)/*.o $(AM)/*.o $(TEST_BINS) $(PF)/testhash a.out $(AM)/a.out
	rm -f $(STRAY) $(addprefix $(PF)/,$(STRAY)) $(addprefix $(AM)/,$(STRAY))
