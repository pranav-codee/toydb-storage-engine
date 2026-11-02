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
void HFPage_Init(unsigned char *page);
int HFPage_FreeSpace(const unsigned char *page);
int HFPage_Insert(unsigned char *page, const char *record, int length);
int HFPage_Read(const unsigned char *page, int slot, char *record, int *length);
int HFPage_Delete(unsigned char *page, int slot);

#endif /* HFPAGE_H */
