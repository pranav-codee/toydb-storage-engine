/* test_dirty.c: tests for PF_MarkDirty (P5).
 *
 * usage: test_dirty
 *
 * Uses one scratch paged file, test_dirty.pf, in the current directory
 * (make test runs this from a scratch dir). It is deleted before it is
 * created (PF_CreateFile uses O_EXCL) and destroyed at the end. Each of
 * the scenarios (a) to (d) rebuilds it, so they do not depend on each
 * other. The pool is set to 8 frames with PF_InitBuffer(8) and the file
 * is opened LRU.
 * The last output line is "test_dirty done!" only if every check passed.
 *
 * Rules the expected values come from (worked out by hand):
 * - a page goes to the head of the used list when it is brought into the
 *   buffer and every time it is unfixed; a fix that hits does not move it;
 * - LRU evicts the unfixed page nearest the tail;
 * - a dirty page is written back when it is evicted or its file is closed,
 *   a clean one is not written;
 * - PF_MarkDirty only sets the dirty bit (no move, no fix change, no I/O).
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

/* debug helper, not part of the public interface */
extern int PFbufResident(int fd, int pagenum);

static int nfail = 0;		/* failed checks */
static int ncheck = 0;		/* all checks */

#define CHECK(cond) do { \
		ncheck++; \
		if (!(cond)) { \
			nfail++; \
			printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

static char fname[] = "test_dirty.pf";	/* scratch file */

#define NPAGES	10	/* pages 0..9 */
#define NBUFS	8	/* pool size for every scenario */

static void make_file(void)
/****************************************************************************
SPECIFICATIONS:
	(Re)create fname with pages 0..NPAGES-1, page i holding the int i
	at the start, then close it. PF_AllocPage marks a new page dirty
	itself, so the pages reach the disk on close. Closing takes the
	pages out of the pool.
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
		if (PF_UnfixPage(fd, pn, FALSE) != PFE_OK)
			ok = 0;
	}
	CHECK(ok);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

static int open_fresh(void)
/****************************************************************************
SPECIFICATIONS:
	Reset the pool to NBUFS empty frames and open fname LRU.

RETURN VALUE:
	the file descriptor, or a negative PF error code.
*****************************************************************************/
{
	CHECK(PF_InitBuffer(NBUFS) == PFE_OK);
	return PF_OpenFileEx(fname, PF_LRU);
}

static int reopen(int fd)
/****************************************************************************
SPECIFICATIONS:
	Close fd (dirty pages are written back) and open fname again.

RETURN VALUE:
	the new file descriptor, or a negative PF error code.
*****************************************************************************/
{
	CHECK(PF_CloseFile(fd) == PFE_OK);
	return PF_OpenFileEx(fname, PF_LRU);
}

static int fix_get(int fd, int page, char **buf)
/****************************************************************************
SPECIFICATIONS:
	Fix one page and return the first int stored in it, or -1 if the
	fix failed. *buf is set to the page data.
*****************************************************************************/
{
	int v;

	if (PF_GetThisPage(fd, page, buf) != PFE_OK)
		return -1;
	memcpy(&v, *buf, sizeof(int));
	return v;
}

static int fix_unfix_range(int fd, int from, int to)
/****************************************************************************
SPECIFICATIONS:
	Fix then unfix (clean) pages from..to in order, checking each
	page holds its own number.

RETURN VALUE:
	1 if every fix and unfix worked, 0 otherwise.
*****************************************************************************/
{
	char *buf;
	int p, ok;

	ok = 1;
	for (p = from; p <= to; p++) {
		if (fix_get(fd, p, &buf) != p)
			ok = 0;
		if (PF_UnfixPage(fd, p, FALSE) != PFE_OK)
			ok = 0;
	}
	return ok;
}

static int resident_is(int fd, const char *want)
/****************************************************************************
SPECIFICATIONS:
	Compare which pages 0..NPAGES-1 of fd are in the pool with "want",
	a string of NPAGES chars, '1' = in the pool, '0' = not. Prints the
	actual set if it differs.

RETURN VALUE:
	1 if they match, 0 otherwise.
*****************************************************************************/
{
	char got[NPAGES + 1];
	int p;

	for (p = 0; p < NPAGES; p++)
		got[p] = PFbufResident(fd, p) ? '1' : '0';
	got[NPAGES] = '\0';
	if (strcmp(got, want) == 0)
		return 1;
	printf("  fd %d resident %s, expected %s\n", fd, got, want);
	return 0;
}

static void put_int(char *buf, int v)
{
	memcpy(buf, &v, sizeof(int));
}

static void test_persist(void)
{
	char *buf;
	int fd;

	/*
	 * (a) Fix 0, write 1111, MarkDirty, unfix clean. Unfix with FALSE
	 * must not clear the bit, so the close writes page 0 back.
	 * After reopening, page 0 holds 1111.
	 */
	make_file();
	fd = open_fresh();
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(fix_get(fd, 0, &buf) == 0);
	put_int(buf, 1111);
	CHECK(PF_MarkDirty(fd, 0) == PFE_OK);
	CHECK(PF_UnfixPage(fd, 0, FALSE) == PFE_OK);
	fd = reopen(fd);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(fix_get(fd, 0, &buf) == 1111);
	CHECK(PF_UnfixPage(fd, 0, FALSE) == PFE_OK);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

static void test_no_call(void)
{
	char *buf;
	int fd;

	/*
	 * (b) Fix 1, write 2222, unfix clean, no MarkDirty: list [1].
	 * Fix/unfix 0: [0 1]. Fix/unfix 2..7: [7 6 5 4 3 2 0 1], pool full.
	 * Fix 8: tail is 1, evicted clean (not written). Unfix 8.
	 * Fix 9: tail is now 0, evicted. Resident {2..9}.
	 * Fix 1 reads it from disk: the original 1.
	 */
	make_file();
	fd = open_fresh();
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(fix_get(fd, 1, &buf) == 1);
	put_int(buf, 2222);
	CHECK(PF_UnfixPage(fd, 1, FALSE) == PFE_OK);
	CHECK(fix_unfix_range(fd, 0, 0));
	CHECK(fix_unfix_range(fd, 2, 7));
	CHECK(resident_is(fd, "1111111100"));
	CHECK(fix_unfix_range(fd, 8, 8));
	CHECK(!PFbufResident(fd, 1));
	CHECK(fix_unfix_range(fd, 9, 9));
	CHECK(resident_is(fd, "0011111111"));
	CHECK(fix_get(fd, 1, &buf) == 1);
	CHECK(PF_UnfixPage(fd, 1, FALSE) == PFE_OK);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

static void test_unfix_dirty(void)
{
	char *buf;
	int fd;

	/* (c) Fix 2, write 3333, unfix with TRUE, close, reopen: 3333. */
	make_file();
	fd = open_fresh();
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(fix_get(fd, 2, &buf) == 2);
	put_int(buf, 3333);
	CHECK(PF_UnfixPage(fd, 2, TRUE) == PFE_OK);
	fd = reopen(fd);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(fix_get(fd, 2, &buf) == 3333);
	CHECK(PF_UnfixPage(fd, 2, FALSE) == PFE_OK);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

static void test_no_recency(void)
{
	char *buf;
	int fd;

	/*
	 * (d) Fix 0, write 4444, unfix clean: [0].
	 * Fix/unfix 1..7: [7 6 5 4 3 2 1 0], page 0 at the tail, unfixed.
	 * MarkDirty(0): only the bit, page 0 stays at the tail.
	 * Fix 8: LRU takes the tail, page 0, and writes it back because it
	 * is dirty. Resident {1..8}. (Had MarkDirty moved 0 to the head
	 * like PFbufUsed, page 1 would have gone instead.)
	 * Page 0 is out of the pool before the close, so the 4444 seen after
	 * reopening can only come from the eviction write.
	 */
	make_file();
	fd = open_fresh();
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(fix_get(fd, 0, &buf) == 0);
	put_int(buf, 4444);
	CHECK(PF_UnfixPage(fd, 0, FALSE) == PFE_OK);
	CHECK(fix_unfix_range(fd, 1, 7));
	CHECK(resident_is(fd, "1111111100"));
	CHECK(PF_MarkDirty(fd, 0) == PFE_OK);
	CHECK(resident_is(fd, "1111111100"));
	CHECK(fix_get(fd, 8, &buf) == 8);
	CHECK(!PFbufResident(fd, 0));
	CHECK(resident_is(fd, "0111111110"));
	CHECK(PF_UnfixPage(fd, 8, FALSE) == PFE_OK);
	fd = reopen(fd);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(fix_get(fd, 0, &buf) == 4444);
	CHECK(PF_UnfixPage(fd, 0, FALSE) == PFE_OK);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

static void test_idempotent(void)
{
	char *buf;
	int fd;

	/*
	 * (e) MarkDirty twice on a fixed page: both PFE_OK. The fix state
	 * is unchanged, so one unfix works and a second one is
	 * PFE_PAGEUNFIXED. On the now unfixed page MarkDirty twice is also
	 * PFE_OK, and the page is still unfixed afterwards.
	 */
	make_file();
	fd = open_fresh();
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(fix_get(fd, 3, &buf) == 3);
	CHECK(PF_MarkDirty(fd, 3) == PFE_OK);
	CHECK(PF_MarkDirty(fd, 3) == PFE_OK);
	CHECK(PF_UnfixPage(fd, 3, FALSE) == PFE_OK);
	CHECK(PF_UnfixPage(fd, 3, FALSE) == PFE_PAGEUNFIXED);
	CHECK(PF_MarkDirty(fd, 3) == PFE_OK);
	CHECK(PF_MarkDirty(fd, 3) == PFE_OK);
	CHECK(PF_UnfixPage(fd, 3, FALSE) == PFE_PAGEUNFIXED);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

static void test_errors(void)
{
	int fd, closed;

	/*
	 * (f) Fresh pool, file open, nothing fixed yet.
	 * fd -1 and a closed fd: PFE_FD. Page -1 and page 10 (file has
	 * pages 0..9): PFE_INVALIDPAGE. Page 5, valid but not in the
	 * pool: PFE_PAGENOTINBUF. PFerrno matches each time.
	 */
	closed = open_fresh();		/* open, then close: a stale fd */
	CHECK(closed >= 0);
	if (closed < 0)
		return;
	CHECK(PF_CloseFile(closed) == PFE_OK);
	CHECK(PF_MarkDirty(closed, 0) == PFE_FD);
	CHECK(PFerrno == PFE_FD);
	CHECK(PF_MarkDirty(-1, 0) == PFE_FD);
	CHECK(PFerrno == PFE_FD);

	fd = PF_OpenFileEx(fname, PF_LRU);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(PF_MarkDirty(fd, -1) == PFE_INVALIDPAGE);
	CHECK(PFerrno == PFE_INVALIDPAGE);
	CHECK(PF_MarkDirty(fd, NPAGES) == PFE_INVALIDPAGE);
	CHECK(PFerrno == PFE_INVALIDPAGE);
	CHECK(!PFbufResident(fd, 5));
	CHECK(PF_MarkDirty(fd, 5) == PFE_PAGENOTINBUF);
	CHECK(PFerrno == PFE_PAGENOTINBUF);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

int main(void)
{
	PF_Init();

	test_persist();
	test_no_call();
	test_unfix_dirty();
	test_no_recency();
	test_idempotent();
	test_errors();

	CHECK(PF_DestroyFile(fname) == PFE_OK);
	printf("test_dirty: %d checks, %d failed\n", ncheck, nfail);
	if (nfail > 0)
		return 1;
	printf("test_dirty done!\n");
	return 0;
}
