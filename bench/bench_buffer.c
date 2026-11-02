/* bench_buffer.c: workload benchmark of the PF buffer manager (Part 1)
 *
 * usage: bench_buffer [conf=<file>] [key=value ...] [check]
 *
 * Full run (no "check" argument):
 *   Builds one scratch paged file of buf_file_pages pages in work_dir, then
 *   for every pool size, policy (LRU, MRU), access pattern, read percentage
 *   and repeat it opens the file with a cold pool, runs buf_queries page
 *   operations and prints one CSV line with the PF counters on stdout.
 *   Everything else (progress, errors) goes to stderr, so stdout is clean
 *   CSV. Settings come from toydb.conf (keys buf_file_pages, buf_queries,
 *   buf_pool_sizes, buf_read_pcts, buf_patterns, hot_fraction, hot_prob,
 *   seed, repeats, work_dir); key=value arguments override the file.
 *   A relative work_dir is taken from $TOYDB_ROOT when that is set.
 *
 * Check mode ("check" argument):
 *   A few tiny runs with hand-checkable counters, on a 10-page file in the
 *   current directory. The last line is "bench_buffer check done!" only if
 *   every check passed. make test runs this.
 *
 * One operation is "fix a page, then unfix it". A read unfixes clean. A
 * write changes byte WBYTE of the page and marks it dirty, half the time by
 * PF_UnfixPage(..., TRUE) and half the time by PF_MarkDirty followed by a
 * clean unfix, so both forms are used in every run.
 *
 * Counters: the CSV columns up to dirty_evictions are PF_GetStats right
 * after the timed operations. physical_writes there is only the write-back
 * of dirty victims. Then PF_FlushAll writes the pages still dirty in the
 * pool, and total_physical_writes is physical_writes read again after the
 * flush (dirty evictions plus that final write-back). The flush is not in
 * the ms column.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "pf.h"
#include "config.h"

/*
 * Stopgap, same as in tests/test_policy.c: pf.h does not declare these PF
 * functions (they are K&R definitions in pf.c), and -Wall warns about
 * implicit declarations.
 */
extern int PF_CreateFile(char *fname);
extern int PF_DestroyFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_AllocPage(int fd, int *pagenum, char **pagebuf);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);

/* debug helper in buf.c, not part of the public interface */
extern int PFbufResident(int fd, int pagenum);

/*
 * Page layout of the scratch file: the page number as an int at offset 0
 * (the tag, checked on every fix), every other byte set to the low byte of
 * the page number. Writes change only byte WBYTE.
 */
#define WBYTE	((int)sizeof(int))

/*
 * Access patterns. These three are the ones that tell LRU and MRU apart:
 *   uniform    random page, no locality. Both policies should get a hit
 *              ratio near pool/pages, so this is the control where the two
 *              curves should overlap.
 *   hotcold    hot_prob of the accesses go to the first hot_fraction of the
 *              pages. LRU keeps the hot pages in the pool, MRU throws out
 *              the page it just used (very often a hot one), so LRU wins.
 *   sequential looping scan 0, 1, ..., pages-1, 0, 1, ... With a pool
 *              smaller than the file this is LRU's worst case (every
 *              access misses), while MRU keeps about pool-1 pages from one
 *              pass to the next, so MRU wins.
 */
#define PAT_UNIFORM	0
#define PAT_HOTCOLD	1
#define PAT_SEQUENTIAL	2

static const char *patname[] = { "uniform", "hotcold", "sequential" };

typedef unsigned long long u64;

/* ---------------- deterministic random numbers ---------------- */

/*
 * Our own generator instead of rand(), so a seed gives the same operation
 * stream on every machine and libc. splitmix64 turns the seed parts into a
 * well mixed 64-bit state, xorshift64* produces the stream.
 */
static u64
splitmix64(u64 x)
{
	x += 0x9E3779B97F4A7C15ULL;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
	return x ^ (x >> 31);
}

static u64
xorshift64s(u64 *state)
{
	u64 x = *state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/*
 * Seed of one operation stream. It depends on the config seed, the repeat,
 * the pattern and the read percentage, but NOT on the policy or the pool
 * size, so LRU and MRU (and every pool size) see the identical stream.
 */
static u64
stream_seed(long seed, int repeat, int pattern, int read_pct)
{
	u64 s;

	s = splitmix64((u64)seed);
	s = splitmix64(s ^ (u64)repeat);
	s = splitmix64(s ^ (u64)pattern);
	s = splitmix64(s ^ (u64)read_pct);
	if (s == 0)		/* xorshift must not start at 0 */
		s = 1;
	return s;
}

/* ---------------- one benchmark run ---------------- */

/* what to run */
struct run {
	char *fname;		/* scratch paged file, already built */
	int npages;		/* pages in it */
	int pool;		/* buffer pool frames */
	int policy;		/* PF_LRU or PF_MRU */
	int pattern;		/* PAT_* */
	int read_pct;		/* 0..100, share of reads */
	long ops;		/* operations to run */
	u64 seed;		/* from stream_seed */
	double hot_fraction;	/* hotcold: share of pages that are hot */
	double hot_prob;	/* hotcold: share of accesses to hot pages */
};

/* what came out */
struct result {
	PFstats st;		/* counters right after the operations */
	long total_pw;		/* physical_writes after PF_FlushAll */
	double ms;		/* wall time of the operations only */
	int dirty_end;		/* see run_one, only if expected != NULL */
};

static void
pf_error(const char *what)
{
	fprintf(stderr, "bench_buffer: %s:", what);
	PF_PrintError("");
}

static int
run_one(const struct run *r, struct result *res, int *expected)
/****************************************************************************
SPECIFICATIONS:
	Resize the pool to r->pool frames, open r->fname with r->policy
	(the pool is cold, since no file is open), reset its counters and
	time r->ops operations. Then read the counters, PF_FlushAll, read
	physical_writes again, and close the file.
	If "expected" is not NULL it has r->npages entries: a write stores
	the byte it wrote into expected[page], and before the flush
	res->dirty_end is set to the number of written pages still in the
	pool (with 100% writes those are exactly the dirty pages).

RETURN VALUE:
	0 if ok, -1 after printing an error (pool size, PF error, or a page
	whose tag is not its page number).
*****************************************************************************/
{
	struct timespec t0, t1;
	PFstats after;
	u64 state;
	char *buf;
	int fd, page, hot, tag, p;
	long i;
	unsigned char val;

	if (PF_InitBuffer(r->pool) != PFE_OK) {
		fprintf(stderr, "bench_buffer: pool size %d:", r->pool);
		PF_PrintError("");
		return -1;
	}
	fd = PF_OpenFileEx(r->fname, r->policy);
	if (fd < 0) {
		pf_error("open");
		return -1;
	}
	PF_ResetStats(fd);

	/* hot pages are 0..hot-1, at least one hot and one cold page */
	hot = (int)(r->hot_fraction * r->npages);
	if (hot < 1)
		hot = 1;
	if (hot > r->npages - 1)
		hot = r->npages - 1;

	state = r->seed;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < r->ops; i++) {
		/* pick the page (modulo bias is negligible for these sizes) */
		if (r->pattern == PAT_SEQUENTIAL)
			page = (int)(i % r->npages);
		else if (r->pattern == PAT_UNIFORM || r->npages < 2)
			page = (int)(xorshift64s(&state) % (u64)r->npages);
		else if ((double)(xorshift64s(&state) >> 11) / 9007199254740992.0
			 < r->hot_prob)
			page = (int)(xorshift64s(&state) % (u64)hot);
		else
			page = hot + (int)(xorshift64s(&state) %
					   (u64)(r->npages - hot));

		if (PF_GetThisPage(fd, page, &buf) != PFE_OK) {
			pf_error("get page");
			return -1;
		}
		/* every fix checks the tag, so a buffer bug is an error and
		   not a plausible looking number */
		memcpy(&tag, buf, sizeof(int));
		if (tag != page) {
			fprintf(stderr, "bench_buffer: page %d holds tag %d\n",
				page, tag);
			return -1;
		}

		if ((int)(xorshift64s(&state) % 100) < r->read_pct) {
			/* read: unfix clean */
			if (PF_UnfixPage(fd, page, FALSE) != PFE_OK) {
				pf_error("unfix");
				return -1;
			}
			continue;
		}

		/* write: change one byte, value from the op number */
		val = (unsigned char)(i % 251 + 1);
		buf[WBYTE] = (char)val;
		if (expected != NULL)
			expected[page] = val;
		if (i % 2 == 0) {
			if (PF_UnfixPage(fd, page, TRUE) != PFE_OK) {
				pf_error("unfix dirty");
				return -1;
			}
		} else {
			if (PF_MarkDirty(fd, page) != PFE_OK ||
			    PF_UnfixPage(fd, page, FALSE) != PFE_OK) {
				pf_error("mark dirty / unfix");
				return -1;
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	res->ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
		  (t1.tv_nsec - t0.tv_nsec) / 1e6;

	if (PF_GetStats(fd, &res->st) != PFE_OK) {
		pf_error("stats");
		return -1;
	}

	/* pages written in this run and still in the pool (before the
	   flush, which does not move pages in or out anyway) */
	res->dirty_end = 0;
	if (expected != NULL)
		for (p = 0; p < r->npages; p++)
			if (expected[p] >= 0 && PFbufResident(fd, p))
				res->dirty_end++;

	/* untimed: write back what is still dirty, count those writes too */
	if (PF_FlushAll() != PFE_OK || PF_GetStats(fd, &after) != PFE_OK) {
		pf_error("flush");
		return -1;
	}
	res->total_pw = after.physical_writes;

	if (PF_CloseFile(fd) != PFE_OK) {
		pf_error("close");
		return -1;
	}
	return 0;
}

static int
make_file(char *fname, int npages)
/****************************************************************************
SPECIFICATIONS:
	(Re)create fname with pages 0..npages-1 from PF_AllocPage, each
	filled with the layout described at WBYTE, then close it (which
	writes every page and empties the pool).

RETURN VALUE:
	0 if ok, -1 after printing an error.
*****************************************************************************/
{
	char *buf;
	int fd, i, pn;

	unlink(fname);		/* PF_CreateFile uses O_EXCL */
	if (PF_CreateFile(fname) != PFE_OK) {
		pf_error(fname);
		return -1;
	}
	fd = PF_OpenFileEx(fname, PF_LRU);
	if (fd < 0) {
		pf_error(fname);
		return -1;
	}
	for (i = 0; i < npages; i++) {
		if (PF_AllocPage(fd, &pn, &buf) != PFE_OK || pn != i) {
			pf_error("alloc page");
			return -1;
		}
		memset(buf, pn & 0xff, PF_PAGE_SIZE);
		memcpy(buf, &pn, sizeof(int));
		/* a new page is dirty already, a clean unfix is enough */
		if (PF_UnfixPage(fd, pn, FALSE) != PFE_OK) {
			pf_error("unfix new page");
			return -1;
		}
	}
	if (PF_CloseFile(fd) != PFE_OK) {
		pf_error("close new file");
		return -1;
	}
	return 0;
}

/* ---------------- check mode ---------------- */

static int nfail = 0;		/* failed checks */
static int ncheck = 0;		/* all checks */

#define CHECK(cond) do { \
		ncheck++; \
		if (!(cond)) { \
			nfail++; \
			printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

/* check-mode fixtures: tiny, so every counter can be worked out by hand */
#define CK_PAGES	10
#define CK_SEED		1L

static char ckname[] = "bench_buffer_check.pf";

static int
ck_run(int policy, int pattern, int pool, int read_pct, long ops,
       struct result *res, int *expected)
/****************************************************************************
SPECIFICATIONS:
	One check-mode run on ckname, plus check 1 (which holds for every
	run): hits + misses == logical_reads == ops.

RETURN VALUE:
	0 if the run worked, -1 if it failed (counted as a failed check).
*****************************************************************************/
{
	struct run r;

	r.fname = ckname;
	r.npages = CK_PAGES;
	r.pool = pool;
	r.policy = policy;
	r.pattern = pattern;
	r.read_pct = read_pct;
	r.ops = ops;
	r.seed = stream_seed(CK_SEED, 0, pattern, read_pct);
	r.hot_fraction = 0.2;
	r.hot_prob = 0.8;
	if (run_one(&r, res, expected) != 0) {
		CHECK(!"run_one failed");
		return -1;
	}
	/* check 1 */
	CHECK(res->st.hits + res->st.misses == res->st.logical_reads);
	CHECK(res->st.logical_reads == ops);
	return 0;
}

static int
check_mode(void)
{
	struct result a, b;
	int expected[CK_PAGES];
	int pol, p, fd, tag;
	char *buf;

	if (make_file(ckname, CK_PAGES) != 0)
		return 1;

	/*
	 * check 2: pool (12) >= pages (10), a sequential loop of 2 passes
	 * touches every page. Each page misses once and is never evicted.
	 */
	for (pol = PF_LRU; pol <= PF_MRU; pol++) {
		if (ck_run(pol, PAT_SEQUENTIAL, 12, 50, 2 * CK_PAGES, &a, NULL))
			goto out;
		CHECK(a.st.misses == CK_PAGES);
		CHECK(a.st.physical_reads == CK_PAGES);
		CHECK(a.st.evictions == 0);
	}

	/* check 3: only reads, so nothing is ever dirty or written */
	for (pol = PF_LRU; pol <= PF_MRU; pol++) {
		if (ck_run(pol, PAT_UNIFORM, 8, 100, 50, &a, NULL))
			goto out;
		CHECK(a.st.logical_writes == 0);
		CHECK(a.st.physical_writes == 0);
		CHECK(a.st.dirty_evictions == 0);
		CHECK(a.total_pw == 0);
	}

	/*
	 * check 4: looping scan, 10 pages, pool 8, 3 passes (30 ops), reads
	 * only. Rules from buf.c: a page goes to the head of the used list
	 * when it is loaded and when it is unfixed; LRU evicts from the
	 * tail, MRU evicts the first unfixed page from the head (the page
	 * used just before).
	 *
	 * LRU: pass 1 misses 0..9 (8 and 9 evict 0 and 1). From then on the
	 * page needed next is always the one evicted longest ago, so every
	 * access misses: hits 0, misses 30, evictions 30 - 8 = 22.
	 *
	 * MRU:
	 *  pass 1: 0..7 miss and fill the pool, 8 evicts 7, 9 evicts 8.
	 *          10 misses, 2 evictions, pool {0..6, 9}.
	 *  pass 2: 0..6 hit (7). 7 evicts 6, 8 evicts 7, 9 hits.
	 *          8 hits, 2 misses, 2 evictions, pool {0..5, 8, 9}.
	 *  pass 3: 0..5 hit (6). 6 evicts 5, 7 evicts 6, 8 and 9 hit.
	 *          8 hits, 2 misses, 2 evictions.
	 *  total:  hits 16, misses 14, evictions 6.
	 */
	if (ck_run(PF_LRU, PAT_SEQUENTIAL, 8, 100, 3 * CK_PAGES, &a, NULL))
		goto out;
	CHECK(a.st.hits == 0);
	CHECK(a.st.misses == 30);
	CHECK(a.st.evictions == 22);
	if (ck_run(PF_MRU, PAT_SEQUENTIAL, 8, 100, 3 * CK_PAGES, &b, NULL))
		goto out;
	CHECK(b.st.hits == 16);
	CHECK(b.st.misses == 14);
	CHECK(b.st.evictions == 6);
	CHECK(b.st.hits > a.st.hits);

	/* check 5: the same seed gives the same counters, for both policies */
	for (pol = PF_LRU; pol <= PF_MRU; pol++) {
		if (ck_run(pol, PAT_HOTCOLD, 8, 50, 200, &a, NULL) ||
		    ck_run(pol, PAT_HOTCOLD, 8, 50, 200, &b, NULL))
			goto out;
		CHECK(memcmp(&a.st, &b.st, sizeof(PFstats)) == 0);
		CHECK(a.total_pw == b.total_pw);
	}

	/*
	 * check 6: writes only, pool 8 < 10 pages, so there are evictions
	 * and every victim is dirty. total_physical_writes must be the dirty
	 * evictions plus the pages still dirty at the end: with 100% writes
	 * those are the written pages still in the pool, which is also
	 * misses - evictions (the pool started empty). Then every write
	 * must be on disk: after close, reopen and check byte WBYTE.
	 */
	for (pol = PF_LRU; pol <= PF_MRU; pol++) {
		for (p = 0; p < CK_PAGES; p++)
			expected[p] = -1;
		if (ck_run(pol, PAT_UNIFORM, 8, 0, 100, &a, expected))
			goto out;
		CHECK(a.st.evictions > 0);
		CHECK(a.st.dirty_evictions == a.st.evictions);
		CHECK(a.dirty_end == a.st.misses - a.st.evictions);
		CHECK(a.total_pw == a.st.dirty_evictions + a.dirty_end);

		fd = PF_OpenFileEx(ckname, pol);
		CHECK(fd >= 0);
		if (fd < 0)
			goto out;
		for (p = 0; p < CK_PAGES; p++) {
			CHECK(PF_GetThisPage(fd, p, &buf) == PFE_OK);
			memcpy(&tag, buf, sizeof(int));
			CHECK(tag == p);
			if (expected[p] >= 0)
				CHECK((unsigned char)buf[WBYTE] == expected[p]);
			CHECK(PF_UnfixPage(fd, p, FALSE) == PFE_OK);
		}
		CHECK(PF_CloseFile(fd) == PFE_OK);
	}

out:
	CHECK(PF_DestroyFile(ckname) == PFE_OK);
	printf("bench_buffer check: %d checks, %d failed\n", ncheck, nfail);
	if (nfail > 0)
		return 1;
	printf("bench_buffer check done!\n");
	return 0;
}

/* ---------------- full run ---------------- */

static int
bench_mode(void)
{
	int pools[CONFIG_MAXLIST], pcts[CONFIG_MAXLIST];
	int pats[CONFIG_MAXLIST];
	char names[CONFIG_MAXLIST][CONFIG_VALLEN];
	char dir[4096], fname[4096];
	const char *wd, *root;
	struct run r;
	struct result res;
	int npools, npcts, npats, repeats, rep, pi, ti, ci, k, pol;
	long seed;

	r.npages = Config_GetInt("buf_file_pages", 1000);
	r.ops = Config_GetInt("buf_queries", 10000);
	r.hot_fraction = Config_GetDouble("hot_fraction", 0.2);
	r.hot_prob = Config_GetDouble("hot_prob", 0.8);
	seed = Config_GetInt("seed", 42);
	repeats = Config_GetInt("repeats", 5);
	npools = Config_GetIntList("buf_pool_sizes", pools, CONFIG_MAXLIST,
				   "20,50,100,900,1200");
	npcts = Config_GetIntList("buf_read_pcts", pcts, CONFIG_MAXLIST,
				  "100,90,75,50,25,10,0");
	npats = Config_GetStrList("buf_patterns", names, CONFIG_MAXLIST,
				  "uniform,hotcold,sequential");

	if (r.npages < 1 || r.ops < 0 || repeats < 1) {
		fprintf(stderr, "bench_buffer: need buf_file_pages >= 1, "
			"buf_queries >= 0, repeats >= 1\n");
		return 1;
	}
	for (ci = 0; ci < npcts; ci++)
		if (pcts[ci] < 0 || pcts[ci] > 100) {
			fprintf(stderr, "bench_buffer: buf_read_pcts value %d "
				"is not in 0..100\n", pcts[ci]);
			return 1;
		}
	for (ti = 0; ti < npats; ti++) {
		for (k = 0; k < 3; k++)
			if (strcmp(names[ti], patname[k]) == 0)
				break;
		if (k == 3) {
			fprintf(stderr, "bench_buffer: unknown pattern \"%s\" "
				"in buf_patterns\n", names[ti]);
			return 1;
		}
		pats[ti] = k;
	}

	/* scratch file in work_dir, a relative one taken from TOYDB_ROOT */
	wd = Config_GetStr("work_dir", "/tmp/toydb_run");
	root = getenv("TOYDB_ROOT");
	if (wd[0] != '/' && root != NULL && root[0] != '\0')
		k = snprintf(dir, sizeof(dir), "%s/%s", root, wd);
	else
		k = snprintf(dir, sizeof(dir), "%s", wd);
	if (k >= (int)sizeof(dir) ||
	    snprintf(fname, sizeof(fname), "%s/bench_buffer.pf", dir) >=
	    (int)sizeof(fname)) {
		fprintf(stderr, "bench_buffer: work_dir path too long\n");
		return 1;
	}
	if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "bench_buffer: cannot create work_dir %s\n",
			dir);
		return 1;
	}
	if (make_file(fname, r.npages) != 0)
		return 1;
	r.fname = fname;

	printf("policy,pattern,pool,read_pct,repeat,ops,logical_reads,"
	       "logical_writes,hits,misses,physical_reads,physical_writes,"
	       "total_physical_writes,evictions,dirty_evictions,hit_ratio,ms\n");
	for (pi = 0; pi < npools; pi++) {
		fprintf(stderr, "bench_buffer: pool %d (%d of %d)\n",
			pools[pi], pi + 1, npools);
		r.pool = pools[pi];
		for (pol = PF_LRU; pol <= PF_MRU; pol++)
		for (ti = 0; ti < npats; ti++)
		for (ci = 0; ci < npcts; ci++)
		for (rep = 0; rep < repeats; rep++) {
			r.policy = pol;
			r.pattern = pats[ti];
			r.read_pct = pcts[ci];
			r.seed = stream_seed(seed, rep, pats[ti], pcts[ci]);
			if (run_one(&r, &res, NULL) != 0) {
				unlink(fname);
				return 1;
			}
			printf("%s,%s,%d,%d,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,"
			       "%ld,%ld,%.6f,%.3f\n",
			       pol == PF_LRU ? "LRU" : "MRU",
			       patname[r.pattern], r.pool, r.read_pct, rep,
			       r.ops, res.st.logical_reads,
			       res.st.logical_writes, res.st.hits,
			       res.st.misses, res.st.physical_reads,
			       res.st.physical_writes, res.total_pw,
			       res.st.evictions, res.st.dirty_evictions,
			       res.st.logical_reads > 0 ?
			       (double)res.st.hits / res.st.logical_reads : 0.0,
			       res.ms);
		}
	}
	if (PF_DestroyFile(fname) != PFE_OK) {
		pf_error("destroy");
		return 1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int i, check, status;

	status = Config_Init(argc, argv);
	if (status != CONFIG_OK && status != CONFIG_ENOFILE) {
		fprintf(stderr, "bench_buffer: bad configuration\n");
		return 1;
	}
	check = 0;
	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "check") == 0)
			check = 1;
	PF_Init();
	return check ? check_mode() : bench_mode();
}
