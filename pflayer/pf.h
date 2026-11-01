/* pf.h: externs and error codes for Paged File Interface*/
#ifndef TRUE
#define TRUE 1		
#endif
#ifndef FALSE
#define FALSE 0
#endif

/************** Error Codes *********************************/
#define PFE_OK		0	/* OK */
#define PFE_NOMEM	-1	/* no memory */
#define PFE_NOBUF	-2	/* no buffer space */
#define PFE_PAGEFIXED 	-3	/* page already fixed in buffer */
#define PFE_PAGENOTINBUF -4	/* page to be unfixed is not in the buffer */
#define PFE_UNIX	-5	/* unix error */
#define PFE_INCOMPLETEREAD -6	/* incomplete read of page from file */
#define PFE_INCOMPLETEWRITE -7	/* incomplete write of page to file */
#define PFE_HDRREAD	-8	/* incomplete read of header from file */
#define PFE_HDRWRITE	-9	/* incomplte write of header to file */
#define PFE_INVALIDPAGE -10	/* invalid page number */
#define PFE_FILEOPEN	-11	/* file already open */
#define	PFE_FTABFULL	-12	/* file table is full */
#define PFE_FD		-13	/* invalid file descriptor */
#define PFE_EOF		-14	/* end of file */
#define PFE_PAGEFREE	-15	/* page already free */
#define PFE_PAGEUNFIXED	-16	/* page already unfixed */

/* Internal error: please report to the TA */
#define PFE_PAGEINBUF	-17	/* new page to be allocated already in buffer */
#define PFE_HASHNOTFOUND -18	/* hash table entry not found */
#define PFE_HASHPAGEEXIST -19	/* page already exist in hash table */


/* page size */
/* CHANGED [B2]: allow compile-time page-size overrides for shared PF/AM builds. */
#ifndef PF_PAGE_SIZE
#define PF_PAGE_SIZE	4096
#endif

/* externs from the PF layer */
extern int PFerrno;		/* error number of last error */
extern void PF_Init();
extern void PF_PrintError();

/* CHANGED [P3-P6]: interface frozen in commit #5, implemented in commits #6-#9 */
#ifndef PF_FROZEN_INTERFACE_H
#define PF_FROZEN_INTERFACE_H

#include <stdio.h>

#define PF_LRU 0
#define PF_MRU 1

typedef struct {
	long logical_reads;
	long logical_writes;
	long hits;
	long misses;
	long physical_reads;
	long physical_writes;
	long evictions;
	long dirty_evictions;
} PFstats;

/*
 * Open fname with the requested replacement policy.  PF_OpenFile remains
 * available and uses the default policy.
 */
int PF_OpenFileEx(char *fname, int policy);

/*
 * Mark pageNum dirty without changing its recency.  Return PFE_OK or a PF
 * error code.
 */
int PF_MarkDirty(int fd, int pageNum);

/*
 * Copy statistics for fd into out.  fd == -1 requests totals over all open
 * files.  Return PFE_OK or a PF error code.
 */
int PF_GetStats(int fd, PFstats *out);

/*
 * Reset statistics for fd.  fd == -1 resets totals for all open files.
 */
void PF_ResetStats(int fd);

/*
 * Print statistics for fd to out.  fd == -1 prints totals over all open
 * files.
 */
void PF_PrintStats(int fd, FILE *out);

/*
 * Flush all dirty pages from every open file.  Return PFE_OK or a PF error
 * code.
 */
int PF_FlushAll(void);

#endif /* PF_FROZEN_INTERFACE_H */
