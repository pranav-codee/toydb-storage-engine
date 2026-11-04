/* bench_index.c: build the roll-number B+ tree three ways and compare the
 * builds and the queries (Part 3)
 *
 * usage: bench_index [conf=<file>] [key=value ...] [check]
 *
 * Full run (no "check" argument):
 *   Reads the student file (data_file; a relative path is taken from
 *   $TOYDB_ROOT when that is set) and, for every repeat, builds a B+ tree
 *   on roll no (field 2, char(8)) in three ways:
 *     M1 existing     the heap file is already loaded (not measured); one
 *                     scan of it inserts every (roll no, rid) with
 *                     AM_InsertEntry
 *     M2 incremental  from an empty heap file and an empty index: each
 *                     record is put in the heap file with HF_Insert and its
 *                     key goes into the index right away
 *     M3 bulk         one scan of the heap file collects the (roll no, rid)
 *                     pairs, qsort puts them in AM order (memcmp, the same
 *                     as the AM's strncmp on 8-byte keys) and AM_BulkLoad
 *                     builds the tree at bulk_fill_factor. sort_ms is the
 *                     scan and the sort; idx_ms is the load alone, which is
 *                     the cost when the file is already sorted
 *   M2 runs first and its heap file is the one M1 and M3 index, so all
 *   three trees hold the same (key, rid) pairs. Then the same query set
 *   (made from seed and the repeat number) is run on each index: point
 *   (EQ on existing keys), missing (absent keys, half in the gap between
 *   two leaves, half in a gap inside a leaf), range (GE from an existing
 *   key, stopped after idx_range_len results) and one full scan. One CSV
 *   row per (method, repeat, phase) goes to stdout, progress to stderr.
 *   Every setting comes from toydb.conf (buffer_pool_size, data_file,
 *   bulk_fill_factor, idx_point_queries, idx_missing_queries,
 *   idx_range_queries, idx_range_len, repeats, seed, work_dir); key=value
 *   arguments override the file. All of them are checked before any output
 *   or file is made. The files are made in work_dir.
 *
 * Check mode ("check" argument):
 *   A synthetic data set of 604 records (600 keys, 4 of them with a second
 *   record) through the same code, with hand-computed expectations. It sets
 *   its own pool size and fill factor, so it does not depend on toydb.conf
 *   or student.txt. Files are made in the current directory. The last line
 *   is "bench_index check done!" only if every check passed. make test runs
 *   this.
 *
 * How index I/O is kept apart from heap-file I/O. Only the heap file and
 * the index file are ever open, and the PF counters (see PF_GetStats in
 * pflayer/pf.c) put every page access on the file that owns the page,
 * also the write-back of an evicted dirty page, so heap and index pages
 * that compete for one pool are still counted apart.
 *   - While a file is open its own PF_GetStats(fd) counters are used. They
 *     start at zero when the file is opened and cannot be read after the
 *     close, so PF_FlushAll runs first: the counters then include the
 *     write-back of every dirty page, and the close writes nothing more.
 *   - AM_CreateIndex, HF_CreateFile and AM_BulkLoad open and close their
 *     file themselves. Each runs while no other file is open, so the delta
 *     of the global counters PF_GetStats(-1) around the call is that file's
 *     I/O alone.
 *   - Check: for every build, the global delta over the whole build must
 *     equal index + heap for logical and physical reads and writes, and
 *     for every measurement hits + misses must equal logical reads. Any
 *     difference stops the program.
 * The pool is cold for every build and every query batch: all files are
 * closed and PF_InitBuffer(buffer_pool_size) sets up an empty pool. For
 * M1 and M2, idx_ms is the time spent inside the AM calls (for M2, the
 * index share of a build that cannot be split in two).
 *
 * Correctness: every query's record ids are compared with an in-memory
 * sorted copy of the (key, rid) pairs. Each rid must come once, its key
 * must be the next expected key, and the count must match. For EQ, the
 * full scan and every key a range scan returns in full, that is exactly
 * the expected set; only the last key of a range scan may be cut off,
 * since the order of the rids of one key differs between the trees. All
 * three indexes must pass, so they return the same sets.
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "am.h"
#include "ambulk.h"
#include "config.h"
#include "hf.h"
#include "pf.h"

/*
 * No <stdlib.h>: am.h declares malloc and calloc the K&R way, which
 * conflicts with the prototypes there. The PF and AM functions below are
 * K&R definitions without a prototype in any header; their char and short
 * parameters are passed as int.
 */
extern void exit(int);
extern void free(void *);
extern char *getenv(const char *);
extern char *realloc(void *, size_t);
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
extern long strtol(const char *, char **, int);
extern double strtod(const char *, char **);
extern void PF_Init();
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_DestroyFile(char *fname);
extern int PF_GetNextPage(int fd, int *pagenum, char **pagebuf);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);
extern int AM_CreateIndex(char *fileName, int indexNo, int attrType,
			  int attrLength);
extern int AM_DestroyIndex(char *fileName, int indexNo);
extern int AM_InsertEntry(int fileDesc, int attrType, int attrLength,
			  char *value, int recId);
extern int AM_OpenIndexScan(int fileDesc, int attrType, int attrLength,
			    int op, char *value);
extern int AM_FindNextEntry(int scanDesc);
extern int AM_CloseIndexScan(int scanDesc);
extern int AM_EmptyStack();

#define KEYLEN		8
#define RELNAME		"bench_index"		/* indexes bench_index.<m> */
#define HEAPNAME	"bench_index.pf"
#define NMETHODS	3
#define M1		0
#define M2		1
#define M3		2

static const char *method_name[NMETHODS] = { "M1", "M2", "M3" };

/* query kinds (phases after "build") */
#define Q_POINT		0
#define Q_MISSING	1
#define Q_RANGE		2
#define Q_FULL		3
#define NKINDS		4
static const char *kind_name[NKINDS] = { "point", "missing", "range",
	"fullscan" };

/* ---------------- small helpers ---------------- */

static void
fail(const char *message)
{
	fprintf(stderr, "bench_index: %s\n", message);
	exit(1);
}

static void *
xmalloc(size_t size)
{
	char *p;

	p = malloc(size > 0 ? size : 1);
	if (p == NULL)
		fail("out of memory");
	return p;
}

static double
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1.0e6;
}

/* the LCG of bench_space, two draws for 30 bits */
static unsigned long
next_random(unsigned long *state)
{
	unsigned long hi;

	*state = *state * 1103515245UL + 12345UL;
	hi = (*state >> 16) & 0x7fffUL;
	*state = *state * 1103515245UL + 12345UL;
	return (hi << 15) | ((*state >> 16) & 0x7fffUL);
}

static int
random_below(unsigned long *state, int n)
{
	return (int)(next_random(state) % (unsigned long)n);
}

/* ---------------- configuration ---------------- */

static void
bad_value(const char *key, const char *value, const char *why)
{
	fprintf(stderr, "bench_index: %s: '%s' %s\n", key, value, why);
	exit(1);
}

static int
conf_int(const char *key, int def, long low, long high)
/* The int value of key (def if the key is missing), checked strictly:
   the whole value must be an int in low..high. */
{
	const char *s;
	char *end;
	char text[64];
	long v;

	s = Config_GetStr(key, NULL);
	if (s == NULL)
		return def;
	errno = 0;
	v = strtol(s, &end, 10);
	if (s[0] == '\0' || *end != '\0' || errno == ERANGE)
		bad_value(key, s, "is not an integer");
	if (v < low || v > high) {
		sprintf(text, "is outside %ld..%ld", low, high);
		bad_value(key, s, text);
	}
	return (int)v;
}

static double
conf_fill(const char *key, double def)
/* bulk_fill_factor: a number in (0, 1] */
{
	const char *s;
	char *end;
	double v;

	s = Config_GetStr(key, NULL);
	if (s == NULL)
		return def;
	errno = 0;
	v = strtod(s, &end);
	if (s[0] == '\0' || *end != '\0' || errno == ERANGE)
		bad_value(key, s, "is not a number");
	if (!(v > 0.0 && v <= 1.0))
		bad_value(key, s, "is outside (0, 1]");
	return v;
}

struct settings {
	int pool;
	int point, missing, range, range_len, repeats;
	long seed;
	double fill;
};

/* ---------------- the data ---------------- */

struct record {			/* one input line, in file order */
	char *line;
	int len;
	char key[KEYLEN];
};

struct pair {			/* (key, rid) */
	char key[KEYLEN];
	int rid;
};

static struct record *recs;
static int nrecs;
static struct pair *sorted;	/* by key, then rid */
static struct pair *by_rid;	/* by rid */
static int *stamp;		/* per by_rid entry, for repeated-rid checks */
static int stamp_now;
static int *heap_rids;		/* rid of each record (file order), from M2 */
static int have_rids;

static int
read_line(FILE *input, char *line, int size, long line_number)
{
	char *newline;
	int c, length;

	if (fgets(line, size, input) == NULL)
		return 0;
	newline = strchr(line, '\n');
	if (newline != NULL) {
		*newline = '\0';
	} else {
		c = getc(input);
		if (c != EOF) {
			fprintf(stderr, "bench_index: line %ld does not fit the "
				"read buffer\n", line_number);
			exit(1);
		}
	}
	length = (int)strlen(line);
	if (length > 0 && line[length - 1] == '\r')
		line[length - 1] = '\0';
	return 1;
}

/* a student record has exactly 16 ';'-separated fields */
static int
student_record(const char *line)
{
	const char *p;
	int fields;

	if (line[0] == '\0')
		return 0;
	fields = 1;
	for (p = line; *p != '\0'; p++)
		if (*p == ';')
			fields++;
	return fields == 16;
}

/* field 2, which must be exactly 8 bytes; 1 if it is */
static int
roll_number(const char *line, char key[KEYLEN])
{
	const char *first, *second;

	first = strchr(line, ';');
	if (first == NULL)
		return 0;
	first++;
	second = strchr(first, ';');
	if (second == NULL || second - first != KEYLEN)
		return 0;
	memcpy(key, first, KEYLEN);
	return 1;
}

static void
add_record(const char *line, long line_number, int *capacity)
{
	if (nrecs == *capacity) {
		*capacity = *capacity > 0 ? 2 * *capacity : 1024;
		recs = (struct record *)realloc(recs, (size_t)*capacity *
						sizeof(*recs));
		if (recs == NULL)
			fail("out of memory");
	}
	recs[nrecs].len = (int)strlen(line);
	if (recs[nrecs].len > HF_MAX_RECLEN) {
		fprintf(stderr, "bench_index: line %ld is longer than "
			"HF_MAX_RECLEN (%d)\n", line_number, HF_MAX_RECLEN);
		exit(1);
	}
	if (!roll_number(line, recs[nrecs].key)) {
		fprintf(stderr, "bench_index: line %ld: field 2 is not exactly "
			"%d bytes\n", line_number, KEYLEN);
		exit(1);
	}
	recs[nrecs].line = xmalloc((size_t)recs[nrecs].len + 1);
	memcpy(recs[nrecs].line, line, (size_t)recs[nrecs].len + 1);
	nrecs++;
}

static void
load_data(const char *data_file)
{
	FILE *input;
	char line[2048];
	char path[4096];
	const char *root;
	long line_number;
	int capacity;

	root = getenv("TOYDB_ROOT");
	if (data_file[0] != '/' && root != NULL && root[0] != '\0') {
		if (snprintf(path, sizeof(path), "%s/%s", root, data_file) >=
		    (int)sizeof(path))
			fail("data_file: resolved path is too long");
		data_file = path;
	}
	input = fopen(data_file, "r");
	if (input == NULL) {
		fprintf(stderr, "bench_index: data_file: cannot open '%s'\n",
			data_file);
		exit(1);
	}
	capacity = 0;
	line_number = 0;
	while (read_line(input, line, (int)sizeof(line), line_number + 1)) {
		line_number++;
		if (student_record(line))
			add_record(line, line_number, &capacity);
	}
	fclose(input);
}

static int
compare_pairs(const void *left, const void *right)
{
	const struct pair *a, *b;
	int r;

	a = (const struct pair *)left;
	b = (const struct pair *)right;
	r = memcmp(a->key, b->key, KEYLEN);
	if (r != 0)
		return r;
	return (a->rid > b->rid) - (a->rid < b->rid);
}

static int
compare_rids(const void *left, const void *right)
{
	const struct pair *a, *b;

	a = (const struct pair *)left;
	b = (const struct pair *)right;
	return (a->rid > b->rid) - (a->rid < b->rid);
}

static void
make_model(void)
/* sorted and by_rid from the records and the rids M2 gave them */
{
	int i;

	sorted = (struct pair *)xmalloc((size_t)nrecs * sizeof(*sorted));
	by_rid = (struct pair *)xmalloc((size_t)nrecs * sizeof(*by_rid));
	stamp = (int *)xmalloc((size_t)nrecs * sizeof(*stamp));
	for (i = 0; i < nrecs; i++) {
		memcpy(sorted[i].key, recs[i].key, KEYLEN);
		sorted[i].rid = heap_rids[i];
		stamp[i] = 0;
	}
	qsort(sorted, (size_t)nrecs, sizeof(*sorted), compare_pairs);
	memcpy(by_rid, sorted, (size_t)nrecs * sizeof(*by_rid));
	qsort(by_rid, (size_t)nrecs, sizeof(*by_rid), compare_rids);
	for (i = 1; i < nrecs; i++)
		if (by_rid[i].rid == by_rid[i - 1].rid)
			fail("the heap file gave two records the same rid");
}

static int
rid_index(int rid)
/* position of rid in by_rid, -1 if no record has it */
{
	int low, high, mid;

	low = 0;
	high = nrecs - 1;
	while (low <= high) {
		mid = (low + high) / 2;
		if (by_rid[mid].rid == rid)
			return mid;
		if (by_rid[mid].rid < rid)
			low = mid + 1;
		else
			high = mid - 1;
	}
	return -1;
}

static int
lower_bound(const char *key)
/* first position in sorted whose key is >= key */
{
	int low, high, mid;

	low = 0;
	high = nrecs;
	while (low < high) {
		mid = (low + high) / 2;
		if (memcmp(sorted[mid].key, key, KEYLEN) < 0)
			low = mid + 1;
		else
			high = mid;
	}
	return low;
}

static int
key_present(const char *key)
{
	int p;

	p = lower_bound(key);
	return p < nrecs && memcmp(sorted[p].key, key, KEYLEN) == 0;
}

/* ---------------- PF counters ---------------- */

static void
get_stats(int fd, PFstats *out)
{
	if (PF_GetStats(fd, out) != PFE_OK)
		fail("PF_GetStats failed");
}

static void
stats_diff(PFstats *out, const PFstats *after, const PFstats *before)
{
	out->logical_reads = after->logical_reads - before->logical_reads;
	out->logical_writes = after->logical_writes - before->logical_writes;
	out->hits = after->hits - before->hits;
	out->misses = after->misses - before->misses;
	out->physical_reads = after->physical_reads - before->physical_reads;
	out->physical_writes = after->physical_writes -
			       before->physical_writes;
	out->evictions = after->evictions - before->evictions;
	out->dirty_evictions = after->dirty_evictions -
			       before->dirty_evictions;
}

static void
stats_add(PFstats *out, const PFstats *a)
{
	out->logical_reads += a->logical_reads;
	out->logical_writes += a->logical_writes;
	out->hits += a->hits;
	out->misses += a->misses;
	out->physical_reads += a->physical_reads;
	out->physical_writes += a->physical_writes;
	out->evictions += a->evictions;
	out->dirty_evictions += a->dirty_evictions;
}

static void
check_hits(const char *what, const PFstats *s)
{
	if (s->hits + s->misses != s->logical_reads) {
		fprintf(stderr, "bench_index: %s: hits %ld + misses %ld != "
			"logical reads %ld\n", what, s->hits, s->misses,
			s->logical_reads);
		exit(1);
	}
}

static void
check_split(const char *what, const PFstats *total, const PFstats *idx,
	    const PFstats *heap)
/* the global delta of a build must be index + heap */
{
	check_hits(what, total);
	check_hits(what, idx);
	check_hits(what, heap);
	if (total->logical_reads != idx->logical_reads + heap->logical_reads ||
	    total->logical_writes != idx->logical_writes +
	    heap->logical_writes ||
	    total->physical_reads != idx->physical_reads +
	    heap->physical_reads ||
	    total->physical_writes != idx->physical_writes +
	    heap->physical_writes) {
		fprintf(stderr, "bench_index: %s: global I/O (lr %ld lw %ld "
			"pr %ld pw %ld) is not index + heap (lr %ld+%ld lw "
			"%ld+%ld pr %ld+%ld pw %ld+%ld)\n", what,
			total->logical_reads, total->logical_writes,
			total->physical_reads, total->physical_writes,
			idx->logical_reads, heap->logical_reads,
			idx->logical_writes, heap->logical_writes,
			idx->physical_reads, heap->physical_reads,
			idx->physical_writes, heap->physical_writes);
		exit(1);
	}
}

static void
cold_pool(int pool)
{
	if (PF_InitBuffer(pool) != PFE_OK)
		fail("PF_InitBuffer failed (a page still in the pool?)");
}

/* ---------------- one measurement ---------------- */

struct shape {
	int pages, leaves, internal, height;
	long leaf_keys;
	double leaf_fill;
	/* leaf boundaries in chain order */
	int nleaf;
	char (*first)[KEYLEN];
	char (*last)[KEYLEN];
};

struct row {
	long ops;
	double ms, idx_ms, sort_ms, heap_ms;
	PFstats idx, heap;
	long results;
};

static void
zero_row(struct row *r)
{
	memset(r, 0, sizeof(*r));
}

static void
print_header(void)
{
	printf("method,repeat,phase,ops,ms,ms_per_op,idx_ms,sort_ms,"
	       "idx_logical_reads,idx_logical_writes,idx_physical_reads,"
	       "idx_physical_writes,idx_hits,idx_misses,heap_logical_reads,"
	       "heap_physical_reads,heap_physical_writes,heap_ms,results,"
	       "index_pages,leaves,height,leaf_fill\n");
}

static void
print_row(int m, int repeat, const char *phase, const struct row *r,
	  const struct shape *s)
{
	printf("%s,%d,%s,%ld,%.3f,%.6f,%.3f,%.3f,%ld,%ld,%ld,%ld,%ld,%ld,"
	       "%ld,%ld,%ld,%.3f,%ld,%d,%d,%d,%.4f\n", method_name[m], repeat,
	       phase, r->ops, r->ms, r->ops > 0 ? r->ms / r->ops : 0.0,
	       r->idx_ms, r->sort_ms, r->idx.logical_reads,
	       r->idx.logical_writes, r->idx.physical_reads,
	       r->idx.physical_writes, r->idx.hits, r->idx.misses,
	       r->heap.logical_reads, r->heap.physical_reads,
	       r->heap.physical_writes, r->heap_ms, r->results, s->pages,
	       s->leaves, s->height, s->leaf_fill);
}

/* ---------------- the three builds ---------------- */

static char *
index_file(int m, char *buf)
{
	sprintf(buf, "%s.%d", RELNAME, m + 1);
	return buf;
}

static void
remove_files(void)
{
	char name[64];
	int m;

	unlink(HEAPNAME);
	for (m = 0; m < NMETHODS; m++)
		unlink(index_file(m, name));
}

static void
create_index(int m, struct row *r)
/* AM_CreateIndex with nothing else open: its global delta is index I/O */
{
	PFstats g0, g1, d;
	double t0;

	get_stats(-1, &g0);
	t0 = now_ms();
	if (AM_CreateIndex(RELNAME, m + 1, 'c', KEYLEN) != AME_OK)
		fail("AM_CreateIndex failed");
	r->idx_ms += now_ms() - t0;
	get_stats(-1, &g1);
	stats_diff(&d, &g1, &g0);
	stats_add(&r->idx, &d);
}

static void
insert_key(int ifd, char *key, int rid, struct row *r)
{
	double t0;

	t0 = now_ms();
	if (AM_InsertEntry(ifd, 'c', KEYLEN, key, rid) != AME_OK)
		fail("AM_InsertEntry failed");
	r->idx_ms += now_ms() - t0;
}

static void
finish_open_build(const char *what, int hfd, int ifd, const PFstats *g0,
		  struct row *r)
/* flush, add the per-file counters, close, check the split */
{
	PFstats s, g1, total;

	if (PF_FlushAll() != PFE_OK)
		fail("PF_FlushAll failed");
	get_stats(ifd, &s);
	stats_add(&r->idx, &s);
	get_stats(hfd, &s);
	stats_add(&r->heap, &s);
	if (PF_CloseFile(ifd) != PFE_OK || HF_CloseFile(hfd) != HF_OK)
		fail("close after a build failed");
	get_stats(-1, &g1);
	stats_diff(&total, &g1, g0);
	check_split(what, &total, &r->idx, &r->heap);
}

static void
build_m2(int pool, struct row *r)
/* empty heap and index; HF_Insert then AM_InsertEntry for each record */
{
	PFstats g0, g1, d;
	char name[64];
	double t0;
	int hfd, ifd, i, rid;

	zero_row(r);
	cold_pool(pool);
	get_stats(-1, &g0);
	t0 = now_ms();
	if (HF_CreateFile(HEAPNAME) != HF_OK)
		fail("HF_CreateFile failed");
	get_stats(-1, &g1);
	stats_diff(&d, &g1, &g0);
	stats_add(&r->heap, &d);
	create_index(M2, r);
	hfd = HF_OpenFile(HEAPNAME);
	ifd = PF_OpenFile(index_file(M2, name));
	if (hfd < 0 || ifd < 0)
		fail("M2: open failed");
	for (i = 0; i < nrecs; i++) {
		rid = HF_Insert(hfd, recs[i].line, recs[i].len);
		if (rid < 0)
			fail("M2: HF_Insert failed");
		if (have_rids && rid != heap_rids[i])
			fail("M2: HF_Insert gave a different rid than in the "
			     "first repeat");
		heap_rids[i] = rid;
		insert_key(ifd, recs[i].key, rid, r);
	}
	finish_open_build("M2 build", hfd, ifd, &g0, r);
	r->ms = now_ms() - t0;
	r->ops = nrecs;
	r->results = nrecs;
	have_rids = 1;
}

static void
key_of_record(const char *buf, int len, char key[KEYLEN])
{
	char line[HF_MAX_RECLEN + 1];

	memcpy(line, buf, (size_t)len);
	line[len] = '\0';
	if (!roll_number(line, key))
		fail("a heap record has no 8-byte field 2");
}

static void
check_scan_rid(int rid, const char key[KEYLEN], int *seen_count)
/* a heap scan must return every record once with its own key */
{
	int p;

	p = rid_index(rid);
	if (p < 0 || memcmp(by_rid[p].key, key, KEYLEN) != 0)
		fail("heap scan returned an unknown rid or a wrong key");
	if (stamp[p] == stamp_now)
		fail("heap scan returned a rid twice");
	stamp[p] = stamp_now;
	(*seen_count)++;
}

static void
build_m1(int pool, struct row *r)
/* the heap file exists: scan it and AM_InsertEntry every record */
{
	PFstats g0;
	struct pair *scanned;
	char buf[HF_MAX_RECLEN], name[64];
	double t0;
	int hfd, ifd, sd, len, rid, n, seen, rc;

	zero_row(r);
	scanned = (struct pair *)xmalloc((size_t)nrecs * sizeof(*scanned));
	cold_pool(pool);
	get_stats(-1, &g0);
	t0 = now_ms();
	create_index(M1, r);
	hfd = HF_OpenFile(HEAPNAME);
	ifd = PF_OpenFile(index_file(M1, name));
	if (hfd < 0 || ifd < 0)
		fail("M1: open failed");
	sd = HF_ScanOpen(hfd);
	if (sd < 0)
		fail("M1: HF_ScanOpen failed");
	n = 0;
	while ((rc = HF_ScanNext(sd, buf, &len, &rid)) == HF_OK) {
		if (n == nrecs)
			fail("M1: the heap scan returned too many records");
		key_of_record(buf, len, scanned[n].key);
		scanned[n].rid = rid;
		insert_key(ifd, scanned[n].key, rid, r);
		n++;
	}
	if (rc != HFE_EOF || HF_ScanClose(sd) != HF_OK)
		fail("M1: heap scan failed");
	finish_open_build("M1 build", hfd, ifd, &g0, r);
	r->ms = now_ms() - t0;
	/* not timed: the scan gave every record once with its own key */
	stamp_now++;
	seen = 0;
	for (rid = 0; rid < n; rid++)
		check_scan_rid(scanned[rid].rid, scanned[rid].key, &seen);
	if (seen != nrecs)
		fail("M1: the heap scan did not return every record");
	free(scanned);
	r->ops = nrecs;
	r->results = nrecs;
}

struct stream {
	struct pair *pairs;
	int count, next;
};

static int
next_pair(void *context, char *key, int *rid)
{
	struct stream *s;

	s = (struct stream *)context;
	if (s->next == s->count)
		return AM_BULK_NEXT_EOF;
	memcpy(key, s->pairs[s->next].key, KEYLEN);
	*rid = s->pairs[s->next].rid;
	s->next++;
	return AM_BULK_NEXT_ITEM;
}

static void
build_m3(int pool, struct row *r)
/* heap scan + sort (sort_ms), then AM_BulkLoad (idx_ms) */
{
	PFstats g0, g1, g2, s, d;
	struct pair *pairs;
	struct stream stream;
	char buf[HF_MAX_RECLEN];
	double t0, t1, t2;
	int hfd, sd, len, rid, n, rc, seen;

	zero_row(r);
	pairs = (struct pair *)xmalloc((size_t)nrecs * sizeof(*pairs));
	cold_pool(pool);
	get_stats(-1, &g0);
	t0 = now_ms();
	hfd = HF_OpenFile(HEAPNAME);
	if (hfd < 0)
		fail("M3: open failed");
	sd = HF_ScanOpen(hfd);
	if (sd < 0)
		fail("M3: HF_ScanOpen failed");
	n = 0;
	while ((rc = HF_ScanNext(sd, buf, &len, &rid)) == HF_OK) {
		if (n == nrecs)
			fail("M3: the heap scan returned too many records");
		key_of_record(buf, len, pairs[n].key);
		pairs[n].rid = rid;
		n++;
	}
	if (rc != HFE_EOF || HF_ScanClose(sd) != HF_OK)
		fail("M3: heap scan failed");
	if (PF_FlushAll() != PFE_OK)
		fail("PF_FlushAll failed");
	get_stats(hfd, &s);
	stats_add(&r->heap, &s);
	if (HF_CloseFile(hfd) != HF_OK)
		fail("M3: heap close failed");
	qsort(pairs, (size_t)n, sizeof(*pairs), compare_pairs);
	t1 = now_ms();
	r->sort_ms = t1 - t0;	/* heap scan and sort */
	get_stats(-1, &g1);
	stats_diff(&d, &g1, &g0);
	memset(&s, 0, sizeof(s));
	check_split("M3 heap scan", &d, &s, &r->heap);
	stamp_now++;
	seen = 0;
	for (rid = 0; rid < n; rid++)
		check_scan_rid(pairs[rid].rid, pairs[rid].key, &seen);
	if (seen != nrecs)
		fail("M3: the heap scan did not return every record");
	/* the load runs with no file open: its global delta is index I/O */
	stream.pairs = pairs;
	stream.count = n;
	stream.next = 0;
	t2 = now_ms();
	rc = AM_BulkLoad(RELNAME, M3 + 1, 'c', KEYLEN, next_pair, &stream);
	r->idx_ms = now_ms() - t2;
	if (rc != AME_OK) {
		fprintf(stderr, "bench_index: AM_BulkLoad failed (%d)\n", rc);
		exit(1);
	}
	/* scan + sort + load; the order check above is not timed */
	r->ms = r->sort_ms + r->idx_ms;
	get_stats(-1, &g2);
	stats_diff(&r->idx, &g2, &g1);
	check_hits("M3 load", &r->idx);
	r->ops = nrecs;
	r->results = nrecs;
	free(pairs);
}

/* ---------------- tree shape ---------------- */

static void
read_shape(int m, struct shape *t)
/****************************************************************************
SPECIFICATIONS:
	Pages in the index file of method m, leaves and internal pages
	reachable from the root (page 0; every page must be reachable),
	height (the leftmost path), keys in the leaves and the average leaf
	fill (keys per leaf / leaf capacity), and the first and last key of
	every leaf in chain order. Done outside every measurement.
*****************************************************************************/
{
	AM_LEAFHEADER leaf;
	AM_INTHEADER node;
	char name[64];
	char *page, *seen;
	int *queue;
	int fd, pn, head, tail, i, child, capacity;

	fd = PF_OpenFile(index_file(m, name));
	if (fd < 0)
		fail("shape: open failed");
	t->pages = 0;
	pn = -1;
	while (PF_GetNextPage(fd, &pn, &page) == PFE_OK) {
		t->pages++;
		if (PF_UnfixPage(fd, pn, FALSE) != PFE_OK)
			fail("shape: unfix failed");
	}
	seen = xmalloc((size_t)t->pages);
	queue = (int *)xmalloc((size_t)t->pages * sizeof(*queue));
	memset(seen, 0, (size_t)t->pages);
	t->leaves = t->internal = 0;
	t->leaf_keys = 0;
	head = tail = 0;
	queue[tail++] = 0;
	seen[0] = 1;
	while (head < tail) {
		pn = queue[head++];
		if (PF_GetThisPage(fd, pn, &page) != PFE_OK)
			fail("shape: get failed");
		if (page[0] == 'l') {
			t->leaves++;
			memcpy(&leaf, page, sizeof(leaf));
			t->leaf_keys += leaf.numKeys;
		} else {
			t->internal++;
			memcpy(&node, page, sizeof(node));
			for (i = 0; i <= node.numKeys; i++) {
				memcpy(&child, page + AM_sint +
				       i * (AM_si + KEYLEN), sizeof(child));
				if (child <= 0 || child >= t->pages || seen[child])
					fail("shape: bad child pointer");
				seen[child] = 1;
				queue[tail++] = child;
			}
		}
		if (PF_UnfixPage(fd, pn, FALSE) != PFE_OK)
			fail("shape: unfix failed");
	}
	if (tail != t->pages)
		fail("shape: a page is not reachable from the root");
	/* leftmost path */
	t->height = 1;
	pn = 0;
	for (;;) {
		if (PF_GetThisPage(fd, pn, &page) != PFE_OK)
			fail("shape: get failed");
		if (page[0] == 'l')
			break;
		memcpy(&child, page + AM_sint, sizeof(child));
		if (PF_UnfixPage(fd, pn, FALSE) != PFE_OK)
			fail("shape: unfix failed");
		pn = child;
		t->height++;
	}
	if (PF_UnfixPage(fd, pn, FALSE) != PFE_OK)
		fail("shape: unfix failed");
	/* leaf chain: boundaries */
	t->first = (char (*)[KEYLEN])xmalloc((size_t)t->leaves * KEYLEN);
	t->last = (char (*)[KEYLEN])xmalloc((size_t)t->leaves * KEYLEN);
	t->nleaf = 0;
	while (pn != AM_NULL_PAGE) {
		if (t->nleaf >= t->leaves)
			fail("shape: leaf chain longer than the leaf count");
		if (PF_GetThisPage(fd, pn, &page) != PFE_OK)
			fail("shape: get failed");
		memcpy(&leaf, page, sizeof(leaf));
		if (leaf.numKeys > 0) {
			memcpy(t->first[t->nleaf], page + AM_sl, KEYLEN);
			memcpy(t->last[t->nleaf], page + AM_sl +
			       (leaf.numKeys - 1) * (KEYLEN + AM_ss), KEYLEN);
			t->nleaf++;
		}
		if (PF_UnfixPage(fd, pn, FALSE) != PFE_OK)
			fail("shape: unfix failed");
		pn = leaf.nextLeafPage;
	}
	if (PF_CloseFile(fd) != PFE_OK)
		fail("shape: close failed (a page left fixed?)");
	/* keys of one rid each that fit a leaf, as in AM_BulkLoad */
	capacity = (PF_PAGE_SIZE - (int)sizeof(AM_LEAFHEADER)) / (KEYLEN + 8);
	t->leaf_fill = t->leaves > 0 ?
		(double)t->leaf_keys / ((double)t->leaves * capacity) : 0.0;
	free(seen);
	free(queue);
}

static void
free_shape(struct shape *t)
{
	free(t->first);
	free(t->last);
}

/* ---------------- queries ---------------- */

struct query {
	char value[KEYLEN];
};

struct queryset {
	struct query *q[NKINDS];
	int n[NKINDS];
	int range_len;
};

static int
gap_after(const char *a, const char *b, char *x)
/* x = a with its last byte raised by one; 1 if a < x < b */
{
	if ((unsigned char)a[KEYLEN - 1] == 0xff)
		return 0;
	memcpy(x, a, KEYLEN);
	x[KEYLEN - 1]++;
	return memcmp(x, b, KEYLEN) < 0;
}

static int
is_boundary(const char *key, struct shape *shapes)
/* key is the last key of a leaf that has a next leaf, in some tree */
{
	int m, j;

	for (m = 0; m < NMETHODS; m++)
		for (j = 0; j + 1 < shapes[m].nleaf; j++)
			if (memcmp(shapes[m].last[j], key, KEYLEN) == 0)
				return 1;
	return 0;
}

static void
make_queries(struct queryset *qs, const struct settings *cf, int repeat,
	     struct shape *shapes)
/****************************************************************************
SPECIFICATIONS:
	One query set from (seed, repeat), the same for all three methods.
	point: keys of random records. missing: absent values; the first
	half in boundary gaps (just above the last key of a leaf that has a
	next leaf, in any of the three trees, and below the next key), the
	rest in gaps between two keys of the same leaf. If one kind has no
	gap the other is used; if neither has any (fewer than 2 distinct
	keys), random 8-digit values that are not keys. range: keys of
	random records. fullscan: one query.
*****************************************************************************/
{
	unsigned long state;
	char (*boundary)[KEYLEN];
	char (*inside)[KEYLEN];
	char x[KEYLEN + 1];
	int nb, ni, i, p, k, which;

	state = (unsigned long)cf->seed * 1000003UL +
		(unsigned long)repeat * 7919UL + 1UL;
	qs->range_len = cf->range_len;
	qs->n[Q_POINT] = nrecs > 0 ? cf->point : 0;
	qs->n[Q_MISSING] = cf->missing;
	qs->n[Q_RANGE] = nrecs > 0 ? cf->range : 0;
	qs->n[Q_FULL] = 1;
	for (k = 0; k < NKINDS; k++)
		qs->q[k] = (struct query *)xmalloc((size_t)qs->n[k] *
						   sizeof(struct query));
	for (i = 0; i < qs->n[Q_POINT]; i++)
		memcpy(qs->q[Q_POINT][i].value,
		       recs[random_below(&state, nrecs)].key, KEYLEN);
	for (i = 0; i < qs->n[Q_RANGE]; i++)
		memcpy(qs->q[Q_RANGE][i].value,
		       recs[random_below(&state, nrecs)].key, KEYLEN);
	memset(qs->q[Q_FULL][0].value, 0, KEYLEN);

	/* gap candidates between adjacent distinct keys */
	boundary = (char (*)[KEYLEN])xmalloc((size_t)nrecs * KEYLEN);
	inside = (char (*)[KEYLEN])xmalloc((size_t)nrecs * KEYLEN);
	nb = ni = 0;
	for (p = 0; p < nrecs; p = k) {
		for (k = p + 1; k < nrecs &&
		     memcmp(sorted[k].key, sorted[p].key, KEYLEN) == 0; k++)
			;
		if (k < nrecs && gap_after(sorted[p].key, sorted[k].key, x)) {
			if (is_boundary(sorted[p].key, shapes))
				memcpy(boundary[nb++], x, KEYLEN);
			else
				memcpy(inside[ni++], x, KEYLEN);
		}
	}
	for (i = 0; i < qs->n[Q_MISSING]; i++) {
		which = i < qs->n[Q_MISSING] / 2 ? 0 : 1;
		if (which == 0 && nb == 0)
			which = 1;
		if (which == 1 && ni == 0)
			which = nb > 0 ? 0 : 2;
		if (which == 0)
			memcpy(qs->q[Q_MISSING][i].value,
			       boundary[random_below(&state, nb)], KEYLEN);
		else if (which == 1)
			memcpy(qs->q[Q_MISSING][i].value,
			       inside[random_below(&state, ni)], KEYLEN);
		else {
			do {
				sprintf(x, "%08d", random_below(&state,
					100000000));
			} while (key_present(x));
			memcpy(qs->q[Q_MISSING][i].value, x, KEYLEN);
		}
	}
	free(boundary);
	free(inside);
}

static void
free_queries(struct queryset *qs)
{
	int k;

	for (k = 0; k < NKINDS; k++)
		free(qs->q[k]);
}

static void
mismatch(int m, int kind, int qi, const char *value, const char *what)
{
	char v[KEYLEN + 1];

	memcpy(v, value, KEYLEN);
	v[KEYLEN] = '\0';
	fprintf(stderr, "bench_index: MISMATCH %s %s query %d (value '%s'): "
		"%s\n", method_name[m], kind_name[kind], qi,
		kind == Q_FULL ? "" : v, what);
	exit(1);
}

static void
verify(int m, int kind, int qi, const char *value, const int *rids, int got,
       int start, int want)
/****************************************************************************
SPECIFICATIONS:
	The rids of one query against sorted[start .. start+want-1]: the
	count must be want, the key of the k-th rid must be the key of
	sorted[start+k], and no rid may come twice. For EQ and the full scan
	(and every key a range scan returns in full) that makes the set
	exactly the expected one; the last key of a range scan may be cut
	off anywhere in its rids.
*****************************************************************************/
{
	int k, p;

	if (got != want) {
		char text[96];

		sprintf(text, "%d record ids, expected %d", got, want);
		mismatch(m, kind, qi, value, text);
	}
	stamp_now++;
	for (k = 0; k < got; k++) {
		p = rid_index(rids[k]);
		if (p < 0)
			mismatch(m, kind, qi, value, "a record id that no "
				 "record has");
		if (memcmp(by_rid[p].key, sorted[start + k].key, KEYLEN) != 0)
			mismatch(m, kind, qi, value, "a record id with the wrong "
				 "key or out of key order");
		if (stamp[p] == stamp_now)
			mismatch(m, kind, qi, value, "a record id came twice");
		stamp[p] = stamp_now;
	}
}

static int
expected(int kind, const char *value, int range_len, int *start)
/* expected count of a query, *start its first position in sorted */
{
	int p, k;

	if (kind == Q_FULL) {
		*start = 0;
		return nrecs;
	}
	p = lower_bound(value);
	*start = p;
	if (kind == Q_RANGE)
		return nrecs - p < range_len ? nrecs - p : range_len;
	for (k = p; k < nrecs && memcmp(sorted[k].key, value, KEYLEN) == 0;
	     k++)
		;
	return k - p;
}

static void
run_batch(int m, int kind, const struct queryset *qs, int pool,
	  struct row *r, int *point_rids, int *npoint_rids, int *counts)
/****************************************************************************
SPECIFICATIONS:
	Run the queries of one kind on index m with a cold pool, each query
	timed alone (the check of its result is not timed). Fills r with the
	index-only time and counters. For point queries the rids found are
	appended to point_rids. counts[qi], if not NULL, gets each query's
	result count.
*****************************************************************************/
{
	static const int op_of[NKINDS] = { EQUAL, EQUAL, GREATER_THAN_EQUAL,
		ALL };
	char name[64];
	int *rids;
	double t0;
	int ifd, qi, sd, rid, got, start, want, cap;

	zero_row(r);
	cap = nrecs > qs->range_len ? nrecs : qs->range_len;
	rids = (int *)xmalloc((size_t)(cap + 1) * sizeof(*rids));
	cold_pool(pool);
	ifd = PF_OpenFile(index_file(m, name));
	if (ifd < 0)
		fail("query: index open failed");
	for (qi = 0; qi < qs->n[kind]; qi++) {
		const char *value = qs->q[kind][qi].value;

		t0 = now_ms();
		sd = AM_OpenIndexScan(ifd, 'c', KEYLEN, op_of[kind],
				      kind == Q_FULL ? NULL : (char *)value);
		AM_EmptyStack();
		if (sd < 0)
			mismatch(m, kind, qi, value, "AM_OpenIndexScan failed");
		got = 0;
		rid = 0;
		while (!(kind == Q_RANGE && got == qs->range_len)) {
			rid = AM_FindNextEntry(sd);
			if (rid < 0)
				break;
			if (got > cap)
				mismatch(m, kind, qi, value, "too many results");
			rids[got++] = rid;
		}
		if (AM_CloseIndexScan(sd) != AME_OK)
			mismatch(m, kind, qi, value, "AM_CloseIndexScan failed");
		r->ms += now_ms() - t0;
		if (rid < 0 && rid != AME_EOF)
			mismatch(m, kind, qi, value, "the scan ended with an "
				 "error");
		want = expected(kind, value, qs->range_len, &start);
		verify(m, kind, qi, value, rids, got, start, want);
		r->results += got;
		if (counts != NULL)
			counts[qi] = got;
		if (kind == Q_POINT) {
			memcpy(point_rids + *npoint_rids, rids,
			       (size_t)got * sizeof(*rids));
			*npoint_rids += got;
		}
	}
	get_stats(ifd, &r->idx);
	check_hits("query batch", &r->idx);
	if (PF_CloseFile(ifd) != PFE_OK)
		fail("query: index close failed (a page left fixed?)");
	r->ops = qs->n[kind];
	free(rids);
}

static void
fetch_records(int pool, const int *rids, int n, struct row *r)
/* the heap pages the point queries need: HF_Get of each rid, cold pool */
{
	char buf[HF_MAX_RECLEN], key[KEYLEN];
	double t0;
	int hfd, i, len, p;

	if (n == 0)
		return;		/* nothing to fetch: no heap I/O */
	cold_pool(pool);
	hfd = HF_OpenFile(HEAPNAME);
	if (hfd < 0)
		fail("heap open for the record fetch failed");
	t0 = now_ms();
	for (i = 0; i < n; i++)
		if (HF_Get(hfd, rids[i], buf, &len) != HF_OK)
			fail("HF_Get of a point-query rid failed");
	r->heap_ms = now_ms() - t0;
	get_stats(hfd, &r->heap);
	check_hits("record fetch", &r->heap);
	/* each record must hold the key the index gave for it (not timed,
	   not counted) */
	for (i = 0; i < n; i++) {
		if (HF_Get(hfd, rids[i], buf, &len) != HF_OK)
			fail("HF_Get failed");
		key_of_record(buf, len, key);
		p = rid_index(rids[i]);
		if (p < 0 || memcmp(by_rid[p].key, key, KEYLEN) != 0)
			fail("a fetched record does not hold its index key");
	}
	if (HF_CloseFile(hfd) != HF_OK)
		fail("heap close after the record fetch failed");
}

/* ---------------- one repeat ---------------- */

struct repeat_result {
	struct row build[NMETHODS];
	struct row query[NMETHODS][NKINDS];
	struct shape shape[NMETHODS];
};

static void
run_repeat(const struct settings *cf, int repeat, struct queryset *qs,
	   int use_given_queries, struct repeat_result *res, int *counts)
{
	int *point_rids;
	int m, k, npoint;
	long cap;

	remove_files();
	fprintf(stderr, "bench_index: repeat %d: M2 build\n", repeat);
	build_m2(cf->pool, &res->build[M2]);
	if (sorted == NULL)
		make_model();
	fprintf(stderr, "bench_index: repeat %d: M1 build\n", repeat);
	build_m1(cf->pool, &res->build[M1]);
	fprintf(stderr, "bench_index: repeat %d: M3 build\n", repeat);
	build_m3(cf->pool, &res->build[M3]);
	for (m = 0; m < NMETHODS; m++)
		read_shape(m, &res->shape[m]);
	if (!use_given_queries)
		make_queries(qs, cf, repeat, res->shape);
	cap = 0;
	for (k = 0; k < qs->n[Q_POINT]; k++) {
		int start;

		cap += expected(Q_POINT, qs->q[Q_POINT][k].value, 0, &start);
	}
	point_rids = (int *)xmalloc((size_t)(cap + 1) * sizeof(int));
	for (m = 0; m < NMETHODS; m++) {
		fprintf(stderr, "bench_index: repeat %d: %s queries\n", repeat,
			method_name[m]);
		for (k = 0; k < NKINDS; k++) {
			npoint = 0;
			run_batch(m, k, qs, cf->pool, &res->query[m][k],
				  point_rids, &npoint,
				  counts != NULL ? counts + k * 64 : NULL);
			if (k == Q_POINT)
				fetch_records(cf->pool, point_rids, npoint,
					      &res->query[m][k]);
		}
	}
	free(point_rids);
}

static void
print_repeat(int repeat, struct repeat_result *res)
{
	int m, k;

	for (m = 0; m < NMETHODS; m++)
		print_row(m, repeat, "build", &res->build[m], &res->shape[m]);
	for (m = 0; m < NMETHODS; m++)
		for (k = 0; k < NKINDS; k++)
			print_row(m, repeat, kind_name[k], &res->query[m][k],
				  &res->shape[m]);
}

static void
free_repeat(struct repeat_result *res)
{
	int m;

	for (m = 0; m < NMETHODS; m++)
		free_shape(&res->shape[m]);
}

static void
cleanup(void)
{
	int m;

	if (PF_DestroyFile(HEAPNAME) != PFE_OK)
		fail("could not remove the heap file");
	for (m = 0; m < NMETHODS; m++)
		if (AM_DestroyIndex(RELNAME, m + 1) != AME_OK)
			fail("could not remove an index file");
}

/* ---------------- full run ---------------- */

static int
bench_mode(void)
{
	struct settings cf;
	struct queryset qs;
	struct repeat_result res;
	char dir[4096];
	const char *wd, *root, *data_file;
	int rep, n;

	/* every value is checked before PF_Init, output or any file */
	cf.pool = conf_int("buffer_pool_size", PF_DEFAULT_BUFS, PF_MIN_BUFS,
			   1000000L);
	cf.fill = conf_fill("bulk_fill_factor", 1.0);
	cf.point = conf_int("idx_point_queries", 1000, 0, 1000000L);
	cf.missing = conf_int("idx_missing_queries", 100, 0, 1000000L);
	cf.range = conf_int("idx_range_queries", 200, 0, 1000000L);
	cf.range_len = conf_int("idx_range_len", 100, 1, 1000000L);
	cf.repeats = conf_int("repeats", 5, 1, 1000);
	cf.seed = conf_int("seed", 42, -2147483647L - 1, 2147483647L);
	wd = Config_GetStr("work_dir", "/tmp/toydb_run");
	if (wd[0] == '\0')
		bad_value("work_dir", wd, "is empty");
	root = getenv("TOYDB_ROOT");
	if (wd[0] != '/' && root != NULL && root[0] != '\0')
		n = snprintf(dir, sizeof(dir), "%s/%s", root, wd);
	else
		n = snprintf(dir, sizeof(dir), "%s", wd);
	if (n >= (int)sizeof(dir))
		bad_value("work_dir", wd, "is too long");
	data_file = Config_GetStr("data_file", "../data/student.txt");
	load_data(data_file);

	PF_Init();
	if (mkdir(dir, 0777) != 0 && errno != EEXIST)
		bad_value("work_dir", dir, "cannot be created");
	if (chdir(dir) != 0)
		bad_value("work_dir", dir, "cannot be entered");
	heap_rids = (int *)xmalloc((size_t)nrecs * sizeof(int));
	fprintf(stderr, "bench_index: %d records, pool %d, fill %g, %d "
		"repeats\n", nrecs, cf.pool, cf.fill, cf.repeats);
	print_header();
	for (rep = 0; rep < cf.repeats; rep++) {
		run_repeat(&cf, rep, &qs, 0, &res, NULL);
		print_repeat(rep, &res);
		fflush(stdout);
		free_queries(&qs);
		free_repeat(&res);
	}
	cleanup();
	return 0;
}

/* ---------------- check mode ---------------- */

static int nfail;

static void
expect(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		nfail++;
}

static void
synth_key(int i, char *key)
{
	char text[16];

	sprintf(text, "%08d", 10000000 + 10 * i);
	memcpy(key, text, KEYLEN);
}

static void
synth_data(void)
/* 600 keys 10000000 + 10i in the order i = 7k mod 600, then a second
   record for i = 520, 540, 560, 580: 604 records, 16 fields each */
{
	char line[256], key[KEYLEN + 1];
	int capacity, k, i;

	capacity = 0;
	key[KEYLEN] = '\0';
	for (k = 0; k < 604; k++) {
		i = k < 600 ? (7 * k) % 600 : 520 + 20 * (k - 600);
		synth_key(i, key);
		sprintf(line, "%d;%s;N%d;M;a;b;c;d;e;f;;1;BTECH;;;%s", k, key,
			i, k < 600 ? "" : "dup");
		add_record(line, k + 1, &capacity);
	}
}

static void
set_query(struct queryset *qs, int kind, int qi, const char *text)
{
	memcpy(qs->q[kind][qi].value, text, KEYLEN);
}

static int
check_mode(void)
{
	static char arg0[] = "bench_index";
	static char arg1[] = "bulk_fill_factor=1.0";
	static const int point_i[5] = { 0, 253, 254, 599, 520 };
	static const int point_want[5] = { 1, 1, 1, 1, 2 };
	static const char *missing_v[4] = { "10002535", "10000005",
		"00000000", "99999999" };
	static const int range_i[4] = { 250, 595, 515, 516 };
	static const int range_want[4] = { 10, 5, 10, 5 };
	char *argv[2];
	struct settings cf;
	struct queryset qs;
	struct repeat_result res;
	char key[KEYLEN];
	int counts[NKINDS * 64];
	int k, m, ok;
	char text[160];

	/* independent of toydb.conf: main set the pool and the policy,
	   the fill factor is set here */
	argv[0] = arg0;
	argv[1] = arg1;
	if (Config_ParseArgs(2, argv) != 1)
		fail("check: cannot set bulk_fill_factor");
	memset(&cf, 0, sizeof(cf));
	cf.pool = 20;
	cf.fill = 1.0;
	cf.seed = 1;
	synth_data();
	heap_rids = (int *)xmalloc((size_t)nrecs * sizeof(int));

	/* part 1: fixed queries with hand-computed counts. The range length
	   is per set, so the GE-from-516 query (length 5) gets its own set. */
	qs.range_len = 10;
	qs.n[Q_POINT] = 5;
	qs.n[Q_MISSING] = 4;
	qs.n[Q_RANGE] = 3;
	qs.n[Q_FULL] = 1;
	for (k = 0; k < NKINDS; k++)
		qs.q[k] = (struct query *)xmalloc((size_t)qs.n[k] *
						  sizeof(struct query));
	for (k = 0; k < 5; k++) {
		synth_key(point_i[k], key);
		set_query(&qs, Q_POINT, k, key);
	}
	for (k = 0; k < 4; k++)
		set_query(&qs, Q_MISSING, k, missing_v[k]);
	for (k = 0; k < 3; k++) {
		synth_key(range_i[k], key);
		set_query(&qs, Q_RANGE, k, key);
	}
	memset(qs.q[Q_FULL][0].value, 0, KEYLEN);
	run_repeat(&cf, 0, &qs, 1, &res, counts);

	/* M3 at f = 1.0: leaves of 254, 254 and 92 keys (the 4 second
	   records are in leaf 3: 92*16 + 4*6 = 1496 bytes) and a root */
	sprintf(text, "M3 shape: pages %d leaves %d height %d leaf_fill "
		"%.4f (want 4, 3, 2, 600/762 = %.4f)", res.shape[M3].pages,
		res.shape[M3].leaves, res.shape[M3].height,
		res.shape[M3].leaf_fill, 600.0 / 762.0);
	expect(res.shape[M3].pages == 4 && res.shape[M3].leaves == 3 &&
	       res.shape[M3].height == 2 && res.shape[M3].leaf_keys == 600,
	       text);
	for (m = 0; m < NMETHODS; m++) {
		sprintf(text, "%s shape: pages %d leaves %d internal %d "
			"height %d keys %ld", method_name[m],
			res.shape[m].pages, res.shape[m].leaves,
			res.shape[m].internal, res.shape[m].height,
			res.shape[m].leaf_keys);
		expect(res.shape[m].leaf_keys == 600 &&
		       res.shape[m].pages == res.shape[m].leaves +
		       res.shape[m].internal, text);
	}
	/* the run above already compared every result with the model and
	   would have stopped on a mismatch; counts are those of M3 (the
	   last method run), the same for all three */
	ok = 1;
	for (k = 0; k < 5; k++)
		ok = ok && counts[Q_POINT * 64 + k] == point_want[k];
	expect(ok, "point counts 1, 1, 1, 1, 2 (keys 0, 253, 254, 599, 520)");
	ok = 1;
	for (k = 0; k < 4; k++)
		ok = ok && counts[Q_MISSING * 64 + k] == 0;
	expect(ok, "missing counts 0 (boundary gap 10002535, inside gap "
	       "10000005, below and above the range)");
	ok = 1;
	for (k = 0; k < 3; k++)
		ok = ok && counts[Q_RANGE * 64 + k] == range_want[k];
	expect(ok, "range counts 10, 5, 10 (GE from keys 250, 595, 515, "
	       "length 10)");
	expect(counts[Q_FULL * 64] == 604, "full scan 604");
	for (m = 0; m < NMETHODS; m++) {
		sprintf(text, "%s build: index lr %ld lw %ld pr %ld pw %ld, "
			"heap lr %ld pw %ld", method_name[m],
			res.build[m].idx.logical_reads,
			res.build[m].idx.logical_writes,
			res.build[m].idx.physical_reads,
			res.build[m].idx.physical_writes,
			res.build[m].heap.logical_reads,
			res.build[m].heap.physical_writes);
		expect(res.build[m].idx.physical_writes > 0 &&
		       res.build[m].results == 604, text);
	}
	expect(res.build[M1].heap.logical_writes == 0 &&
	       res.build[M3].heap.logical_writes == 0 &&
	       res.build[M2].heap.logical_writes > 0,
	       "heap writes only in M2 (M1 and M3 only read the heap)");
	free_queries(&qs);
	free_repeat(&res);

	/* part 2: a range of length 5 from key 516 ends inside the rids of
	   key 520 (516..519 and one of 520's two) */
	qs.range_len = 5;
	qs.n[Q_POINT] = qs.n[Q_MISSING] = 0;
	qs.n[Q_RANGE] = 1;
	qs.n[Q_FULL] = 1;
	for (k = 0; k < NKINDS; k++)
		qs.q[k] = (struct query *)xmalloc((size_t)(qs.n[k] + 1) *
						  sizeof(struct query));
	synth_key(range_i[3], key);
	set_query(&qs, Q_RANGE, 0, key);
	memset(qs.q[Q_FULL][0].value, 0, KEYLEN);
	run_repeat(&cf, 1, &qs, 1, &res, counts);
	expect(counts[Q_RANGE * 64] == range_want[3],
	       "range count 5 from key 516, cut inside key 520");
	free_queries(&qs);
	free_repeat(&res);

	/* part 3: random query sets from the generator, all checked against
	   the model on all three trees */
	cf.point = 50;
	cf.missing = 20;
	cf.range = 10;
	cf.range_len = 7;
	run_repeat(&cf, 2, &qs, 0, &res, NULL);
	sprintf(text, "random queries: point %ld rids, missing %ld, range %ld,"
		" identical and correct on M1, M2, M3",
		res.query[M1][Q_POINT].results,
		res.query[M1][Q_MISSING].results,
		res.query[M1][Q_RANGE].results);
	expect(res.query[M1][Q_MISSING].results == 0 &&
	       res.query[M1][Q_POINT].results ==
	       res.query[M3][Q_POINT].results, text);
	sprintf(text, "point record fetch: heap logical reads %ld (one per "
		"rid, %ld rids)", res.query[M1][Q_POINT].heap.logical_reads,
		res.query[M1][Q_POINT].results);
	expect(res.query[M1][Q_POINT].heap.logical_reads >=
	       res.query[M1][Q_POINT].results, text);
	free_queries(&qs);
	free_repeat(&res);
	cleanup();
	if (nfail > 0) {
		printf("bench_index check: %d checks FAILED\n", nfail);
		return 1;
	}
	printf("bench_index check done!\n");
	return 0;
}

int
main(int argc, char **argv)
{
	int i, check, status;

	status = Config_Init(argc, argv);
	if (status != CONFIG_OK && status != CONFIG_ENOFILE) {
		fprintf(stderr, "bench_index: bad configuration\n");
		return 1;
	}
	check = 0;
	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "check") == 0)
			check = 1;
	if (check) {
		/* pool size and policy fixed here, whatever toydb.conf says */
		static char a0[] = "bench_index";
		static char a1[] = "buffer_pool_size=20";
		static char a2[] = "default_policy=LRU";
		char *args[3];

		args[0] = a0;
		args[1] = a1;
		args[2] = a2;
		if (Config_ParseArgs(3, args) != 2)
			fail("check: cannot set the pool size and policy");
		PF_Init();
		return check_mode();
	}
	return bench_mode();
}
