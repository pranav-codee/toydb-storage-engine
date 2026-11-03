/* test_hfstatic.c: fixed-slot layout, stored lengths, truncation, reuse,
   statistics and scans */
#include "hf.h"
#include "config.h"
#include "pf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int PF_DestroyFile(char *fname);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);

static void
check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "test_hfstatic: %s\n", message);
		exit(1);
	}
}

static void
check_stats(int fd, long pages, long records, long useful, long truncated,
	    const char *message)
{
	HFSpaceStats stats;

	check(HF_StaticGetSpaceStats(fd, &stats) == HF_OK, message);
	if (stats.pages != pages || stats.records != records ||
	    stats.useful_bytes != useful || stats.truncated != truncated) {
		fprintf(stderr, "test_hfstatic: %s: got pages=%ld records=%ld "
			"useful=%ld truncated=%ld, want %ld %ld %ld %ld\n",
			message, stats.pages, stats.records, stats.useful_bytes,
			stats.truncated, pages, records, useful, truncated);
		exit(1);
	}
	check(stats.records_per_page == (pages ? records / pages : 0),
	      "records per page");
	check(stats.utilization == (pages ? (double)useful /
				    ((double)pages * PF_PAGE_SIZE) : 0.0),
	      "utilization");
}

/* get rid and check its length and that every byte is fill */
static void
check_get(int fd, int rid, int want_len, char fill, const char *message)
{
	char got[HF_MAX_RECLEN];
	int len;
	int i;

	len = -1;
	check(HF_StaticGet(fd, rid, got, &len) == HF_OK, message);
	check(len == want_len, message);
	for (i = 0; i < len; i++)
		check(got[i] == fill, message);
}

/* expected scan result, in page and slot order */
struct expect {
	int rid;
	int len;
	char fill;
};

static void
check_scan(int fd, struct expect *want, int n, const char *message)
{
	char got[HF_MAX_RECLEN];
	int scanfd;
	int rid;
	int len;
	int found;
	int status;
	int i;

	scanfd = HF_StaticScanOpen(fd);
	check(scanfd >= 0, message);
	found = 0;
	while ((status = HF_StaticScanNext(scanfd, got, &len, &rid)) == HF_OK) {
		check(found < n, message);
		check(rid == want[found].rid, message);
		check(len == want[found].len, message);
		for (i = 0; i < len; i++)
			check(got[i] == want[found].fill, message);
		found++;
	}
	check(status == HFE_EOF && found == n, message);
	check(HF_StaticScanClose(scanfd) == HF_OK, message);
}

int
main(int argc, char **argv)
{
	char data[HF_MAX_RECLEN + 1];
	char *fname;
	char *page;
	struct expect want[64];
	int config_status;
	int fd;
	int scanfd;
	int rid;
	int len;
	int n;
	int i;

	fname = "test_hfstatic.pf";
	unlink(fname);
	config_status = Config_Init(argc, argv);
	check(config_status == CONFIG_OK || config_status == CONFIG_ENOFILE,
	      "config initialization");
	PF_Init();

	/* invalid slot lengths */
	check(HF_StaticCreateFile(fname, 0) == HFE_INVALID, "create L=0");
	check(HF_StaticCreateFile(fname, -1) == HFE_INVALID, "create L=-1");
	check(HF_StaticCreateFile(fname, HF_MAX_RECLEN + 1) == HFE_INVALID,
	      "create L=HF_MAX_RECLEN+1");

	/*
	 * L = 100: slots per page = (4096 - 4) / (100 + 2) = 40.
	 * Page 1 gets 40 records of 100 'A' bytes: rids 4096..4135.
	 */
	check(HF_StaticCreateFile(fname, 100) == HF_OK, "create");
	fd = HF_StaticOpenFile(fname);
	check(fd >= 0, "open");
	check_stats(fd, 0, 0, 0, 0, "empty stats");
	memset(data, 'A', sizeof(data));
	for (i = 0; i < 40; i++)
		check(HF_StaticInsert(fd, data, 100) == HF_OK, "fill first page");
	check_stats(fd, 1, 40, 4000, 0, "first page stats");

	/*
	 * Page 2: 101 'B' bytes (truncated to 100, rid 8192), "xyz" (3,
	 * rid 8193), an empty record (0, rid 8194), 50 'C' bytes (rid 8195).
	 * useful = 4000 + 100 + 3 + 0 + 50 = 4153, truncated = 1.
	 */
	memset(data, 'B', sizeof(data));
	check(HF_StaticInsert(fd, data, 101) == HF_OK, "truncating insert");
	check(HF_StaticInsert(fd, "xyz", 3) == HF_OK, "short insert");
	check(HF_StaticInsert(fd, NULL, 0) == HF_OK, "empty insert");
	memset(data, 'C', sizeof(data));
	check(HF_StaticInsert(fd, data, 50) == HF_OK, "50-byte insert");
	check_stats(fd, 2, 44, 4153, 1, "after short and truncated inserts");

	/*
	 * The length words on page 2: slot 0 at byte 4 is 100 with the
	 * truncated bit, 0x8064; slot 1 at byte 4 + 102 is 3, 0x0003.
	 */
	check(PF_GetThisPage(fd, 2, &page) == PFE_OK, "raw page");
	check((unsigned char)page[4] == 0x64 && (unsigned char)page[5] == 0x80,
	      "truncated flag in length word");
	check((unsigned char)page[106] == 0x03 && (unsigned char)page[107] == 0,
	      "short length word");
	check(PF_UnfixPage(fd, 2, FALSE) == PFE_OK, "raw unfix");

	/* get returns the stored length */
	check_get(fd, 8192, 100, 'B', "get truncated");
	check(HF_StaticGet(fd, 8193, data, &len) == HF_OK && len == 3 &&
	      memcmp(data, "xyz", 3) == 0, "get xyz");
	check_get(fd, 8194, 0, 0, "get empty record");
	check_get(fd, 8195, 50, 'C', "get 50");
	check(HF_StaticGet(fd, 8196, data, &len) == HFE_NOTFOUND, "get free slot");

	/* delete the truncated record: 43 records, 4053 bytes, 0 truncated */
	check(HF_StaticDelete(fd, 8192) == HF_OK, "delete truncated");
	check(HF_StaticGet(fd, 8192, data, &len) == HFE_NOTFOUND, "deleted get");
	check(HF_StaticDelete(fd, 8192) == HFE_NOTFOUND, "double delete");
	check_stats(fd, 2, 43, 4053, 0, "after deleting the truncated record");
	/* delete "xyz": 42 records, 4050 bytes */
	check(HF_StaticDelete(fd, 8193) == HF_OK, "delete short");
	check_stats(fd, 2, 42, 4050, 0, "after deleting the short record");
	/* delete a full record on page 1 (slot 5, rid 4101): 41, 3950 */
	check(HF_StaticDelete(fd, 4101) == HF_OK, "delete full");
	check_stats(fd, 2, 41, 3950, 0, "after deleting a full record");

	/*
	 * Reinsert 101 'D' bytes: the lowest free slot is page 1 slot 5,
	 * rid 4101; stored 100, truncated.  41 + 1 = 42 records, 4050 bytes,
	 * 1 truncated.  Then 1024 'E' bytes take page 2 slot 0 (rid 8192),
	 * stored 100, truncated: 43 records, 4150 bytes, 2 truncated.
	 */
	memset(data, 'D', sizeof(data));
	check(HF_StaticInsert(fd, data, 101) == HF_OK, "reinsert truncated");
	check_get(fd, 4101, 100, 'D', "reused slot on page 1");
	memset(data, 'E', sizeof(data));
	check(HF_StaticInsert(fd, data, HF_MAX_RECLEN) == HF_OK,
	      "insert HF_MAX_RECLEN");
	check_get(fd, 8192, 100, 'E', "reused slot on page 2");
	check_stats(fd, 2, 43, 4150, 2, "after reinserts");

	/* longer than HF_MAX_RECLEN: rejected, nothing changes */
	check(HF_StaticInsert(fd, data, HF_MAX_RECLEN + 1) == HFE_TOOLONG,
	      "insert HF_MAX_RECLEN+1");
	check(HF_StaticInsert(fd, data, -1) == HFE_INVALID, "negative length");
	check_stats(fd, 2, 43, 4150, 2, "after rejected inserts");

	/* expected scan: page 1 slots 0..39 ('D' at slot 5), page 2 */
	n = 0;
	for (i = 0; i < 40; i++) {
		want[n].rid = HF_RID(1, i);
		want[n].len = 100;
		want[n].fill = (char)(i == 5 ? 'D' : 'A');
		n++;
	}
	want[n].rid = 8192; want[n].len = 100; want[n].fill = 'E'; n++;
	want[n].rid = 8194; want[n].len = 0; want[n].fill = 0; n++;
	want[n].rid = 8195; want[n].len = 50; want[n].fill = 'C'; n++;
	check_scan(fd, want, n, "scan before reopen");

	/* reopen keeps the counters and the records */
	check(HF_StaticCloseFile(fd) == HF_OK, "close");
	fd = HF_StaticOpenFile(fname);
	check(fd >= 0, "reopen");
	check_stats(fd, 2, 43, 4150, 2, "reopen stats");
	check_scan(fd, want, n, "scan after reopen");
	check(HF_StaticGet(fd, 8193, data, &len) == HFE_NOTFOUND,
	      "deleted rid stays deleted after reopen");

	/* a scan whose file is closed returns HFE_INVALID */
	scanfd = HF_StaticScanOpen(fd);
	check(scanfd >= 0, "scan open before close");
	check(HF_StaticScanNext(scanfd, data, &len, &rid) == HF_OK,
	      "scan next before close");
	check(HF_StaticCloseFile(fd) == HF_OK, "close with open scan");
	check(HF_StaticScanNext(scanfd, data, &len, &rid) == HFE_INVALID,
	      "scan next after close");
	check(HF_StaticScanClose(scanfd) == HF_OK, "scan close after close");
	check(HF_StaticScanClose(scanfd) == HFE_INVALID, "scan double close");
	check(HF_StaticCloseFile(fd) == HFE_INVALID, "double close");
	check(HF_StaticInsert(fd, "x", 1) == HFE_INVALID, "insert after close");
	check(PF_DestroyFile(fname) == 0, "destroy");

	/* L = 256: (4096 - 4) / 258 = 15 slots, so 16 records need 2 pages */
	fname = "test_hfstatic_256.pf";
	unlink(fname);
	check(HF_StaticCreateFile(fname, 256) == HF_OK, "second create");
	fd = HF_StaticOpenFile(fname);
	check(fd >= 0, "second open");
	for (i = 0; i < 15; i++)
		check(HF_StaticInsert(fd, data, 1) == HF_OK, "second fill");
	check_stats(fd, 1, 15, 15, 0, "15 slots on one page");
	check(HF_StaticInsert(fd, data, 1) == HF_OK, "second page insert");
	check_stats(fd, 2, 16, 16, 0, "second capacity");
	check(HF_StaticCloseFile(fd) == HF_OK, "second close");
	check(PF_DestroyFile(fname) == 0, "second destroy");

	/* L = HF_MAX_RECLEN: (4096 - 4) / 1026 = 3 slots */
	fname = "test_hfstatic_max.pf";
	unlink(fname);
	check(HF_StaticCreateFile(fname, HF_MAX_RECLEN) == HF_OK, "max create");
	fd = HF_StaticOpenFile(fname);
	check(fd >= 0, "max open");
	for (i = 0; i < 4; i++)
		check(HF_StaticInsert(fd, data, HF_MAX_RECLEN) == HF_OK,
		      "max fill");
	check_stats(fd, 2, 4, 4L * HF_MAX_RECLEN, 0, "max capacity");
	check(HF_StaticCloseFile(fd) == HF_OK, "max close");
	check(PF_DestroyFile(fname) == 0, "max destroy");

	printf("test_hfstatic done!\n");
	return 0;
}
