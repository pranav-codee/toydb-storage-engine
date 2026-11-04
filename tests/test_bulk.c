/* test_bulk.c: AM_BulkLoad shape, scan and error tests with hand-computed
   expectations.

   Sizes used below (PF_PAGE_SIZE 4096, sizeof(AM_LEAFHEADER) 24,
   sizeof(AM_INTHEADER) 8):
     char(8):   leaf capacity (4096-24)/(8+8) = 254 keys, internal maxKeys
                (4096-8-4)/(4+8) = 340, so 341 children;
                rid limit per key (4096-24-10)/6 = 677
     char(200): leaf capacity 4072/208 = 19 keys, internal maxKeys
                4084/204 = 20, so 21 children

   Scans avoid four pre-existing AM scan bugs, which are fixed on a
   separate branch, not here:
     P1  NE scan on a value absent from the index (crash or garbage)
     P2  any value scan on an absent value that falls in the gap after
         the last key of a non-last leaf (AME_PF, pages left fixed)
     P3  LE scan on the largest key (runs off the last leaf)
     P4  LT scan on the first key of a non-first leaf (returns 0 records)
   So there are no NE scans, absent values are only below the minimum or
   above the maximum, LE is never on the maximum and LT is never on the
   first key of a leaf other than the first. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "am.h"
#include "ambulk.h"
#include "config.h"
#include "pf.h"

extern void exit(int);
extern void PF_Init();
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_GetNextPage(int fd, int *pagenum, char **pagebuf);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);
extern int AM_OpenIndexScan(int fileDesc, int attrType, int attrLength,
			    int op, char *value);
extern int AM_FindNextEntry(int scanDesc);
extern int AM_CloseIndexScan(int scanDesc);
extern int AM_InsertEntry(int fileDesc, int attrType, int attrLength,
			  char *value, int recId);
extern int AM_EmptyStack();
extern int AM_DestroyIndex(char *fileName, int indexNo);

#define NAME "bulktest"
#define FILE_NAME "bulktest.0"
#define MAX_PAGES 64
#define MAX_RIDS 2048

/*
 * The stream: keys 1..count as "K0000001", "K0000002", ... padded with 'x'
 * to the key length. Key dup_key gets dup_count rids, every other key one.
 * Rids are numbered 0, 1, 2, ... in stream order. If bad_at >= 0, the item
 * at that position repeats an earlier, smaller key (unsorted input).
 */
struct stream {
	int length;
	int count;
	int dup_key;
	int dup_count;
	int bad_at;
	int key;		/* key being sent */
	int sent;		/* rids sent for that key */
	int position;		/* items sent */
};

struct shape {
	int pages;
	int reachable;
	int leaves;
	int internal;
	int height;
	int root_type;
	int leaf_keys[MAX_PAGES];	/* keys per leaf, in chain order */
	int chain;
};

static void
check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "test_bulk: %s\n", message);
		exit(1);
	}
}

static void
make_key(char *key, int length, int number, char pad)
{
	char text[16];

	memset(key, pad, (size_t)length);
	sprintf(text, "K%07d", number);
	memcpy(key, text, 8);
}

static void
start(struct stream *s, int length, int count, int dup_key, int dup_count)
{
	s->length = length;
	s->count = count;
	s->dup_key = dup_key;
	s->dup_count = dup_count;
	s->bad_at = -1;
	s->key = 1;
	s->sent = 0;
	s->position = 0;
}

static int
next_item(void *context, char *key, int *rid)
{
	struct stream *s;
	int per_key;

	s = (struct stream *)context;
	if (s->key > s->count)
		return AM_BULK_NEXT_EOF;
	if (s->position == s->bad_at) {
		make_key(key, s->length, 1, 'x');
		*rid = s->position++;
		return AM_BULK_NEXT_ITEM;
	}
	make_key(key, s->length, s->key, 'x');
	*rid = s->position++;
	per_key = s->key == s->dup_key ? s->dup_count : 1;
	if (++s->sent == per_key) {
		s->key++;
		s->sent = 0;
	}
	return AM_BULK_NEXT_ITEM;
}

static void
set_fill(const char *value)
{
	char argument[64];
	char *argv[2];

	sprintf(argument, "bulk_fill_factor=%s", value);
	argv[0] = "test_bulk";
	argv[1] = argument;
	check(Config_ParseArgs(2, argv) == 1, "fill factor override");
}

static int
load(struct stream *s, const char *fill)
{
	set_fill(fill);
	unlink(FILE_NAME);
	return AM_BulkLoad(NAME, 0, 'c', s->length, next_item, s);
}

/* every page reachable from page 0, leaf chain from the leftmost leaf */
static void
get_shape(int fd, int length, struct shape *t)
{
	char seen[MAX_PAGES];
	int queue[MAX_PAGES];
	AM_LEAFHEADER leaf;
	AM_INTHEADER node;
	char *page;
	int head, tail, page_number, child, i;

	memset(t, 0, sizeof(*t));
	page_number = -1;
	while (PF_GetNextPage(fd, &page_number, &page) == PFE_OK) {
		t->pages++;
		check(PF_UnfixPage(fd, page_number, FALSE) == PFE_OK, "unfix");
	}
	check(t->pages <= MAX_PAGES, "too many pages for the test");
	memset(seen, 0, sizeof(seen));
	head = tail = 0;
	queue[tail++] = 0;
	seen[0] = 1;
	while (head < tail) {
		page_number = queue[head++];
		check(PF_GetThisPage(fd, page_number, &page) == PFE_OK, "get");
		if (page_number == 0)
			t->root_type = page[0];
		if (page[0] == 'l') {
			t->leaves++;
		} else {
			check(page[0] == 'i', "unknown page type");
			t->internal++;
			memcpy(&node, page, sizeof(node));
			for (i = 0; i <= node.numKeys; i++) {
				memcpy(&child, page + AM_sint + i * (AM_si + length),
				       sizeof(child));
				check(child > 0 && child < t->pages && !seen[child],
				      "bad or repeated child pointer");
				seen[child] = 1;
				queue[tail++] = child;
			}
		}
		check(PF_UnfixPage(fd, page_number, FALSE) == PFE_OK, "unfix");
	}
	t->reachable = tail;
	/* leftmost path */
	page_number = 0;
	t->height = 1;
	for (;;) {
		check(PF_GetThisPage(fd, page_number, &page) == PFE_OK, "get");
		if (page[0] == 'l')
			break;
		memcpy(&child, page + AM_sint, sizeof(child));
		check(PF_UnfixPage(fd, page_number, FALSE) == PFE_OK, "unfix");
		page_number = child;
		t->height++;
	}
	check(PF_UnfixPage(fd, page_number, FALSE) == PFE_OK, "unfix");
	while (page_number != AM_NULL_PAGE) {
		check(t->chain < MAX_PAGES, "leaf chain too long");
		check(PF_GetThisPage(fd, page_number, &page) == PFE_OK, "get");
		memcpy(&leaf, page, sizeof(leaf));
		t->leaf_keys[t->chain++] = leaf.numKeys;
		check(PF_UnfixPage(fd, page_number, FALSE) == PFE_OK, "unfix");
		page_number = leaf.nextLeafPage;
	}
}

/* shape of the index file now on disk; fails on any orphan page */
static void
expect_shape(int fd, int length, int pages, int leaves, int height,
	     const char *what)
{
	struct shape t;

	get_shape(fd, length, &t);
	if (t.pages != pages || t.leaves != leaves || t.height != height ||
	    t.reachable != t.pages || t.chain != t.leaves) {
		fprintf(stderr, "test_bulk: %s: pages %d (want %d), reachable %d,"
			" leaves %d (want %d), chain %d, height %d (want %d)\n",
			what, t.pages, pages, t.reachable, t.leaves, leaves,
			t.chain, t.height, height);
		exit(1);
	}
	printf("%-34s pages=%d leaves=%d height=%d orphans=0\n", what,
	       t.pages, t.leaves, t.height);
}

/*
 * Run one scan and check it returns exactly the rids of keys low..high
 * (key numbers, inclusive; low > high means none). With the stream above,
 * key k has rid k-1 when no earlier key has extra rids; extra_rids is the
 * number of extra rids of a duplicated key inside low..high (all rids are
 * distinct and in range, and the count matches, so the set is exact).
 */
static void
expect_scan(int fd, int length, int op, int value, int low, int high,
	    int extra_rids)
{
	char key[AM_MAXATTRLENGTH];
	char seen[MAX_RIDS];
	int scan, rid, count, want;

	make_key(key, length, value, 'x');
	want = low <= high ? high - low + 1 + extra_rids : 0;
	scan = AM_OpenIndexScan(fd, 'c', length, op, op == ALL ? NULL : key);
	check(scan >= 0, "open scan");
	memset(seen, 0, sizeof(seen));
	count = 0;
	while ((rid = AM_FindNextEntry(scan)) >= 0) {
		check(rid >= low - 1 && rid <= high - 1 + extra_rids &&
		      rid < MAX_RIDS && !seen[rid], "rid outside the expected set");
		seen[rid] = 1;
		count++;
	}
	check(rid == AME_EOF, "scan did not end with AME_EOF");
	check(AM_CloseIndexScan(scan) == AME_OK, "close scan");
	AM_EmptyStack();
	if (count != want) {
		fprintf(stderr, "test_bulk: scan op %d value %d: %d rids, want %d\n",
			op, value, count, want);
		exit(1);
	}
}

static int
open_index(void)
{
	int fd;

	fd = PF_OpenFile(FILE_NAME);
	check(fd >= 0, "open index");
	return fd;
}

static void
close_index(int fd)
{
	check(PF_CloseFile(fd) == PFE_OK, "close index (a page left fixed?)");
	check(AM_DestroyIndex(NAME, 0) == AME_OK, "destroy index");
}

/* scans on a tree of keys 1..n, one rid each; first_of_leaf2 is the
   first key of the second leaf (0 if there is one leaf) */
static void
scan_set(int fd, int n, int first_of_leaf2)
{
	int probes[5];
	int i, v;

	expect_scan(fd, 8, ALL, 0, 1, n, 0);
	/* absent values: below the minimum (key 0) and above the maximum */
	expect_scan(fd, 8, EQUAL, 0, 1, 0, 0);
	expect_scan(fd, 8, LESS_THAN, 0, 1, 0, 0);
	expect_scan(fd, 8, LESS_THAN_EQUAL, 0, 1, 0, 0);
	expect_scan(fd, 8, GREATER_THAN, 0, 1, n, 0);
	expect_scan(fd, 8, GREATER_THAN_EQUAL, 0, 1, n, 0);
	expect_scan(fd, 8, EQUAL, n + 1, 1, 0, 0);
	expect_scan(fd, 8, LESS_THAN, n + 1, 1, n, 0);
	expect_scan(fd, 8, LESS_THAN_EQUAL, n + 1, 1, n, 0);
	expect_scan(fd, 8, GREATER_THAN, n + 1, 1, 0, 0);
	expect_scan(fd, 8, GREATER_THAN_EQUAL, n + 1, 1, 0, 0);
	/* present: the minimum, a middle key, the last key of leaf 1, a key
	   past leaf 2's first, the maximum */
	probes[0] = 1;
	probes[1] = n / 2 + 1;
	probes[2] = first_of_leaf2 > 0 ? first_of_leaf2 - 1 : n / 3 + 1;
	probes[3] = first_of_leaf2 > 0 ? first_of_leaf2 + 3 : n / 4 + 1;
	probes[4] = n;
	for (i = 0; i < 5; i++) {
		v = probes[i];
		expect_scan(fd, 8, EQUAL, v, v, v, 0);
		expect_scan(fd, 8, GREATER_THAN, v, v + 1, n, 0);
		expect_scan(fd, 8, GREATER_THAN_EQUAL, v, v, n, 0);
		expect_scan(fd, 8, LESS_THAN, v, 1, v - 1, 0);	/* never P4 */
		if (v != n)					/* P3 */
			expect_scan(fd, 8, LESS_THAN_EQUAL, v, 1, v, 0);
	}
}

int
main(int argc, char **argv)
{
	struct stream s;
	struct shape t;
	char key[AM_MAXATTRLENGTH];
	int status, fd, i, rc;

	status = Config_Init(argc, argv);
	check(status == CONFIG_OK || status == CONFIG_ENOFILE, "config");
	PF_Init();

	/* 0 keys: the empty leaf of AM_CreateIndex, 1 page */
	start(&s, 8, 0, 0, 0);
	check(load(&s, "1.0") == AME_OK, "load 0 keys");
	fd = open_index();
	expect_shape(fd, 8, 1, 1, 1, "0 keys");
	expect_scan(fd, 8, ALL, 0, 1, 0, 0);
	expect_scan(fd, 8, GREATER_THAN_EQUAL, 1, 1, 0, 0);
	close_index(fd);

	/* 1 key: the root leaf in page 0, 1 page */
	start(&s, 8, 1, 0, 0);
	check(load(&s, "1.0") == AME_OK, "load 1 key");
	fd = open_index();
	expect_shape(fd, 8, 1, 1, 1, "1 key");
	expect_scan(fd, 8, ALL, 0, 1, 1, 0);
	expect_scan(fd, 8, EQUAL, 1, 1, 1, 0);
	expect_scan(fd, 8, GREATER_THAN, 0, 1, 1, 0);
	close_index(fd);

	/* 254 keys = one full char(8) leaf: still 1 page */
	start(&s, 8, 254, 0, 0);
	check(load(&s, "1.0") == AME_OK, "load one full leaf");
	fd = open_index();
	expect_shape(fd, 8, 1, 1, 1, "254 keys (one full leaf)");
	scan_set(fd, 254, 0);
	close_index(fd);

	/* 255 keys: leaves of 254 and 1 key, root in page 0: 3 pages */
	start(&s, 8, 255, 0, 0);
	check(load(&s, "1.0") == AME_OK, "load one leaf + 1");
	fd = open_index();
	expect_shape(fd, 8, 3, 2, 2, "255 keys (one leaf + 1)");
	get_shape(fd, 8, &t);
	check(t.leaf_keys[0] == 254 && t.leaf_keys[1] == 1, "254 + 1 split");
	expect_scan(fd, 8, ALL, 0, 1, 255, 0);
	expect_scan(fd, 8, EQUAL, 255, 255, 255, 0);
	expect_scan(fd, 8, GREATER_THAN, 254, 255, 255, 0);
	close_index(fd);

	/* 1000 keys at f=1.0: leaves of 254, 254, 254, 238, so 4 + 1 = 5
	   pages; leaf 2 starts at key 255 */
	start(&s, 8, 1000, 0, 0);
	check(load(&s, "1.0") == AME_OK, "load 1000 at f=1.0");
	fd = open_index();
	expect_shape(fd, 8, 5, 4, 2, "1000 keys f=1.0");
	scan_set(fd, 1000, 255);
	close_index(fd);

	/* 1000 keys at f=0.5: target floor(0.5 * 254) = 127 keys per leaf,
	   ceil(1000 / 127) = 8 leaves (7 x 127 + 111), 9 pages; leaf 2 starts
	   at key 128 */
	start(&s, 8, 1000, 0, 0);
	check(load(&s, "0.5") == AME_OK, "load 1000 at f=0.5");
	fd = open_index();
	expect_shape(fd, 8, 9, 8, 2, "1000 keys f=0.5");
	get_shape(fd, 8, &t);
	check(t.leaf_keys[0] == 127 && t.leaf_keys[7] == 111, "f=0.5 leaf sizes");
	scan_set(fd, 1000, 128);
	close_index(fd);

	/* duplicate at a leaf boundary: 255 keys, key 254 has 3 rids. Keys
	   1..253 use 253 x 16 = 4048 bytes; key 254 needs 10 + 3 x 6 = 28 more,
	   4076 > 4072, so it starts leaf 2 whole: leaves of 253 and 2 keys */
	start(&s, 8, 255, 254, 3);
	check(load(&s, "1.0") == AME_OK, "load boundary duplicate");
	fd = open_index();
	expect_shape(fd, 8, 3, 2, 2, "duplicate at leaf boundary");
	get_shape(fd, 8, &t);
	check(t.leaf_keys[0] == 253 && t.leaf_keys[1] == 2, "253 + 2 split");
	/* rids: keys 1..253 -> 0..252, key 254 -> 253..255, key 255 -> 256 */
	expect_scan(fd, 8, EQUAL, 254, 254, 254, 2);
	expect_scan(fd, 8, GREATER_THAN_EQUAL, 254, 254, 255, 2);
	expect_scan(fd, 8, ALL, 0, 1, 255, 2);
	close_index(fd);

	/* rid limit: 677 rids on one char(8) key fit a leaf exactly
	   (10 + 677 x 6 = 4072); 678 is refused and leaves no file */
	start(&s, 8, 1, 1, 677);
	check(load(&s, "1.0") == AME_OK, "load 677 rids");
	fd = open_index();
	expect_shape(fd, 8, 1, 1, 1, "one key with 677 rids");
	expect_scan(fd, 8, EQUAL, 1, 1, 1, 676);
	close_index(fd);
	start(&s, 8, 1, 1, 678);
	rc = load(&s, "1.0");
	check(rc == AME_INVALIDATTRLENGTH && AM_Errno == rc, "678 rids refused");
	check(access(FILE_NAME, F_OK) != 0, "no file after 678 rids");

	/* 3 levels: char(200), 418 = 22 x 19 keys -> 22 full leaves; 22
	   children > 21, so 2 internal nodes of 11 and a root: 25 pages */
	start(&s, 200, 418, 0, 0);
	check(load(&s, "1.0") == AME_OK, "load 3 levels");
	fd = open_index();
	expect_shape(fd, 200, 25, 22, 3, "char(200) 418 keys");
	expect_scan(fd, 200, ALL, 0, 1, 418, 0);
	expect_scan(fd, 200, EQUAL, 209, 209, 209, 0);
	expect_scan(fd, 200, GREATER_THAN, 400, 401, 418, 0);
	close_index(fd);

	/* inserts after the load: char(200), 399 = 21 x 19 keys -> 21 full
	   leaves and a root with 21 children (20 keys = maxKeys), 22 pages.
	   One insert into leaf 1 (K0000005 padded with 'y' sorts between keys
	   5 and 6) splits the leaf (+1 page) and the full root (+2 pages:
	   a new internal node and the copy of the old root's left half):
	   25 pages, height 3 */
	start(&s, 200, 399, 0, 0);
	check(load(&s, "1.0") == AME_OK, "load for inserts");
	fd = open_index();
	expect_shape(fd, 200, 22, 21, 2, "char(200) 399 keys");
	make_key(key, 200, 5, 'y');
	check(AM_InsertEntry(fd, 'c', 200, key, 399) == AME_OK, "insert");
	expect_shape(fd, 200, 25, 22, 3, "after 1 insert (leaf+root split)");
	/* more inserts at the end: K0000400.. one rid each (rids 400..) */
	for (i = 400; i <= 450; i++) {
		make_key(key, 200, i, 'x');
		check(AM_InsertEntry(fd, 'c', 200, key, i) == AME_OK,
		      "insert at end");
	}
	get_shape(fd, 200, &t);
	check(t.reachable == t.pages && t.chain == t.leaves && t.height == 3,
	      "shape after 52 inserts");
	printf("%-34s pages=%d leaves=%d height=%d orphans=0\n",
	       "after 52 inserts", t.pages, t.leaves, t.height);
	/* rids now: loaded keys 1..399 -> 0..398, the 'y' twin of key 5 ->
	   399, inserted keys 400..450 -> 400..450; 451 in all */
	{
		char seen[MAX_RIDS];
		int scan, rid, count;

		scan = AM_OpenIndexScan(fd, 'c', 200, ALL, NULL);
		check(scan >= 0, "open full scan after inserts");
		memset(seen, 0, sizeof(seen));
		count = 0;
		while ((rid = AM_FindNextEntry(scan)) >= 0) {
			check(rid < MAX_RIDS && !seen[rid], "rid after inserts");
			seen[rid] = 1;
			count++;
		}
		check(rid == AME_EOF && count == 451, "full scan after inserts");
		check(AM_CloseIndexScan(scan) == AME_OK, "close");
	}
	for (i = 1; i <= 450; i++) {
		int scan, rid, want;

		make_key(key, 200, i, 'x');
		want = i < 400 ? i - 1 : i;
		scan = AM_OpenIndexScan(fd, 'c', 200, EQUAL, key);
		check(scan >= 0, "open EQ after inserts");
		check(AM_FindNextEntry(scan) == want, "EQ rid after inserts");
		rid = AM_FindNextEntry(scan);
		check(rid == AME_EOF, "EQ end after inserts");
		check(AM_CloseIndexScan(scan) == AME_OK, "close");
		AM_EmptyStack();
	}
	make_key(key, 200, 5, 'y');
	{
		int scan;

		scan = AM_OpenIndexScan(fd, 'c', 200, EQUAL, key);
		check(scan >= 0 && AM_FindNextEntry(scan) == 399 &&
		      AM_FindNextEntry(scan) == AME_EOF, "EQ inserted key");
		check(AM_CloseIndexScan(scan) == AME_OK, "close");
		AM_EmptyStack();
	}
	close_index(fd);

	/* unsorted input: item 3 repeats key 1. The documented error, the same
	   value in AM_Errno, no file; 25 tries (the PF file table has 20
	   slots) and then a good load show nothing stays open */
	for (i = 0; i < 25; i++) {
		start(&s, 8, 10, 0, 0);
		s.bad_at = 3;
		rc = load(&s, "1.0");
		check(rc == AME_INVALIDVALUE && AM_Errno == AME_INVALIDVALUE,
		      "unsorted input refused");
		check(access(FILE_NAME, F_OK) != 0, "no file after unsorted input");
	}
	start(&s, 8, 10, 0, 0);
	check(load(&s, "1.0") == AME_OK, "load after refused loads");
	fd = open_index();
	expect_shape(fd, 8, 1, 1, 1, "10 keys after 25 refused loads");
	close_index(fd);

	printf("test_bulk done!\n");
	return 0;
}
