/* hfpage.h: pure slotted-page operations for the heap-file layer */
#ifndef HFPAGE_H
#define HFPAGE_H

#include "hf.h"
#include "pf.h"

#define HF_PAGE_HEADER_SIZE 4
#define HF_PAGE_SLOT_SIZE   4
#define HF_PAGE_NOSPACE     (-7)

/*
 * The page header stores the slot count followed by the offset at which
 * record data begins.  Each slot stores a two-byte offset and length.
 * Offset zero marks a deleted slot; live records always have a nonzero
 * offset, including zero-length records.
 */

/* Zero the page: no slots, record data starts at PF_PAGE_SIZE. */
void HFPage_Init(unsigned char *page);

/*
 * Contiguous free bytes between the slot directory and the record data (a
 * new slot needs HF_PAGE_SLOT_SIZE of them too); HFE_INVALID for a NULL or
 * corrupt page.
 */
int HFPage_FreeSpace(const unsigned char *page);

/*
 * Store the record in the first deleted slot, else a new one.  Returns the
 * slot number (>= 0), HF_PAGE_NOSPACE if it does not fit, HFE_TOOLONG if
 * length > HF_MAX_RECLEN, or HFE_INVALID for bad arguments or a bad page.
 */
int HFPage_Insert(unsigned char *page, const char *record, int length);

/*
 * Copy the record in slot into record and its length into *length.  Returns
 * HF_OK, HFE_NOTFOUND for a deleted slot or a slot past the last one, or
 * HFE_INVALID for bad arguments.
 */
int HFPage_Read(const unsigned char *page, int slot, char *record, int *length);

/*
 * Free the slot and compact the remaining records so free space stays
 * contiguous; slot numbers (and RIDs) of the other records do not change.
 * Returns HF_OK, HFE_NOTFOUND for a deleted or missing slot, or HFE_INVALID.
 */
int HFPage_Delete(unsigned char *page, int slot);

#endif /* HFPAGE_H */
