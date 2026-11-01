/* ambulk.h: frozen sorted-stream B+ tree bulk-loader interface */
#ifndef AMBULK_H
#define AMBULK_H

#define AM_BULK_NEXT_ERROR (-1)
#define AM_BULK_NEXT_EOF 0
#define AM_BULK_NEXT_ITEM 1

/*
 * Supply the next sorted key and non-negative heap-file RID to a bulk load.
 * Return AM_BULK_NEXT_ITEM for a pair, AM_BULK_NEXT_EOF at end of input, or
 * AM_BULK_NEXT_ERROR on failure.  Keys must be in memcmp order and all RIDs
 * for one key must be consecutive.
 */
typedef int (*AMBulkNext)(void *context, char *key, int *rid);

/*
 * Create and bottom-up bulk-load indexNo for relName from the sorted stream
 * supplied by next.  The attrType and attrLength describe each key.  The
 * fill factor is read from toydb.conf, not passed as an argument.  Return
 * AME_OK on success or an AM error code on invalid input or failure.
 */
int AM_BulkLoad(char *relName, int indexNo, char attrType, int attrLength,
		AMBulkNext next, void *context);

#endif /* AMBULK_H */
