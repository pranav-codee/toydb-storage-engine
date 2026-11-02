/* buf.c: buffer management routines. The interface routines are:
PFbufGet(), PFbufUnfix(), PFbufAlloc(), PFbufReleaseFile(), PFbufUsed() and
PFbufPrint() */
/* CHANGED [P5]: PFbufMarkDirty() is a new interface routine, used by
   PF_MarkDirty() */
/* CHANGED [P6]: the I/O counters are bumped here, where hits, misses,
   reads, writes and evictions actually happen (definitions in the spec of
   PF_GetStats() in pf.c). PFbufFlushAll() is a new interface routine. */
#include <stdio.h>
#include "pf.h"
#include "pftypes.h"

static int PFnumbpage = 0;	/* # of buffer pages in memory */
static PFbpage *PFfirstbpage= NULL;	/* ptr to first buffer page, or NULL */
static PFbpage *PFlastbpage = NULL;	/* ptr to last buffer page, or NULL */
static PFbpage *PFfreebpage= NULL;	/* list of free buffer pages */
/* CHANGED [P1]: all frames come from one malloc'd array of the configured size */
static PFbpage *PFbufpool = NULL;	/* the frame array, or NULL before PFbufInit */

extern char *malloc();

static void PFbufInsertFree(bpage)
PFbpage *bpage;
/****************************************************************************
SPECIFICATIONS:
	Insert the buffer page pointed by "bpage" into the free list.

AUTHOR: clc
*****************************************************************************/
{
	bpage->nextpage = PFfreebpage;
	PFfreebpage = bpage;
}


static void PFbufLinkHead(bpage)
PFbpage *bpage;		/* pointer to buffer page to be linked */
/****************************************************************************
SPECIFICATIONS:

	Link the buffer page pointed by "bpage" as the head
	of the used buffer list. No other field of bpage is modified.

AUTHOR: clc

RETURN VALUE:
	none.

GLOBAL VARIABLES MODIFIED:
	PFfirstbpage, PFlastbpage.

*****************************************************************************/
{

	bpage->nextpage = PFfirstbpage;
	bpage->prevpage = NULL;
	if (PFfirstbpage != NULL)
		PFfirstbpage->prevpage = bpage;
	PFfirstbpage = bpage;
	if (PFlastbpage == NULL)
		PFlastbpage = bpage;
}
	
void PFbufUnlink(bpage)
PFbpage *bpage;		/* buffer page to be unlinked from the used list */
/****************************************************************************
SPECIFICATIONS:
	Unlink the page pointed by bpage from the buffer list. Assume
	that bpage is a valid pointer.  Set the "prevpage" and "nextpage"
	fields to NULL. The caller is responsible to either place
	the unlinked page into the free list, or insert it back
	into the used list.

AUTHOR: clc

RETURN VALUE:
	none

GLOBAL VARIABLES MODIFIED:
	PFfirstbpage,PFlastbpage.
*****************************************************************************/
{

	if (PFfirstbpage == bpage)
		PFfirstbpage = bpage->nextpage;
	
	if (PFlastbpage == bpage)
		PFlastbpage = bpage->prevpage;
	
	if (bpage->nextpage != NULL)
		bpage->nextpage->prevpage = bpage->prevpage;
	
	if (bpage->prevpage != NULL)
		bpage->prevpage->nextpage = bpage->nextpage;

	bpage->prevpage = bpage->nextpage = NULL;

}


/* CHANGED [P3]: new "policy" argument, the policy of the file the incoming
   page belongs to */
static PFbufInternalAlloc(bpage,writefcn,policy)
PFbpage **bpage;	/* pointer to pointer to buffer bpage to be allocated*/
int (*writefcn)();
int policy;		/* PF_LRU or PF_MRU */
/****************************************************************************
SPECIFICATIONS:
	Allocate a buffer page and set *bpage to point to it. *bpage
	is set to NULL if one can not be allocated.
	The "nextpage" and "prevpage" fields of *bpage are linked as
	the head of the list of used buffers.All the other fields are undefined.
	writefcn() is used to write pages. (See PFbufGet()).

ALGORITHM:
	If there is something on the free list, then use it.
	(CHANGED [P1]: all frames are on the free list after PFbufInit(),
	so there is no malloc() here any more.)
	Otherwise, choose a victim to write out, and then use that
	page as the page to be used.
	CHANGED [P3]: the victim depends on "policy" (the incoming page's
	file), and it can be a page of any file since the pool is shared:
		PF_LRU: walk from the tail, take the first unfixed page
			(least recently used, the old code).
		PF_MRU: walk from the head, take the first unfixed page
			(most recently used).
	The head is the page most recently brought in or unfixed.
	A dirty victim is written with writefcn(victim fd, victim page),
	so with PFwritefcn it goes to the victim's own file even when the
	victim belongs to another file.
	If a victim cannot be chosen (because all the pages are fixed),
	then return error.

AUTHOR: clc

RETURN VALUE:

	PFE_OK	if no error.
	PF_NOMEM	if no memory.
	PF_NOBUF	if no buffer space left because all pages are fixed.

GLOBAL VARIABLES MODIFIED:
	PFnumbpage, PFfirstbpage, PFlastbpage, PFfreebpage
*****************************************************************************/
{
PFbpage *tbpage;	/* temporary pointer to buffer page */
int error;		/* error value returned*/

	/* CHANGED [P1]: a program that never called PF_Init() (testpf.c)
	used to get frames malloc'd on demand; give it the old default
	pool of PF_DEFAULT_BUFS frames the first time a frame is needed */
	if (PFbufpool == NULL &&
			(error=PFbufInit(PF_DEFAULT_BUFS)) != PFE_OK){
		*bpage = NULL;
		return(error);
	}

	/* Set *bpage to the buffer page to be returned */
	if (PFfreebpage != NULL){
		/* Free list not empty, use the one from the free list. */
		*bpage = PFfreebpage;
		PFfreebpage = (*bpage)->nextpage;
	}
	/* CHANGED [P1]: removed the one-frame-at-a-time malloc() branch
	(PFnumbpage < PF_MAX_BUFS); PFbufInit() puts every frame on the
	free list up front, so an empty free list means the pool is full */
	else {
		/* we have reached max buffer limit */
		/* choose a victim from the buffer*/

		*bpage = NULL;		/* set initial return value */

		/* CHANGED [P3]: MRU searches from the head, LRU from the tail */
		if (policy == PF_MRU){
			for (tbpage=PFfirstbpage;tbpage!=NULL;
					tbpage=tbpage->nextpage){
				if (!tbpage->fixed)
					/* found a page that can be swapped out */
					break;
			}
		}
		else {
		for (tbpage=PFlastbpage;tbpage!=NULL;tbpage=tbpage->prevpage){
			if (!tbpage->fixed)
				/* found a page that can be swapped out */
				break;
		}
		}

		if (tbpage == NULL){
			/* couldn't find a free page */
			PFerrno = PFE_NOBUF;
			return(PFerrno);
		}

		/* write out the dirty page */
		if (tbpage->dirty&&((error=(*writefcn)(tbpage->fd,
				tbpage->page,&tbpage->fpage))!= PFE_OK))
			return(error);
		/* CHANGED [P6]: the victim is taken now. Charge the eviction
		(and the write-back, if it was dirty) to the victim's file,
		which may not be the file of the incoming page */
		PFstatInc(tbpage->fd, evictions);
		if (tbpage->dirty){
			PFstatInc(tbpage->fd, dirty_evictions);
			PFstatInc(tbpage->fd, physical_writes);
		}
		tbpage->dirty = FALSE;

		/* unlink from hash table */
		if ((error=PFhashDelete(tbpage->fd,tbpage->page))!= PFE_OK)
			return(error);
		
		/* unlink from buffer list */
		PFbufUnlink(tbpage);

		*bpage = tbpage;

	}

	/* Link the page as the head of the used list */
	PFbufLinkHead(*bpage);
	return(PFE_OK);
}


/************************* Interface to the Outside World ****************/

/* CHANGED [P1]: new function, sets up a pool of n frames at runtime */
PFbufInit(n)
int n;		/* number of frames in the pool */
/****************************************************************************
SPECIFICATIONS:
	Allocate a pool of "n" buffer frames with a single malloc() and put
	them all on the free list. The caller (PF_InitBuffer) checks that
	n >= PF_MIN_BUFS.
	Can be called again to resize the pool, but only when no page is
	in the buffer at all (all files closed). An unfixed page can still
	be dirty and is still in the hash table, so it is not enough that
	no page is fixed. On a valid re-init the old pool is freed. If the
	new malloc() fails the old pool is kept unchanged.

RETURN VALUE:
	PFE_OK		if ok.
	PFE_BUFINUSE	if some page is still in the buffer.
	PFE_NOMEM	if the pool can't be allocated.

GLOBAL VARIABLES MODIFIED:
	PFbufpool, PFnumbpage, PFfreebpage
*****************************************************************************/
{
PFbpage *newpool;	/* the new frame array */
int i;

	if (PFfirstbpage != NULL){
		/* pages are still in the buffer, can't resize */
		PFerrno = PFE_BUFINUSE;
		return(PFerrno);
	}

	if ((newpool=(PFbpage *)malloc((unsigned)n*sizeof(PFbpage)))==NULL){
		PFerrno = PFE_NOMEM;
		return(PFerrno);
	}

	/* the used list is empty, so every old frame is on the free list
	and nothing points into the old pool any more */
	if (PFbufpool != NULL)
		free((char *)PFbufpool);
	PFbufpool = newpool;
	PFnumbpage = n;

	/* link all frames into the free list, frame 0 first */
	PFfreebpage = NULL;
	for (i = n-1; i >= 0; i--)
		PFbufInsertFree(&PFbufpool[i]);

	return(PFE_OK);
}

/* CHANGED [P6]: new argument "count" */
PFbufGet(fd,pagenum,fpage,readfcn,writefcn,count)
int fd;	/* file descriptor */
int pagenum;	/* page number */
PFfpage **fpage;	/* pointer to pointer to file page */
int (*readfcn)();	/* function to read a page */
int (*writefcn)();	/* function to write a page */
int count;	/* TRUE: count a logical read and a hit or miss */
/****************************************************************************
SPECIFICATIONS:
	Get a page whose number is "pagenum" from the file pointed
	by "fd". Set *fpage to point to the data for that page.
	This function requires two functions:
		readfcn(fd,pagenum,fpage) 
		int fd;
		int pagenum;
		PFfpage *fpage;
	which will read one page whose number is "pagenum" from the file "fd"
	into the buffer area pointed by "fpage".
		writefcn(fd,pagenum,fpage)
		int fd;
		in pagenum;
		PFpage *fpage;
	which will write one page into the file.
	It is an error to read a page already fixed in the buffer.
	CHANGED [P6]: statistics, all charged to file "fd":
	- every page actually read with readfcn() is a physical read,
	  whatever "count" is;
	- if "count" is TRUE and the page ends up fixed (PFE_OK), it is
	  one logical read plus a hit (page was in the pool) or a miss
	  (it was read). pf.c passes TRUE only from PF_GetThisPage() and
	  PF_GetNextPage(); PF_AllocPage() and PF_DisposePage() pass FALSE.
	- a failed request (PFE_PAGEFIXED, no buffer, read error) is not a
	  logical read.
	The page's lwcounted flag is cleared, since a new fix period starts.

RETURN VALUE:
	PFE_OK	if no error.
	PF error code if error.
	If error code is PFE_PAGEFIXED, *fpage is still set to point to the buffer
	page of the page in memory.

GLOBAL VARIABLES MODIFIED:
*****************************************************************************/
{
PFbpage *bpage;	/* pointer to buffer */
int error;

	if ((bpage=PFhashFind(fd,pagenum)) == NULL){
		/* page not in buffer. */
		
		/* allocate an empty page */
		/* CHANGED [P3]: victim chosen by the policy of file fd */
		if ((error=PFbufInternalAlloc(&bpage,writefcn,
				PFftabGetPolicy(fd)))!= PFE_OK){
			/* error */
			*fpage = NULL;
			return(error);
		}
		
		/* read the page */
		if ((error=(*readfcn)(fd,pagenum,&bpage->fpage))!= PFE_OK){
			/* error reading the page. put buffer back into 
			the free list, and return gracefully */
			PFbufUnlink(bpage);
			PFbufInsertFree(bpage);
			*fpage = NULL;
			return(error);
		}
		/* CHANGED [P6]: one page read from the file */
		PFstatInc(fd, physical_reads);

		/* insert new page into hash table */
		if ((error=PFhashInsert(fd,pagenum,bpage))!=PFE_OK){
			/* failed to insert into hash table */
			/* put page into free list */
			PFbufUnlink(bpage);
			PFbufInsertFree(bpage);
			return(error);
		}

		/* set the fields for this page*/
		bpage->fd = fd;
		bpage->page = pagenum;
		bpage->dirty = FALSE;
		/* CHANGED [P6]: a miss */
		if (count)
			PFstatInc(fd, misses);
	}
	else if (bpage->fixed){
		/* page already in memory, and is fixed, so we can't
		get it again. */
		*fpage = &bpage->fpage;
		PFerrno = PFE_PAGEFIXED;
		return(PFerrno);
	}
	/* CHANGED [P6]: a hit */
	else if (count)
		PFstatInc(fd, hits);

	/* CHANGED [P6]: hit or miss, it is one logical read, and a new fix
	period starts for logical write counting */
	if (count)
		PFstatInc(fd, logical_reads);
	bpage->lwcounted = FALSE;

	/* Fix the page in the buffer then return*/
	bpage->fixed = TRUE;
	*fpage = &bpage->fpage;
	return(PFE_OK);
}

PFbufUnfix(fd,pagenum,dirty)
int fd;		/* file descriptor */
int pagenum;	/* page number */
int dirty;	/* TRUE if page is dirty */
/****************************************************************************
SPECIFICATIONS:
	Unfix the file page whose number is "pagenum" from the buffer.
	If dirty is TRUE, then mark the buffer as having been modified.
	Otherwise, the dirty flag is left unchanged.

AUTHOR: clc

RETURN VALUE:
	PFE_OK if no error.
	PF error codes if error occurs.

*****************************************************************************/
{
PFbpage *bpage;

	if ((bpage= PFhashFind(fd,pagenum))==NULL){
		/* page not in buffer */
		PFerrno = PFE_PAGENOTINBUF;
		return(PFerrno);
	}

	if (!bpage->fixed){
		/* page already unfixed */
		PFerrno = PFE_PAGEUNFIXED;
		return(PFerrno);
	}

	if (dirty){
		/* mark this page dirty */
		bpage->dirty = TRUE;
		/* CHANGED [P6]: a logical write, unless one was already
		counted for this page since it was fixed */
		if (!bpage->lwcounted){
			PFstatInc(fd, logical_writes);
			bpage->lwcounted = TRUE;
		}
	}

	/* unfix the page */
	bpage->fixed = FALSE;
	
	/* unlink this page */
	PFbufUnlink(bpage);

	/* insert it as head of linked list to make it most recently used*/
	PFbufLinkHead(bpage);

	return(PFE_OK);
}

PFbufAlloc(fd,pagenum,fpage,writefcn)
int fd;		/* file descriptor */
int pagenum;	/* page number */
PFfpage **fpage;	/* pointer to file page */
int (*writefcn)();
/****************************************************************************
SPECIFICATIONS:
	Allocate a buffer and mark it belonging to page "pagenum"
	of file "fd".  Set *fpage to point to the buffer data.
	The function "writefcn" is used to write out pages. (See PFbufGet()).

AUTHOR: clc

RETURN VALUE:
	PFE_OK if successful.
	PF error codes if unsuccessful
*****************************************************************************/
{
PFbpage *bpage;
int error;

	*fpage = NULL;	/* initial value of fpage */

	if ((bpage=PFhashFind(fd,pagenum))!= NULL){
		/* page already in buffer*/
		PFerrno = PFE_PAGEINBUF;
		return(PFerrno);
	}

	/* CHANGED [P3]: victim chosen by the policy of file fd */
	if ((error=PFbufInternalAlloc(&bpage,writefcn,
			PFftabGetPolicy(fd)))!= PFE_OK)
		/* can't get any buffer */
		return(error);
	
	/* put ourselves into the hash table */
	if ((error=PFhashInsert(fd,pagenum,bpage))!= PFE_OK){
		/* can't insert into the hash table */
		/* unlink bpage, and put it into the free list */
		PFbufUnlink(bpage);
		PFbufInsertFree(bpage);
		return(error);
	}

	/* init the fields of bpage and return */
	bpage->fd = fd;
	bpage->page = pagenum;
	bpage->fixed = TRUE;
	bpage->dirty = FALSE;
	/* CHANGED [P6]: a new fix period; no read is counted, the page
	is new and not read from the file */
	bpage->lwcounted = FALSE;

	*fpage = &bpage->fpage;
	return(PFE_OK);
}


PFbufReleaseFile(fd,writefcn)
int fd;		/* file descriptor */
int (*writefcn)();	/* function to write a page of file */
/****************************************************************************
SPECIFICATIONS:
	Release all pages of file "fd" from the buffer and
	put them into the free list
	CHANGED [P6]: each dirty page written back is a physical write of
	file "fd". These pages are not counted as evictions. PF_CloseFile()
	calls this before it frees the file table entry, so fd is still
	valid and the per-file counters get the writes too.

AUTHOR: clc

RETURN VALUE:
	PFE_OK if no error.
	PF error code if error.

IMPLEMENTATION NOTES:
	A linear search of the buffer is performed.
	A better method is not needed because # of buffers are small.
*****************************************************************************/
{
PFbpage *bpage;	/* ptr to buffer pages to search */
PFbpage *temppage;
int error;		/* error code */

	/* Do linear scan of the buffer to find pages belonging to the file */
	bpage = PFfirstbpage;
	while (bpage != NULL){
		if (bpage->fd == fd){
			/* The file descriptor matches*/
			if (bpage->fixed){
				PFerrno = PFE_PAGEFIXED;
				return(PFerrno);
			}

			/* write out dirty page */
			if (bpage->dirty&&((error=(*writefcn)(fd,bpage->page,
					&bpage->fpage))!= PFE_OK))
				/* error writing file */
				return(error);
			/* CHANGED [P6]: a write-back at close */
			if (bpage->dirty)
				PFstatInc(fd, physical_writes);
			bpage->dirty = FALSE;

			/* get rid of it from the hash table */
			if ((error=PFhashDelete(fd,bpage->page))!= PFE_OK){
				/* internal error */
				printf("Internal error:PFbufReleaseFile()\n");
				exit(1);
			}

			/* put the page into free list */
			temppage = bpage;
			bpage = bpage->nextpage;
			PFbufUnlink(temppage);
			PFbufInsertFree(temppage);

		}
		else	bpage = bpage->nextpage;
	}
	return(PFE_OK);
}


PFbufUsed(fd,pagenum)
int fd;		/* file descriptor */
int pagenum;	/* page number */
/****************************************************************************
SPECIFICATIONS:
	Mark page numbered "pagenum" of file descriptor "fd" as used.
	The page must be fixed in the buffer. Make this page most
	recently used.
	CHANGED [P6]: marking the page dirty is a logical write (at most
	once per fix period). Only PF_AllocPage() calls this, for a page
	that is new, so this is where "dirty from birth" is counted.

AUTHOR: clc

RETURN VALUE: PF error codes.

*****************************************************************************/
{
PFbpage *bpage;	/* pointer to the bpage we are looking for */

	/* Find page in the buffer */
	if ((bpage=PFhashFind(fd,pagenum))==NULL){
		/* page not in the buffer */
		PFerrno = PFE_PAGENOTINBUF;
		return(PFerrno);
	}

	if (!(bpage->fixed)){
		/* page not fixed */
		PFerrno = PFE_PAGEUNFIXED;
		return(PFerrno);
	}

	/* mark this page dirty */
	bpage->dirty = TRUE;
	/* CHANGED [P6]: a logical write, once per fix period */
	if (!bpage->lwcounted){
		PFstatInc(fd, logical_writes);
		bpage->lwcounted = TRUE;
	}

	/* make this page head of the list of buffers*/
	PFbufUnlink(bpage);
	PFbufLinkHead(bpage);

	return(PFE_OK);
}

/* CHANGED [P5]: new function behind PF_MarkDirty(), dirty bit only */
PFbufMarkDirty(fd,pagenum)
int fd;		/* file descriptor */
int pagenum;	/* page number */
/****************************************************************************
SPECIFICATIONS:
	Set the dirty bit of page "pagenum" of file "fd", which must be
	in the buffer (fixed or unfixed). Unlike PFbufUsed(), the page is
	not moved in the used list and its fixed flag is not touched, and
	no I/O is done. Setting the bit again is harmless. The bit is only
	cleared when the page is written back (eviction in
	PFbufInternalAlloc() or PFbufReleaseFile()); PFbufUnfix() with
	dirty FALSE leaves it as it is.
	CHANGED [P6]: this is a logical write of file "fd", unless one was
	already counted for the page since it was last fixed (so MarkDirty
	followed by PFbufUnfix(TRUE) in one fix period counts once).
	CHANGED [P6]: PFbufFlushAll() also clears the dirty bit.

RETURN VALUE:
	PFE_OK		if no error.
	PFE_PAGENOTINBUF	if the page is not in the buffer.

GLOBAL VARIABLES MODIFIED:
	none (only the dirty field of the page's buffer).
*****************************************************************************/
{
PFbpage *bpage;	/* pointer to the bpage we are looking for */

	if ((bpage=PFhashFind(fd,pagenum))==NULL){
		/* page not in the buffer */
		PFerrno = PFE_PAGENOTINBUF;
		return(PFerrno);
	}

	/* mark this page dirty, nothing else */
	bpage->dirty = TRUE;
	/* CHANGED [P6]: a logical write, once per fix period */
	if (!bpage->lwcounted){
		PFstatInc(fd, logical_writes);
		bpage->lwcounted = TRUE;
	}

	return(PFE_OK);
}

/* CHANGED [P6]: new function behind PF_FlushAll() */
PFbufFlushAll(writefcn)
int (*writefcn)();	/* function to write a page */
/****************************************************************************
SPECIFICATIONS:
	Write every dirty page in the pool to its own file with
	writefcn(fd,page,fpage), fixed or not, and clear its dirty bit.
	The pages stay in the pool and keep their place in the used list,
	and the fixed flag is not touched, so LRU/MRU see nothing. Each
	page written is one physical write of the file it belongs to.
	If a write fails, the page stays dirty, the other pages are still
	written, and the first error is returned.

RETURN VALUE:
	PFE_OK		if every write worked.
	the first PF error code from writefcn() otherwise.

GLOBAL VARIABLES MODIFIED:
	none (only the dirty fields of the pages).
*****************************************************************************/
{
PFbpage *bpage;
int error;
int first = PFE_OK;	/* first error seen */

	for (bpage = PFfirstbpage; bpage != NULL; bpage = bpage->nextpage){
		if (!bpage->dirty)
			continue;
		if ((error=(*writefcn)(bpage->fd,bpage->page,
				&bpage->fpage)) != PFE_OK){
			if (first == PFE_OK)
				first = error;
			continue;
		}
		PFstatInc(bpage->fd, physical_writes);
		bpage->dirty = FALSE;
	}
	return(first);
}

/* CHANGED [P3]: new debug helper for tests/test_policy.c, not in pf.h */
PFbufResident(fd,pagenum)
int fd;		/* file descriptor */
int pagenum;	/* page number */
/****************************************************************************
SPECIFICATIONS:
	Tell whether page "pagenum" of file "fd" is in the buffer pool
	right now. Only a hash table lookup: nothing is fixed, read or
	moved, so the recency order is not changed. Debug helper, not part
	of the public interface.

RETURN VALUE:
	1 if the page is in the buffer, 0 if not.
*****************************************************************************/
{
	return(PFhashFind(fd,pagenum) != NULL);
}

void PFbufPrint()
/****************************************************************************
SPECIFICATIONS:
	Print the current page buffers.

AUTHOR: clc

*****************************************************************************/
{
PFbpage *bpage;

	printf("buffer content:\n");
	if (PFfirstbpage == NULL)
		printf("empty\n");
	else {
		printf("fd\tpage\tfixed\tdirty\tfpage\n");
		/* CHANGED [P7]: print the frame address with %p; the old
		(int) cast and %d cut a 64-bit pointer in half */
		for(bpage = PFfirstbpage; bpage != NULL; bpage= bpage->nextpage)
			printf("%d\t%d\t%d\t%d\t%p\n",
				bpage->fd,bpage->page,(int)bpage->fixed,
				(int)bpage->dirty,(void *)&bpage->fpage);
	}
}
