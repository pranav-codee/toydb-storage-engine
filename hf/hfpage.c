/* hfpage.c: in-memory slotted-page layout */
#include "hfpage.h"

#include <string.h>

static unsigned short
get_u16(const unsigned char *p)
{
	return (unsigned short)p[0] | (unsigned short)p[1] << 8;
}

static void
put_u16(unsigned char *p, unsigned short value)
{
	p[0] = (unsigned char)(value & 0xffU);
	p[1] = (unsigned char)(value >> 8);
}

static unsigned char *
slot_ptr(unsigned char *page, int slot)
{
	return page + HF_PAGE_HEADER_SIZE + slot * HF_PAGE_SLOT_SIZE;
}

static const unsigned char *
const_slot_ptr(const unsigned char *page, int slot)
{
	return page + HF_PAGE_HEADER_SIZE + slot * HF_PAGE_SLOT_SIZE;
}

void
HFPage_Init(unsigned char *page)
{
	memset(page, 0, PF_PAGE_SIZE);
	put_u16(page, 0);
	put_u16(page + 2, (unsigned short)PF_PAGE_SIZE);
}

int
HFPage_FreeSpace(const unsigned char *page)
{
	int slots;
	int data_start;

	if (page == NULL)
		return HFE_INVALID;
	slots = (int)get_u16(page);
	data_start = (int)get_u16(page + 2);
	if (data_start < HF_PAGE_HEADER_SIZE + slots * HF_PAGE_SLOT_SIZE ||
	    data_start > PF_PAGE_SIZE)
		return HFE_INVALID;
	return data_start - (HF_PAGE_HEADER_SIZE + slots * HF_PAGE_SLOT_SIZE);
}

int
HFPage_Insert(unsigned char *page, const char *record, int length)
{
	int slots;
	int slot;
	int data_start;
	int free_space;
	unsigned char *entry;

	if (page == NULL || length < 0)
		return HFE_INVALID;
	if (length > HF_MAX_RECLEN)
		return HFE_TOOLONG;
	if (length > 0 && record == NULL)
		return HFE_INVALID;

	slots = (int)get_u16(page);
	data_start = (int)get_u16(page + 2);
	if (data_start < HF_PAGE_HEADER_SIZE + slots * HF_PAGE_SLOT_SIZE ||
	    data_start > PF_PAGE_SIZE)
		return HFE_INVALID;

	for (slot = 0; slot < slots; slot++) {
		entry = slot_ptr(page, slot);
		if (get_u16(entry) == 0)
			break;
	}

	free_space = data_start - (HF_PAGE_HEADER_SIZE +
				   slots * HF_PAGE_SLOT_SIZE);
	if (slot == slots && free_space < HF_PAGE_SLOT_SIZE + length)
		return HF_PAGE_NOSPACE;
	if (slot < slots && free_space < length)
		return HF_PAGE_NOSPACE;

	data_start -= length;
	if (length > 0)
		memcpy(page + data_start, record, (size_t)length);
	entry = slot_ptr(page, slot);
	put_u16(entry, (unsigned short)data_start);
	put_u16(entry + 2, (unsigned short)length);
	if (slot == slots)
		put_u16(page, (unsigned short)(slots + 1));
	put_u16(page + 2, (unsigned short)data_start);
	return slot;
}

int
HFPage_Read(const unsigned char *page, int slot, char *record, int *length)
{
	int slots;
	int offset;
	int record_length;
	const unsigned char *entry;

	if (page == NULL || length == NULL || slot < 0)
		return HFE_INVALID;
	slots = (int)get_u16(page);
	if (slot >= slots)
		return HFE_NOTFOUND;
	entry = const_slot_ptr(page, slot);
	offset = (int)get_u16(entry);
	record_length = (int)get_u16(entry + 2);
	if (offset == 0 || offset + record_length > PF_PAGE_SIZE)
		return HFE_NOTFOUND;
	if (record_length > 0 && record == NULL)
		return HFE_INVALID;
	if (record_length > 0)
		memcpy(record, page + offset, (size_t)record_length);
	*length = record_length;
	return HF_OK;
}

int
HFPage_Delete(unsigned char *page, int slot)
{
	int slots;
	unsigned char *entry;

	if (page == NULL || slot < 0)
		return HFE_INVALID;
	slots = (int)get_u16(page);
	if (slot >= slots)
		return HFE_NOTFOUND;
	entry = slot_ptr(page, slot);
	if (get_u16(entry) == 0)
		return HFE_NOTFOUND;
	put_u16(entry, 0);
	put_u16(entry + 2, 0);
	return HF_OK;
}
