/* test_hf.c: lifecycle and metadata persistence tests for heap files */
#include "hf.h"
#include "hfinternal.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int PF_DestroyFile(char *fname);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
static void
check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "test_hf: %s\n", message);
		exit(1);
	}
}

static unsigned long
next_random(unsigned long *state)
{
	*state = *state * 1103515245UL + 12345UL;
	return (*state >> 16) & 0x7fffUL;
}

int
main(int argc, char **argv)
{
	struct model_record {
		int rid;
		int len;
		int live;
		char data[64];
	};
	char *page;
	char *fname;
	char *model_name;
	int fd;
	int pf_fd;
	long data_pages;
	long records;
	int config_status;
	char record[100];
	char maxrec[HF_MAX_RECLEN];	/* source for the maximum-length insert */
	int rids[40];
	int i;
	int j;
	int model_count;
	int live_count;
	int choice;
	int model_index;
	int operation;
	int result;
	int got_len;
	int extra_rids[3];
	int scanfd;
	int scanfd2;
	int scan_rid;
	int scan_count;
	int expected_rids[43];
	char got[HF_MAX_RECLEN];
	unsigned long random_state;
	struct model_record model[256];

	fname = "test_hf.pf";
	unlink(fname);
	config_status = Config_Init(argc, argv);
	check(config_status == CONFIG_OK || config_status == CONFIG_ENOFILE,
	      "config initialization");
	PF_Init();
	check(HF_CreateFile(fname) == HF_OK, "create");
	fd = HF_OpenFile(fname);
	check(fd >= 0, "open");
	check(HF_CloseFile(fd) == HF_OK, "close");
	check(HF_CloseFile(fd) == HFE_INVALID, "double close");
	check(HF_CloseFile(-1) == HFE_INVALID, "bad handle");

	fd = HF_OpenFile(fname);
	check(fd >= 0, "reopen");
	memset(record, 'R', sizeof(record));
	memset(maxrec, 'M', sizeof(maxrec));
	for (i = 0; i < 40; i++) {
		rids[i] = HF_Insert(fd, record, (int)sizeof(record));
		check(rids[i] >= 0, "insert returns nonnegative rid");
		for (j = 0; j < i; j++)
			check(rids[i] != rids[j], "insert returns unique rid");
	}
	for (i = 0; i < 39; i++)
		check(rids[i] == HF_RID(1, i), "hand-computed first-page rid");
	check(rids[39] == HF_RID(2, 0), "hand-computed second-page rid");
	extra_rids[0] = HF_Insert(fd, record, 0);
	check(extra_rids[0] >= 0, "zero-length insert");
	extra_rids[1] = HF_Insert(fd, record, 1);
	check(extra_rids[1] >= 0, "one-byte insert");
	extra_rids[2] = HF_Insert(fd, maxrec, HF_MAX_RECLEN);
	check(extra_rids[2] >= 0, "maximum-length insert");
	check(HF_Insert(fd, record, HF_MAX_RECLEN + 1) == HFE_TOOLONG,
	      "record above maximum rejected");
	check(HF_Get(fd, rids[1], got, &got_len) == HF_OK &&
	      got_len == 100 && memcmp(got, record, 100) == 0,
	      "get returns inserted bytes");
	check(HF_Delete(fd, rids[0]) == HF_OK, "delete");
	check(HF_Get(fd, rids[0], got, &got_len) == HFE_NOTFOUND,
	      "deleted record is not found");
	check(HF_Delete(fd, rids[0]) == HFE_NOTFOUND,
	      "repeated delete is not found");
	check(HF_Get(fd, rids[1], got, &got_len) == HF_OK &&
	      got_len == 100 && memcmp(got, record, 100) == 0,
	      "other rid survives delete");
	result = HF_Insert(fd, record, 100);
	check(result == rids[0], "deleted slot and space are reused");
	check(HF_Get(fd, -1, got, &got_len) == HFE_INVALID,
	      "negative rid is invalid");
	check(HF_Get(fd, 0, got, &got_len) == HFE_INVALID,
	      "header-page rid is invalid");
	check(HF_Get(fd, HF_RID(3, 0), got, &got_len) == HFE_NOTFOUND,
	      "out-of-range page is not found");
	scanfd = HF_ScanOpen(fd);
	check(scanfd >= 0, "open scan");
	scanfd2 = HF_ScanOpen(fd);
	check(scanfd2 >= 0 && scanfd2 != scanfd, "open independent scan");
	scan_count = 0;
	expected_rids[scan_count++] = rids[0];
	for (i = 1; i < 39; i++)
		expected_rids[scan_count++] = rids[i];
	expected_rids[scan_count++] = extra_rids[0];
	expected_rids[scan_count++] = extra_rids[1];
	expected_rids[scan_count++] = rids[39];
	expected_rids[scan_count++] = extra_rids[2];
	for (i = 0; i < scan_count; i++) {
		check(HF_ScanNext(scanfd, got, &got_len, &scan_rid) == HF_OK,
		      "scan returns live record");
		check(scan_rid == expected_rids[i], "scan RID order");
	}
	check(HF_ScanNext(scanfd, got, &got_len, &scan_rid) == HFE_EOF,
	      "scan reaches EOF");
	check(HF_ScanNext(scanfd2, got, &got_len, &scan_rid) == HF_OK &&
	      scan_rid == expected_rids[0], "independent scan position");
	check(HF_ScanClose(scanfd) == HF_OK, "close scan");
	check(HF_ScanClose(scanfd) == HFE_INVALID, "double close scan");
	check(HF_ScanClose(scanfd2) == HF_OK, "close second scan");
	check(HF_CloseFile(fd) == HF_OK, "close after reopen");

	model_name = "test_hf_model.pf";
	unlink(model_name);
	check(HF_CreateFile(model_name) == HF_OK, "create model file");
	fd = HF_OpenFile(model_name);
	check(fd >= 0, "open model file");
	memset(model, 0, sizeof(model));
	model_count = 0;
	live_count = 0;
	random_state = 1234567UL;
	for (operation = 0; operation < 300; operation++) {
		choice = (int)(next_random(&random_state) % 100UL);
		if (choice < 50 && model_count < 256) {
			model[model_count].len =
				(int)(next_random(&random_state) % 64UL);
			for (i = 0; i < model[model_count].len; i++)
				model[model_count].data[i] = (char)(operation + i);
			model[model_count].rid = HF_Insert(fd, model[model_count].data,
							   model[model_count].len);
			check(model[model_count].rid >= 0, "model insert");
			model[model_count].live = 1;
			model_count++;
			live_count++;
		} else if (choice < 75 && live_count > 0) {
			model_index = (int)(next_random(&random_state) %
					    (unsigned long)model_count);
			for (j = 0; j < model_count && !model[model_index].live; j++)
				model_index = (model_index + 1) % model_count;
			check(HF_Delete(fd, model[model_index].rid) == HF_OK,
			      "model delete");
			model[model_index].live = 0;
			live_count--;
		} else if (live_count > 0) {
			model_index = (int)(next_random(&random_state) %
					    (unsigned long)model_count);
			for (j = 0; j < model_count && !model[model_index].live; j++)
				model_index = (model_index + 1) % model_count;
			check(HF_Get(fd, model[model_index].rid, got, &got_len) == HF_OK &&
			      got_len == model[model_index].len &&
			      memcmp(got, model[model_index].data, got_len) == 0,
			      "model get");
		}
	}
	for (i = 0; i < model_count; i++) {
		if (model[i].live) {
			check(HF_Get(fd, model[i].rid, got, &got_len) == HF_OK &&
			      got_len == model[i].len &&
			      memcmp(got, model[i].data, got_len) == 0,
			      "final model get");
		}
	}
	check(HF_CloseFile(fd) == HF_OK, "close model file");
	check(PF_DestroyFile(model_name) == PFE_OK, "destroy model file");

	check(HF_CreateFile("test_hf_empty.pf") == HF_OK, "create empty file");
	fd = HF_OpenFile("test_hf_empty.pf");
	check(fd >= 0, "open empty file");
	scanfd = HF_ScanOpen(fd);
	check(scanfd >= 0, "open empty scan");
	check(HF_ScanNext(scanfd, got, &got_len, &scan_rid) == HFE_EOF,
	      "empty scan reaches EOF immediately");
	check(HF_ScanClose(scanfd) == HF_OK, "close empty scan");
	check(HF_CloseFile(fd) == HF_OK, "close empty file after scan");
	check(PF_DestroyFile("test_hf_empty.pf") == PFE_OK,
	      "destroy empty file");

	fd = HF_OpenFile(fname);
	check(fd >= 0, "reopen after inserts");
	check(HF_CloseFile(fd) == HF_OK, "close after persisted inserts");
	pf_fd = PF_OpenFile(fname);
	check(pf_fd >= 0, "raw open for metadata check");
	check(PF_GetThisPage(pf_fd, 0, &page) == PFE_OK, "read metadata page");
	check(HFInternal_ReadHeader(page, &data_pages, &records) == HF_OK,
	      "valid metadata");
	check(data_pages == 2 && records == 43,
	      "insert counts survive reopen");
	check(PF_UnfixPage(pf_fd, 0, FALSE) == PFE_OK, "unfix metadata page");
	check(PF_CloseFile(pf_fd) == PFE_OK, "close raw metadata handle");

	check(HF_OpenFile("missing-hf-file") == HFE_PF,
	      "missing file maps to HFE_PF");
	check(PF_DestroyFile(fname) == PFE_OK, "destroy");
	check(HF_OpenFile(fname) == HFE_PF, "destroyed file is missing");
	printf("test_hf done!\n");
	return 0;
}
