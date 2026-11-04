/* test_bulk_data.c: bulk-load the roll-number index from student.txt */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "am.h"
#include "ambulk.h"
#include "config.h"
#include "hf.h"
#include "pf.h"

extern void exit(int);
extern void free(void *);
extern char *getenv(const char *);
extern char *realloc(void *, size_t);
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
extern void PF_Init();
extern int PF_DestroyFile(char *fname);
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_GetNextPage(int fd, int *pagenum, char **pagebuf);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);
extern int AM_OpenIndexScan(int fileDesc, int attrType, int attrLength,
			    int op, char *value);
extern int AM_FindNextEntry(int scanDesc);
extern int AM_CloseIndexScan(int scanDesc);
extern int AM_EmptyStack();
extern int AM_DestroyIndex(char *fileName, int indexNo);
extern int AM_BulkLoad(char *relName, int indexNo, char attrType,
		       int attrLength, AMBulkNext next, void *context);

#define KEY_LENGTH 8

struct indexed_record {
	char key[KEY_LENGTH];
	int rid;
};

struct stream {
	struct indexed_record *records;
	int count;
	int next;
};

static void
fail(const char *message)
{
	fprintf(stderr, "test_bulk_data: %s\n", message);
	exit(1);
}

static void
fail_line(const char *message, long line_number)
{
	fprintf(stderr, "test_bulk_data: line %ld: %s\n", line_number, message);
	exit(1);
}

static int
read_line(FILE *input, char *line, int size, long line_number)
{
	char *newline;
	int c;
	int length;

	if (fgets(line, size, input) == NULL)
		return 0;
	newline = strchr(line, '\n');
	if (newline != NULL) {
		*newline = '\0';
	} else {
		c = getc(input);
		if (c != EOF)
			fail_line("line does not fit the read buffer", line_number);
	}
	length = (int)strlen(line);
	if (length > 0 && line[length - 1] == '\r')
		line[length - 1] = '\0';
	return 1;
}

static int
student_record(const char *line)
{
	const char *p;
	int fields;

	if (line[0] == '\0')
		return 0;
	fields = 1;
	for (p = line; *p != '\0'; p++)
		if (*p == ';')
			fields++;
	return fields == 16;
}

static int
roll_number(const char *line, char key[KEY_LENGTH])
{
	const char *first;
	const char *second;

	first = strchr(line, ';');
	if (first == NULL)
		return 0;
	first++;
	second = strchr(first, ';');
	if (second == NULL || second - first != KEY_LENGTH)
		return 0;
	memcpy(key, first, KEY_LENGTH);
	return 1;
}

static int
compare_records(const void *left, const void *right)
{
	const struct indexed_record *a;
	const struct indexed_record *b;
	int result;

	a = (const struct indexed_record *)left;
	b = (const struct indexed_record *)right;
	result = memcmp(a->key, b->key, KEY_LENGTH);
	if (result != 0)
		return result;
	return (a->rid > b->rid) - (a->rid < b->rid);
}

static int
compare_rids(const void *left, const void *right)
{
	const struct indexed_record *a;
	const struct indexed_record *b;

	a = (const struct indexed_record *)left;
	b = (const struct indexed_record *)right;
	return (a->rid > b->rid) - (a->rid < b->rid);
}

static int
next_record(void *context, char *key, int *rid)
{
	struct stream *stream;

	stream = (struct stream *)context;
	if (stream->next == stream->count)
		return AM_BULK_NEXT_EOF;
	memcpy(key, stream->records[stream->next].key, KEY_LENGTH);
	*rid = stream->records[stream->next].rid;
	stream->next++;
	return AM_BULK_NEXT_ITEM;
}

static int
find_expected(struct indexed_record *records, int count, int rid)
{
	int low;
	int high;
	int middle;

	low = 0;
	high = count;
	while (low < high) {
		middle = low + (high - low) / 2;
		if (records[middle].rid < rid)
			low = middle + 1;
		else
			high = middle;
	}
	return low < count && records[low].rid == rid;
}

static int
expected_index(struct indexed_record *records, int count, int rid)
{
	int low;
	int high;
	int middle;

	low = 0;
	high = count;
	while (low < high) {
		middle = low + (high - low) / 2;
		if (records[middle].rid < rid)
			low = middle + 1;
		else
			high = middle;
	}
	return low < count && records[low].rid == rid ? low : -1;
}

static int
leftmost_leaf(int fd, int *height)
{
	char *page;
	int page_number;
	int child;
	int level;

	page_number = 0;
	level = 1;
	for (;;) {
		if (PF_GetThisPage(fd, page_number, &page) != PFE_OK)
			fail("could not read a tree page");
		if (page[0] == 'l') {
			if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
				fail("could not unfix a tree page");
			*height = level;
			return page_number;
		}
		if (page[0] != 'i')
			fail("tree contains an unknown page type");
		memcpy(&child, page + AM_sint, sizeof(child));
		if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
			fail("could not unfix an internal page");
		page_number = child;
		level++;
		if (level > 32)
			fail("tree height is unreasonable");
	}
}

static int
count_leaves(int fd, int first_leaf)
{
	char *page;
	AM_LEAFHEADER header;
	int page_number;
	int next;
	int leaves;

	page_number = first_leaf;
	leaves = 0;
	while (page_number != AM_NULL_PAGE) {
		if (PF_GetThisPage(fd, page_number, &page) != PFE_OK)
			fail("could not read a leaf page");
		memcpy(&header, page, sizeof(header));
		next = header.nextLeafPage;
		if (page[0] != 'l')
			fail("leaf chain contains a non-leaf page");
		if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
			fail("could not unfix a leaf page");
		leaves++;
		page_number = next;
		if (leaves > 10000)
			fail("leaf chain is cyclic");
	}
	return leaves;
}

static void
check_equal_scans(int fd, struct indexed_record *records,
		  struct indexed_record *by_rid, int count)
{
	int i;
	int scan;
	int rid;
	int matches;

	for (i = 0; i < count; i++) {
		scan = AM_OpenIndexScan(fd, 'c', KEY_LENGTH, EQUAL,
					records[i].key);
		if (scan < 0)
			fail("could not open an equality scan");
		matches = 0;
		while ((rid = AM_FindNextEntry(scan)) >= 0) {
			if (!find_expected(by_rid, count, rid))
				fail("equality scan returned an unknown RID");
			if (memcmp(by_rid[expected_index(by_rid, count, rid)].key,
				   records[i].key, KEY_LENGTH) != 0)
				fail("equality scan returned a RID for another key");
			if (rid == records[i].rid)
				matches++;
		}
		if (rid != AME_EOF || matches != 1)
			fail("equality scan did not return the record RID");
		if (AM_CloseIndexScan(scan) != AME_OK)
			fail("could not close an equality scan");
		AM_EmptyStack();
	}
}

static void
check_full_scan(int fd, struct indexed_record *by_rid, int count)
{
	char previous[KEY_LENGTH];
	char *seen;
	int scan;
	int rid;
	int i;
	int index;

	scan = AM_OpenIndexScan(fd, 'c', KEY_LENGTH, ALL, NULL);
	if (scan < 0)
		fail("could not open a full scan");
	seen = (char *)calloc((size_t)count + 1, sizeof(*seen));
	if (seen == NULL)
		fail("full scan seen-set allocation failed");
	for (i = 0; i < count; i++) {
		rid = AM_FindNextEntry(scan);
		if (rid < 0)
			fail("full scan ended before all records");
		index = expected_index(by_rid, count, rid);
		if (index < 0 || seen[index])
			fail("full scan returned an unknown RID");
		seen[index] = 1;
		if (i > 0 && memcmp(previous, by_rid[index].key, KEY_LENGTH) > 0)
			fail("full scan keys are out of order");
		memcpy(previous, by_rid[index].key, KEY_LENGTH);
	}
	if (AM_FindNextEntry(scan) != AME_EOF)
		fail("full scan returned too many records");
	if (AM_CloseIndexScan(scan) != AME_OK)
		fail("could not close the full scan");
	free(seen);
}

int
main(int argc, char **argv)
{
	FILE *input;
	HFSpaceStats stats;
	struct indexed_record *records;
	struct indexed_record *by_rid;
	struct stream stream;
	char line[2048];
	char key[KEY_LENGTH];
	char path[4096];
	const char *data_file;
	const char *root;
	double leaf_fill;
	int distinct;
	int leaf_capacity;
	int i;
	long line_number;
	int capacity;
	int count;
	int fd;
	int index_fd;
	int rid;
	int status;
	int first_leaf;
	int leaves;
	int height;
	int pages;
	int page_number;
	char *page;

	status = Config_Init(argc, argv);
	if (status != CONFIG_OK && status != CONFIG_ENOFILE)
		fail("configuration initialization failed");
	data_file = Config_GetStr("data_file", "../data/student.txt");
	root = getenv("TOYDB_ROOT");
	if (data_file[0] != '/' && root != NULL && root[0] != '\0') {
		if (snprintf(path, sizeof(path), "%s/%s", root, data_file) >=
		    (int)sizeof(path))
			fail("resolved data_file path is too long");
		data_file = path;
	}
	input = fopen(data_file, "r");
	if (input == NULL) {
		printf("test_bulk_data: data file not found at %s, SKIPPED\n",
		       data_file);
		printf("test_bulk_data done!\n");
		return 0;
	}

	PF_Init();
	unlink("test_bulk_data.pf");
	unlink("test_bulk_data.0");
	if (HF_CreateFile("test_bulk_data.pf") != HF_OK)
		fail("heap-file creation failed");
	fd = HF_OpenFile("test_bulk_data.pf");
	if (fd < 0)
		fail("heap-file open failed");
	capacity = 1024;
	records = (struct indexed_record *)malloc((size_t)capacity *
						  sizeof(*records));
	if (records == NULL)
		fail("record allocation failed");
	count = 0;
	line_number = 0;
	while (read_line(input, line, (int)sizeof(line), line_number + 1)) {
		line_number++;
		if (!student_record(line))
			continue;
		if (!roll_number(line, key))
			fail_line("field 2 is not exactly eight bytes", line_number);
		if (count == capacity) {
			capacity *= 2;
			records = (struct indexed_record *)realloc(records,
				(size_t)capacity * sizeof(*records));
			if (records == NULL)
				fail("record reallocation failed");
		}
		rid = HF_Insert(fd, line, (int)strlen(line));
		if (rid < 0)
			fail_line("heap insertion failed", line_number);
		memcpy(records[count].key, key, KEY_LENGTH);
		records[count].rid = rid;
		count++;
	}
	fclose(input);
	if (HF_GetSpaceStats(fd, &stats) != HF_OK)
		fail("heap statistics failed");
	if (HF_CloseFile(fd) != HF_OK)
		fail("heap-file close failed");
	qsort(records, (size_t)count, sizeof(*records), compare_records);
	by_rid = (struct indexed_record *)malloc(((size_t)count + 1) *
						 sizeof(*by_rid));
	if (by_rid == NULL)
		fail("RID model allocation failed");
	memcpy(by_rid, records, (size_t)count * sizeof(*by_rid));
	qsort(by_rid, (size_t)count, sizeof(*by_rid), compare_rids);
	stream.records = records;
	stream.count = count;
	stream.next = 0;
	if (AM_BulkLoad("test_bulk_data", 0, 'c', KEY_LENGTH,
			next_record, &stream) != AME_OK)
		fail("bulk load failed");
	index_fd = PF_OpenFile("test_bulk_data.0");
	if (index_fd < 0)
		fail("index open failed");
	check_equal_scans(index_fd, records, by_rid, count);
	check_full_scan(index_fd, by_rid, count);
	first_leaf = leftmost_leaf(index_fd, &height);
	leaves = count_leaves(index_fd, first_leaf);
	pages = 0;
	page_number = -1;
	while (PF_GetNextPage(index_fd, &page_number, &page) == PFE_OK) {
		pages++;
		if (PF_UnfixPage(index_fd, page_number, FALSE) != PFE_OK)
			fail("could not unfix an index page");
	}
	/* everything below comes from the records read, not from constants
	   about the data set; an empty or header-only file is valid input */
	distinct = 0;
	for (i = 0; i < count; i++)
		if (i == 0 || memcmp(records[i - 1].key, records[i].key,
				     KEY_LENGTH) != 0)
			distinct++;
	/* keys with one rid each that fit a leaf, as in AM_BulkLoad */
	leaf_capacity = (PF_PAGE_SIZE - (int)sizeof(AM_LEAFHEADER)) /
			(KEY_LENGTH + 8);
	leaf_fill = (double)distinct / leaves / leaf_capacity;
	if (count == 0 && (pages != 1 || leaves != 1 || height != 1))
		fail("empty input did not give a single empty leaf");
	printf("records=%d distinct_keys=%d data_pages=%ld\n", count,
	       distinct, stats.pages);
	printf("index_pages=%d leaves=%d tree_height=%d\n", pages, leaves,
	       height);
	printf("leaf_fill=%.3f (%.1f keys per leaf / capacity %d)\n",
	       leaf_fill, (double)distinct / leaves, leaf_capacity);
	if (PF_CloseFile(index_fd) != PFE_OK)
		fail("index close failed");
	if (AM_DestroyIndex("test_bulk_data", 0) != AME_OK)
		fail("index destroy failed");
	if (PF_DestroyFile("test_bulk_data.pf") != PFE_OK)
		fail("heap destroy failed");
	free(records);
	free(by_rid);
	printf("test_bulk_data done!\n");
	return 0;
}
