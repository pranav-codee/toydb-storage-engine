/* test_leftpage.c: the scans that start at the leftmost leaf (A1) and the
 * NOT_EQUAL scan (A3)
 *
 * usage: test_leftpage
 *
 * AM_OpenIndexScan starts a full scan (value NULL) and the LT, LE and NE
 * scans at the leftmost leaf, which it gets from GetLeftPageNum() in
 * amscan.c. This test builds B+ trees of different page layouts and checks
 * GetLeftPageNum() and every scan operator against the keys that were put
 * in. S4 and S5 check that a NE scan skips its value (all of its record
 * ids) wherever that key sits in the tree.
 *
 *   S0  built with AM_InsertEntry only (3000 int keys, scrambled order).
 *       Here the leftmost leaf is page 2: the first root split puts the
 *       right half in page 1 and the copy of the left half in page 2.
 *   S1  bulk-load layout, height 2: leaves in pages 1..4, root in page 0
 *       (the layout PLAN.md gives for the bulk loader, A2). Leftmost = 1.
 *       After the scans, 200 keys below the minimum are inserted with
 *       AM_InsertEntry (they split the leftmost leaf) and the full scan is
 *       run again.
 *   S2  bulk-load layout, height 3: leaves in pages 1..6, internal nodes
 *       in 7 and 8, root in page 0. Leftmost = 1.
 *   S3  internal nodes allocated before the leaves: internal nodes in
 *       pages 1 and 2, leaves in 3..8, root in page 0. Leftmost = 3, and
 *       page 2 is an internal node.
 *   S4  NE scans on the S1 layout with the excluded key (a) in the middle
 *       of a leaf, (b) the last key of a leaf that has a next leaf, (c) the
 *       first key of a leaf that is not the first, (d) the smallest and
 *       the largest key. Each case gets its own freshly built index, so a
 *       case that leaves a page fixed cannot spoil the next one.
 *   S5  the same five cases, with the excluded key holding 3 record ids
 *       (the AM keeps duplicates as one key with a list of record ids);
 *       every one of them must be skipped. EQ on the key must return all 3.
 *
 * S1-S5 are written page by page with the AM's own page code
 * (AM_InsertintoLeaf for leaves, AM_FillRootPage and AM_AddtoIntPage for
 * internal nodes), on a leaf header copied from the page 0 that
 * AM_CreateIndex made, so the pages are in exactly the AM format.
 *
 * Key i is 10*i and its record id is i (extra record ids of a duplicated
 * key are numbered after those). Every scan's record ids are checked
 * against the expected set: each one exactly once, nothing else, and in
 * key order for the full, LT and LE scans. After the scans the index file
 * is closed with PF_CloseFile, which fails if any page of it is still
 * fixed, so a page left fixed by a scan fails the test.
 *
 * Each scenario runs in a child process with a time limit, so a crash or
 * an endless scan is reported as a failure of that scenario. The last
 * line is "test_leftpage done!" only if every check passed. make test runs
 * this from a scratch dir; the index files are made in the cwd.
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "pf.h"
#include "am.h"

/*
 * No <stdlib.h>: am.h declares malloc and calloc the K&R way, which
 * conflicts with the prototypes there.
 *
 * Stopgap, as in tests/test_policy.c: the PF and AM functions below are
 * K&R definitions with no prototype in any header. Parameters declared
 * char or short in the K&R definitions are passed as int (default
 * argument promotion), so int here matches them.
 */
extern int PF_CreateFile(char *fname);
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_AllocPage(int fd, int *pagenum, char **pagebuf);
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
extern int GetLeftPageNum(int fileDesc);
/* AM page helpers (aminsert.c, am.c) */
extern int AM_InsertintoLeaf(char *pageBuf, int attrLength, char *value,
			     int recId, int index, int status);
extern int AM_FillRootPage(char *pageBuf, int pageNum1, int pageNum2,
			   char *value, int attrLength, int maxKeys);
extern int AM_AddtoIntPage(char *pageBuf, char *value, int pageNum,
			   AM_INTHEADER *header, int offset);

static int nfail = 0;		/* failed checks (in this process) */
static int ncheck = 0;		/* all checks (in this process) */

#define CHECK(cond) do { \
		ncheck++; \
		if (!(cond)) { \
			nfail++; \
			printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define RELNAME		"leftpage"	/* index files leftpage.<n> */
#define KEYLEN		((int)sizeof(int))
#define MAXKEYS		3200		/* most keys in any scenario */
#define TIMELIMIT	20		/* seconds per scenario */
#define NSCEN		6		/* scenarios S0..S5 */

/* ---------------- expected set and scan checking ---------------- */

/*
 * The (key, record id) pairs in the index: rid_by_pos[p] is the record id
 * of the p-th smallest key, nkeys of them. Record ids are 0..nkeys-1, each
 * used once; a key can have several.
 */
static int rid_by_pos[MAXKEYS];
static int key_of[MAXKEYS];		/* key of record id rid */
static int nkeys;

static void
add_key(int key, int rid)
/* Remember one (key, rid) pair. Call sort_by_key() afterwards if they
   were not added in key order. */
{
	rid_by_pos[nkeys] = rid;
	key_of[rid] = key;
	nkeys++;
}

static void
sort_by_key(void)
/* Put rid_by_pos in key order after pairs were added out of order (S0,
   S1 after its extra inserts, S5). Insertion sort, small n. */
{
	int i, j, r;

	for (i = 1; i < nkeys; i++) {
		r = rid_by_pos[i];
		for (j = i; j > 0 && key_of[rid_by_pos[j - 1]] > key_of[r]; j--)
			rid_by_pos[j] = rid_by_pos[j - 1];
		rid_by_pos[j] = r;
	}
}

static int
wanted(int op, int key, int value)
/* Should a key be returned by a scan with operator op and this value? */
{
	switch (op) {
	case ALL:			return 1;
	case EQUAL:			return key == value;
	case LESS_THAN:			return key < value;
	case GREATER_THAN:		return key > value;
	case LESS_THAN_EQUAL:		return key <= value;
	case GREATER_THAN_EQUAL:	return key >= value;
	case NOT_EQUAL:			return key != value;
	}
	return 0;
}

static const char *
opname(int op)
{
	switch (op) {
	case ALL:			return "full";
	case EQUAL:			return "EQ";
	case LESS_THAN:			return "LT";
	case GREATER_THAN:		return "GT";
	case LESS_THAN_EQUAL:		return "LE";
	case GREATER_THAN_EQUAL:	return "GE";
	case NOT_EQUAL:			return "NE";
	}
	return "?";
}

static void
check_scan(const char *sc, int fd, int op, int value)
/****************************************************************************
SPECIFICATIONS:
	Run one scan (op ALL means value NULL) to the end and compare its
	record ids with the keys that should match: every one exactly once,
	no others, and for full, LT and LE scans in key order. At most
	2*nkeys+10 results are read, so a scan that never ends is cut off
	and counted as a failure. Prints one line per scan.
*****************************************************************************/
{
	char seen[MAXKEYS];
	int sd, rid, got, want, missing, dup, bad, unordered, lastkey, i;
	int ordered, eof, firstmiss, endcode, first;

	ordered = (op == ALL || op == LESS_THAN || op == LESS_THAN_EQUAL);
	memset(seen, 0, sizeof(seen));
	sd = AM_OpenIndexScan(fd, 'i', KEYLEN, op,
			      op == ALL ? NULL : (char *)&value);
	CHECK(sd >= 0);
	if (sd < 0) {
		printf("%s %s %d: AM_OpenIndexScan failed (%d)\n", sc,
		       opname(op), value, sd);
		return;
	}

	got = dup = bad = unordered = 0;
	first = 1;
	lastkey = 0;
	eof = 0;
	endcode = 0;		/* 0: cut off after 2*nkeys+10 results */
	while (got < 2 * nkeys + 10) {
		rid = AM_FindNextEntry(sd);
		if (rid < 0) {
			endcode = rid;
			eof = (rid == AME_EOF);
			break;
		}
		got++;
		if (rid >= nkeys || !wanted(op, key_of[rid], value)) {
			bad++;		/* garbage, or a key that must not match */
			continue;
		}
		if (seen[rid])
			dup++;
		seen[rid] = 1;
		/* keys must not go down; the record ids of one key may come
		   in any order */
		if (ordered) {
			if (!first && key_of[rid] < lastkey)
				unordered++;
			lastkey = key_of[rid];
			first = 0;
		}
	}

	want = missing = 0;
	firstmiss = -1;
	for (i = 0; i < nkeys; i++) {
		if (!wanted(op, key_of[rid_by_pos[i]], value))
			continue;
		want++;
		if (!seen[rid_by_pos[i]]) {
			if (missing == 0)
				firstmiss = key_of[rid_by_pos[i]];
			missing++;
		}
	}

	printf("%s %-4s %s%-6d want %4d got %4d  missing %4d  dup %d  "
	       "wrong %d  out-of-order %d", sc, opname(op),
	       op == ALL ? "" : "v=", op == ALL ? 0 : value, want, got,
	       missing, dup, bad, unordered);
	if (!eof && endcode < 0)
		printf("  (ended with error %d, AM_Errno %d)", endcode,
		       AM_Errno);
	else if (!eof)
		printf("  (cut off, no EOF)");
	if (missing > 0)
		printf("  first missing key %d", firstmiss);
	printf("\n");

	CHECK(eof);
	CHECK(missing == 0);
	CHECK(dup == 0);
	CHECK(bad == 0);
	CHECK(unordered == 0);
	CHECK(got == want);
	CHECK(AM_CloseIndexScan(sd) == AME_OK);
}

static void
check_all_scans(const char *sc, int fd)
/* Every operator, with a value in the middle of the key range that is
   itself a key (so EQ finds it and NE must skip it). */
{
	int v = key_of[rid_by_pos[nkeys / 2]];

	check_scan(sc, fd, ALL, 0);
	check_scan(sc, fd, LESS_THAN, v);
	check_scan(sc, fd, LESS_THAN_EQUAL, v);
	check_scan(sc, fd, NOT_EQUAL, v);
	check_scan(sc, fd, EQUAL, v);
	check_scan(sc, fd, GREATER_THAN, v);
	check_scan(sc, fd, GREATER_THAN_EQUAL, v);
}

/* ---------------- building the trees ---------------- */

static AM_LEAFHEADER tmpl;	/* empty leaf header from AM_CreateIndex */

static int
open_new_index(int no)
/* Create index RELNAME.no (removing an old one) and open it; also keep
   the empty-leaf header of its page 0 in tmpl. */
{
	char fname[64], *buf;
	int fd;

	sprintf(fname, "%s.%d", RELNAME, no);
	unlink(fname);			/* PF_CreateFile uses O_EXCL */
	CHECK(AM_CreateIndex(RELNAME, no, 'i', KEYLEN) == AME_OK);
	fd = PF_OpenFile(fname);
	CHECK(fd >= 0);
	if (fd < 0)
		return -1;
	CHECK(PF_GetThisPage(fd, 0, &buf) == PFE_OK);
	memcpy(&tmpl, buf, sizeof(tmpl));
	CHECK(PF_UnfixPage(fd, 0, FALSE) == PFE_OK);
	return fd;
}

static int
reopen(int fd, int no)
/* Close (asserting no page is left fixed) and open the index again. */
{
	char fname[64];

	sprintf(fname, "%s.%d", RELNAME, no);
	CHECK(PF_CloseFile(fd) == PFE_OK);
	fd = PF_OpenFile(fname);
	CHECK(fd >= 0);
	return fd;
}

/*
 * Duplicates for fill_leaf: the key dupkey (if dup_copies > 0) gets
 * dup_copies extra record ids, numbered from dup_rid on.
 */
static int dupkey, dup_copies, dup_rid;

static void
fill_leaf(int fd, int page, int next, int first, int count)
/****************************************************************************
SPECIFICATIONS:
	Write leaf "page" holding keys 10*first .. 10*(first+count-1)
	(record ids first ..), next leaf "next" (AM_NULL_PAGE for the last),
	and add the keys to the expected set. The key dupkey also gets
	dup_copies more record ids, added to its list the way the AM adds
	a record id to a key that is already there (status AM_FOUND).
*****************************************************************************/
{
	AM_LEAFHEADER h;
	char *buf;
	int i, k, key;

	CHECK(PF_GetThisPage(fd, page, &buf) == PFE_OK);
	memset(buf, 0, PF_PAGE_SIZE);
	h = tmpl;
	h.nextLeafPage = next;
	memcpy(buf, &h, sizeof(h));
	for (i = 0; i < count; i++) {
		key = 10 * (first + i);
		CHECK(AM_InsertintoLeaf(buf, KEYLEN, (char *)&key, first + i,
					i + 1, AM_NOT_FOUND) == TRUE);
		add_key(key, first + i);
		if (dup_copies > 0 && key == dupkey)
			for (k = 0; k < dup_copies; k++) {
				CHECK(AM_InsertintoLeaf(buf, KEYLEN,
					(char *)&key, dup_rid, i + 1,
					AM_FOUND) == TRUE);
				add_key(key, dup_rid);
				dup_rid++;
			}
	}
	CHECK(PF_UnfixPage(fd, page, TRUE) == PFE_OK);
}

static void
fill_int(int fd, int page, int n, const int *child, const int *minkey)
/****************************************************************************
SPECIFICATIONS:
	Write internal node "page" with n >= 2 children child[0..n-1]. The
	key between child i-1 and child i is minkey[i], the smallest key
	under child i (AM_BinSearch sends a key equal to a separator to
	the right).
*****************************************************************************/
{
	AM_INTHEADER h;
	char *buf;
	int i;

	CHECK(PF_GetThisPage(fd, page, &buf) == PFE_OK);
	memset(buf, 0, PF_PAGE_SIZE);
	AM_FillRootPage(buf, child[0], child[1], (char *)&minkey[1], KEYLEN,
			tmpl.maxKeys);
	for (i = 2; i < n; i++) {
		memcpy(&h, buf, sizeof(h));
		AM_AddtoIntPage(buf, (char *)&minkey[i], child[i], &h, i - 1);
		memcpy(buf, &h, sizeof(h));
	}
	CHECK(PF_UnfixPage(fd, page, TRUE) == PFE_OK);
}

static void
alloc_pages(int fd, int n, int firstwant)
/* Allocate n new pages, which must come out as firstwant, firstwant+1.. */
{
	char *buf;
	int i, pn;

	for (i = 0; i < n; i++) {
		CHECK(PF_AllocPage(fd, &pn, &buf) == PFE_OK);
		CHECK(pn == firstwant + i);
		CHECK(PF_UnfixPage(fd, pn, FALSE) == PFE_OK);
	}
}

/* ---------------- scenarios ---------------- */

static void
s0(void)
{
	int fd, i, p, key;

	fd = open_new_index(0);
	if (fd < 0)
		return;
	nkeys = 0;
	for (i = 0; i < 3000; i++) {
		p = (int)((i * 1237L) % 3000);	/* 1237 is prime to 3000 */
		key = 10 * p;
		CHECK(AM_InsertEntry(fd, 'i', KEYLEN, (char *)&key, p) == AME_OK);
		key_of[p] = key;
		rid_by_pos[nkeys++] = p;
	}
	sort_by_key();
	fd = reopen(fd, 0);
	printf("S0 GetLeftPageNum = %d (leftmost leaf is page 2)\n",
	       GetLeftPageNum(fd));
	CHECK(GetLeftPageNum(fd) == 2);
	check_all_scans("S0", fd);
	CHECK(PF_CloseFile(fd) == PFE_OK);
	CHECK(AM_DestroyIndex(RELNAME, 0) == AME_OK);
}

static void
s1(void)
{
	int child[4] = { 1, 2, 3, 4 };
	int minkey[4] = { 0, 250, 500, 750 };
	int fd, i, key;

	fd = open_new_index(1);
	if (fd < 0)
		return;
	nkeys = 0;
	alloc_pages(fd, 4, 1);			/* leaves 1..4 */
	for (i = 0; i < 4; i++)
		fill_leaf(fd, 1 + i, i < 3 ? 2 + i : AM_NULL_PAGE, 25 * i, 25);
	fill_int(fd, 0, 4, child, minkey);	/* root over the leaves */
	fd = reopen(fd, 1);
	printf("S1 GetLeftPageNum = %d (leftmost leaf is page 1)\n",
	       GetLeftPageNum(fd));
	CHECK(GetLeftPageNum(fd) == 1);
	check_all_scans("S1", fd);

	/* keys below the minimum all go to the leftmost leaf and split it;
	   the left half stays in page 1, so the full scan must still start
	   there and see everything */
	for (i = 0; i < 200; i++) {
		key = -10 * (i + 1);
		CHECK(AM_InsertEntry(fd, 'i', KEYLEN, (char *)&key, nkeys) ==
		      AME_OK);
		key_of[nkeys] = key;
		rid_by_pos[nkeys] = nkeys;
		nkeys++;
	}
	sort_by_key();
	fd = reopen(fd, 1);
	check_scan("S1+", fd, ALL, 0);
	CHECK(PF_CloseFile(fd) == PFE_OK);
	CHECK(AM_DestroyIndex(RELNAME, 1) == AME_OK);
}

static void
s23(int no, int internal_first)
/* Height 3: six leaves of 20 keys under two internal nodes of three. */
{
	int leaf[6], inner[2], c[3], mk[3], top[2], topmin[2];
	int fd, i, j, firstleaf;
	char sc[4];

	sprintf(sc, "S%d", no);
	fd = open_new_index(no);
	if (fd < 0)
		return;
	nkeys = 0;
	if (internal_first) {
		alloc_pages(fd, 2, 1);		/* internal 1, 2 */
		alloc_pages(fd, 6, 3);		/* leaves 3..8 */
		inner[0] = 1;
		inner[1] = 2;
		firstleaf = 3;
	} else {
		alloc_pages(fd, 6, 1);		/* leaves 1..6 */
		alloc_pages(fd, 2, 7);		/* internal 7, 8 */
		inner[0] = 7;
		inner[1] = 8;
		firstleaf = 1;
	}
	for (i = 0; i < 6; i++)
		leaf[i] = firstleaf + i;
	for (i = 0; i < 6; i++)
		fill_leaf(fd, leaf[i], i < 5 ? leaf[i + 1] : AM_NULL_PAGE,
			  20 * i, 20);
	for (j = 0; j < 2; j++) {
		for (i = 0; i < 3; i++) {
			c[i] = leaf[3 * j + i];
			mk[i] = 10 * 20 * (3 * j + i);
		}
		fill_int(fd, inner[j], 3, c, mk);
		top[j] = inner[j];
		topmin[j] = mk[0];
	}
	fill_int(fd, 0, 2, top, topmin);	/* root */
	fd = reopen(fd, no);
	printf("%s GetLeftPageNum = %d (leftmost leaf is page %d)\n", sc,
	       GetLeftPageNum(fd), firstleaf);
	CHECK(GetLeftPageNum(fd) == firstleaf);
	check_all_scans(sc, fd);
	CHECK(PF_CloseFile(fd) == PFE_OK);
	CHECK(AM_DestroyIndex(RELNAME, no) == AME_OK);
}

/*
 * The NE cases of S4 and S5, on the S1 layout (4 leaves of 25 keys,
 * leaf j holds 250*j .. 250*j+240). The value of each case is a key.
 */
#define NNE	5
static const int ne_value[NNE] = { 370, 490, 500, 0, 990 };
static const char *ne_case[NNE] = {
	"(a) middle of leaf 2",
	"(b) last of leaf 2",
	"(c) first of leaf 3",
	"(d) minimum",
	"(d) maximum",
};

static void
s45(int no, int copies)
/****************************************************************************
SPECIFICATIONS:
	For each NE case: build a fresh index (number 10*no + case) in the
	S1 layout, where the case's value has "copies" extra record ids, run
	the NE scan on it, close the file (which fails if the scan left a
	page fixed) and destroy it. With copies > 0 an EQ scan on the
	value must also return all of its record ids.
*****************************************************************************/
{
	int child[4] = { 1, 2, 3, 4 };
	int minkey[4] = { 0, 250, 500, 750 };
	char sc[64];
	int c, i, fd, idx;

	for (c = 0; c < NNE; c++) {
		idx = 10 * no + c;
		sprintf(sc, "S%d %s", no, ne_case[c]);
		fd = open_new_index(idx);
		if (fd < 0)
			return;
		nkeys = 0;
		dupkey = ne_value[c];
		dup_copies = copies;
		dup_rid = 100;			/* after the 100 keys' own */
		alloc_pages(fd, 4, 1);
		for (i = 0; i < 4; i++)
			fill_leaf(fd, 1 + i, i < 3 ? 2 + i : AM_NULL_PAGE,
				  25 * i, 25);
		fill_int(fd, 0, 4, child, minkey);
		dup_copies = 0;
		sort_by_key();
		fd = reopen(fd, idx);
		if (copies > 0)
			check_scan(sc, fd, EQUAL, ne_value[c]);
		check_scan(sc, fd, NOT_EQUAL, ne_value[c]);
		CHECK(PF_CloseFile(fd) == PFE_OK);
		CHECK(AM_DestroyIndex(RELNAME, idx) == AME_OK);
	}
}

static int
run(int no)
/****************************************************************************
SPECIFICATIONS:
	Run scenario "no" in a child process with a time limit of
	TIMELIMIT seconds.

RETURN VALUE:
	1 if the scenario passed, 0 if a check failed, it crashed or it
	ran out of time (each reported on stdout).
*****************************************************************************/
{
	char fname[64];
	pid_t pid;
	int status, i;

	fflush(stdout);
	pid = fork();
	if (pid < 0) {
		printf("S%d: fork failed\n", no);
		return 0;
	}
	if (pid == 0) {
		alarm(TIMELIMIT);
		if (no == 0)
			s0();
		else if (no == 1)
			s1();
		else if (no <= 3)
			s23(no, no == 3);
		else
			s45(no, no == 5 ? 2 : 0);	/* S5: 3 rids */
		printf("S%d: %d checks, %d failed\n", no, ncheck, nfail);
		fflush(stdout);
		_exit(nfail > 0 ? 1 : 0);
	}
	if (waitpid(pid, &status, 0) != pid) {
		printf("S%d: waitpid failed\n", no);
		return 0;
	}
	/* files left behind if the child died or could not close them */
	sprintf(fname, "%s.%d", RELNAME, no);
	unlink(fname);
	for (i = 0; i < NNE; i++) {
		sprintf(fname, "%s.%d", RELNAME, 10 * no + i);
		unlink(fname);
	}
	if (WIFSIGNALED(status)) {
		printf("S%d: FAILED, killed by signal %d (%s)\n", no,
		       WTERMSIG(status), WTERMSIG(status) == SIGALRM ?
		       "time limit, scan never ended" : "crash");
		return 0;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int
main(void)
{
	int no, passed;

	PF_Init();
	passed = 0;
	for (no = 0; no < NSCEN; no++)
		passed += run(no);
	printf("test_leftpage: %d of %d scenarios passed\n", passed, NSCEN);
	if (passed != NSCEN)
		return 1;
	printf("test_leftpage done!\n");
	return 0;
}
