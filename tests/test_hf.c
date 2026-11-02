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

int
main(int argc, char **argv)
{
	char *page;
	char *fname;
	int fd;
	int pf_fd;
	long data_pages;
	long records;
	int config_status;

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
	check(HF_CloseFile(fd) == HF_OK, "close after reopen");

	pf_fd = PF_OpenFile(fname);
	check(pf_fd >= 0, "raw open for metadata check");
	check(PF_GetThisPage(pf_fd, 0, &page) == PFE_OK, "read metadata page");
	check(HFInternal_ReadHeader(page, &data_pages, &records) == HF_OK,
	      "valid metadata");
	check(data_pages == 0 && records == 0, "metadata values survive reopen");
	check(PF_UnfixPage(pf_fd, 0, FALSE) == PFE_OK, "unfix metadata page");
	check(PF_CloseFile(pf_fd) == PFE_OK, "close raw metadata handle");

	check(HF_OpenFile("missing-hf-file") == HFE_PF,
	      "missing file maps to HFE_PF");
	check(PF_DestroyFile(fname) == PFE_OK, "destroy");
	check(HF_OpenFile(fname) == HFE_PF, "destroyed file is missing");
	printf("test_hf done!\n");
	return 0;
}
