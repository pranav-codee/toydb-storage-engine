/* test_policy.c: tests for the per-file replacement policy (P3) and the
 * PF_OpenFile wrapper with a default policy from the config (P4).
 *
 * usage: test_policy
 *
 * Uses two scratch paged files, test_policy_x.pf and test_policy_y.pf, in
 * the current directory (make test runs this from a scratch dir). They are
 * deleted before they are created (PF_CreateFile uses O_EXCL) and at the end.
 * The pool is set to 8 frames with PF_InitBuffer(8).
 * The last output line is "test_policy done!" only if every check passed.
 *
 * Recency in buf.c: a page goes to the head of the used list when it is
 * brought into the buffer and every time it is unfixed. A fix that hits
 * does not move it. LRU evicts the unfixed page nearest the tail, MRU the
 * first unfixed page from the head. The expected values below were worked
 * out by hand from these rules.
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

static char xname[] = "test_policy_x.pf";	/* scratch file X */
static char yname[] = "test_policy_y.pf";	/* scratch file Y */

#define NPAGES	10	/* pages 0..9 in each scratch file */
#define NBUFS	8	/* pool size for every scenario */

static void make_file(char *fname)
/****************************************************************************
SPECIFICATIONS:
	Create fname with pages 0..NPAGES-1, page i holding the int i at
	the start, each unfixed clean, then close it. PF_AllocPage marks
	a new page dirty itself, so the pages still reach the disk on close.
	Closing takes the pages out of the pool.
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

static int fix(int fd, int page)
/****************************************************************************
SPECIFICATIONS:
	Fix one page and check it holds its own page number.

RETURN VALUE:
	1 if the fix worked and the data matched, 0 otherwise.
*****************************************************************************/
{
	char *buf;
	int p;

	if (PF_GetThisPage(fd, page, &buf) != PFE_OK)
		return 0;
	memcpy(&p, buf, sizeof(int));
	return p == page;
}

static int unfix(int fd, int page)
{
	return PF_UnfixPage(fd, page, FALSE) == PFE_OK;
}

static int fix_unfix_range(int fd, int from, int to)
/****************************************************************************
SPECIFICATIONS:
	Fix then unfix pages from..to in order.

RETURN VALUE:
	1 if every fix and unfix worked, 0 otherwise.
*****************************************************************************/
{
	int p, ok;

	ok = 1;
	for (p = from; p <= to; p++)
		if (!fix(fd, p) || !unfix(fd, p))
			ok = 0;
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

static void test_mru(int fd)
{
	/*
	 * Pool empty. Fix/unfix 0..7: list from the head 7 6 5 4 3 2 1 0.
	 * Fix 8 (MRU): first unfixed from the head is 7, so 7 goes.
	 * Resident {0..6, 8}.
	 */
	CHECK(fix_unfix_range(fd, 0, 7));
	CHECK(resident_is(fd, "1111111100"));
	CHECK(fix(fd, 8));
	CHECK(!PFbufResident(fd, 7));
	CHECK(resident_is(fd, "1111111010"));
	/*
	 * Unfix 8: head is now 8 6 5 4 3 2 1 0.
	 * Fix 9 (MRU): first unfixed from the head is 8, so 8 goes.
	 * Resident {0..6, 9}.
	 */
	CHECK(unfix(fd, 8));
	CHECK(fix(fd, 9));
	CHECK(!PFbufResident(fd, 8));
	CHECK(resident_is(fd, "1111111001"));
	CHECK(unfix(fd, 9));
}

static void test_lru(int fd)
{
	/*
	 * Pool empty. Fix/unfix 0..7: list from the head 7 6 5 4 3 2 1 0.
	 * Fix 8 (LRU): unfixed page nearest the tail is 0, so 0 goes.
	 * Resident {1..8}.
	 */
	CHECK(fix_unfix_range(fd, 0, 7));
	CHECK(resident_is(fd, "1111111100"));
	CHECK(fix(fd, 8));
	CHECK(!PFbufResident(fd, 0));
	CHECK(resident_is(fd, "0111111110"));
	/*
	 * Unfix 8: 8 7 6 5 4 3 2 1.
	 * Fix 1 is a hit (no move), unfix 1 moves it to the head:
	 * 1 8 7 6 5 4 3 2.
	 * Fix 9 (LRU): tail is 2, so 2 goes. Resident {1,3,4,5,6,7,8,9}.
	 */
	CHECK(unfix(fd, 8));
	CHECK(fix(fd, 1));
	CHECK(unfix(fd, 1));
	CHECK(resident_is(fd, "0111111110"));
	CHECK(fix(fd, 9));
	CHECK(!PFbufResident(fd, 2));
	CHECK(resident_is(fd, "0101111111"));
	CHECK(unfix(fd, 9));
}

static void run_one(char *fname, int policy, int use_default)
/****************************************************************************
SPECIFICATIONS:
	Reset the pool to NBUFS frames, open fname with PF_OpenFileEx
	(policy) or, if use_default, with PF_OpenFile, run the scenario of
	"policy" and close the file.
*****************************************************************************/
{
	int fd;

	CHECK(PF_InitBuffer(NBUFS) == PFE_OK);
	fd = use_default ? PF_OpenFile(fname) : PF_OpenFileEx(fname, policy);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	if (policy == PF_MRU)
		test_mru(fd);
	else
		test_lru(fd);
	CHECK(PF_CloseFile(fd) == PFE_OK);
}

static void test_mixed(void)
{
	int fx, fy;

	CHECK(PF_InitBuffer(NBUFS) == PFE_OK);
	fx = PF_OpenFileEx(xname, PF_LRU);
	fy = PF_OpenFileEx(yname, PF_MRU);
	CHECK(fx >= 0);
	CHECK(fy >= 0);
	if (fx < 0 || fy < 0)
		return;

	/*
	 * Fix/unfix X0..X7: list from the head X7 X6 ... X0, pool full.
	 * Fix Y0: Y is MRU, first unfixed from the head is X7, so X7 goes
	 * (a page of the other file). Resident X{0..6}, Y{0}.
	 */
	CHECK(fix_unfix_range(fx, 0, 7));
	CHECK(fix(fy, 0));
	CHECK(!PFbufResident(fx, 7));
	CHECK(resident_is(fx, "1111111000"));
	CHECK(resident_is(fy, "1000000000"));
	/*
	 * Unfix Y0: Y0 X6 X5 X4 X3 X2 X1 X0.
	 * Fix X8: X is LRU, the tail is X0, so X0 goes.
	 * Resident X{1..6, 8}, Y{0}.
	 */
	CHECK(unfix(fy, 0));
	CHECK(fix(fx, 8));
	CHECK(!PFbufResident(fx, 0));
	CHECK(resident_is(fx, "0111111010"));
	CHECK(resident_is(fy, "1000000000"));
	CHECK(unfix(fx, 8));

	CHECK(PF_CloseFile(fx) == PFE_OK);
	CHECK(PF_CloseFile(fy) == PFE_OK);
}

static void test_default(void)
{
	char *args[2];
	int fd;

	/* no config: PF_OpenFile gives LRU */
	Config_Load(NULL);		/* empties the config table */
	PF_Init();
	run_one(xname, PF_LRU, 1);

	/* default_policy=mru (any case): PF_OpenFile gives MRU */
	args[0] = "test_policy";
	args[1] = "default_policy=mru";
	CHECK(Config_ParseArgs(2, args) == 1);
	PF_Init();
	run_one(xname, PF_MRU, 1);
	Config_Load(NULL);
	PF_Init();

	/* a bad policy is refused and takes no file table entry */
	fd = PF_OpenFileEx(xname, 99);
	CHECK(fd == PFE_INVALIDPOLICY);
	CHECK(PFerrno == PFE_INVALIDPOLICY);
	fd = PF_OpenFileEx(xname, -1);
	CHECK(fd == PFE_INVALIDPOLICY);
	fd = PF_OpenFileEx(xname, PF_LRU);
	CHECK(fd >= 0);
	if (fd >= 0)
		CHECK(PF_CloseFile(fd) == PFE_OK);
}

static void test_bad_config(void)
{
	char *args[2];
	char msg[256];
	int fds[2];
	int status;
	pid_t pid;
	ssize_t n;

	/* default_policy not LRU/MRU: PF_Init prints and exits 1 */
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
		args[0] = "test_policy";
		args[1] = "default_policy=FIFO";
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
	CHECK(strcmp(msg, "toydb: default_policy=FIFO must be LRU or MRU\n") == 0);
	printf("child stderr: %s", msg);
}

int main(void)
{
	PF_Init();
	CHECK(PF_InitBuffer(NBUFS) == PFE_OK);
	make_file(xname);
	make_file(yname);

	run_one(xname, PF_MRU, 0);
	run_one(xname, PF_LRU, 0);
	test_mixed();
	test_default();
	test_bad_config();

	CHECK(PF_DestroyFile(xname) == PFE_OK);
	CHECK(PF_DestroyFile(yname) == PFE_OK);
	printf("test_policy: %d checks, %d failed\n", ncheck, nfail);
	if (nfail > 0)
		return 1;
	printf("test_policy done!\n");
	return 0;
}
