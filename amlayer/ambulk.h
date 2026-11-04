/* ambulk.h: frozen sorted-stream B+ tree bulk-loader interface */
#ifndef AMBULK_H
#define AMBULK_H

#define AM_BULK_NEXT_ERROR (-1)
#define AM_BULK_NEXT_EOF 0
#define AM_BULK_NEXT_ITEM 1

/*
 * Supply the next sorted key and non-negative heap-file RID to a bulk load.
 * Return AM_BULK_NEXT_ITEM for a pair, AM_BULK_NEXT_EOF at end of input, or
 * AM_BULK_NEXT_ERROR on failure.  Keys must be in AM comparison order
 * (AM_Compare: numeric for 'i' and 'f', strncmp for 'c') and all RIDs for
 * one key must be consecutive.  All RIDs of one key must fit in one leaf:
 * floor((PF_PAGE_SIZE - sizeof(AM_LEAFHEADER) - (attrLength + 2)) / 6) of
 * them, 677 for char(8).
 */
typedef int (*AMBulkNext)(void *context, char *key, int *rid);

/*
 * Create and bottom-up bulk-load indexNo for relName from the sorted stream
 * supplied by next.  The attrType and attrLength describe each key.  The
 * fill factor is read from toydb.conf (bulk_fill_factor, in (0, 1]), not
 * passed as an argument; it applies to leaves and internal nodes.  The root
 * is page 0 and every page is reachable from it.  Returns (and stores in
 * AM_Errno) AME_OK on success, or:
 *   AME_INVALIDATTRTYPE    invalid argument or fill factor
 *   AME_INVALIDVALUE       unsorted input or a negative RID
 *   AME_INVALIDATTRLENGTH  a key has more RIDs than fit in one leaf
 *   AME_PF                 a PF call failed
 *   AME_INTERROR           the load could not complete for a reason outside
 *                          the input order and arguments (next() returned
 *                          AM_BULK_NEXT_ERROR or an unknown status, or out
 *                          of memory)
 * On failure no index file is left behind.
 */
int AM_BulkLoad(char *relName, int indexNo, char attrType, int attrLength,
		AMBulkNext next, void *context);

#endif /* AMBULK_H */
