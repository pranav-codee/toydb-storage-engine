/* hf.c: heap-file lifecycle on top of the paged-file layer */
#include "hf.h"
#include "hfinternal.h"
#include "hfpage.h"

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

void
HFInternal_WriteHeader(char *page, long data_pages, long records)
{
	hf_put_u32(page + HF_HEADER_MAGIC_OFFSET, HF_FILE_MAGIC);
	hf_put_u32(page + HF_HEADER_VERSION_OFFSET, HF_FILE_VERSION);
	hf_put_u32(page + HF_HEADER_DATA_PAGES_OFFSET, (unsigned long)data_pages);
	hf_put_u32(page + HF_HEADER_RECORDS_OFFSET, (unsigned long)records);
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

int
HF_Insert(int fd, char *rec, int len)
{
	char *header_page;
	char *data_page;
	long data_pages;
	long records;
	int page_number;
	int slot;
	int error;

	if (hf_find_fd(fd) < 0 || len < 0)
		return HFE_INVALID;
	if (len > HF_MAX_RECLEN)
		return HFE_TOOLONG;
	if (len > 0 && rec == NULL)
		return HFE_INVALID;

	data_pages = 0;
	records = 0;
	error = PF_GetThisPage(fd, 0, &header_page);
	if (error != PFE_OK)
		return HFE_PF;
	error = HFInternal_ReadHeader(header_page, &data_pages, &records);
	if (PF_UnfixPage(fd, 0, FALSE) != PFE_OK)
		return HFE_PF;
	if (error != HF_OK || data_pages < 0 || records < 0)
		return HFE_PF;

	page_number = 0;
	slot = HF_PAGE_NOSPACE;
	/*
	 * Before deletes, the last page was sufficient because earlier pages
	 * could not gain space.  Deletes can free any page, so inspect each
	 * existing page and reuse the first one with enough contiguous space.
	 */
	for (page_number = 1; page_number <= data_pages; page_number++) {
		error = PF_GetThisPage(fd, page_number, &data_page);
		if (error != PFE_OK)
			return HFE_PF;
		slot = HFPage_Insert((unsigned char *)data_page, rec, len);
		if (slot == HF_PAGE_NOSPACE) {
			if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
				return HFE_PF;
		} else if (slot < 0) {
			if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
				return HFE_PF;
			return slot == HFE_TOOLONG ? HFE_TOOLONG : HFE_INVALID;
		} else {
			if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK)
				return HFE_PF;
			records++;
			goto update_header;
		}
	}

	error = PF_AllocPage(fd, &page_number, &data_page);
	if (error != PFE_OK)
		return HFE_PF;
	HFPage_Init((unsigned char *)data_page);
	slot = HFPage_Insert((unsigned char *)data_page, rec, len);
	if (slot < 0) {
		PF_UnfixPage(fd, page_number, FALSE);
		return slot == HFE_TOOLONG ? HFE_TOOLONG : HFE_PF;
	}
	if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK)
		return HFE_PF;
	data_pages++;
	records++;

update_header:
	error = PF_GetThisPage(fd, 0, &header_page);
	if (error != PFE_OK)
		return HFE_PF;
	HFInternal_WriteHeader(header_page, data_pages, records);
	if (PF_UnfixPage(fd, 0, TRUE) != PFE_OK)
		return HFE_PF;
	return HF_RID(page_number, slot);
}

static int
hf_decode_rid(int rid, long data_pages, int *page_number, int *slot)
{
	long page;

	if (rid < 0 || page_number == NULL || slot == NULL)
		return HFE_INVALID;
	page = HF_RID_PAGE(rid);
	if (page <= 0 || page > 0x7fffffffL)
		return HFE_INVALID;
	if (page > data_pages)
		return HFE_NOTFOUND;
	*page_number = (int)page;
	*slot = HF_RID_SLOT(rid);
	return HF_OK;
}

int
HF_Get(int fd, int rid, char *buf, int *len)
{
	char *header_page;
	char *data_page;
	long data_pages;
	long records;
	int page_number;
	int slot;
	int error;
	int result;

	if (hf_find_fd(fd) < 0 || buf == NULL || len == NULL)
		return HFE_INVALID;
	error = PF_GetThisPage(fd, 0, &header_page);
	if (error != PFE_OK)
		return HFE_PF;
	error = HFInternal_ReadHeader(header_page, &data_pages, &records);
	if (PF_UnfixPage(fd, 0, FALSE) != PFE_OK)
		return HFE_PF;
	if (error != HF_OK)
		return HFE_PF;
	error = hf_decode_rid(rid, data_pages, &page_number, &slot);
	if (error != HF_OK)
		return error;
	error = PF_GetThisPage(fd, page_number, &data_page);
	if (error != PFE_OK)
		return HFE_PF;
	result = HFPage_Read((unsigned char *)data_page, slot, buf, len);
	if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
		return HFE_PF;
	return result;
}

int
HF_Delete(int fd, int rid)
{
	char *header_page;
	char *data_page;
	long data_pages;
	long records;
	int page_number;
	int slot;
	int error;
	int result;

	if (hf_find_fd(fd) < 0)
		return HFE_INVALID;
	error = PF_GetThisPage(fd, 0, &header_page);
	if (error != PFE_OK)
		return HFE_PF;
	error = HFInternal_ReadHeader(header_page, &data_pages, &records);
	if (PF_UnfixPage(fd, 0, FALSE) != PFE_OK)
		return HFE_PF;
	if (error != HF_OK)
		return HFE_PF;
	error = hf_decode_rid(rid, data_pages, &page_number, &slot);
	if (error != HF_OK)
		return error;
	if (records <= 0)
		return HFE_PF;
	error = PF_GetThisPage(fd, page_number, &data_page);
	if (error != PFE_OK)
		return HFE_PF;
	result = HFPage_Delete((unsigned char *)data_page, slot);
	if (result != HF_OK) {
		if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
			return HFE_PF;
		return result;
	}
	if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK)
		return HFE_PF;

	error = PF_GetThisPage(fd, 0, &header_page);
	if (error != PFE_OK)
		return HFE_PF;
	HFInternal_WriteHeader(header_page, data_pages, records - 1);
	if (PF_UnfixPage(fd, 0, TRUE) != PFE_OK)
		return HFE_PF;
	return HF_OK;
}
