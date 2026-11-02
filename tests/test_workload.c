/* test_workload.c: end-to-end LRU vs MRU comparison on one workload.
 *
 * usage: test_workload
 *
 * Uses one scratch paged file, test_workload.pf, in the current directory
 * (make test runs this from a scratch dir). It is deleted before it is
 * created (PF_CreateFile uses O_EXCL) and destroyed at the end.
 * The last output line is "test_workload done!" only if every check passed.
 *
 * Workload: a 10-page file (pages 0..9), a pool of 8 frames, two
 * sequential passes over pages 0..9, each access a fix then an unfix.
 * It runs once per policy and per variant (read-only: unfix FALSE,
 * write: unfix TRUE), each run with a fresh pool (PF_InitBuffer(8)), a
 * freshly opened file and PF_ResetStats(-1) right after the open.
 *
 * Expected values, worked out by hand (a page goes to the head when it is
 * brought in or unfixed, a hit does not move it; LRU takes the victim
 * from the tail, MRU from the head):
 * LRU: pass 1 loads 0..7, then 8 evicts 0 and 9 evicts 1. In pass 2 every
 *      page was evicted just before it is asked for (0 evicts 2, ...,
 *      7 evicts 9, 8 evicts 0, 9 evicts 1), so 20 misses, 12 evictions.
 * MRU: pass 1 loads 0..7, 8 evicts 7, 9 evicts 8, pool {0..6, 9}.
 *      Pass 2: 0..6 hit, 7 evicts 6, 8 evicts 7, 9 hits.
 *      So 8 hits, 12 misses, 4 evictions.
 * Write variant: every page in the pool is dirty, so every eviction is a
 * dirty eviction and a physical write, and each of the 20 fix periods
 * has one logical write. Closing the file writes back the 8 pages left
 * in the pool (not evictions), so after the close
 * physical_writes = evictions + 8.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pf.h"

/*
 * Stopgap: pf.h does not declare these PF functions yet (they are K&R
 * definitions in pf.c), and -Wall warns about implicit declarations.
 * Remove these once pf.h has real prototypes for them.
 */
extern int PF_CreateFile(char *fname);
extern int PF_DestroyFile(char *fname);
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_AllocPage(int fd, int *pagenum, char **pagebuf);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);

static int nfail = 0;		/* failed checks */
static int ncheck = 0;		/* all checks */

#define CHECK(cond) do { \
		ncheck++; \
		if (!(cond)) { \
			nfail++; \
			printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

static char fname[] = "test_workload.pf";

#define NPAGES	10	/* pages 0..9 */
#define NBUFS	8	/* pool size for every run */
#define NPASSES	2	/* sequential passes over the file */

/* hand-computed expected counts for one policy (see the top comment) */
typedef struct {
	int policy;
	long hits;
	long misses;
	long evictions;
} expect;

static expect exp_lru = { PF_LRU, 0, 20, 12 };
static expect exp_mru = { PF_MRU, 8, 12, 4 };

static void make_file(void)
/****************************************************************************
SPECIFICATIONS:
	(Re)create fname with pages 0..NPAGES-1, page i holding the int i
	at the start, then close it, so no page of it is left in the pool.
*****************************************************************************/
{
	char *buf;
	int fd, i, pn, ok;

	unlink(fname);
	CHECK(PF_CreateFile(fname) == PFE_OK);
	fd = PF_OpenFile(fname);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	ok = 1;
	for (i = 0; i < NPAGES; i++) {
		if (PF_AllocPage(fd, &pn, &buf) != PFE_OK || pn != i) {
			ok = 0;
			break;
		}
		memcpy(buf, &pn, sizeof(int));
		if (PF_UnfixPage(fd, pn, TRUE) != PFE_OK)
			ok = 0;
	}
	CHECK(ok);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

static PFstats get_total(void)
/****************************************************************************
SPECIFICATIONS:
	Return the global counters; all fields are -1 if PF_GetStats
	fails, so every later check on them fails too.
*****************************************************************************/
{
	PFstats s;

	if (PF_GetStats(-1, &s) != PFE_OK)
		memset(&s, 0xff, sizeof(s));
	return s;
}

static void run(int policy, int dirty, PFstats *before, PFstats *after)
/****************************************************************************
SPECIFICATIONS:
	Run the workload once with a fresh pool of NBUFS frames on fname
	opened with "policy", unfixing every page with "dirty". Set
	*before to the global counters just before the file is closed
	and *after to the global counters just after.
*****************************************************************************/
{
	char *buf;
	int fd, pass, page, v, ok;

	memset(before, 0xff, sizeof(*before));
	memset(after, 0xff, sizeof(*after));
	CHECK(PF_InitBuffer(NBUFS) == PFE_OK);
	fd = PF_OpenFileEx(fname, policy);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	PF_ResetStats(-1);

	ok = 1;
	for (pass = 0; pass < NPASSES; pass++)
		for (page = 0; page < NPAGES; page++) {
			if (PF_GetThisPage(fd, page, &buf) != PFE_OK) {
				ok = 0;
				continue;
			}
			memcpy(&v, buf, sizeof(int));
			if (v != page)
				ok = 0;
			if (PF_UnfixPage(fd, page, dirty) != PFE_OK)
				ok = 0;
		}
	CHECK(ok);

	*before = get_total();
	CHECK(PF_CloseFile(fd) == PFE_OK);
	*after = get_total();
}

static void check_common(expect *e, PFstats s)
/****************************************************************************
SPECIFICATIONS:
	Checks that hold for both variants: hits, misses, logical and
	physical reads, evictions.
*****************************************************************************/
{
	CHECK(s.hits == e->hits);
	CHECK(s.misses == e->misses);
	CHECK(s.logical_reads == NPASSES * NPAGES);
	CHECK(s.physical_reads == e->misses);
	CHECK(s.evictions == e->evictions);
	CHECK(s.hits + s.misses == s.logical_reads);
}

static void test_read(expect *e, PFstats *out)
/****************************************************************************
SPECIFICATIONS:
	Read-only variant for one policy; *out gets the totals after the
	close.
*****************************************************************************/
{
	PFstats b, a;

	run(e->policy, FALSE, &b, &a);
	check_common(e, b);
	CHECK(b.dirty_evictions == 0);
	CHECK(b.logical_writes == 0);
	CHECK(b.physical_writes == 0);
	/* nothing dirty, so the close writes nothing */
	check_common(e, a);
	CHECK(a.dirty_evictions == 0);
	CHECK(a.logical_writes == 0);
	CHECK(a.physical_writes == 0);
	*out = a;
}

static void test_write(expect *e, PFstats *out)
/****************************************************************************
SPECIFICATIONS:
	Write variant for one policy; *out gets the totals after the close.
*****************************************************************************/
{
	PFstats b, a;

	run(e->policy, TRUE, &b, &a);
	check_common(e, b);
	CHECK(b.logical_writes == NPASSES * NPAGES);
	CHECK(b.dirty_evictions == e->evictions);
	CHECK(b.physical_writes == b.dirty_evictions);
	/* the close writes back the NBUFS dirty pages left in the pool */
	check_common(e, a);
	CHECK(a.logical_writes == NPASSES * NPAGES);
	CHECK(a.dirty_evictions == e->evictions);
	CHECK(a.physical_writes == e->evictions + NBUFS);
	*out = a;
}

int main(void)
{
	PFstats lru_r, mru_r, lru_w, mru_w;

	PF_Init();
	make_file();

	test_read(&exp_lru, &lru_r);
	test_read(&exp_mru, &mru_r);
	test_write(&exp_lru, &lru_w);
	test_write(&exp_mru, &mru_w);

	/* sequential flooding: MRU reads fewer pages than LRU */
	CHECK(mru_r.physical_reads < lru_r.physical_reads);
	CHECK(mru_w.physical_reads < lru_w.physical_reads);

	CHECK(PF_DestroyFile(fname) == PFE_OK);
	printf("test_workload: %d checks, %d failed\n", ncheck, nfail);
	if (nfail > 0)
		return 1;
	printf("test_workload done!\n");
	return 0;
}
