/* hfinternal.h: heap-file metadata shared by heap-file implementation files */
#ifndef HFINTERNAL_H
#define HFINTERNAL_H

#include "pf.h"

#define HF_FILE_MAGIC   0x48465031UL
#define HF_FILE_VERSION 1UL
#define HF_FILE_HEADER_SIZE 16
#define HF_MAX_OPEN_FILES 64

/*
 * The first heap page is metadata, not a record page.  Fields are stored as
 * little-endian 32-bit values so the on-page format has fixed offsets.
 */
#define HF_HEADER_MAGIC_OFFSET       0
#define HF_HEADER_VERSION_OFFSET     4
#define HF_HEADER_DATA_PAGES_OFFSET  8
#define HF_HEADER_RECORDS_OFFSET    12

int HFInternal_ReadHeader(const char *page, long *data_pages, long *records);
void HFInternal_InitHeader(char *page);

#endif /* HFINTERNAL_H */
