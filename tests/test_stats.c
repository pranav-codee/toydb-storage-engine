/* test_stats.c: tests for the PF I/O statistics and PF_FlushAll (P6).
 *
 * usage: test_stats
 *
 * Uses two scratch paged files, test_stats.pf and test_stats2.pf, in the
 * current directory (make test runs this from a scratch dir). Each is
 * deleted before it is created (PF_CreateFile uses O_EXCL) and destroyed
 * at the end. The pool is set to 8 frames with PF_InitBuffer(8) and files
 * are opened LRU.
 * The last output line is "test_stats done!" only if every check passed.
 *
 * Counter rules the expected values come from (worked out by hand, see
 * the spec of PF_GetStats in pf.c):
 * - logical_reads: one per fix of an existing page (GetThisPage,
 *   GetFirstPage, GetNextPage), hit or miss; hits + misses = logical_reads;
 * - physical_reads: one per page read from the file into a frame;
 * - logical_writes: Unfix(TRUE), MarkDirty, a new page from AllocPage,
 *   at most once per page between two fixes of it;
 * - physical_writes: dirty eviction, write-back at close, PF_FlushAll;
 * - evictions / dirty_evictions: victims taken (and the dirty ones),
 *   charged to the file that owns the victim.
 * Buffer rules: a page goes to the head of the used list when it is
 * brought in and when it is unfixed, a hit does not move it, LRU evicts
 * the unfixed page nearest the tail.
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

static char fname[] = "test_stats.pf";		/* 10-page file */
static char fname2[] = "test_stats2.pf";	/* second file */

#define NPAGES	10	/* pages 0..9 of fname */
#define NBUFS	8	/* pool size for every scenario */

static void make_file(char *name, int npages)
/****************************************************************************
SPECIFICATIONS:
	(Re)create "name" with pages 0..npages-1, page i holding the int i
	at the start, then close it, so no page of it is left in the pool.
*****************************************************************************/
{
	char *buf;
	int fd, i, pn, ok;

	unlink(name);
	CHECK(PF_CreateFile(name) == PFE_OK);
	fd = PF_OpenFile(name);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	ok = 1;
	for (i = 0; i < npages; i++) {
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

static int fix(int fd, int page)
/****************************************************************************
SPECIFICATIONS:
	Fix one page and check it holds its own number.

RETURN VALUE:
	1 if the fix worked and the page holds "page", 0 otherwise.
*****************************************************************************/
{
	char *buf;
	int v;

	if (PF_GetThisPage(fd, page, &buf) != PFE_OK)
		return 0;
	memcpy(&v, buf, sizeof(int));
	return v == page;
}

static int fix_unfix(int fd, int page)
/****************************************************************************
SPECIFICATIONS:
	Fix one page, then unfix it clean.

RETURN VALUE:
	1 if both worked, 0 otherwise.
*****************************************************************************/
{
	int ok;

	ok = fix(fd, page);
	if (PF_UnfixPage(fd, page, FALSE) != PFE_OK)
		ok = 0;
	return ok;
}

static PFstats get(int fd)
/****************************************************************************
SPECIFICATIONS:
	Return the counters of fd (-1 = global); all fields are -1 if
	PF_GetStats fails, so every later check on them fails too.
*****************************************************************************/
{
	PFstats s;

	if (PF_GetStats(fd, &s) != PFE_OK)
		memset(&s, 0xff, sizeof(s));
	return s;
}

static int all_zero(PFstats s)
{
	return s.logical_reads == 0 && s.logical_writes == 0 &&
		s.hits == 0 && s.misses == 0 && s.physical_reads == 0 &&
		s.physical_writes == 0 && s.evictions == 0 &&
		s.dirty_evictions == 0;
}

static int same(PFstats a, PFstats b)
{
	return memcmp(&a, &b, sizeof(PFstats)) == 0;
}

static void test_main(void)
{
	PFstats s, g;
	FILE *f;
	char line[100];
	int fd, closed, p, ok, nlines;

	make_file(fname, NPAGES);
	CHECK(PF_InitBuffer(NBUFS) == PFE_OK);
	fd = PF_OpenFileEx(fname, PF_LRU);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	PF_ResetStats(-1);
	CHECK(all_zero(get(fd)));
	CHECK(all_zero(get(-1)));

	/* 1. pages 0..7 all miss. List (head first) 7 6 5 4 3 2 1 0. */
	ok = 1;
	for (p = 0; p <= 7; p++)
		ok &= fix_unfix(fd, p);
	CHECK(ok);
	s = get(fd);
	CHECK(s.misses == 8);
	CHECK(s.logical_reads == 8);
	CHECK(s.physical_reads == 8);
	CHECK(s.hits == 0);

	/* 2. page 3 hits. List 3 7 6 5 4 2 1 0. */
	CHECK(fix_unfix(fd, 3));
	s = get(fd);
	CHECK(s.logical_reads == 9);
	CHECK(s.hits == 1);
	CHECK(s.physical_reads == 8);

	/* 3. page 8 evicts the tail 0 (clean), unfix dirty.
	   List 8 3 7 6 5 4 2 1. */
	CHECK(fix(fd, 8));
	CHECK(PF_UnfixPage(fd, 8, TRUE) == PFE_OK);
	s = get(fd);
	CHECK(s.evictions == 1);
	CHECK(s.dirty_evictions == 0);
	CHECK(s.logical_writes == 1);
	CHECK(s.physical_writes == 0);

	/* 4. page 9 evicts 1, page 0 evicts 2. List 0 9 8 3 7 6 5 4. */
	CHECK(fix_unfix(fd, 9));
	CHECK(fix_unfix(fd, 0));
	s = get(fd);
	CHECK(s.evictions == 3);

	/* 5. pages 1 2 4 5 6 evict 4 5 6 7 3, all clean.
	   List 6 5 4 2 1 0 9 8. */
	CHECK(fix_unfix(fd, 1));
	CHECK(fix_unfix(fd, 2));
	CHECK(fix_unfix(fd, 4));
	CHECK(fix_unfix(fd, 5));
	CHECK(fix_unfix(fd, 6));
	s = get(fd);
	CHECK(s.evictions == 8);
	CHECK(s.dirty_evictions == 0);
	CHECK(s.physical_writes == 0);

	/* 6. page 7 evicts the tail 8, which is dirty: written back.
	   Page 7 stays fixed. */
	CHECK(fix(fd, 7));
	s = get(fd);
	CHECK(s.evictions == 9);
	CHECK(s.dirty_evictions == 1);
	CHECK(s.physical_writes == 1);
	CHECK(s.logical_reads == 18);
	CHECK(s.hits == 1);
	CHECK(s.misses == 17);
	CHECK(s.physical_reads == 17);
	CHECK(s.logical_writes == 1);

	/* 7. MarkDirty then Unfix(TRUE) in one fix period: counted once. */
	CHECK(PF_MarkDirty(fd, 7) == PFE_OK);
	CHECK(get(fd).logical_writes == 2);
	CHECK(PF_UnfixPage(fd, 7, TRUE) == PFE_OK);
	CHECK(get(fd).logical_writes == 2);

	/* 8. page 7 hits, a new fix period, so Unfix(TRUE) counts again. */
	CHECK(fix(fd, 7));
	CHECK(PF_UnfixPage(fd, 7, TRUE) == PFE_OK);
	s = get(fd);
	CHECK(s.hits == 2);
	CHECK(s.logical_reads == 19);
	CHECK(s.logical_writes == 3);

	/* 9. only page 7 is dirty. */
	CHECK(PF_FlushAll() == PFE_OK);
	CHECK(get(fd).physical_writes == 2);

	/* 10. final counters, per file and global */
	s = get(fd);
	CHECK(s.logical_reads == 19);
	CHECK(s.logical_writes == 3);
	CHECK(s.hits == 2);
	CHECK(s.misses == 17);
	CHECK(s.physical_reads == 17);
	CHECK(s.physical_writes == 2);
	CHECK(s.evictions == 9);
	CHECK(s.dirty_evictions == 1);
	CHECK(s.hits + s.misses == s.logical_reads);
	g = get(-1);
	CHECK(same(g, s));

	f = tmpfile();
	CHECK(f != NULL);
	if (f != NULL) {
		PF_PrintStats(fd, f);
		rewind(f);
		CHECK(fgets(line, sizeof(line), f) != NULL &&
			strcmp(line, "logical_reads: 19\n") == 0);
		nlines = 1;
		while (fgets(line, sizeof(line), f) != NULL)
			nlines++;
		CHECK(nlines == 8);
		CHECK(strcmp(line, "dirty_evictions: 1\n") == 0);
		fclose(f);
	}

	/* 11. close: page 7 was flushed, nothing dirty is left */
	closed = fd;
	CHECK(PF_CloseFile(fd) == PFE_OK);
	CHECK(same(get(-1), s));
	CHECK(PF_GetStats(closed, &g) == PFE_FD);

	/* 12. */
	PF_ResetStats(-1);
	CHECK(all_zero(get(-1)));
}

static void test_new_page(void)
{
	PFstats s;
	char *buf;
	int fd, pn;

	/* AllocPage then Unfix(TRUE) is one fix period: one logical write,
	   and no read at all. */
	CHECK(PF_InitBuffer(NBUFS) == PFE_OK);
	unlink(fname2);
	CHECK(PF_CreateFile(fname2) == PFE_OK);
	fd = PF_OpenFileEx(fname2, PF_LRU);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	PF_ResetStats(-1);
	CHECK(PF_AllocPage(fd, &pn, &buf) == PFE_OK);
	CHECK(PF_UnfixPage(fd, pn, TRUE) == PFE_OK);
	s = get(fd);
	CHECK(s.logical_reads == 0);
	CHECK(s.misses == 0);
	CHECK(s.physical_reads == 0);
	CHECK(s.logical_writes == 1);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

static void test_two_files(void)
{
	PFstats x, y, g, y0;
	int fx, fy, p, ok;

	/*
	 * 8 pages of X fill the pool (X's page 0 at the tail), then one page
	 * of Y needs a frame: the victim is X's page 0, so the eviction is
	 * X's, the read is Y's.
	 */
	make_file(fname, NPAGES);
	make_file(fname2, 2);
	CHECK(PF_InitBuffer(NBUFS) == PFE_OK);
	fx = PF_OpenFileEx(fname, PF_LRU);
	fy = PF_OpenFileEx(fname2, PF_LRU);
	CHECK(fx >= 0 && fy >= 0);
	if (fx < 0 || fy < 0)
		return;
	PF_ResetStats(-1);
	ok = 1;
	for (p = 0; p <= 7; p++)
		ok &= fix_unfix(fx, p);
	CHECK(ok);
	CHECK(fix(fy, 0));
	x = get(fx);
	y = get(fy);
	g = get(-1);
	CHECK(x.physical_reads == 8);
	CHECK(x.evictions == 1);
	CHECK(y.physical_reads == 1);
	CHECK(y.evictions == 0);
	CHECK(g.physical_reads == 9);
	CHECK(g.evictions == 1);

	y0 = get(fy);
	PF_ResetStats(fx);
	CHECK(all_zero(get(fx)));
	CHECK(same(get(fy), y0));

	CHECK(PF_UnfixPage(fy, 0, FALSE) == PFE_OK);
	CHECK(PF_CloseFile(fx) == PFE_OK);
	CHECK(PF_CloseFile(fy) == PFE_OK);
}

static void test_errors(void)
{
	PFstats s;
	int fd;

	CHECK(PF_GetStats(-2, &s) == PFE_FD);
	fd = PF_OpenFileEx(fname, PF_LRU);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(PF_CloseFile(fd) == PFE_OK);
	CHECK(PF_GetStats(fd, &s) == PFE_FD);
}

int main(void)
{
	PF_Init();

	test_main();
	test_new_page();
	test_two_files();
	test_errors();

	CHECK(PF_DestroyFile(fname) == PFE_OK);
	CHECK(PF_DestroyFile(fname2) == PFE_OK);
	printf("test_stats: %d checks, %d failed\n", ncheck, nfail);
	if (nfail > 0)
		return 1;
	printf("test_stats done!\n");
	return 0;
}
