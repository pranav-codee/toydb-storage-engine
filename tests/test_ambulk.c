/* test_ambulk.c: sorted-stream bulk-load and AM-format scan checks */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "am.h"
#include "ambulk.h"
#include "config.h"
#include "pf.h"

extern void PF_Init();
extern int PF_DestroyFile(char *fname);
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);
extern int AM_OpenIndexScan(int fileDesc, int attrType, int attrLength,
			    int op, char *value);
extern int AM_FindNextEntry(int scanDesc);
extern int AM_CloseIndexScan(int scanDesc);
extern int AM_DestroyIndex(char *fileName, int indexNo);
extern int AM_BulkLoad(char *relName, int indexNo, char attrType,
		       int attrLength, AMBulkNext next, void *context);

struct stream {
	int count;
	int next;
	int bad;
};

static void
check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "test_ambulk: %s\n", message);
		_exit(1);
	}
}

static int
next_item(void *context, char *key, int *rid)
{
	struct stream *stream;
	int value;

	stream = (struct stream *)context;
	if (stream->next == stream->count)
		return AM_BULK_NEXT_EOF;
	value = stream->next / 2;
	if (stream->bad && stream->next == 3)
		value = 0;
	key[0] = (char)((value >> 24) & 0xff);
	key[1] = (char)((value >> 16) & 0xff);
	key[2] = (char)((value >> 8) & 0xff);
	key[3] = (char)(value & 0xff);
	*rid = stream->next;
	stream->next++;
	return AM_BULK_NEXT_ITEM;
}

static int
empty_item(void *context, char *key, int *rid)
{
	(void)context;
	(void)key;
	(void)rid;
	return AM_BULK_NEXT_EOF;
}

int
main(int argc, char **argv)
{
	struct stream stream;
	char *page;
	char name[64];
	int fd;
	int scan;
	int rid;
	int count;
	int config_status;
	int result;
	char seen[2600];

	config_status = Config_Init(argc, argv);
	check(config_status == CONFIG_OK || config_status == CONFIG_ENOFILE,
	      "config initialization");
	PF_Init();
	strcpy(name, "bulk_test.0");
	unlink(name);
	stream.count = 2600;
	stream.next = 0;
	stream.bad = 0;
	result = AM_BulkLoad("bulk_test", 0, 'c', (int)sizeof(int),
			     next_item, &stream);
	if (result != AME_OK)
		fprintf(stderr, "bulk result=%d AMerrno=%d\n", result, AM_Errno);
	check(result == AME_OK, "bulk load");
	fd = PF_OpenFile(name);
	check(fd >= 0, "open index");
	check(PF_GetThisPage(fd, 0, &page) == PFE_OK, "root page");
	check(page[0] == 'i', "root is internal");
	check(PF_UnfixPage(fd, 0, FALSE) == PFE_OK, "unfix root");
	scan = AM_OpenIndexScan(fd, 'c', (int)sizeof(int), ALL, NULL);
	check(scan >= 0, "open full scan");
	memset(seen, 0, sizeof(seen));
	count = 0;
	while ((rid = AM_FindNextEntry(scan)) >= 0) {
		check(rid >= 0 && rid < stream.count && !seen[rid],
		      "RID uniqueness");
		if (rid >= 0 && rid < stream.count)
			seen[rid] = 1;
		count++;
	}
	check(rid == AME_EOF && count == stream.count, "scan count");
	check(AM_CloseIndexScan(scan) == AME_OK, "close scan");
	check(PF_CloseFile(fd) == PFE_OK, "close index");
	check(AM_DestroyIndex("bulk_test", 0) == AME_OK, "destroy index");

	stream.count = 8;
	stream.next = 0;
	stream.bad = 1;
	check(AM_BulkLoad("bulk_bad", 0, 'c', (int)sizeof(int),
			  next_item, &stream) == AME_INVALIDVALUE,
	      "out of order rejection");
	unlink("bulk_bad.0");

	stream.next = 0;
	check(AM_BulkLoad("bulk_empty", 0, 'c', (int)sizeof(int),
			  empty_item, &stream) == AME_OK, "empty load");
	fd = PF_OpenFile("bulk_empty.0");
	check(fd >= 0, "open empty index");
	scan = AM_OpenIndexScan(fd, 'c', (int)sizeof(int), ALL, NULL);
	check(scan >= 0 && AM_FindNextEntry(scan) == AME_EOF,
	      "empty scan");
	check(AM_CloseIndexScan(scan) == AME_OK, "close empty scan");
	check(PF_CloseFile(fd) == PFE_OK, "close empty index");
	check(AM_DestroyIndex("bulk_empty", 0) == AME_OK, "destroy empty");
	stream.count = 2;
	stream.next = 0;
	check(AM_BulkLoad("bulk_one", 0, 'c', (int)sizeof(int),
			  next_item, &stream) == AME_OK, "single-leaf load");
	fd = PF_OpenFile("bulk_one.0");
	check(fd >= 0, "open single-leaf index");
	check(PF_GetThisPage(fd, 0, &page) == PFE_OK, "single root page");
	check(page[0] == 'l', "single root is leaf");
	check(PF_UnfixPage(fd, 0, FALSE) == PFE_OK, "unfix single root");
	check(PF_CloseFile(fd) == PFE_OK, "close single-leaf index");
	check(AM_DestroyIndex("bulk_one", 0) == AME_OK,
	      "destroy single-leaf index");
	printf("test_ambulk done!\n");
	return 0;
}
