/* hf.c: heap-file lifecycle on top of the paged-file layer */
#include "hf.h"
#include "hfinternal.h"

#include <string.h>

extern int PF_CreateFile(char *fname);
extern int PF_DestroyFile(char *fname);
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_AllocPage(int fd, int *pagenum, char **pagebuf);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);

static int hf_open_fds[HF_MAX_OPEN_FILES];
static int hf_open_count;

static unsigned long
hf_get_u32(const char *p)
{
	return (unsigned long)(unsigned char)p[0] |
		(unsigned long)(unsigned char)p[1] << 8 |
		(unsigned long)(unsigned char)p[2] << 16 |
		(unsigned long)(unsigned char)p[3] << 24;
}

static void
hf_put_u32(char *p, unsigned long value)
{
	p[0] = (char)(value & 0xffUL);
	p[1] = (char)((value >> 8) & 0xffUL);
	p[2] = (char)((value >> 16) & 0xffUL);
	p[3] = (char)((value >> 24) & 0xffUL);
}

void
HFInternal_InitHeader(char *page)
{
	memset(page, 0, PF_PAGE_SIZE);
	hf_put_u32(page + HF_HEADER_MAGIC_OFFSET, HF_FILE_MAGIC);
	hf_put_u32(page + HF_HEADER_VERSION_OFFSET, HF_FILE_VERSION);
}

int
HFInternal_ReadHeader(const char *page, long *data_pages, long *records)
{
	unsigned long pages;
	unsigned long live_records;

	if (page == NULL || data_pages == NULL || records == NULL)
		return HFE_INVALID;
	if (hf_get_u32(page + HF_HEADER_MAGIC_OFFSET) != HF_FILE_MAGIC ||
	    hf_get_u32(page + HF_HEADER_VERSION_OFFSET) != HF_FILE_VERSION)
		return HFE_PF;
	pages = hf_get_u32(page + HF_HEADER_DATA_PAGES_OFFSET);
	live_records = hf_get_u32(page + HF_HEADER_RECORDS_OFFSET);
	if (pages > 0x7fffffffUL || live_records > 0x7fffffffUL)
		return HFE_PF;
	*data_pages = (long)pages;
	*records = (long)live_records;
	return HF_OK;
}

static int
hf_find_fd(int fd)
{
	int i;

	for (i = 0; i < hf_open_count; i++)
		if (hf_open_fds[i] == fd)
			return i;
	return -1;
}

static int
hf_track_fd(int fd)
{
	if (hf_open_count >= HF_MAX_OPEN_FILES)
		return HFE_NOMEM;
	hf_open_fds[hf_open_count++] = fd;
	return HF_OK;
}

static void
hf_untrack_fd(int index)
{
	hf_open_count--;
	hf_open_fds[index] = hf_open_fds[hf_open_count];
}

int
HF_CreateFile(char *fname)
{
	char *page;
	int fd;
	int page_number;
	int error;

	if (fname == NULL)
		return HFE_INVALID;
	error = PF_CreateFile(fname);
	if (error != PFE_OK)
		return HFE_PF;
	fd = PF_OpenFile(fname);
	if (fd < 0) {
		PF_DestroyFile(fname);
		return HFE_PF;
	}
	error = PF_AllocPage(fd, &page_number, &page);
	if (error != PFE_OK || page_number != 0) {
		PF_CloseFile(fd);
		PF_DestroyFile(fname);
		return HFE_PF;
	}
	HFInternal_InitHeader(page);
	error = PF_UnfixPage(fd, page_number, TRUE);
	if (error != PFE_OK) {
		PF_CloseFile(fd);
		PF_DestroyFile(fname);
		return HFE_PF;
	}
	error = PF_CloseFile(fd);
	if (error != PFE_OK) {
		PF_DestroyFile(fname);
		return HFE_PF;
	}
	return HF_OK;
}

int
HF_OpenFile(char *fname)
{
	char *page;
	int fd;
	int error;
	long data_pages;
	long records;

	if (fname == NULL)
		return HFE_INVALID;
	fd = PF_OpenFile(fname);
	if (fd < 0)
		return HFE_PF;
	error = PF_GetThisPage(fd, 0, &page);
	if (error != PFE_OK) {
		PF_CloseFile(fd);
		return HFE_PF;
	}
	error = HFInternal_ReadHeader(page, &data_pages, &records);
	if (PF_UnfixPage(fd, 0, FALSE) != PFE_OK) {
		PF_CloseFile(fd);
		return HFE_PF;
	}
	if (error != HF_OK || data_pages < 0 || records < 0) {
		PF_CloseFile(fd);
		return HFE_PF;
	}
	error = hf_track_fd(fd);
	if (error != HF_OK) {
		PF_CloseFile(fd);
		return error;
	}
	return fd;
}

int
HF_CloseFile(int fd)
{
	int index;

	index = hf_find_fd(fd);
	if (index < 0)
		return HFE_INVALID;
	if (PF_CloseFile(fd) != PFE_OK)
		return HFE_PF;
	hf_untrack_fd(index);
	return HF_OK;
}
