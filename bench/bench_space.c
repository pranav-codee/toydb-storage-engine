/* bench_space.c: compare slotted and fixed-length heap-file space use */
#include "hf.h"
#include "config.h"
#include "pf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int PF_DestroyFile(char *fname);
extern void PF_Init();

struct input_record {
	int len;
	char data[HF_MAX_RECLEN];
};

static void
fail(const char *message)
{
	fprintf(stderr, "bench_space: %s\n", message);
	exit(1);
}

static unsigned long
next_random(unsigned long *state)
{
	*state = *state * 1103515245UL + 12345UL;
	return (*state >> 16) & 0x7fffUL;
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
			fprintf(stderr, "bench_space: line %ld does not fit the "
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

static void
print_row(const char *layout, int length, HFSpaceStats *stats)
{
	printf("%s,%d,%ld,%ld,%ld,%ld,%ld,%.6f\n", layout, length,
	       stats->records, stats->truncated, stats->pages,
	       stats->useful_bytes, stats->records_per_page,
	       stats->utilization);
}

static int
load_input(struct input_record **out, int *count, const char *data_file)
{
	FILE *input;
	struct input_record *records;
	char line[2048];
	int capacity;
	int len;
	int n;
	long lineno;
	char path[4096];
	const char *root;

	root = getenv("TOYDB_ROOT");
	if (data_file[0] != '/' && root != NULL && root[0] != '\0') {
		if (snprintf(path, sizeof(path), "%s/%s", root, data_file) >=
		    (int)sizeof(path))
			fail("resolved data_file path is too long");
		data_file = path;
	}
	input = fopen(data_file, "r");
	if (input == NULL)
		return 1;
	capacity = 1024;
	records = (struct input_record *)malloc((size_t)capacity *
						sizeof(*records));
	if (records == NULL)
		fail("input allocation failed");
	n = 0;
	lineno = 0;
	while (read_line(input, line, (int)sizeof(line), lineno + 1)) {
		lineno++;
		if (!student_record(line))
			continue;
		len = (int)strlen(line);
		if (len > HF_MAX_RECLEN) {
			fprintf(stderr, "bench_space: line %ld: input record "
				"exceeds HF_MAX_RECLEN\n", lineno);
			exit(1);
		}
		if (n == capacity) {
			capacity *= 2;
			records = (struct input_record *)realloc(records,
				(size_t)capacity * sizeof(*records));
			if (records == NULL)
				fail("input reallocation failed");
		}
		records[n].len = len;
		memcpy(records[n].data, line, (size_t)len);
		n++;
	}
	fclose(input);
	*out = records;
	*count = n;
	return 0;
}

static void
shuffle_indices(int *indices, int count, unsigned long *state)
{
	int i;
	int j;
	int temp;

	for (i = count - 1; i > 0; i--) {
		j = (int)(next_random(state) % (unsigned long)(i + 1));
		temp = indices[i];
		indices[i] = indices[j];
		indices[j] = temp;
	}
}

static int
churn_count(int count, double fraction)
{
	int selected;

	selected = (int)(fraction * (double)count);
	if (selected < 0)
		selected = 0;
	if (selected > count)
		selected = count;
	return selected;
}

static int
run_slotted(struct input_record *records, int count, double fraction,
	    unsigned long seed, HFSpaceStats *before, HFSpaceStats *after)
{
	int *rids;
	int *indices;
	int fd;
	int i;
	int selected;
	long useful;
	char fname[] = "bench_space_slotted.pf";

	unlink(fname);
	if (HF_CreateFile(fname) != HF_OK)
		fail("slotted create failed");
	fd = HF_OpenFile(fname);
	if (fd < 0)
		fail("slotted open failed");
	rids = (int *)malloc((size_t)(count > 0 ? count : 1) * sizeof(*rids));
	indices = (int *)malloc((size_t)(count > 0 ? count : 1) *
				sizeof(*indices));
	if (rids == NULL || indices == NULL)
		fail("slotted churn allocation failed");
	for (i = 0; i < count; i++) {
		rids[i] = HF_Insert(fd, records[i].data, records[i].len);
		indices[i] = i;
		if (rids[i] < 0)
			fail("slotted insert failed");
	}
	if (HF_GetSpaceStats(fd, before) != HF_OK)
		fail("slotted statistics failed");
	useful = 0;
	for (i = 0; i < count; i++)
		useful += records[i].len;
	if (before->records != count || before->useful_bytes != useful)
		fail("slotted statistics differ from the inserted records");
	selected = churn_count(count, fraction);
	shuffle_indices(indices, count, &seed);
	for (i = 0; i < selected; i++)
		if (HF_Delete(fd, rids[indices[i]]) != HF_OK)
			fail("slotted churn delete failed");
	for (i = 0; i < selected; i++)
		if (HF_Insert(fd, records[indices[i]].data,
			      records[indices[i]].len) < 0)
			fail("slotted churn insert failed");
	if (HF_GetSpaceStats(fd, after) != HF_OK)
		fail("slotted churn statistics failed");
	if (HF_CloseFile(fd) != HF_OK || PF_DestroyFile(fname) != PFE_OK)
		fail("slotted cleanup failed");
	free(rids);
	free(indices);
	return HF_OK;
}

static int
run_static(struct input_record *records, int count, int length,
	   double fraction, unsigned long seed, HFSpaceStats *before,
	   HFSpaceStats *after)
{
	int *rids;
	int *indices;
	int fd;
	int scanfd;
	int i;
	int selected;
	int rid;
	int len;
	long useful;
	long cut;
	char buf[HF_MAX_RECLEN];
	char fname[64];

	if (snprintf(fname, sizeof(fname), "bench_space_static_%d.pf",
		     length) >= (int)sizeof(fname))
		fail("static file name is too long");
	unlink(fname);
	if (HF_StaticCreateFile(fname, length) != HF_OK)
		fail("static create failed");
	fd = HF_StaticOpenFile(fname);
	if (fd < 0)
		fail("static open failed");
	for (i = 0; i < count; i++)
		if (HF_StaticInsert(fd, records[i].data, records[i].len) != HF_OK)
			fail("static insert failed");
	if (HF_StaticGetSpaceStats(fd, before) != HF_OK)
		fail("static statistics failed");
	useful = 0;
	cut = 0;
	for (i = 0; i < count; i++) {
		useful += records[i].len < length ? records[i].len : length;
		if (records[i].len > length)
			cut++;
	}
	if (before->records != count || before->useful_bytes != useful ||
	    before->truncated != cut)
		fail("static statistics differ from the inserted records");
	rids = (int *)malloc((size_t)(count > 0 ? count : 1) * sizeof(*rids));
	indices = (int *)malloc((size_t)(count > 0 ? count : 1) *
				sizeof(*indices));
	if (rids == NULL || indices == NULL)
		fail("static churn allocation failed");
	scanfd = HF_StaticScanOpen(fd);
	if (scanfd < 0)
		fail("static scan open failed");
	i = 0;
	while (i < count &&
	       HF_StaticScanNext(scanfd, buf, &len, &rid) == HF_OK) {
		rids[i] = rid;
		indices[i] = i;
		i++;
	}
	if (i != count || HF_StaticScanClose(scanfd) != HF_OK)
		fail("static scan failed");
	selected = churn_count(count, fraction);
	shuffle_indices(indices, count, &seed);
	for (i = 0; i < selected; i++)
		if (HF_StaticDelete(fd, rids[indices[i]]) != HF_OK)
			fail("static churn delete failed");
	for (i = 0; i < selected; i++)
		if (HF_StaticInsert(fd, records[indices[i]].data,
				    records[indices[i]].len) != HF_OK)
			fail("static churn insert failed");
	if (HF_StaticGetSpaceStats(fd, after) != HF_OK)
		fail("static churn statistics failed");
	if (HF_StaticCloseFile(fd) != HF_OK || PF_DestroyFile(fname) != PFE_OK)
		fail("static cleanup failed");
	free(rids);
	free(indices);
	return HF_OK;
}

static int
check_mode(void)
{
	HFSpaceStats slotted;
	HFSpaceStats fixed;
	char record[10];
	int fd;
	int i;
	char slotted_name[] = "bench_space_check_hf.pf";
	char static_name[] = "bench_space_check_static.pf";

	memset(record, 'x', sizeof(record));
	unlink(slotted_name);
	if (HF_CreateFile(slotted_name) != HF_OK)
		return 1;
	fd = HF_OpenFile(slotted_name);
	if (fd < 0)
		return 1;
	for (i = 0; i < 300; i++)
		if (HF_Insert(fd, record, 10) < 0)
			return 1;
	if (HF_GetSpaceStats(fd, &slotted) != HF_OK ||
	    slotted.pages != 2 || slotted.records != 300 ||
	    slotted.useful_bytes != 3000 ||
	    slotted.utilization != 3000.0 / (2.0 * PF_PAGE_SIZE))
		return 1;
	if (HF_CloseFile(fd) != HF_OK || PF_DestroyFile(slotted_name) != PFE_OK)
		return 1;
	unlink(static_name);
	if (HF_StaticCreateFile(static_name, 10) != HF_OK)
		return 1;
	fd = HF_StaticOpenFile(static_name);
	if (fd < 0)
		return 1;
	for (i = 0; i < 300; i++)
		if (HF_StaticInsert(fd, record, 10) != HF_OK)
			return 1;
	if (HF_StaticGetSpaceStats(fd, &fixed) != HF_OK ||
	    fixed.pages != 1 || fixed.records != 300 ||
	    fixed.useful_bytes != 3000 ||
	    fixed.utilization != 3000.0 / (double)PF_PAGE_SIZE)
		return 1;
	if (HF_StaticCloseFile(fd) != HF_OK || PF_DestroyFile(static_name) != PFE_OK)
		return 1;
	printf("bench_space check: slotted pages=%ld static pages=%ld\n",
	       slotted.pages, fixed.pages);
	printf("bench_space check done!\n");
	return 0;
}

int
main(int argc, char **argv)
{
	struct input_record *records;
	HFSpaceStats before;
	HFSpaceStats after;
	const char *data_file;
	int lengths[CONFIG_MAXLIST];
	int length_count;
	int count;
	int status;
	int i;
	int check;
	double fraction;
	unsigned long seed;

	status = Config_Init(argc, argv);
	if (status != CONFIG_OK && status != CONFIG_ENOFILE)
		fail("configuration initialization failed");
	PF_Init();
	check = 0;
	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "check") == 0)
			check = 1;
	if (check)
		return check_mode();
	data_file = Config_GetStr("data_file", "../data/student.txt");
	if (load_input(&records, &count, data_file) != 0) {
		printf("bench_space: data file not found at %s, SKIPPED\n",
		       data_file);
		printf("bench_space done!\n");
		return 0;
	}
	length_count = Config_GetIntList("static_lengths", lengths,
					 CONFIG_MAXLIST,
					 "100,104,110,128,160,200,256");
	fraction = Config_GetDouble("churn_fraction", 0.3);
	seed = (unsigned long)Config_GetInt("seed", 42);
	if (fraction < 0.0 || fraction > 1.0 || length_count <= 0)
		fail("invalid static or churn configuration");
	printf("layout,L,records,truncated,data_pages,useful_bytes,"
	       "records_per_page,utilization\n");
	run_slotted(records, count, fraction, seed, &before, &after);
	print_row("slotted", 0, &before);
	print_row("slotted_churn", 0, &after);
	for (i = 0; i < length_count; i++) {
		run_static(records, count, lengths[i], fraction, seed,
			   &before, &after);
		print_row("static", lengths[i], &before);
		print_row("static_churn", lengths[i], &after);
	}
	free(records);
	printf("bench_space done!\n");
	return 0;
}
