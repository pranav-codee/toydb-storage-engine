/* stress_pool.c: randomized model-based stress test of the PF buffer pool
 * (P1, P3, P5, P6).
 *
 * usage: stress_pool [seed] [nops]	(defaults: seed 1, nops 200000)
 *
 * Runs nops random PF calls and, next to them, an independent model of the
 * buffer pool, and checks after every call that the return code is the one
 * the model predicts. Every 1000 calls it also checks which pages are
 * resident (PFbufResident) and all eight counters of both files and of the
 * global totals (PF_GetStats). At the end every page is read back from disk
 * and compared with the model's disk image. A run is fully determined by
 * the seed (own xorshift generator, not rand()).
 * On the first mismatch it prints the step, the operation, the expected and
 * actual values and the last 30 operations, and exits with 1. The last
 * output line is "stress_pool done!" only if every step agreed.
 *
 * Setup from the seed: pool of 8..32 frames (PF_InitBuffer), two files of
 * 40 pages, stress_pool_a.pf and stress_pool_b.pf in the current directory
 * (deleted first, destroyed at the end), each opened PF_LRU or PF_MRU at
 * random. Each page body starts with three ints (file, page, version).
 *
 * Where the model comes from:
 * - From the documented rules (README "Buffer manager", the spec comments
 *   of PF_OpenFileEx, PF_MarkDirty, PF_GetStats, PF_FlushAll and
 *   PF_CloseFile in pf.c), written without looking at how buf.c does it:
 *   used list order (head = last loaded or unfixed, a hit does not move a
 *   page); victim = first unfixed page from the tail (LRU) or the head
 *   (MRU), by the policy of the file whose page comes in; PFE_NOBUF when
 *   every frame is fixed; dirty flag (set by unfix TRUE and PF_MarkDirty,
 *   cleared by a write-back); memory and disk version of every page (a
 *   clean eviction or close drops the memory version, a dirty eviction, a
 *   close and PF_FlushAll copy it to disk, so a change that was never
 *   marked dirty is lost when its page leaves the pool clean); the eight
 *   counters with the attribution rules of PF_GetStats; zeroed per-file
 *   counters on open; the order of the argument checks (fd, then page
 *   number, then buffer state).
 * - From observed behaviour, ONLY the side effect of a failed close: the
 *   docs say closing a file with a fixed page is an error (PFE_PAGEFIXED),
 *   but not that PFbufReleaseFile (buf.c) walks the used list from the
 *   head and has already written back (if dirty) and released every
 *   unfixed page of that file that comes before the first fixed one. The
 *   model copies that so the rest of the run can be checked; it is not a
 *   documented rule.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include "pf.h"

/*
 * Stopgap: pf.h does not declare these PF functions yet (they are K&R
 * definitions in pf.c), and -Wall warns about implicit declarations.
 * PFbufResident is a debug helper in buf.c, not part of pf.h.
 */
extern int PF_CreateFile(char *fname);
extern int PF_DestroyFile(char *fname);
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_AllocPage(int fd, int *pagenum, char **pagebuf);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);
extern int PFbufResident(int fd, int pagenum);

#define NFILES		2	/* files in the test */
#define NPAGES		40	/* pages per file */
#define MINPOOL		8	/* pool size range */
#define MAXPOOL		32
#define FTAB_SIZE	20	/* PF_FTAB_SIZE in pftypes.h, an invalid fd */
#define NRING		30	/* operations shown on a mismatch */
#define CHECK_EVERY	1000	/* full state check interval */
#define PHASE_LEN	500	/* ops between changes of the fix target */

/* one frame of the model's used list, used[0] is the head */
typedef struct {
	int f, p;	/* file index (0/1) and page number */
	int fixed;
	int dirty;
	int lw;		/* logical write counted since the last fix */
	char *buf;	/* page data pointer from PF_GetThisPage */
} Mframe;

static Mframe used[MAXPOOL];
static int nused;		/* frames in the used list */
static int npool;		/* pool size */
static int fds[NFILES];		/* PF fd of each file */
static int pol[NFILES];		/* policy of each file */
static int memver[NFILES][NPAGES];	/* version in the frame (if resident) */
static int diskver[NFILES][NPAGES];	/* version on disk */
static int vcounter;		/* last version handed out */
static PFstats mst[NFILES];	/* model per-file counters */
static PFstats mglob;		/* model global counters */

static char fname_a[] = "stress_pool_a.pf";
static char fname_b[] = "stress_pool_b.pf";
static char *fnames[NFILES] = { fname_a, fname_b };

#define MINC(f,fld) (mst[f].fld++, mglob.fld++)

/* coverage counters, printed at the end */
enum { C_HIT, C_MISSFREE, C_MISSEVICT, C_FIXFIXED, C_NOBUF, C_UNFIXOK,
	C_UNFIXUNFIXED, C_UNFIXNOTIN, C_MARKOK, C_MARKNOTIN, C_FLUSH,
	C_MODMARKED, C_MODUNMARKED, C_LOST, C_DIRTYEVICT, C_CLOSEOK,
	C_CLOSEFIXED, C_PARTIAL, C_MISUSE, C_NCOV };
static const char *covname[C_NCOV] = { "fix hit", "fix miss (free frame)",
	"fix miss (eviction)", "fix PFE_PAGEFIXED", "fix PFE_NOBUF",
	"unfix ok", "unfix PFE_PAGEUNFIXED", "unfix PFE_PAGENOTINBUF",
	"markdirty ok", "markdirty PFE_PAGENOTINBUF", "flushall",
	"modify + markdirty", "modify unmarked",
	"unmarked change lost on clean eviction", "dirty eviction",
	"close ok + reopen", "close PFE_PAGEFIXED",
	"pages released by a failed close", "misuse" };
static long cov[C_NCOV];

/*************************** random numbers **************************/

static unsigned long long rng_state;

static void rng_seed(unsigned long seed)
/****************************************************************************
SPECIFICATIONS:
	Seed the generator. One splitmix64 step, so seed 0 also works and
	nearby seeds give unrelated streams.
*****************************************************************************/
{
	unsigned long long z = (unsigned long long)seed + 0x9E3779B97F4A7C15ULL;

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	rng_state = (z ^ (z >> 31)) | 1ULL;
}

static int rnd(int n)
/****************************************************************************
SPECIFICATIONS:
	xorshift64* step.

RETURN VALUE:
	a number in 0..n-1.
*****************************************************************************/
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (int)(((rng_state * 0x2545F4914F6CDD1DULL) >> 33)
			% (unsigned long long)n);
}

/*************************** reporting ********************************/

static long step;			/* current step, 0 = setup */
static char curop[128];			/* operation being checked */
static char ring[NRING][160];		/* last finished operations */
static long ringn;

static const char *codename(int code)
/****************************************************************************
RETURN VALUE:
	the PFE_ name of a PF return code, or "fd>=0" for a file descriptor.
*****************************************************************************/
{
	static const char *names[] = { "PFE_OK", "PFE_NOMEM", "PFE_NOBUF",
		"PFE_PAGEFIXED", "PFE_PAGENOTINBUF", "PFE_UNIX",
		"PFE_INCOMPLETEREAD", "PFE_INCOMPLETEWRITE", "PFE_HDRREAD",
		"PFE_HDRWRITE", "PFE_INVALIDPAGE", "PFE_FILEOPEN",
		"PFE_FTABFULL", "PFE_FD", "PFE_EOF", "PFE_PAGEFREE",
		"PFE_PAGEUNFIXED", "PFE_PAGEINBUF", "PFE_HASHNOTFOUND",
		"PFE_HASHPAGEEXIST", "PFE_INVALIDBUFS", "PFE_BUFINUSE",
		"PFE_INVALIDPOLICY" };

	if (code > 0)
		return "fd>=0";
	if (-code < (int)(sizeof names / sizeof names[0]))
		return names[-code];
	return "unknown";
}

static void setop(const char *fmt, ...)
/****************************************************************************
SPECIFICATIONS:
	Describe the operation about to be checked (printf style).
*****************************************************************************/
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(curop, sizeof curop, fmt, ap);
	va_end(ap);
}

static void finish(int code)
/****************************************************************************
SPECIFICATIONS:
	The current operation agreed with the model; add it, with the code
	it returned, to the ring of recent operations.
*****************************************************************************/
{
	snprintf(ring[ringn % NRING], sizeof ring[0], "step %ld: %s -> %s (%d)",
		step, curop, codename(code), code);
	ringn++;
}

static void fail(const char *what, long expected, long actual)
/****************************************************************************
SPECIFICATIONS:
	Report a mismatch with the model and exit(1): step, operation,
	expected and actual value, and the last NRING finished operations.
*****************************************************************************/
{
	long i;

	printf("MISMATCH at step %ld\n", step);
	printf("  operation: %s\n", curop);
	printf("  %s: expected %ld, actual %ld\n", what, expected, actual);
	printf("  last %d operations before it:\n", NRING);
	for (i = ringn > NRING ? ringn - NRING : 0; i < ringn; i++)
		printf("    %s\n", ring[i % NRING]);
	fflush(stdout);
	exit(1);
}

static void expect_code(int expected, int actual)
/****************************************************************************
SPECIFICATIONS:
	Fail unless the PF call returned the code the model predicts.
*****************************************************************************/
{
	char what[96];

	if (expected != actual) {
		snprintf(what, sizeof what, "return code (expected %s, actual %s)",
			codename(expected), codename(actual));
		fail(what, expected, actual);
	}
}

static void cmp_stats(const char *who, PFstats *exp, PFstats *act)
/****************************************************************************
SPECIFICATIONS:
	Compare all eight counters, fail on the first difference.
*****************************************************************************/
{
	char what[96];

#define CMPF(fld) \
	if (exp->fld != act->fld) { \
		snprintf(what, sizeof what, "PF_GetStats(%s).%s", who, #fld); \
		fail(what, exp->fld, act->fld); \
	}
	CMPF(logical_reads)
	CMPF(logical_writes)
	CMPF(hits)
	CMPF(misses)
	CMPF(physical_reads)
	CMPF(physical_writes)
	CMPF(evictions)
	CMPF(dirty_evictions)
#undef CMPF
}

/*************************** page bodies ******************************/

static void put_body(char *buf, int f, int p, int v)
/****************************************************************************
SPECIFICATIONS:
	Write (file, page, version) at the start of a page body.
*****************************************************************************/
{
	int b[3];

	b[0] = f;
	b[1] = p;
	b[2] = v;
	memcpy(buf, b, sizeof b);
}

static void check_body(char *buf, int f, int p, int v)
/****************************************************************************
SPECIFICATIONS:
	Fail unless the page body holds (f, p, v).
*****************************************************************************/
{
	int b[3];

	memcpy(b, buf, sizeof b);
	if (b[0] != f)
		fail("page body: file", f, b[0]);
	if (b[1] != p)
		fail("page body: page", p, b[1]);
	if (b[2] != v)
		fail("page body: version", v, b[2]);
}

/*************************** the model ********************************/

static int mfind(int f, int p)
/****************************************************************************
RETURN VALUE:
	index of page p of file f in used[], or -1 if not resident.
*****************************************************************************/
{
	int i;

	for (i = 0; i < nused; i++)
		if (used[i].f == f && used[i].p == p)
			return i;
	return -1;
}

static void mremove(int i)
/****************************************************************************
SPECIFICATIONS:
	Take entry i out of the used list (its frame becomes free).
*****************************************************************************/
{
	memmove(&used[i], &used[i + 1], (size_t)(nused - i - 1) * sizeof(Mframe));
	nused--;
}

static void minsert_head(Mframe *m)
/****************************************************************************
SPECIFICATIONS:
	Put *m at the head of the used list.
*****************************************************************************/
{
	memmove(&used[1], &used[0], (size_t)nused * sizeof(Mframe));
	used[0] = *m;
	nused++;
}

static void mto_head(int i)
/****************************************************************************
SPECIFICATIONS:
	Move entry i to the head (load or unfix makes a page most recent).
*****************************************************************************/
{
	Mframe t;

	t = used[i];
	mremove(i);
	minsert_head(&t);
}

static void mrelease(int i)
/****************************************************************************
SPECIFICATIONS:
	Release entry i because its file is being closed: a dirty page is
	written back (disk gets the memory version, one physical write of
	its file), a clean one is dropped. Not an eviction.
*****************************************************************************/
{
	if (used[i].dirty) {
		diskver[used[i].f][used[i].p] = memver[used[i].f][used[i].p];
		MINC(used[i].f, physical_writes);
	}
	mremove(i);
}

static int mvictim(int policy)
/****************************************************************************
RETURN VALUE:
	index of the victim for an incoming page of a file with "policy":
	first unfixed page from the head for MRU, from the tail for LRU;
	-1 if every frame is fixed.
*****************************************************************************/
{
	int i;

	if (policy == PF_MRU) {
		for (i = 0; i < nused; i++)
			if (!used[i].fixed)
				return i;
	} else {
		for (i = nused - 1; i >= 0; i--)
			if (!used[i].fixed)
				return i;
	}
	return -1;
}

static int nfixed(int f)
/****************************************************************************
RETURN VALUE:
	number of fixed frames of file f, or of all files if f == -1.
*****************************************************************************/
{
	int i, n = 0;

	for (i = 0; i < nused; i++)
		if (used[i].fixed && (f == -1 || used[i].f == f))
			n++;
	return n;
}

/*************************** operations *******************************/

static void op_fix(int f, int p)
/****************************************************************************
SPECIFICATIONS:
	PF_GetThisPage(f, p): predict the code, residency, order and
	counters; on success check the page body against the model.
*****************************************************************************/
{
	char *buf = NULL;
	int i, v, vf, exp, act;
	Mframe m;

	setop("fix f%d p%d", f, p);
	i = mfind(f, p);
	if (i >= 0 && used[i].fixed) {
		exp = PFE_PAGEFIXED;
		cov[C_FIXFIXED]++;
	} else if (i >= 0) {
		/* hit: no move, a new fix period */
		exp = PFE_OK;
		used[i].fixed = 1;
		used[i].lw = 0;
		MINC(f, hits);
		MINC(f, logical_reads);
		cov[C_HIT]++;
	} else {
		exp = PFE_OK;
		if (nused < npool) {
			cov[C_MISSFREE]++;
		} else if ((v = mvictim(pol[f])) < 0) {
			exp = PFE_NOBUF;
			cov[C_NOBUF]++;
		} else {
			/* evict, charged to the victim's file */
			vf = used[v].f;
			MINC(vf, evictions);
			if (used[v].dirty) {
				MINC(vf, dirty_evictions);
				MINC(vf, physical_writes);
				diskver[vf][used[v].p] = memver[vf][used[v].p];
				cov[C_DIRTYEVICT]++;
			} else if (memver[vf][used[v].p] != diskver[vf][used[v].p]) {
				cov[C_LOST]++;
			}
			mremove(v);
			cov[C_MISSEVICT]++;
		}
		if (exp == PFE_OK) {
			m.f = f;
			m.p = p;
			m.fixed = 1;
			m.dirty = 0;
			m.lw = 0;
			m.buf = NULL;
			minsert_head(&m);
			memver[f][p] = diskver[f][p];
			MINC(f, physical_reads);
			MINC(f, misses);
			MINC(f, logical_reads);
		}
	}

	act = PF_GetThisPage(fds[f], p, &buf);
	expect_code(exp, act);
	i = mfind(f, p);
	if (act == PFE_OK) {
		used[i].buf = buf;
		check_body(buf, f, p, memver[f][p]);
	} else if (act == PFE_PAGEFIXED && buf != used[i].buf) {
		fail("PFE_PAGEFIXED page pointer (1 = same as at fix)", 1, 0);
	}
	finish(act);
}

static void op_unfix(int f, int p, int dirty)
/****************************************************************************
SPECIFICATIONS:
	PF_UnfixPage(f, p, dirty).
*****************************************************************************/
{
	int i, exp, act;

	setop("unfix f%d p%d %s", f, p, dirty ? "TRUE" : "FALSE");
	i = mfind(f, p);
	if (i < 0) {
		exp = PFE_PAGENOTINBUF;
		cov[C_UNFIXNOTIN]++;
	} else if (!used[i].fixed) {
		exp = PFE_PAGEUNFIXED;
		cov[C_UNFIXUNFIXED]++;
	} else {
		exp = PFE_OK;
		if (dirty) {
			used[i].dirty = 1;
			if (!used[i].lw) {
				MINC(f, logical_writes);
				used[i].lw = 1;
			}
		}
		used[i].fixed = 0;
		used[i].buf = NULL;
		mto_head(i);
		cov[C_UNFIXOK]++;
	}
	act = PF_UnfixPage(fds[f], p, dirty);
	expect_code(exp, act);
	finish(act);
}

static void op_mark(int f, int p)
/****************************************************************************
SPECIFICATIONS:
	PF_MarkDirty(f, p): dirty bit only, no move.
*****************************************************************************/
{
	int i, exp, act;

	setop("markdirty f%d p%d", f, p);
	i = mfind(f, p);
	if (i < 0) {
		exp = PFE_PAGENOTINBUF;
		cov[C_MARKNOTIN]++;
	} else {
		exp = PFE_OK;
		used[i].dirty = 1;
		if (!used[i].lw) {
			MINC(f, logical_writes);
			used[i].lw = 1;
		}
		cov[C_MARKOK]++;
	}
	act = PF_MarkDirty(fds[f], p);
	expect_code(exp, act);
	finish(act);
}

static void op_flush(void)
/****************************************************************************
SPECIFICATIONS:
	PF_FlushAll(): every dirty page (fixed or not) goes to disk, stays
	in place, becomes clean.
*****************************************************************************/
{
	int i, act;

	setop("flushall");
	for (i = 0; i < nused; i++)
		if (used[i].dirty) {
			diskver[used[i].f][used[i].p] = memver[used[i].f][used[i].p];
			MINC(used[i].f, physical_writes);
			used[i].dirty = 0;
		}
	cov[C_FLUSH]++;
	act = PF_FlushAll();
	expect_code(PFE_OK, act);
	finish(act);
}

static void op_modify(void)
/****************************************************************************
SPECIFICATIONS:
	Change the body of a random fixed page to a new version. Half the
	time also PF_MarkDirty it; otherwise the change is only in memory
	and the model expects it to be lost if the page leaves clean.
	With no fixed page, fix a random page instead.
*****************************************************************************/
{
	int idx[MAXPOOL];
	int i, n = 0, f, p;

	for (i = 0; i < nused; i++)
		if (used[i].fixed)
			idx[n++] = i;
	if (n == 0) {
		op_fix(rnd(NFILES), rnd(NPAGES));
		return;
	}
	i = idx[rnd(n)];
	f = used[i].f;
	p = used[i].p;
	memver[f][p] = ++vcounter;
	put_body(used[i].buf, f, p, memver[f][p]);
	setop("modify f%d p%d -> v%d", f, p, memver[f][p]);
	finish(PFE_OK);
	if (rnd(2)) {
		cov[C_MODMARKED]++;
		op_mark(f, p);
	} else {
		cov[C_MODUNMARKED]++;
	}
}

static void op_close(int f, int reopen)
/****************************************************************************
SPECIFICATIONS:
	PF_CloseFile(f). With a fixed page of f: PFE_PAGEFIXED, and the
	partial release seen in buf.c PFbufReleaseFile (the only modelled
	behaviour that is not documented, see the header). Otherwise
	PFE_OK, every page of f is released; if "reopen", open it again
	with a new random policy and check its counters start at zero.
*****************************************************************************/
{
	PFstats s;
	int i, exp, act, fd, newpol;

	setop("close f%d (fd %d)", f, fds[f]);
	if (nfixed(f) > 0) {
		exp = PFE_PAGEFIXED;
		/* observed, not documented: unfixed pages of f ahead of the
		first fixed one (from the head) are released anyway */
		i = 0;
		while (i < nused) {
			if (used[i].f != f) {
				i++;
				continue;
			}
			if (used[i].fixed)
				break;
			mrelease(i);
			cov[C_PARTIAL]++;
		}
		cov[C_CLOSEFIXED]++;
		act = PF_CloseFile(fds[f]);
		expect_code(exp, act);
		finish(act);
		return;
	}

	for (i = nused - 1; i >= 0; i--)
		if (used[i].f == f)
			mrelease(i);
	act = PF_CloseFile(fds[f]);
	expect_code(PFE_OK, act);
	finish(act);
	if (!reopen)
		return;

	newpol = rnd(2) ? PF_MRU : PF_LRU;
	setop("reopen f%d %s", f, newpol == PF_MRU ? "MRU" : "LRU");
	fd = PF_OpenFileEx(fnames[f], newpol);
	if (fd < 0)
		fail("PF_OpenFileEx fd (expected >= 0)", 0, fd);
	fds[f] = fd;
	pol[f] = newpol;
	memset(&mst[f], 0, sizeof mst[f]);
	act = PF_GetStats(fd, &s);
	expect_code(PFE_OK, act);
	cmp_stats("reopened file", &mst[f], &s);
	cov[C_CLOSEOK]++;
	finish(fd);
}

static void op_misuse(void)
/****************************************************************************
SPECIFICATIONS:
	A call with an invalid fd (-1/-2/FTAB_SIZE/an unused table slot)
	or an invalid page number (-1/NPAGES); nothing changes.
*****************************************************************************/
{
	PFstats s;
	char *buf = NULL;
	int k, f, badfd, badpg, slot, exp, act;

	for (slot = 0; slot == fds[0] || slot == fds[1]; slot++)
		;
	switch (rnd(4)) {
	case 0: badfd = -1; break;
	case 1: badfd = -2; break;
	case 2: badfd = FTAB_SIZE; break;
	default: badfd = slot; break;
	}
	f = rnd(NFILES);
	badpg = rnd(2) ? -1 : NPAGES;
	k = rnd(7);
	cov[C_MISUSE]++;
	if (k < 4) {
		exp = PFE_FD;
		switch (k) {
		case 0:
			setop("misuse: fix fd %d p0", badfd);
			act = PF_GetThisPage(badfd, 0, &buf);
			break;
		case 1:
			setop("misuse: unfix fd %d p0", badfd);
			act = PF_UnfixPage(badfd, 0, rnd(2));
			break;
		case 2:
			setop("misuse: markdirty fd %d p0", badfd);
			act = PF_MarkDirty(badfd, 0);
			break;
		default:
			if (badfd == -1)	/* -1 means the totals */
				badfd = -2;
			setop("misuse: getstats fd %d", badfd);
			act = PF_GetStats(badfd, &s);
			break;
		}
	} else {
		exp = PFE_INVALIDPAGE;
		switch (k) {
		case 4:
			setop("misuse: fix f%d p%d", f, badpg);
			act = PF_GetThisPage(fds[f], badpg, &buf);
			break;
		case 5:
			setop("misuse: unfix f%d p%d", f, badpg);
			act = PF_UnfixPage(fds[f], badpg, rnd(2));
			break;
		default:
			setop("misuse: markdirty f%d p%d", f, badpg);
			act = PF_MarkDirty(fds[f], badpg);
			break;
		}
	}
	expect_code(exp, act);
	finish(act);
}

/*************************** checks ***********************************/

static void check_state(void)
/****************************************************************************
SPECIFICATIONS:
	Residency of all pages, every fixed page resident, and the
	counters of both files and the global totals.
*****************************************************************************/
{
	PFstats s;
	char who[32];
	int f, p, i, act;

	for (f = 0; f < NFILES; f++)
		for (p = 0; p < NPAGES; p++) {
			setop("check: PFbufResident f%d p%d", f, p);
			if ((PFbufResident(fds[f], p) != 0) != (mfind(f, p) >= 0))
				fail("resident", mfind(f, p) >= 0,
					PFbufResident(fds[f], p) != 0);
		}
	for (i = 0; i < nused; i++)
		if (used[i].fixed) {
			setop("check: fixed page f%d p%d resident", used[i].f,
				used[i].p);
			if (!PFbufResident(fds[used[i].f], used[i].p))
				fail("resident", 1, 0);
		}
	for (f = 0; f < NFILES; f++) {
		snprintf(who, sizeof who, "f%d fd %d", f, fds[f]);
		setop("check: PF_GetStats(%s)", who);
		act = PF_GetStats(fds[f], &s);
		expect_code(PFE_OK, act);
		cmp_stats(who, &mst[f], &s);
	}
	setop("check: PF_GetStats(-1)");
	act = PF_GetStats(-1, &s);
	expect_code(PFE_OK, act);
	cmp_stats("-1", &mglob, &s);
	setop("check: state");
	finish(PFE_OK);
}

static void check_sums(int fd, const char *who)
/****************************************************************************
SPECIFICATIONS:
	hits + misses == logical_reads on the actual counters.
*****************************************************************************/
{
	PFstats s;
	char what[64];

	setop("check: hits + misses == logical_reads (%s)", who);
	expect_code(PFE_OK, PF_GetStats(fd, &s));
	snprintf(what, sizeof what, "%s hits + misses", who);
	if (s.hits + s.misses != s.logical_reads)
		fail(what, s.logical_reads, s.hits + s.misses);
	finish(PFE_OK);
}

/*************************** setup and main ***************************/

static void make_file(int f)
/****************************************************************************
SPECIFICATIONS:
	(Re)create file f with NPAGES pages, page p holding (f, p, 0),
	and close it.
*****************************************************************************/
{
	char *buf;
	int fd, p, pn;

	setop("setup: create %s", fnames[f]);
	unlink(fnames[f]);
	expect_code(PFE_OK, PF_CreateFile(fnames[f]));
	if ((fd = PF_OpenFile(fnames[f])) < 0)
		fail("PF_OpenFile fd (expected >= 0)", 0, fd);
	for (p = 0; p < NPAGES; p++) {
		expect_code(PFE_OK, PF_AllocPage(fd, &pn, &buf));
		if (pn != p)
			fail("allocated page number", p, pn);
		put_body(buf, f, p, 0);
		expect_code(PFE_OK, PF_UnfixPage(fd, pn, TRUE));
		diskver[f][p] = 0;
	}
	expect_code(PFE_OK, PF_CloseFile(fd));
	finish(PFE_OK);
}

int main(int argc, char *argv[])
{
	unsigned long seed = 1;
	long nops = 200000;
	char *buf;
	int f, p, i, r, fd, target = 0;
	int wfix, wunc, wund;

	if (argc > 3) {
		fprintf(stderr, "usage: %s [seed] [nops]\n", argv[0]);
		return 2;
	}
	if (argc > 1)
		seed = strtoul(argv[1], NULL, 10);
	if (argc > 2 && (nops = strtol(argv[2], NULL, 10)) <= 0) {
		fprintf(stderr, "nops must be > 0\n");
		return 2;
	}
	rng_seed(seed);

	PF_Init();
	for (f = 0; f < NFILES; f++)
		make_file(f);
	npool = MINPOOL + rnd(MAXPOOL - MINPOOL + 1);
	setop("setup: PF_InitBuffer(%d)", npool);
	expect_code(PFE_OK, PF_InitBuffer(npool));
	for (f = 0; f < NFILES; f++) {
		pol[f] = rnd(2) ? PF_MRU : PF_LRU;
		setop("setup: open %s", fnames[f]);
		if ((fds[f] = PF_OpenFileEx(fnames[f], pol[f])) < 0)
			fail("PF_OpenFileEx fd (expected >= 0)", 0, fds[f]);
	}
	PF_ResetStats(-1);
	memset(mst, 0, sizeof mst);
	memset(&mglob, 0, sizeof mglob);
	nused = 0;
	printf("seed %lu, %ld ops, pool %d frames, f0 %s, f1 %s\n", seed, nops,
		npool, pol[0] == PF_MRU ? "MRU" : "LRU",
		pol[1] == PF_MRU ? "MRU" : "LRU");

	for (step = 1; step <= nops; step++) {
		/* the number of fixed pages drifts toward a target that
		changes every PHASE_LEN ops, so a run sees an empty-ish
		pool, a full pool with evictions, and all frames fixed */
		if (step % PHASE_LEN == 1)
			target = rnd(8) == 0 ? npool : rnd(npool + 1);
		if (nfixed(-1) < target) {
			wfix = 40; wunc = 8; wund = 6;
		} else {
			wfix = 15; wunc = 22; wund = 18;
		}
		r = rnd(wfix + wunc + wund + 8 + 10 + 2 + 3 + 5);
		if ((r -= wfix) < 0) {
			op_fix(rnd(NFILES), rnd(NPAGES));
		} else if ((r -= wunc + wund) < 0) {
			int dirty = r + wunc + wund >= wunc;
			/* mostly a fixed page; sometimes any page (errors) */
			if (nfixed(-1) > 0 && rnd(100) < 85) {
				do {
					i = rnd(nused);
				} while (!used[i].fixed);
				op_unfix(used[i].f, used[i].p, dirty);
			} else
				op_unfix(rnd(NFILES), rnd(NPAGES), dirty);
		} else if ((r -= 8) < 0) {
			if (nused > 0 && rnd(2)) {
				i = rnd(nused);
				op_mark(used[i].f, used[i].p);
			} else
				op_mark(rnd(NFILES), rnd(NPAGES));
		} else if ((r -= 10) < 0) {
			op_modify();
		} else if ((r -= 2) < 0) {
			op_flush();
		} else if ((r -= 3) < 0) {
			op_close(rnd(NFILES), 1);
		} else {
			op_misuse();
		}
		if (step % CHECK_EVERY == 0)
			check_state();
	}

	/* end: unfix everything, last state check, the sum rule */
	step = nops + 1;
	/* an unfix moves the page to the head, so rescan each time */
	while (nfixed(-1) > 0) {
		for (i = 0; !used[i].fixed; i++)
			;
		op_unfix(used[i].f, used[i].p, FALSE);
	}
	check_state();
	check_sums(fds[0], "f0");
	check_sums(fds[1], "f1");
	check_sums(-1, "-1");

	/* close, reopen and compare every page with the model's disk image */
	for (f = 0; f < NFILES; f++) {
		setop("final: no fixed page of f%d before close", f);
		if (nfixed(f) != 0)
			fail("fixed pages", 0, nfixed(f));
		op_close(f, 0);
	}
	for (f = 0; f < NFILES; f++) {
		setop("final: reopen %s", fnames[f]);
		if ((fd = PF_OpenFile(fnames[f])) < 0)
			fail("PF_OpenFile fd (expected >= 0)", 0, fd);
		for (p = 0; p < NPAGES; p++) {
			setop("final: read back f%d p%d (disk v%d)", f, p,
				diskver[f][p]);
			expect_code(PFE_OK, PF_GetThisPage(fd, p, &buf));
			check_body(buf, f, p, diskver[f][p]);
			expect_code(PFE_OK, PF_UnfixPage(fd, p, FALSE));
		}
		setop("final: close %s", fnames[f]);
		expect_code(PFE_OK, PF_CloseFile(fd));
		setop("final: destroy %s", fnames[f]);
		expect_code(PFE_OK, PF_DestroyFile(fnames[f]));
	}

	printf("coverage:\n");
	for (i = 0; i < C_NCOV; i++)
		printf("  %-40s %ld\n", covname[i], cov[i]);
	printf("stress_pool done!\n");
	return 0;
}
