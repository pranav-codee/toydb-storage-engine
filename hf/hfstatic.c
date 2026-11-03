/* hfstatic.c: fixed-length heap file used for the static comparison */
#include "hfstatic.h"

#include <string.h>

#include "pf.h"

#define HF_STATIC_MAGIC   0x48535431UL
#define HF_STATIC_VERSION 1UL
#define HF_STATIC_HEADER_SIZE 4
#define HF_STATIC_PAGE_HEADER_SIZE 4
#define HF_STATIC_MAX_FILES 64
#define HF_STATIC_MAX_SCANS 128

extern int PF_CreateFile(char *fname);
extern int PF_DestroyFile(char *fname);
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_AllocPage(int fd, int *pagenum, char **pagebuf);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);

struct static_file {
	int fd;
	int record_length;
	long data_pages;
	long records;
	long truncated;
	int used;
};

struct static_scan {
	int fd;
	long data_pages;
	int slots_per_page;
	int page;
	int slot;
	int used;
};

static struct static_file static_files[HF_STATIC_MAX_FILES];
static struct static_scan static_scans[HF_STATIC_MAX_SCANS];

static unsigned long
static_get_u32(const char *p)
{
	return (unsigned long)(unsigned char)p[0] |
		(unsigned long)(unsigned char)p[1] << 8 |
		(unsigned long)(unsigned char)p[2] << 16 |
		(unsigned long)(unsigned char)p[3] << 24;
}

static void
static_put_u32(char *p, unsigned long value)
{
	p[0] = (char)(value & 0xffUL);
	p[1] = (char)((value >> 8) & 0xffUL);
	p[2] = (char)((value >> 16) & 0xffUL);
	p[3] = (char)((value >> 24) & 0xffUL);
}

static int
static_find_fd(int fd)
{
	int i;

	for (i = 0; i < HF_STATIC_MAX_FILES; i++)
		if (static_files[i].used && static_files[i].fd == fd)
			return i;
	return -1;
}

static int
static_slots(int record_length)
{
	return (PF_PAGE_SIZE - HF_STATIC_PAGE_HEADER_SIZE) /
		(record_length + 1);
}

static int
static_page_header_size(int slots)
{
	return HF_STATIC_PAGE_HEADER_SIZE + slots;
}

static int
static_valid_length(int record_length)
{
	return record_length > 0 && record_length <= HF_MAX_RECLEN &&
		static_slots(record_length) > 0;
}

static void
static_init_page(char *page, int slots)
{
	memset(page, 0, PF_PAGE_SIZE);
	static_put_u32(page, (unsigned long)slots);
}

static int
static_page_has_slot(const char *page, int slot, int slots)
{
	if (slot < 0 || slot >= slots)
		return 0;
	return (unsigned char)page[HF_STATIC_PAGE_HEADER_SIZE + slot] != 0;
}

static void
static_page_set_slot(char *page, int slot, int occupied)
{
	page[HF_STATIC_PAGE_HEADER_SIZE + slot] = (char)(occupied != 0);
}

static char *
static_slot_ptr(char *page, int slot, int record_length)
{
	int slots;

	slots = (int)static_get_u32(page);
	return page + static_page_header_size(slots) + slot * record_length;
}

static int
static_read_header(const char *page, int *record_length, long *data_pages,
		   long *records, long *truncated)
{
	unsigned long pages;
	unsigned long live;
	unsigned long cuts;

	if (static_get_u32(page) != HF_STATIC_MAGIC ||
	    static_get_u32(page + 4) != HF_STATIC_VERSION)
		return HFE_PF;
	*record_length = (int)static_get_u32(page + 8);
	pages = static_get_u32(page + 12);
	live = static_get_u32(page + 16);
	cuts = static_get_u32(page + 20);
	if (!static_valid_length(*record_length) ||
	    pages > 0x7fffffffUL || live > 0x7fffffffUL ||
	    cuts > 0x7fffffffUL)
		return HFE_PF;
	*data_pages = (long)pages;
	*records = (long)live;
	*truncated = (long)cuts;
	return HF_OK;
}

static void
static_write_header(char *page, int record_length, long data_pages,
		    long records, long truncated)
{
	static_put_u32(page, HF_STATIC_MAGIC);
	static_put_u32(page + 4, HF_STATIC_VERSION);
	static_put_u32(page + 8, (unsigned long)record_length);
	static_put_u32(page + 12, (unsigned long)data_pages);
	static_put_u32(page + 16, (unsigned long)records);
	static_put_u32(page + 20, (unsigned long)truncated);
}

static int
static_load_file(int fd, struct static_file **file)
{
	char *page;
	int i;
	int error;

	i = static_find_fd(fd);
	if (i < 0)
		return HFE_INVALID;
	error = PF_GetThisPage(fd, 0, &page);
	if (error != PFE_OK)
		return HFE_PF;
	error = static_read_header(page, &static_files[i].record_length,
				   &static_files[i].data_pages,
				   &static_files[i].records,
				   &static_files[i].truncated);
	if (PF_UnfixPage(fd, 0, FALSE) != PFE_OK)
		return HFE_PF;
	if (error != HF_OK)
		return error;
	*file = &static_files[i];
	return HF_OK;
}

static int
static_update_header(struct static_file *file)
{
	char *page;
	int error;

	error = PF_GetThisPage(file->fd, 0, &page);
	if (error != PFE_OK)
		return HFE_PF;
	static_write_header(page, file->record_length, file->data_pages,
			    file->records, file->truncated);
	if (PF_UnfixPage(file->fd, 0, TRUE) != PFE_OK)
		return HFE_PF;
	return HF_OK;
}

int
HF_StaticCreateFile(char *fname, int record_length)
{
	char *page;
	int fd;
	int page_number;
	int error;

	if (fname == NULL || !static_valid_length(record_length))
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
	memset(page, 0, PF_PAGE_SIZE);
	static_write_header(page, record_length, 0, 0, 0);
	if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK) {
		PF_CloseFile(fd);
		PF_DestroyFile(fname);
		return HFE_PF;
	}
	if (PF_CloseFile(fd) != PFE_OK) {
		PF_DestroyFile(fname);
		return HFE_PF;
	}
	return HF_OK;
}

int
HF_StaticOpenFile(char *fname)
{
	char *page;
	int fd;
	int i;
	int error;
	int record_length;
	long data_pages;
	long records;
	long truncated;

	if (fname == NULL)
		return HFE_INVALID;
	fd = PF_OpenFile(fname);
	if (fd < 0)
		return HFE_PF;
	page = NULL;
	error = PF_GetThisPage(fd, 0, &page);
	if (error == PFE_OK)
		error = static_read_header(page, &record_length, &data_pages,
					   &records, &truncated);
	if (error == PFE_OK) {
		if (PF_UnfixPage(fd, 0, FALSE) != PFE_OK)
			error = HFE_PF;
	} else if (page != NULL) {
		PF_UnfixPage(fd, 0, FALSE);
	}
	if (error != PFE_OK) {
		PF_CloseFile(fd);
		return error == HFE_INVALID ? HFE_INVALID : HFE_PF;
	}
	for (i = 0; i < HF_STATIC_MAX_FILES; i++)
		if (!static_files[i].used)
			break;
	if (i == HF_STATIC_MAX_FILES) {
		PF_CloseFile(fd);
		return HFE_NOMEM;
	}
	static_files[i].fd = fd;
	static_files[i].record_length = record_length;
	static_files[i].data_pages = data_pages;
	static_files[i].records = records;
	static_files[i].truncated = truncated;
	static_files[i].used = 1;
	return fd;
}

int
HF_StaticCloseFile(int fd)
{
	int i;

	i = static_find_fd(fd);
	if (i < 0)
		return HFE_INVALID;
	if (PF_CloseFile(fd) != PFE_OK)
		return HFE_PF;
	static_files[i].used = 0;
	return HF_OK;
}

int
HF_StaticInsert(int fd, char *rec, int len)
{
	struct static_file *file;
	char *page;
	int page_number;
	int slot;
	int slots;
	int stored;
	int error;

	if (len < 0 || (len > 0 && rec == NULL))
		return HFE_INVALID;
	error = static_load_file(fd, &file);
	if (error != HF_OK)
		return error;
	slots = static_slots(file->record_length);
	stored = len < file->record_length ? len : file->record_length;
	for (page_number = 1; page_number <= file->data_pages; page_number++) {
		error = PF_GetThisPage(fd, page_number, &page);
		if (error != PFE_OK)
			return HFE_PF;
		for (slot = 0; slot < slots; slot++)
			if (!static_page_has_slot(page, slot, slots))
				break;
		if (slot < slots)
			goto store;
		if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
			return HFE_PF;
	}
	error = PF_AllocPage(fd, &page_number, &page);
	if (error != PFE_OK)
		return HFE_PF;
	static_init_page(page, slots);
	slot = 0;
store:
	if (stored > 0)
		memcpy(static_slot_ptr(page, slot, file->record_length), rec,
		       (size_t)stored);
	if (stored < file->record_length)
		memset(static_slot_ptr(page, slot, file->record_length) + stored, 0,
		       (size_t)(file->record_length - stored));
	static_page_set_slot(page, slot, 1);
	if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK)
		return HFE_PF;
	if (page_number > file->data_pages)
		file->data_pages++;
	file->records++;
	if (len > file->record_length)
		file->truncated++;
	error = static_update_header(file);
	if (error != HF_OK)
		return error;
	(void)slot;
	return HF_OK;
}

static int
static_decode_rid(struct static_file *file, int rid, int *page, int *slot)
{
	if (rid < 0)
		return HFE_INVALID;
	*page = HF_RID_PAGE(rid);
	*slot = HF_RID_SLOT(rid);
	if (*page <= 0)
		return HFE_INVALID;
	if (*page > file->data_pages)
		return HFE_NOTFOUND;
	if (*slot >= static_slots(file->record_length))
		return HFE_INVALID;
	return HF_OK;
}

int
HF_StaticGet(int fd, int rid, char *buf, int *len)
{
	struct static_file *file;
	char *page;
	int page_number;
	int slot;
	int error;

	if (buf == NULL || len == NULL)
		return HFE_INVALID;
	error = static_load_file(fd, &file);
	if (error != HF_OK)
		return error;
	error = static_decode_rid(file, rid, &page_number, &slot);
	if (error != HF_OK)
		return error;
	error = PF_GetThisPage(fd, page_number, &page);
	if (error != PFE_OK)
		return HFE_PF;
	if (!static_page_has_slot(page, slot, static_slots(file->record_length))) {
		PF_UnfixPage(fd, page_number, FALSE);
		return HFE_NOTFOUND;
	}
	memcpy(buf, static_slot_ptr(page, slot, file->record_length),
	       (size_t)file->record_length);
	*len = file->record_length;
	if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
		return HFE_PF;
	return HF_OK;
}

int
HF_StaticDelete(int fd, int rid)
{
	struct static_file *file;
	char *page;
	int page_number;
	int slot;
	int error;

	error = static_load_file(fd, &file);
	if (error != HF_OK)
		return error;
	error = static_decode_rid(file, rid, &page_number, &slot);
	if (error != HF_OK)
		return error;
	error = PF_GetThisPage(fd, page_number, &page);
	if (error != PFE_OK)
		return HFE_PF;
	if (!static_page_has_slot(page, slot, static_slots(file->record_length))) {
		PF_UnfixPage(fd, page_number, FALSE);
		return HFE_NOTFOUND;
	}
	static_page_set_slot(page, slot, 0);
	if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK)
		return HFE_PF;
	file->records--;
	return static_update_header(file);
}

int
HF_StaticScanOpen(int fd)
{
	struct static_file *file;
	int i;
	int error;

	error = static_load_file(fd, &file);
	if (error != HF_OK)
		return error;
	for (i = 0; i < HF_STATIC_MAX_SCANS; i++)
		if (!static_scans[i].used)
			break;
	if (i == HF_STATIC_MAX_SCANS)
		return HFE_NOMEM;
	static_scans[i].used = 1;
	static_scans[i].fd = fd;
	static_scans[i].data_pages = file->data_pages;
	static_scans[i].slots_per_page = static_slots(file->record_length);
	static_scans[i].page = 1;
	static_scans[i].slot = 0;
	return i;
}

int
HF_StaticScanNext(int scanfd, char *buf, int *len, int *rid)
{
	struct static_scan *scan;
	struct static_file *file;
	char *page;
	int error;

	if (scanfd < 0 || scanfd >= HF_STATIC_MAX_SCANS ||
	    !static_scans[scanfd].used || buf == NULL || len == NULL ||
	    rid == NULL)
		return HFE_INVALID;
	scan = &static_scans[scanfd];
	if (static_find_fd(scan->fd) < 0)
		return HFE_INVALID;
	file = &static_files[static_find_fd(scan->fd)];
	while (scan->page <= scan->data_pages) {
		error = PF_GetThisPage(scan->fd, scan->page, &page);
		if (error != PFE_OK)
			return HFE_PF;
		while (scan->slot < scan->slots_per_page) {
			*rid = HF_RID(scan->page, scan->slot);
			if (static_page_has_slot(page, scan->slot,
						 scan->slots_per_page)) {
				memcpy(buf, static_slot_ptr(page, scan->slot,
							    file->record_length),
				       (size_t)file->record_length);
				*len = file->record_length;
				scan->slot++;
				if (PF_UnfixPage(scan->fd, scan->page, FALSE) != PFE_OK)
					return HFE_PF;
				return HF_OK;
			}
			scan->slot++;
		}
		if (PF_UnfixPage(scan->fd, scan->page, FALSE) != PFE_OK)
			return HFE_PF;
		scan->page++;
		scan->slot = 0;
	}
	return HFE_EOF;
}

int
HF_StaticScanClose(int scanfd)
{
	if (scanfd < 0 || scanfd >= HF_STATIC_MAX_SCANS ||
	    !static_scans[scanfd].used)
		return HFE_INVALID;
	static_scans[scanfd].used = 0;
	return HF_OK;
}

int
HF_StaticGetSpaceStats(int fd, HFSpaceStats *out)
{
	struct static_file *file;
	int error;

	if (out == NULL)
		return HFE_INVALID;
	error = static_load_file(fd, &file);
	if (error != HF_OK)
		return error;
	out->pages = file->data_pages;
	out->records = file->records;
	out->useful_bytes = file->records * (long)file->record_length;
	out->truncated = file->truncated;
	if (file->data_pages == 0) {
		out->records_per_page = 0;
		out->utilization = 0.0;
	} else {
		out->records_per_page = file->records / file->data_pages;
		out->utilization = (double)out->useful_bytes /
			((double)file->data_pages * (double)PF_PAGE_SIZE);
	}
	return HF_OK;
}
