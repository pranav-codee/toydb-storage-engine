/* hfstatic.c: fixed-length heap file used for the static comparison */
#include "hfstatic.h"

#include <string.h>

#include "pf.h"

#define HF_STATIC_MAGIC   0x48535431UL
#define HF_STATIC_VERSION 2UL
#define HF_STATIC_PAGE_HEADER_SIZE 4
#define HF_STATIC_MAX_FILES 64
#define HF_STATIC_MAX_SCANS 128

/*
 * Data page layout: a 4-byte slot count, then slots_per_page slots of
 * HF_STATIC_LEN_SIZE + L bytes each.  A slot starts with a 2-byte
 * little-endian length word: bits 0..14 hold the stored length (at most
 * L <= HF_MAX_RECLEN), bit 15 marks a record that was truncated to L at
 * insert, and HF_STATIC_EMPTY marks a free slot (it cannot be a real
 * length word because a stored length never exceeds HF_MAX_RECLEN).
 * The L payload bytes follow; bytes past the stored length are zero.
 *
 *   slots_per_page = floor((PF_PAGE_SIZE - 4) / (L + 2))
 *
 * Header page 0: magic, version, L, data_pages, records (live),
 * truncated (live records with the truncated bit), stored_bytes (sum of
 * the stored lengths of live records), each a 4-byte little-endian word.
 * Insert and delete keep the three counters exact, because both already
 * have the slot fixed and know its length word.
 */
#define HF_STATIC_LEN_SIZE 2
#define HF_STATIC_TRUNC_BIT 0x8000U
#define HF_STATIC_LEN_MASK 0x7fffU
#define HF_STATIC_EMPTY 0xffffU

/* Byte offsets of the header page 0 fields described above. */
#define HF_STATIC_MAGIC_OFFSET       0
#define HF_STATIC_VERSION_OFFSET     4
#define HF_STATIC_RECLEN_OFFSET      8
#define HF_STATIC_DATA_PAGES_OFFSET 12
#define HF_STATIC_RECORDS_OFFSET    16
#define HF_STATIC_TRUNCATED_OFFSET  20
#define HF_STATIC_BYTES_OFFSET      24

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
	long stored_bytes;
	long free_hint;		/* no page before this one has a free slot */
	int used;
};

struct static_scan {
	int fd;
	long data_pages;
	int slots_per_page;
	int page;
	int slot;
	int used;
	int valid;		/* cleared when the scanned file is closed */
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

/* slots_per_page = floor((PF_PAGE_SIZE - 4) / (L + 2)) */
static int
static_slots(int record_length)
{
	return (PF_PAGE_SIZE - HF_STATIC_PAGE_HEADER_SIZE) /
		(record_length + HF_STATIC_LEN_SIZE);
}

static int
static_valid_length(int record_length)
{
	return record_length > 0 && record_length <= HF_MAX_RECLEN &&
		static_slots(record_length) > 0;
}

static char *
static_slot_ptr(char *page, int slot, int record_length)
{
	return page + HF_STATIC_PAGE_HEADER_SIZE +
		slot * (record_length + HF_STATIC_LEN_SIZE);
}

static unsigned int
static_get_word(const char *slotp)
{
	return (unsigned int)(unsigned char)slotp[0] |
		(unsigned int)(unsigned char)slotp[1] << 8;
}

static void
static_put_word(char *slotp, unsigned int word)
{
	slotp[0] = (char)(word & 0xffU);
	slotp[1] = (char)((word >> 8) & 0xffU);
}

/* a new data page: slot count, every slot empty (0xff bytes) */
static void
static_init_page(char *page, int slots)
{
	memset(page, 0, PF_PAGE_SIZE);
	static_put_u32(page, (unsigned long)slots);
	memset(page + HF_STATIC_PAGE_HEADER_SIZE, 0xff,
	       (size_t)(PF_PAGE_SIZE - HF_STATIC_PAGE_HEADER_SIZE));
}

static int
static_read_header(const char *page, int *record_length, long *data_pages,
		   long *records, long *truncated, long *stored_bytes)
{
	unsigned long pages;
	unsigned long live;
	unsigned long cuts;
	unsigned long bytes;

	if (static_get_u32(page + HF_STATIC_MAGIC_OFFSET) != HF_STATIC_MAGIC ||
	    static_get_u32(page + HF_STATIC_VERSION_OFFSET) !=
	    HF_STATIC_VERSION)
		return HFE_PF;
	*record_length = (int)static_get_u32(page + HF_STATIC_RECLEN_OFFSET);
	pages = static_get_u32(page + HF_STATIC_DATA_PAGES_OFFSET);
	live = static_get_u32(page + HF_STATIC_RECORDS_OFFSET);
	cuts = static_get_u32(page + HF_STATIC_TRUNCATED_OFFSET);
	bytes = static_get_u32(page + HF_STATIC_BYTES_OFFSET);
	if (!static_valid_length(*record_length) ||
	    pages > 0x7fffffffUL || live > 0x7fffffffUL ||
	    cuts > 0x7fffffffUL || bytes > 0x7fffffffUL)
		return HFE_PF;
	*data_pages = (long)pages;
	*records = (long)live;
	*truncated = (long)cuts;
	*stored_bytes = (long)bytes;
	return HF_OK;
}

static void
static_write_header(char *page, int record_length, long data_pages,
		    long records, long truncated, long stored_bytes)
{
	static_put_u32(page + HF_STATIC_MAGIC_OFFSET, HF_STATIC_MAGIC);
	static_put_u32(page + HF_STATIC_VERSION_OFFSET, HF_STATIC_VERSION);
	static_put_u32(page + HF_STATIC_RECLEN_OFFSET,
		       (unsigned long)record_length);
	static_put_u32(page + HF_STATIC_DATA_PAGES_OFFSET,
		       (unsigned long)data_pages);
	static_put_u32(page + HF_STATIC_RECORDS_OFFSET, (unsigned long)records);
	static_put_u32(page + HF_STATIC_TRUNCATED_OFFSET,
		       (unsigned long)truncated);
	static_put_u32(page + HF_STATIC_BYTES_OFFSET,
		       (unsigned long)stored_bytes);
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
				   &static_files[i].truncated,
				   &static_files[i].stored_bytes);
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
			    file->records, file->truncated, file->stored_bytes);
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
	static_write_header(page, record_length, 0, 0, 0, 0);
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
	long stored_bytes;

	if (fname == NULL)
		return HFE_INVALID;
	fd = PF_OpenFile(fname);
	if (fd < 0)
		return HFE_PF;
	page = NULL;
	error = PF_GetThisPage(fd, 0, &page);
	if (error == PFE_OK)
		error = static_read_header(page, &record_length, &data_pages,
					   &records, &truncated,
					   &stored_bytes);
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
	static_files[i].stored_bytes = stored_bytes;
	static_files[i].free_hint = 1;
	static_files[i].used = 1;
	return fd;
}

int
HF_StaticCloseFile(int fd)
{
	int i;
	int s;

	i = static_find_fd(fd);
	if (i < 0)
		return HFE_INVALID;
	if (PF_CloseFile(fd) != PFE_OK)
		return HFE_PF;
	static_files[i].used = 0;
	/* open scans of this file must not read whatever reuses the fd */
	for (s = 0; s < HF_STATIC_MAX_SCANS; s++)
		if (static_scans[s].used && static_scans[s].fd == fd)
			static_scans[s].valid = 0;
	return HF_OK;
}

int
HF_StaticInsert(int fd, char *rec, int len)
{
	struct static_file *file;
	char *page;
	char *slotp;
	int page_number;
	int slot;
	int slots;
	int stored;
	int error;

	if (len < 0 || (len > 0 && rec == NULL))
		return HFE_INVALID;
	if (len > HF_MAX_RECLEN)
		return HFE_TOOLONG;
	error = static_load_file(fd, &file);
	if (error != HF_OK)
		return error;
	slots = static_slots(file->record_length);
	stored = len < file->record_length ? len : file->record_length;
	if (file->free_hint < 1)
		file->free_hint = 1;
	/* pages before free_hint are known to be full */
	for (page_number = (int)file->free_hint;
	     page_number <= file->data_pages; page_number++) {
		error = PF_GetThisPage(fd, page_number, &page);
		if (error != PFE_OK)
			return HFE_PF;
		for (slot = 0; slot < slots; slot++)
			if (static_get_word(static_slot_ptr(page, slot,
					    file->record_length)) ==
			    HF_STATIC_EMPTY)
				break;
		if (slot < slots)
			goto store;
		if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
			return HFE_PF;
		file->free_hint = page_number + 1;
	}
	error = PF_AllocPage(fd, &page_number, &page);
	if (error != PFE_OK)
		return HFE_PF;
	static_init_page(page, slots);
	slot = 0;
store:
	file->free_hint = page_number;
	slotp = static_slot_ptr(page, slot, file->record_length);
	static_put_word(slotp, (unsigned int)stored |
			(len > file->record_length ? HF_STATIC_TRUNC_BIT : 0U));
	memset(slotp + HF_STATIC_LEN_SIZE, 0, (size_t)file->record_length);
	if (stored > 0)
		memcpy(slotp + HF_STATIC_LEN_SIZE, rec, (size_t)stored);
	if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK)
		return HFE_PF;
	if (page_number > file->data_pages)
		file->data_pages++;
	file->records++;
	file->stored_bytes += stored;
	if (len > file->record_length)
		file->truncated++;
	return static_update_header(file);
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
	char *slotp;
	unsigned int word;
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
	slotp = static_slot_ptr(page, slot, file->record_length);
	word = static_get_word(slotp);
	if (word == HF_STATIC_EMPTY) {
		if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
			return HFE_PF;
		return HFE_NOTFOUND;
	}
	*len = (int)(word & HF_STATIC_LEN_MASK);
	memcpy(buf, slotp + HF_STATIC_LEN_SIZE, (size_t)*len);
	if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
		return HFE_PF;
	return HF_OK;
}

int
HF_StaticDelete(int fd, int rid)
{
	struct static_file *file;
	char *page;
	char *slotp;
	unsigned int word;
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
	slotp = static_slot_ptr(page, slot, file->record_length);
	word = static_get_word(slotp);
	if (word == HF_STATIC_EMPTY) {
		if (PF_UnfixPage(fd, page_number, FALSE) != PFE_OK)
			return HFE_PF;
		return HFE_NOTFOUND;
	}
	static_put_word(slotp, HF_STATIC_EMPTY);
	if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK)
		return HFE_PF;
	file->records--;
	file->stored_bytes -= (long)(word & HF_STATIC_LEN_MASK);
	if (word & HF_STATIC_TRUNC_BIT)
		file->truncated--;
	if (page_number < file->free_hint)
		file->free_hint = page_number;
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
	static_scans[i].valid = 1;
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
	char *slotp;
	unsigned int word;
	int i;
	int error;

	if (scanfd < 0 || scanfd >= HF_STATIC_MAX_SCANS ||
	    !static_scans[scanfd].used || !static_scans[scanfd].valid ||
	    buf == NULL || len == NULL || rid == NULL)
		return HFE_INVALID;
	scan = &static_scans[scanfd];
	i = static_find_fd(scan->fd);
	if (i < 0)
		return HFE_INVALID;
	file = &static_files[i];
	while (scan->page <= scan->data_pages) {
		error = PF_GetThisPage(scan->fd, scan->page, &page);
		if (error != PFE_OK)
			return HFE_PF;
		while (scan->slot < scan->slots_per_page) {
			slotp = static_slot_ptr(page, scan->slot,
						file->record_length);
			word = static_get_word(slotp);
			if (word != HF_STATIC_EMPTY) {
				*rid = HF_RID(scan->page, scan->slot);
				*len = (int)(word & HF_STATIC_LEN_MASK);
				memcpy(buf, slotp + HF_STATIC_LEN_SIZE,
				       (size_t)*len);
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
	static_scans[scanfd].valid = 0;
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
	out->useful_bytes = file->stored_bytes;
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
