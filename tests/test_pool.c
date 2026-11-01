/* test_pool.c: tests for the runtime buffer pool size (P1) and the bigger
 * PF hash table (P2).
 *
 * usage: test_pool
 *
 * Uses one scratch paged file, test_pool.pf, in the current directory
 * (make test runs this from a scratch dir). It is deleted before every
 * create (PF_CreateFile uses O_EXCL) and at the end.
 * The last output line is "test_pool done!" only if every check passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "pf.h"
#include "config.h"

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

static char fname[] = "test_pool.pf";	/* scratch paged file */

#define HASH_PAGES	2000	/* pages for the hash test (d) */
#define MAXFIX		2048	/* max pages fixed at once in this test */

static void fill(char *buf, int page, int tag)
/****************************************************************************
SPECIFICATIONS:
	Write the pattern of page "page" into a page buffer: the page
	number as an int at the start, then bytes that depend on the page,
	the offset and "tag" (the pool size of the run).
*****************************************************************************/
{
	int i;

	memcpy(buf, &page, sizeof(int));
	for (i = sizeof(int); i < PF_PAGE_SIZE; i++)
		buf[i] = (char)((page * 131 + i * 7 + tag) & 0xff);
}

static int verify(const char *buf, int page, int tag)
/****************************************************************************
SPECIFICATIONS:
	Check that buf holds exactly what fill(buf, page, tag) wrote.

RETURN VALUE:
	1 if every byte matches, 0 otherwise.
*****************************************************************************/
{
	int i, p;

	memcpy(&p, buf, sizeof(int));
	if (p != page)
		return 0;
	for (i = sizeof(int); i < PF_PAGE_SIZE; i++)
		if (buf[i] != (char)((page * 131 + i * 7 + tag) & 0xff))
			return 0;
	return 1;
}

static int make_file(int npages, int tag)
/****************************************************************************
SPECIFICATIONS:
	Create the scratch file with npages pages, each filled with its
	pattern and unfixed dirty, and leave it open.

RETURN VALUE:
	The PF file descriptor, or -1 if something failed (checks counted).
*****************************************************************************/
{
	char *buf;
	int fd, i, pn, ok;

	unlink(fname);
	CHECK(PF_CreateFile(fname) == PFE_OK);
	fd = PF_OpenFile(fname);
	CHECK(fd >= 0);
	if (fd < 0)
		return -1;
	ok = 1;
	for (i = 0; i < npages && ok; i++) {
		if (PF_AllocPage(fd, &pn, &buf) != PFE_OK || pn != i) {
			ok = 0;
			break;
		}
		fill(buf, pn, tag);
		if (PF_UnfixPage(fd, pn, TRUE) != PFE_OK)
			ok = 0;
	}
	CHECK(ok);
	return ok ? fd : -1;
}

static int read_page(int fd, int page, int tag)
/****************************************************************************
SPECIFICATIONS:
	Fix one page, check its pattern, unfix it clean.

RETURN VALUE:
	1 if the page was read and matched, 0 otherwise.
*****************************************************************************/
{
	char *buf;
	int ok;

	if (PF_GetThisPage(fd, page, &buf) != PFE_OK)
		return 0;
	ok = verify(buf, page, tag);
	if (PF_UnfixPage(fd, page, FALSE) != PFE_OK)
		ok = 0;
	return ok;
}

static int gcd(int a, int b)
{
	while (b != 0) {
		int t = a % b;
		a = b;
		b = t;
	}
	return a;
}

static void close_and_remove(int fd)
{
	CHECK(PF_CloseFile(fd) == PFE_OK);
	CHECK(PF_DestroyFile(fname) == PFE_OK);
}

static void check_exact(int n)
/****************************************************************************
SPECIFICATIONS:
	Check that the current pool has exactly n frames: with a file of
	n+1 pages, n fixes succeed, the (n+1)th gives PFE_NOBUF, and after
	one unfix the same fix succeeds. Cleans up after itself.
*****************************************************************************/
{
	char *buf;
	int fd, i, nok, rc;

	if (n + 1 > MAXFIX) {
		CHECK(n + 1 <= MAXFIX);
		return;
	}
	fd = make_file(n + 1, n);
	if (fd < 0)
		return;

	nok = 0;
	for (i = 0; i < n; i++)
		if (PF_GetThisPage(fd, i, &buf) == PFE_OK)
			nok++;
	CHECK(nok == n);

	rc = PF_GetThisPage(fd, n, &buf);
	CHECK(rc == PFE_NOBUF);
	if (rc == PFE_OK)
		PF_UnfixPage(fd, n, FALSE);

	CHECK(PF_UnfixPage(fd, 0, FALSE) == PFE_OK);
	CHECK(PF_GetThisPage(fd, n, &buf) == PFE_OK);
	CHECK(verify(buf, n, n));

	nok = 0;
	for (i = 1; i <= n; i++)
		if (PF_UnfixPage(fd, i, FALSE) == PFE_OK)
			nok++;
	CHECK(nok == n);
	close_and_remove(fd);
}

static void test_init_limits(void)
{
	char *buf;
	int fd, pn;

	/* (a) below the minimum fails, the minimum works */
	CHECK(PF_InitBuffer(7) == PFE_INVALIDBUFS);
	CHECK(PF_InitBuffer(0) == PFE_INVALIDBUFS);
	CHECK(PF_InitBuffer(-1) == PFE_INVALIDBUFS);
	CHECK(PF_InitBuffer(PF_MIN_BUFS) == PFE_OK);
	CHECK(PF_InitBuffer(7) == PFE_INVALIDBUFS);
	/* a failed init keeps the old pool */
	check_exact(PF_MIN_BUFS);

	/* re-init with a page fixed, and with a page only cached, is refused */
	fd = make_file(1, 0);
	if (fd < 0)
		return;
	CHECK(PF_AllocPage(fd, &pn, &buf) == PFE_OK);
	fill(buf, pn, 0);
	CHECK(PF_InitBuffer(20) == PFE_BUFINUSE);
	CHECK(PF_UnfixPage(fd, pn, TRUE) == PFE_OK);
	CHECK(PF_InitBuffer(20) == PFE_BUFINUSE);
	/* the pool still works after the refused re-init */
	CHECK(read_page(fd, 0, 0));
	CHECK(read_page(fd, 1, 0));
	close_and_remove(fd);
	CHECK(PF_InitBuffer(20) == PFE_OK);
	check_exact(20);
}

static void test_eviction(int n)
{
	int fd, k, p, npages, step, nok;

	/* (b) 3n pages through n frames, read back in other orders */
	CHECK(PF_InitBuffer(n) == PFE_OK);
	npages = 3 * n;
	fd = make_file(npages, n);
	if (fd < 0)
		return;

	nok = 0;
	for (p = npages - 1; p >= 0; p--)
		nok += read_page(fd, p, n);
	CHECK(nok == npages);

	for (step = 7; gcd(step, npages) != 1; step++)
		;
	nok = 0;
	for (k = 0; k < npages; k++)
		nok += read_page(fd, (k * step) % npages, n);
	CHECK(nok == npages);

	/* close and reopen: the data must have reached the disk */
	CHECK(PF_CloseFile(fd) == PFE_OK);
	fd = PF_OpenFile(fname);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	nok = 0;
	for (p = 0; p < npages; p++)
		nok += read_page(fd, p, n);
	CHECK(nok == npages);
	close_and_remove(fd);

	/* (c) exactly n frames */
	check_exact(n);
}

static void test_hash(void)
{
	static char *bufs[HASH_PAGES];
	int fd, p, nok;

	/* (d) a pool big enough to keep all 2000 pages, so the hash table
	holds 2000 entries over 1031 buckets */
	CHECK(PF_InitBuffer(HASH_PAGES) == PFE_OK);
	fd = make_file(HASH_PAGES, 1);
	if (fd < 0)
		return;

	nok = 0;
	for (p = 0; p < HASH_PAGES; p++)
		nok += read_page(fd, p, 1);
	CHECK(nok == HASH_PAGES);

	/* all 2000 fixed at once */
	nok = 0;
	for (p = 0; p < HASH_PAGES; p++)
		if (PF_GetThisPage(fd, p, &bufs[p]) == PFE_OK && verify(bufs[p], p, 1))
			nok++;
	CHECK(nok == HASH_PAGES);
	nok = 0;
	for (p = HASH_PAGES - 1; p >= 0; p--)
		if (PF_UnfixPage(fd, p, FALSE) == PFE_OK)
			nok++;
	CHECK(nok == HASH_PAGES);

	/* and again from disk */
	CHECK(PF_CloseFile(fd) == PFE_OK);
	fd = PF_OpenFile(fname);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	nok = 0;
	for (p = HASH_PAGES - 1; p >= 0; p--)
		nok += read_page(fd, p, 1);
	CHECK(nok == HASH_PAGES);
	close_and_remove(fd);
}

static void test_pf_init(void)
{
	char *args[2];

	/* (e) no config: PF_Init gives the old default of 20 frames */
	Config_Load(NULL);		/* empties the config table */
	PF_Init();
	check_exact(PF_DEFAULT_BUFS);

	/* with buffer_pool_size set, PF_Init uses it */
	args[0] = "test_pool";
	args[1] = "buffer_pool_size=12";
	CHECK(Config_ParseArgs(2, args) == 1);
	PF_Init();
	check_exact(12);
	Config_Load(NULL);
}

static void test_bad_config(void)
{
	char *args[2];
	char msg[256];
	int fds[2];
	int status;
	pid_t pid;
	ssize_t n;

	/* (f) buffer_pool_size below the minimum: PF_Init prints and exits 1 */
	CHECK(pipe(fds) == 0);
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	CHECK(pid >= 0);
	if (pid < 0)
		return;
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], 2);
		args[0] = "test_pool";
		args[1] = "buffer_pool_size=4";
		Config_ParseArgs(2, args);
		PF_Init();
		_exit(0);	/* only reached if PF_Init did not exit */
	}
	close(fds[1]);
	n = read(fds[0], msg, sizeof(msg) - 1);
	msg[n > 0 ? n : 0] = '\0';
	close(fds[0]);

	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status));
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
	CHECK(strcmp(msg, "toydb: buffer_pool_size=4 is below the minimum of 8\n") == 0);
	printf("child stderr: %s", msg);
}

int main(void)
{
	PF_Init();

	test_init_limits();
	test_eviction(8);
	test_eviction(20);
	test_eviction(50);
	test_hash();
	test_pf_init();
	test_bad_config();

	unlink(fname);
	printf("test_pool: %d checks, %d failed\n", ncheck, nfail);
	if (nfail > 0)
		return 1;
	printf("test_pool done!\n");
	return 0;
}
