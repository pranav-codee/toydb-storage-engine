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

static void
fail(const char *message)
{
	fprintf(stderr, "test_hf_load: %s\n", message);
	exit(1);
}

static int
student_record(char *line)
{
	int fields;
	char *p;

	if (line[0] == '\0')
		return 0;
	p = strchr(line, '\n');
	if (p != NULL)
		*p = '\0';
	fields = 1;
	for (p = line; *p != '\0'; p++)
		if (*p == ';')
			fields++;
	return fields == 16;
}

int
main(int argc, char **argv)
{
	FILE *input;
	HFSpaceStats stats;
	char line[2048];
	char record[HF_MAX_RECLEN];
	const char *data_file;
	int fd;
	int scanfd;
	int rid;
	int len;
	int count;
	int longest;
	int status;

	status = Config_Init(argc, argv);
	if (status != CONFIG_OK && status != CONFIG_ENOFILE)
		fail("configuration initialization failed");
	data_file = Config_GetStr("data_file", "../data/student.txt");
	input = fopen(data_file, "r");
	if (input == NULL)
		fail("cannot open configured data_file");
	unlink("test_hf_load.pf");
	if (HF_CreateFile("test_hf_load.pf") != HF_OK)
		fail("heap-file creation failed");
	fd = HF_OpenFile("test_hf_load.pf");
	if (fd < 0)
		fail("heap-file open failed");
	count = 0;
	longest = 0;
	while (fgets(line, sizeof(line), input) != NULL) {
		if (!student_record(line))
			continue;
		len = (int)strlen(line);
		if (len > HF_MAX_RECLEN)
			fail("input record exceeds HF_MAX_RECLEN");
		memcpy(record, line, (size_t)len);
		if (HF_Insert(fd, record, len) < 0)
			fail("record insertion failed");
		if (len > longest)
			longest = len;
		count++;
	}
	fclose(input);
	if (count != 17813 || longest != 109)
		fail("dataset record count or longest line differs");
	scanfd = HF_ScanOpen(fd);
	if (scanfd < 0)
		fail("scan open failed");
	count = 0;
	while ((status = HF_ScanNext(scanfd, record, &len, &rid)) == HF_OK)
		count++;
	if (status != HFE_EOF || count != 17813)
		fail("scan count differs from inserted count");
	if (HF_ScanClose(scanfd) != HF_OK)
		fail("scan close failed");
	if (HF_GetSpaceStats(fd, &stats) != HF_OK)
		fail("space statistics failed");
	if (stats.pages != 458 || stats.records != 17813 ||
	    stats.useful_bytes != 1776824)
		fail("space statistics differ from hand-computed expectations");
	printf("pages=%ld records=%ld useful_bytes=%ld records_per_page=%ld "
	       "utilization=%.6f\n", stats.pages, stats.records,
	       stats.useful_bytes, stats.records_per_page, stats.utilization);
	if (HF_CloseFile(fd) != HF_OK)
		fail("heap-file close failed");
	if (PF_DestroyFile("test_hf_load.pf") != PFE_OK)
		fail("heap-file destroy failed");
	printf("test_hf_load done!\n");
	return 0;
}
