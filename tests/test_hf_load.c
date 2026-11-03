/* test_hf_load.c: load student records and report slotted-page space use */
#include "hf.h"
#include "config.h"
#include "pf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int PF_DestroyFile(char *fname);
extern void PF_Init();

/* one inserted record: its rid, length and a hash of its bytes */
struct loaded {
	int rid;
	int len;
	unsigned long hash;
	int seen;
};

static void
fail(const char *message)
{
	fprintf(stderr, "test_hf_load: %s\n", message);
	exit(1);
}

static void
fail_line(const char *message, long lineno)
{
	fprintf(stderr, "test_hf_load: line %ld: %s\n", lineno, message);
	exit(1);
}

/*
 * Read one line into buf and strip its line ending ("\r\n", "\n", or a
 * lone trailing "\r"; student.txt uses CRLF).  A line that does not fit
 * in buf is an error: it would otherwise be split into pieces that the
 * field check drops silently.  Return 1 for a line, 0 at end of file.
 */
static int
read_line(FILE *input, char *buf, int size, long lineno)
{
	char *p;
	int c;
	int len;

	if (fgets(buf, size, input) == NULL)
		return 0;
	p = strchr(buf, '\n');
	if (p != NULL) {
		*p = '\0';
	} else {
		c = getc(input);
		if (c != EOF) {
			fprintf(stderr, "test_hf_load: line %ld does not fit the "
				"%d-byte read buffer\n", lineno, size);
			exit(1);
		}
	}
	len = (int)strlen(buf);
	if (len > 0 && buf[len - 1] == '\r')
		buf[len - 1] = '\0';
	return 1;
}

/* a student record has exactly 16 ';'-separated fields */
static int
student_record(const char *line)
{
	int fields;
	const char *p;

	if (line[0] == '\0')
		return 0;
	fields = 1;
	for (p = line; *p != '\0'; p++)
		if (*p == ';')
			fields++;
	return fields == 16;
}

/* FNV-1a over len bytes */
static unsigned long
hash_bytes(const char *data, int len)
{
	unsigned long h;
	int i;

	h = 2166136261UL;
	for (i = 0; i < len; i++) {
		h ^= (unsigned char)data[i];
		h = (h * 16777619UL) & 0xffffffffUL;
	}
	return h;
}

static int
by_rid(const void *a, const void *b)
{
	const struct loaded *x = (const struct loaded *)a;
	const struct loaded *y = (const struct loaded *)b;

	return (x->rid > y->rid) - (x->rid < y->rid);
}

int
main(int argc, char **argv)
{
	FILE *input;
	HFSpaceStats stats;
	struct loaded *recs;
	struct loaded *sorted;
	struct loaded key;
	struct loaded *hit;
	char line[2048];
	char record[HF_MAX_RECLEN];
	const char *data_file;
	const char *root;
	char path[4096];
	double expect_util;
	long useful;
	long lineno;
	long out_of_order;
	int capacity;
	int fd;
	int scanfd;
	int rid;
	int len;
	int count;
	int scanned;
	int longest;
	int status;
	int i;

	status = Config_Init(argc, argv);
	if (status != CONFIG_OK && status != CONFIG_ENOFILE)
		fail("configuration initialization failed");
	data_file = Config_GetStr("data_file", "../data/student.txt");
	/* make test runs us in a scratch dir, so a relative data_file is
	   taken from the repo root when TOYDB_ROOT is set */
	root = getenv("TOYDB_ROOT");
	if (data_file[0] != '/' && root != NULL && root[0] != '\0') {
		if (snprintf(path, sizeof(path), "%s/%s", root, data_file) >=
		    (int)sizeof(path))
			fail("resolved data_file path is too long");
		data_file = path;
	}
	input = fopen(data_file, "r");
	if (input == NULL) {
		printf("test_hf_load: data file not found at %s, SKIPPED\n",
		       data_file);
		printf("test_hf_load done!\n");
		return 0;
	}
	PF_Init();
	unlink("test_hf_load.pf");
	if (HF_CreateFile("test_hf_load.pf") != HF_OK)
		fail("heap-file creation failed");
	fd = HF_OpenFile("test_hf_load.pf");
	if (fd < 0)
		fail("heap-file open failed");
	capacity = 1024;
	recs = (struct loaded *)malloc((size_t)capacity * sizeof(*recs));
	if (recs == NULL)
		fail("allocation failed");
	count = 0;
	longest = 0;
	useful = 0;
	lineno = 0;
	while (read_line(input, line, (int)sizeof(line), lineno + 1)) {
		lineno++;
		if (!student_record(line))
			continue;
		len = (int)strlen(line);
		if (len > HF_MAX_RECLEN)
			fail_line("input record exceeds HF_MAX_RECLEN", lineno);
		if (count == capacity) {
			capacity *= 2;
			recs = (struct loaded *)realloc(recs,
				(size_t)capacity * sizeof(*recs));
			if (recs == NULL)
				fail("reallocation failed");
		}
		memcpy(record, line, (size_t)len);
		rid = HF_Insert(fd, record, len);
		if (rid < 0)
			fail_line("record insertion failed", lineno);
		recs[count].rid = rid;
		recs[count].len = len;
		recs[count].hash = hash_bytes(line, len);
		recs[count].seen = 0;
		useful += len;
		if (len > longest)
			longest = len;
		count++;
	}
	fclose(input);

	/* every record reads back by its rid */
	for (i = 0; i < count; i++) {
		if (HF_Get(fd, recs[i].rid, record, &len) != HF_OK)
			fail("get of an inserted rid failed");
		if (len != recs[i].len || hash_bytes(record, len) != recs[i].hash)
			fail("get returned different bytes than were inserted");
	}

	/*
	 * The scan returns records in page and slot order, which need not
	 * be insertion order: a short record may land in free space on an
	 * earlier page.  So sort a copy by rid unconditionally and look
	 * each scanned rid up with bsearch; also count how many rids came
	 * out lower than the one inserted before them, for the report.
	 */
	out_of_order = 0;
	for (i = 1; i < count; i++)
		if (recs[i].rid <= recs[i - 1].rid)
			out_of_order++;
	sorted = (struct loaded *)malloc((size_t)(count > 0 ? count : 1) *
					 sizeof(*sorted));
	if (sorted == NULL)
		fail("allocation failed");
	if (count > 0)
		memcpy(sorted, recs, (size_t)count * sizeof(*sorted));
	qsort(sorted, (size_t)count, sizeof(*sorted), by_rid);
	for (i = 1; i < count; i++)
		if (sorted[i].rid == sorted[i - 1].rid)
			fail("two inserts returned the same rid");

	scanfd = HF_ScanOpen(fd);
	if (scanfd < 0)
		fail("scan open failed");
	scanned = 0;
	while ((status = HF_ScanNext(scanfd, record, &len, &rid)) == HF_OK) {
		key.rid = rid;
		hit = (struct loaded *)bsearch(&key, sorted, (size_t)count,
					       sizeof(*sorted), by_rid);
		if (hit == NULL)
			fail("scan returned a rid that was never inserted");
		if (hit->seen)
			fail("scan returned a rid twice");
		hit->seen = 1;
		if (len != hit->len || hash_bytes(record, len) != hit->hash)
			fail("scan returned different bytes than were inserted");
		scanned++;
	}
	if (status != HFE_EOF)
		fail("scan ended with an error");
	if (scanned != count)
		fail("scan count differs from inserted count");
	if (HF_ScanClose(scanfd) != HF_OK)
		fail("scan close failed");

	if (HF_GetSpaceStats(fd, &stats) != HF_OK)
		fail("space statistics failed");
	if (stats.records != count)
		fail("stats records differ from inserted count");
	if (stats.useful_bytes != useful)
		fail("stats useful_bytes differ from the inserted bytes");
	expect_util = stats.pages == 0 ? 0.0 :
		(double)useful / ((double)stats.pages * (double)PF_PAGE_SIZE);
	if (stats.utilization - expect_util > 1e-12 ||
	    expect_util - stats.utilization > 1e-12)
		fail("utilization differs from useful/(pages*PF_PAGE_SIZE)");
	if (count == 0 && stats.pages != 0)
		fail("empty file has data pages");
	printf("records=%d longest=%d useful_bytes=%ld out_of_order_rids=%ld\n",
	       count, longest, useful, out_of_order);
	printf("pages=%ld records=%ld useful_bytes=%ld records_per_page=%ld "
	       "utilization=%.6f\n", stats.pages, stats.records,
	       stats.useful_bytes, stats.records_per_page, stats.utilization);
	if (HF_CloseFile(fd) != HF_OK)
		fail("heap-file close failed");
	if (PF_DestroyFile("test_hf_load.pf") != PFE_OK)
		fail("heap-file destroy failed");
	free(recs);
	free(sorted);
	printf("test_hf_load done!\n");
	return 0;
}
