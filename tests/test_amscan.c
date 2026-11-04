/* test_amscan.c: every AM scan operator on every key, every gap and both
 * ends, on insert-built and bulk-loaded trees (A4-A7)
 *
 * usage: test_amscan [conf=<toydb.conf>] [key=value ...]
 *
 * For each tree below, the test runs a full scan and then EQ, LT, LE, GT,
 * GE and NE with these values: every key in the index, every gap value
 * between two adjacent keys (keys are 10, 20, 30, ..., gaps 15, 25, ...,
 * so some gaps fall between the last key of one leaf and the first key of
 * the next), a value below the minimum (5) and one above the maximum.
 * Each scan is compared with an in-memory sorted multiset of (key, rid)
 * pairs: every expected record id exactly once, nothing else, keys in
 * non-decreasing order (all operators walk the leaf chain left to right),
 * and the scan must end with AME_EOF. After every scan each page of the
 * file is fixed and unfixed again with PF_GetThisPage/PF_UnfixPage, which
 * fails with PFE_PAGEFIXED if the scan left that page fixed, and at the
 * end PF_CloseFile must succeed.
 *
 * Trees (key i is 10*i; every 7th key has 3 record ids instead of 1):
 *   T1 int,     800 keys inserted in scrambled order
 *   T2 char(8), 700 keys "%08d" inserted in scrambled order
 *   T3 int,     800 keys inserted in ascending order
 *   T4 int,     100 keys inserted in scrambled order (root is a leaf)
 *   T5 char(8), 800 keys bulk-loaded at fill factor 1.0
 *   T6 char(8), 600 keys bulk-loaded at fill factor 0.5
 *   T7 int,    1000 keys bulk-loaded at fill factor 1.0
 *
 * Every probe is put in one class, from the leaf boundaries of its tree:
 *   P1  NE scan on a value that is not in the index
 *   P2  other scan on a value not in the index that lies between the last
 *       key of a leaf that has a next leaf and the first key of that leaf
 *   P3  LE scan on the largest key
 *   P4  LT scan on the first key of a leaf other than the leftmost
 *   ok  everything else (must pass even on the unfixed AM)
 * The failures are counted per class and per tree.
 *
 * The scans run in a child process. A probe that crashes the child, runs
 * past a time limit or leaves a page fixed is counted as failed, and a
 * fresh child goes on with the next probe, so one bad probe does not hide
 * the others. The last line is "test_amscan done!" only if no probe in
 * any class failed. make test runs this from a scratch dir; the index
 * files are made in the cwd.
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "am.h"
#include "ambulk.h"
#include "config.h"
#include "pf.h"

/*
 * No <stdlib.h>: am.h declares malloc and calloc the K&R way. The PF and
 * AM functions are K&R definitions without prototypes; char and short
 * parameters are passed as int, so int here matches them.
 */
extern void PF_Init();
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
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
extern int GetLeftPageNum(int fileDesc);

#define RELNAME		"amscan"	/* index files amscan.<n> */
#define MAXKEYS		1000
#define MAXRIDS		(3 * MAXKEYS)
#define MAXLEAVES	64
#define MAXPAGES	128
#define MAXPROBES	(6 * (2 * MAXKEYS + 1) + 1)
#define DUPEVERY	7		/* every 7th key has 3 rids */
#define PROBE_SECONDS	5		/* time limit per probe */
#define SHOWFAIL	4		/* failing probes printed per class */

/* classes */
#define C_OK	0
#define C_P1	1
#define C_P2	2
#define C_P3	3
#define C_P4	4
#define NCLASS	5
static const char *classname[NCLASS] = { "ok", "P1", "P2", "P3", "P4" };

/* symptoms, a bit mask per probe */
#define S_OPEN		1	/* AM_OpenIndexScan returned an error */
#define S_WRONG		2	/* wrong, missing, repeated or unordered rids */
#define S_NOEOF		4	/* ended with an error other than AME_EOF, or
				   never ended (cut off) */
#define S_LEAK		8	/* a page was left fixed */
#define S_CRASH		16	/* the child died on a signal */
#define S_TIMEOUT	32	/* the child ran out of time */
#define S_CLOSE		64	/* PF_CloseFile failed at the end */
#define NSYM		7
static const char *symname[NSYM] = { "open-error", "wrong-rids", "no-EOF",
	"page-left-fixed", "crash", "timeout", "close-failed" };

struct tree {
	const char *name;
	int no;			/* index number */
	char type;		/* 'i' or 'c' */
	int len;
	int nkeys;
	int ascending;		/* insert order: 1 ascending, 0 scrambled */
	const char *fill;	/* bulk fill factor, NULL = built by inserts */
};

static const struct tree trees[] = {
	{ "T1 int insert scrambled",     1, 'i', 4,  800, 0, NULL },
	{ "T2 char(8) insert scrambled", 2, 'c', 8,  700, 0, NULL },
	{ "T3 int insert ascending",     3, 'i', 4,  800, 1, NULL },
	{ "T4 int insert one leaf",      4, 'i', 4,  100, 0, NULL },
	{ "T5 char(8) bulk f=1.0",       5, 'c', 8,  800, 0, "1.0" },
	{ "T6 char(8) bulk f=0.5",       6, 'c', 8,  600, 0, "0.5" },
	{ "T7 int bulk f=1.0",           7, 'i', 4, 1000, 0, "1.0" },
};
#define NTREES ((int)(sizeof(trees) / sizeof(trees[0])))

/* the tree under test */
static const struct tree *T;
static char fname[64];
static int nrids;
static int key_of[MAXRIDS];		/* key number of each rid */
static int npages, nleaves, leftpage;
static int leaf_first[MAXLEAVES], leaf_last[MAXLEAVES];

struct probe {
	int op;
	int value;
	int cls;
};
static struct probe probes[MAXPROBES];
static int nprobes;

struct result {
	int idx;		/* probe index, -1 for the final close */
	int sym;		/* symptom mask, 0 = passed */
	int got, want, rc;
};

static int fails[NCLASS], totals[NCLASS];	/* this tree */
static int allfails[NCLASS], alltotals[NCLASS];	/* every tree */
static int symcount[NCLASS][NSYM];
static int shown[NCLASS];
static int setup_failed;

static void
encode(int number, char *key)
/* Key bytes for key number "number" in the tree under test. */
{
	char text[16];

	memset(key, 0, AM_MAXATTRLENGTH);
	if (T->type == 'i') {
		memcpy(key, &number, sizeof(int));
	} else {
		sprintf(text, "%08d", number);
		memcpy(key, text, 8);
	}
}

static int
decode(const char *key)
{
	char text[16];
	int number, i;

	if (T->type == 'i') {
		memcpy(&number, key, sizeof(int));
		return number;
	}
	memcpy(text, key, 8);
	text[8] = '\0';
	number = 0;
	for (i = 0; i < 8; i++)
		number = 10 * number + (text[i] - '0');
	return number;
}

/* rids: key i (1-based, number 10*i) has rid i-1; key i with
   i % DUPEVERY == 0 also has rids nkeys + 2*(i/DUPEVERY - 1) and the one
   after it */
static int
extra_rid(int i)
{
	return T->nkeys + 2 * (i / DUPEVERY - 1);
}

static void
make_model(void)
{
	int i;

	nrids = T->nkeys;
	for (i = 1; i <= T->nkeys; i++) {
		key_of[i - 1] = 10 * i;
		if (i % DUPEVERY == 0) {
			key_of[extra_rid(i)] = 10 * i;
			key_of[extra_rid(i) + 1] = 10 * i;
			nrids += 2;
		}
	}
}

/* bulk-load stream, keys in order, the rids of a key consecutive */
struct stream {
	int key;	/* 1-based key index */
	int sent;	/* rids of it sent */
};

static int
next_item(void *context, char *key, int *rid)
{
	struct stream *s;
	int per;

	s = (struct stream *)context;
	if (s->key > T->nkeys)
		return AM_BULK_NEXT_EOF;
	encode(10 * s->key, key);
	*rid = s->sent == 0 ? s->key - 1 : extra_rid(s->key) + s->sent - 1;
	per = s->key % DUPEVERY == 0 ? 3 : 1;
	if (++s->sent == per) {
		s->key++;
		s->sent = 0;
	}
	return AM_BULK_NEXT_ITEM;
}

static int
build(void)
/* Make the index file of tree T. Returns 0 on success. */
{
	char key[AM_MAXATTRLENGTH];
	char argument[64];
	char *argv[2];
	struct stream s;
	int fd, pass, j, i, rc;

	unlink(fname);			/* PF_CreateFile uses O_EXCL */
	if (T->fill != NULL) {
		sprintf(argument, "bulk_fill_factor=%s", T->fill);
		argv[0] = "test_amscan";
		argv[1] = argument;
		if (Config_ParseArgs(2, argv) != 1)
			return -1;
		s.key = 1;
		s.sent = 0;
		rc = AM_BulkLoad(RELNAME, T->no, T->type, T->len, next_item, &s);
		return rc == AME_OK ? 0 : -1;
	}
	if (AM_CreateIndex(RELNAME, T->no, T->type, T->len) != AME_OK)
		return -1;
	fd = PF_OpenFile(fname);
	if (fd < 0)
		return -1;
	/* pass 0: each key's own rid; pass 1 and 2: the extra rids */
	for (pass = 0; pass < 3; pass++)
		for (j = 0; j < T->nkeys; j++) {
			/* 389 is prime and does not divide any nkeys used */
			i = T->ascending ? j + 1 :
			    (int)((j * 389L) % T->nkeys) + 1;
			if (pass > 0 && i % DUPEVERY != 0)
				continue;
			encode(10 * i, key);
			rc = AM_InsertEntry(fd, T->type, T->len, key,
			     pass == 0 ? i - 1 : extra_rid(i) + pass - 1);
			AM_EmptyStack();
			if (rc != AME_OK) {
				printf("%s: AM_InsertEntry %d failed (%d)\n",
				       T->name, 10 * i, rc);
				return -1;
			}
		}
	return PF_CloseFile(fd) == PFE_OK ? 0 : -1;
}

static int
read_shape(void)
/* Page count, leftmost leaf and the first and last key of every leaf in
   chain order. Returns 0 on success. */
{
	AM_LEAFHEADER h;
	char *buf;
	int fd, page;

	fd = PF_OpenFile(fname);
	if (fd < 0)
		return -1;
	npages = 0;
	page = -1;
	while (PF_GetNextPage(fd, &page, &buf) == PFE_OK) {
		npages++;
		PF_UnfixPage(fd, page, FALSE);
	}
	leftpage = GetLeftPageNum(fd);
	nleaves = 0;
	page = leftpage;
	while (page != AM_NULL_PAGE && nleaves < MAXLEAVES) {
		if (PF_GetThisPage(fd, page, &buf) != PFE_OK)
			return -1;
		memcpy(&h, buf, sizeof(h));
		if (h.numKeys > 0) {
			leaf_first[nleaves] = decode(buf + AM_sl);
			leaf_last[nleaves] = decode(buf + AM_sl +
					(h.numKeys - 1) * (T->len + AM_ss));
			nleaves++;
		}
		PF_UnfixPage(fd, page, FALSE);
		page = h.nextLeafPage;
	}
	return PF_CloseFile(fd) == PFE_OK && npages <= MAXPAGES ? 0 : -1;
}

static int
wanted(int op, int key, int value)
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

static int
classify(int op, int v)
{
	int present, j;

	present = v % 10 == 0 && v >= 10 && v <= 10 * T->nkeys;
	if (op == NOT_EQUAL && !present)
		return C_P1;
	if (op != NOT_EQUAL && !present)
		for (j = 0; j + 1 < nleaves; j++)
			if (leaf_last[j] < v && v < leaf_first[j + 1])
				return C_P2;
	if (op == LESS_THAN_EQUAL && v == 10 * T->nkeys)
		return C_P3;
	if (op == LESS_THAN && present)
		for (j = 1; j < nleaves; j++)
			if (v == leaf_first[j])
				return C_P4;
	return C_OK;
}

static void
make_probes(void)
{
	static const int ops[6] = { EQUAL, LESS_THAN, LESS_THAN_EQUAL,
		GREATER_THAN, GREATER_THAN_EQUAL, NOT_EQUAL };
	int v, o;

	nprobes = 0;
	probes[nprobes].op = ALL;
	probes[nprobes].value = 0;
	probes[nprobes].cls = C_OK;
	nprobes++;
	/* 5 (below the minimum), 10, 15, 20, ..., 10*n, 10*n + 5 (above) */
	for (v = 5; v <= 10 * T->nkeys + 5; v += 5)
		for (o = 0; o < 6; o++) {
			probes[nprobes].op = ops[o];
			probes[nprobes].value = v;
			probes[nprobes].cls = classify(ops[o], v);
			nprobes++;
		}
}

static int
pages_free(int fd)
/* 1 if every page of the file can be fixed, i.e. none is left fixed */
{
	char *buf;
	int p, ok;

	ok = 1;
	for (p = 0; p < npages; p++) {
		if (PF_GetThisPage(fd, p, &buf) != PFE_OK)
			ok = 0;
		else if (PF_UnfixPage(fd, p, FALSE) != PFE_OK)
			ok = 0;
	}
	return ok;
}

static void
run_probe(int fd, const struct probe *pr, struct result *r)
{
	char key[AM_MAXATTRLENGTH];
	char seen[MAXRIDS];
	int sd, rid, i, lastkey;

	r->sym = 0;
	r->got = 0;
	r->rc = 0;
	r->want = 0;
	for (i = 0; i < nrids; i++)
		if (wanted(pr->op, key_of[i], pr->value))
			r->want++;
	encode(pr->value, key);
	sd = AM_OpenIndexScan(fd, T->type, T->len, pr->op,
			      pr->op == ALL ? NULL : key);
	AM_EmptyStack();
	if (sd < 0) {
		r->sym |= S_OPEN;
		r->rc = sd;
	} else {
		memset(seen, 0, sizeof(seen));
		lastkey = -1;
		while (r->got < 2 * nrids + 10) {
			rid = AM_FindNextEntry(sd);
			if (rid < 0) {
				r->rc = rid;
				break;
			}
			r->got++;
			if (rid >= nrids || seen[rid] ||
			    !wanted(pr->op, key_of[rid], pr->value) ||
			    key_of[rid] < lastkey) {
				r->sym |= S_WRONG;
				continue;
			}
			seen[rid] = 1;
			lastkey = key_of[rid];
		}
		if (r->rc != AME_EOF)
			r->sym |= S_NOEOF;
		if (r->got != r->want)
			r->sym |= S_WRONG;
		AM_CloseIndexScan(sd);
	}
	if (!pages_free(fd))
		r->sym |= S_LEAK;
}

static void
put(int wfd, const struct result *r)
{
	const char *p;
	ssize_t n;
	size_t left;

	p = (const char *)r;
	left = sizeof(*r);
	while (left > 0) {
		n = write(wfd, p, left);
		if (n <= 0)
			_exit(2);
		p += n;
		left -= (size_t)n;
	}
}

static void
on_crash(int sig)
/* A crash in a probe ends the child at once with exit code 4 (faster than
   a core dump through the system crash handler). */
{
	(void)sig;
	_exit(4);
}

static void
child(int start, int wfd)
/* Run probes start.. and write one result per probe; exit 3 right after
   a probe that left a page fixed, so the next child starts clean. */
{
	struct result r;
	int fd, i;

	signal(SIGSEGV, on_crash);
	signal(SIGBUS, on_crash);
	fd = PF_OpenFile(fname);
	if (fd < 0)
		_exit(2);
	for (i = start; i < nprobes; i++) {
		alarm(PROBE_SECONDS);
		run_probe(fd, &probes[i], &r);
		alarm(0);
		r.idx = i;
		put(wfd, &r);
		if (r.sym & S_LEAK)
			_exit(3);
	}
	r.idx = -1;
	r.sym = PF_CloseFile(fd) == PFE_OK ? 0 : S_CLOSE;
	r.got = r.want = r.rc = 0;
	put(wfd, &r);
	_exit(0);
}

static void
record(const struct result *r)
{
	const struct probe *pr;
	int c, s;

	if (r->idx < 0) {
		if (r->sym) {
			printf("  PF_CloseFile failed at the end\n");
			setup_failed = 1;
		}
		return;
	}
	pr = &probes[r->idx];
	c = pr->cls;
	totals[c]++;
	if (r->sym == 0)
		return;
	fails[c]++;
	for (s = 0; s < NSYM; s++)
		if (r->sym & (1 << s))
			symcount[c][s]++;
	if (shown[c]++ < SHOWFAIL) {
		printf("  FAIL [%s] %s v=%d: want %d got %d rc %d", classname[c],
		       opname(pr->op), pr->value, r->want, r->got, r->rc);
		for (s = 0; s < NSYM; s++)
			if (r->sym & (1 << s))
				printf(" %s", symname[s]);
		printf("\n");
	}
}

static int
get(int rfd, struct result *r)
{
	char *p;
	ssize_t n;
	size_t left;

	p = (char *)r;
	left = sizeof(*r);
	while (left > 0) {
		n = read(rfd, p, left);
		if (n <= 0)
			return 0;
		p += n;
		left -= (size_t)n;
	}
	return 1;
}

static void
run_probes(void)
{
	struct result r;
	int pfd[2], start, last, status, done;
	pid_t pid;

	start = 0;
	done = 0;
	while (!done && start <= nprobes) {
		if (pipe(pfd) != 0) {
			printf("  pipe failed\n");
			setup_failed = 1;
			return;
		}
		fflush(stdout);
		pid = fork();
		if (pid < 0) {
			printf("  fork failed\n");
			setup_failed = 1;
			return;
		}
		if (pid == 0) {
			close(pfd[0]);
			child(start, pfd[1]);
		}
		close(pfd[1]);
		last = start - 1;
		while (get(pfd[0], &r)) {
			record(&r);
			if (r.idx < 0)
				done = 1;
			else
				last = r.idx;
		}
		close(pfd[0]);
		waitpid(pid, &status, 0);
		if (done)
			break;
		if (WIFEXITED(status) && WEXITSTATUS(status) == 3) {
			start = last + 1;	/* page left fixed, reported */
			continue;
		}
		if ((WIFSIGNALED(status) || (WIFEXITED(status) &&
		     WEXITSTATUS(status) == 4)) && last + 1 < nprobes) {
			/* probe last+1 crashed or hung */
			r.idx = last + 1;
			r.sym = WIFSIGNALED(status) &&
			    WTERMSIG(status) == SIGALRM ? S_TIMEOUT : S_CRASH;
			r.got = r.want = 0;
			r.rc = WIFSIGNALED(status) ? WTERMSIG(status) : SIGSEGV;
			record(&r);
			start = last + 2;
			continue;
		}
		printf("  child stopped unexpectedly (status %d)\n", status);
		setup_failed = 1;
		return;
	}
}

static void
run_tree(void)
{
	int c, s;

	sprintf(fname, "%s.%d", RELNAME, T->no);
	memset(fails, 0, sizeof(fails));
	memset(totals, 0, sizeof(totals));
	memset(symcount, 0, sizeof(symcount));
	memset(shown, 0, sizeof(shown));
	make_model();
	if (build() != 0 || read_shape() != 0) {
		printf("%s: could not build the index\n", T->name);
		setup_failed = 1;
		return;
	}
	make_probes();
	printf("%s: %d keys, %d rids, %d pages, %d leaves, leftmost leaf "
	       "page %d, %d probes\n", T->name, T->nkeys, nrids, npages,
	       nleaves, leftpage, nprobes);
	run_probes();
	for (c = 0; c < NCLASS; c++) {
		printf("  %-2s failed %4d of %5d", classname[c], fails[c],
		       totals[c]);
		for (s = 0; s < NSYM; s++)
			if (symcount[c][s])
				printf("  %s %d", symname[s], symcount[c][s]);
		printf("\n");
		allfails[c] += fails[c];
		alltotals[c] += totals[c];
	}
	if (totals[C_OK] + totals[C_P1] + totals[C_P2] + totals[C_P3] +
	    totals[C_P4] != nprobes) {
		printf("  only %d of %d probes reported\n", totals[C_OK] +
		       totals[C_P1] + totals[C_P2] + totals[C_P3] +
		       totals[C_P4], nprobes);
		setup_failed = 1;
	}
	AM_DestroyIndex(RELNAME, T->no);
}

int
main(int argc, char **argv)
{
	int status, t, c, nfail;

	status = Config_Init(argc, argv);
	if (status != CONFIG_OK && status != CONFIG_ENOFILE) {
		printf("test_amscan: bad configuration\n");
		return 1;
	}
	PF_Init();
	for (t = 0; t < NTREES; t++) {
		T = &trees[t];
		run_tree();
	}
	nfail = 0;
	printf("all trees:");
	for (c = 0; c < NCLASS; c++) {
		printf("  %s %d/%d", classname[c], allfails[c], alltotals[c]);
		nfail += allfails[c];
	}
	printf("  (failed/probes)\n");
	if (nfail > 0 || setup_failed) {
		printf("test_amscan: FAILED\n");
		return 1;
	}
	printf("test_amscan done!\n");
	return 0;
}
