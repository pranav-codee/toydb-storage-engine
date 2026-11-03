/* test_hfstatic.c: fixed-slot layout, truncation, reuse, and statistics */
#include "hf.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int PF_Init(void);
extern int PF_DestroyFile(char *fname);

static void
check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "test_hfstatic: %s\n", message);
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	char data[HF_MAX_RECLEN + 1];
	char got[HF_MAX_RECLEN];
	char *fname;
	int config_status;
	int fd;
	int scanfd;
	int rid;
	int first_rid;
	int found;
	int len;
	int i;
	HFSpaceStats stats;

	fname = "test_hfstatic.pf";
	unlink(fname);
	config_status = Config_Init(argc, argv);
	check(config_status == CONFIG_OK || config_status == CONFIG_ENOFILE,
	      "config initialization");
	PF_Init();
	memset(data, 'A', sizeof(data));

	check(HF_StaticCreateFile(fname, 100) == HF_OK, "create");
	fd = HF_StaticOpenFile(fname);
	check(fd >= 0, "open");
	check(HF_StaticInsert(fd, data, 100) == HF_OK, "first insert");
	for (i = 1; i < 41; i++)
		check(HF_StaticInsert(fd, data, 100) == HF_OK, "fill first page");
	check(HF_StaticInsert(fd, data, 101) == HF_OK, "truncating insert");
	check(HF_StaticGetSpaceStats(fd, &stats) == HF_OK, "stats");
	check(stats.pages == 2 && stats.records == 42, "page and record counts");
	check(stats.useful_bytes == 4200 && stats.truncated == 1,
	      "useful and truncated bytes");
	check(stats.records_per_page == 21, "records per page");

	scanfd = HF_StaticScanOpen(fd);
	check(scanfd >= 0, "scan open");
	found = 0;
	first_rid = -1;
	while (HF_StaticScanNext(scanfd, got, &len, &rid) == HF_OK) {
		check(len == 100, "scan length");
		if (found == 0)
			first_rid = rid;
		found++;
	}
	check(found == 42, "scan count");
	check(HF_StaticScanClose(scanfd) == HF_OK, "scan close");
	check(first_rid >= 0, "first rid");
	check(HF_StaticGet(fd, first_rid, got, &len) == HF_OK &&
	      len == 100 && got[0] == 'A', "get");
	check(HF_StaticDelete(fd, first_rid) == HF_OK, "delete");
	check(HF_StaticGet(fd, first_rid, got, &len) == HFE_NOTFOUND,
	      "deleted get");
	check(HF_StaticInsert(fd, "xyz", 3) == HF_OK, "reuse insert");
	scanfd = HF_StaticScanOpen(fd);
	check(scanfd >= 0, "second scan open");
	found = 0;
	while (HF_StaticScanNext(scanfd, got, &len, &rid) == HF_OK) {
		if (rid == first_rid) {
			check(len == 100 && got[0] == 'x' && got[1] == 'y' &&
			      got[2] == 'z', "reused slot");
			found++;
		}
	}
	check(found == 1, "reused rid count");
	check(HF_StaticScanClose(scanfd) == HF_OK, "second scan close");
	check(HF_StaticCloseFile(fd) == HF_OK, "close");
	fd = HF_StaticOpenFile(fname);
	check(fd >= 0, "reopen");
	check(HF_StaticGetSpaceStats(fd, &stats) == HF_OK &&
	      stats.records == 42 && stats.truncated == 1, "reopen stats");
	check(HF_StaticCloseFile(fd) == HF_OK, "reclose");
	check(HF_StaticCloseFile(fd) == HFE_INVALID, "double close");
	check(PF_DestroyFile(fname) == 0, "destroy");

	fname = "test_hfstatic_256.pf";
	unlink(fname);
	check(HF_StaticCreateFile(fname, 256) == HF_OK, "second create");
	fd = HF_StaticOpenFile(fname);
	check(fd >= 0, "second open");
	for (i = 0; i < 16; i++)
		check(HF_StaticInsert(fd, data, 1) == HF_OK, "second fill");
	check(HF_StaticGetSpaceStats(fd, &stats) == HF_OK && stats.pages == 2,
	      "second capacity");
	check(HF_StaticCloseFile(fd) == HF_OK, "second close");
	check(PF_DestroyFile(fname) == 0, "second destroy");
	printf("test_hfstatic done!\n");
	return 0;
}
