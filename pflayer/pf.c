/* pf.c: Paged File Interface Routines+ support routines */
#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/file.h>
#include "pf.h"
#include "pftypes.h"
#include "config.h"	/* CHANGED [P1]: PF_Init reads buffer_pool_size */
#include <strings.h>	/* CHANGED [P4]: strcasecmp for default_policy */

/* To keep system V and PC users happy */
#ifndef L_SET
#define L_SET 0
#endif

int PFerrno = PFE_OK;	/* last error message */

static PFftab_ele PFftab[PF_FTAB_SIZE]; /* table of opened files */

/* CHANGED [P4]: policy PF_OpenFile uses. PF_Init sets it from
   default_policy in the config; it starts as LRU so a program that never
   calls PF_Init (testpf.c) behaves as before */
static int PFdefaultPolicy = PF_LRU;

/* true if file descriptor fd is invaild */
#define PFinvalidFd(fd) ((fd) < 0 || (fd) >= PF_FTAB_SIZE \
				|| PFftab[fd].fname == NULL)

/* true if page number "pagenum" of file "fd" is invalid in the
sense that it's <0 or >= # of pages in the file */
#define PFinvalidPagenum(fd,pagenum) ((pagenum)<0 || (pagenum) >= \
				PFftab[fd].hdr.numpages)

extern char *malloc();

/****************** Internal Support Functions *****************************/
static char *savestr(str)
char *str;		/* string to be saved */
/****************************************************************************
SPECIFICATIONS:
	Allocate memory and save string pointed by str.
	Return a pointer to the saved string, or NULL if no memory.
*****************************************************************************/
{
char *s;

	if ((s=malloc(strlen(str)+1))!= NULL)
		strcpy(s,str);
	return(s);
}

static PFtabFindFname(fname)
char *fname;		/* file name to find */
/****************************************************************************
SPECIFICATIONS:
	Find the index to PFftab[] entry whose "fname" field is the
	same as "fname". 

AUTHOR: clc

RETURN VALUE:
	The desired index, or 
	-1	if not found

*****************************************************************************/
{
int i;

	for (i=0; i < PF_FTAB_SIZE; i++){
		if(PFftab[i].fname != NULL&& strcmp(PFftab[i].fname,fname) == 0)
			/* found it */
			return(i);
	}
	return(-1);
}

static PFftabFindFree()
/****************************************************************************
SPECIFICATIONS:
	Find a free entry in the open file table "PFtab", and return its
	index.

AUTHOR: clc

RETURN VALUE:
	If >=0, the index of the free entry.
	Otherwise, none can be found.

*****************************************************************************/
{
int i;

	for (i=0; i < PF_FTAB_SIZE; i++)
		if (PFftab[i].fname == NULL)
			return(i);
	return(-1);
}

PFreadfcn(fd,pagenum,buf)
int fd;	/* file descriptor */
int pagenum; /* page number */
PFfpage *buf;
/****************************************************************************
SPECIFICATIONS:
	Read the paged numbered "pagenum" from the file indexed by "fd"
	into the page buffer "buf".

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if ok
	PF error code if not OK.
*****************************************************************************/
{
int error;

	/* seek to the appropriate place */
	if ((error=lseek(PFftab[fd].unixfd,pagenum*sizeof(PFfpage)+PF_HDR_SIZE,
				L_SET)) == -1){
		PFerrno = PFE_UNIX;
		return(PFerrno);
	}

	/* read the data */
	if((error=read(PFftab[fd].unixfd,(char *)buf,sizeof(PFfpage)))
			!=sizeof(PFfpage)){
		if (error <0)
			PFerrno = PFE_UNIX;
		else	PFerrno = PFE_INCOMPLETEREAD;
		return(PFerrno);
	}

	return(PFE_OK);
}

PFwritefcn(fd,pagenum,buf)
int fd;		/* file descriptor */
int pagenum;	/* page to read */
PFfpage *buf;	/* buffer where to read the page */
/****************************************************************************
SPECIFICATIONS:
	Write the page numbered "pagenum" from the buffer indexed
	by "buf" into the file indexed by "fd".

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if ok.
	PF errod code if not OK.

*****************************************************************************/
{
int error;

	/* seek to the right place */
	if ((error=lseek(PFftab[fd].unixfd,pagenum*sizeof(PFfpage)+PF_HDR_SIZE,
				L_SET)) == -1){
		PFerrno = PFE_UNIX;
		return(PFerrno);
	}

	/* write out the page */
	if((error=write(PFftab[fd].unixfd,(char *)buf,sizeof(PFfpage)))
			!=sizeof(PFfpage)){
		if (error <0)
			PFerrno = PFE_UNIX;
		else	PFerrno = PFE_INCOMPLETEWRITE;
		return(PFerrno);
	}

	return(PFE_OK);

}


/************************* Interface Routines ****************************/

void PF_Init()
/****************************************************************************
SPECIFICATIONS:
	Initialize the PF interface. Must be the first function called
	in order to use the PF ADT.
	CHANGED [P1]: also sets up the buffer pool with
	Config_GetInt("buffer_pool_size", PF_DEFAULT_BUFS) frames. The
	config is only seen if Config_Init() was called before PF_Init();
	otherwise the pool has PF_DEFAULT_BUFS frames, as before. If the
	pool can't be set up (size below PF_MIN_BUFS, or no memory) an
	error is printed and the program exits, so a bad config never
	gives results for some other pool size.
	CHANGED [P4]: also reads Config_GetStr("default_policy", "LRU")
	once, the policy PF_OpenFile() uses from then on. "LRU" and "MRU"
	are accepted in any case; any other value prints an error and the
	program exits, like a bad pool size. With no config loaded the
	default is LRU, as before.

AUTHOR: clc

RETURN VALUE: none

GLOBAL VARIABLES MODIFIED:
	PFftab
*****************************************************************************/
{
int i;
int nbufs;	/* CHANGED [P1]: configured pool size */
const char *pol;	/* CHANGED [P4]: configured default policy */
	/* init the hash table */
	PFhashInit();

	/* init the file table to be not used*/
	for (i=0; i < PF_FTAB_SIZE; i++){
		PFftab[i].fname = NULL;
	}

	/* CHANGED [P1]: size the buffer pool from the config */
	nbufs = Config_GetInt("buffer_pool_size", PF_DEFAULT_BUFS);
	if (PF_InitBuffer(nbufs) != PFE_OK){
		if (PFerrno == PFE_INVALIDBUFS)
			fprintf(stderr,
				"toydb: buffer_pool_size=%d is below the minimum of %d\n",
				nbufs, PF_MIN_BUFS);
		else {
			fprintf(stderr, "toydb: buffer_pool_size=%d", nbufs);
			PF_PrintError("");
		}
		exit(1);
	}

	/* CHANGED [P4]: default replacement policy for PF_OpenFile */
	pol = Config_GetStr("default_policy", "LRU");
	if (strcasecmp(pol, "LRU") == 0)
		PFdefaultPolicy = PF_LRU;
	else if (strcasecmp(pol, "MRU") == 0)
		PFdefaultPolicy = PF_MRU;
	else {
		fprintf(stderr, "toydb: default_policy=%s must be LRU or MRU\n",
			pol);
		exit(1);
	}
}

/* CHANGED [P1]: new function, runtime buffer pool size */
int PF_InitBuffer(nbufs)
int nbufs;	/* number of buffer frames wanted */
/****************************************************************************
SPECIFICATIONS:
	Set up the buffer pool with "nbufs" frames (one malloc() in
	PFbufInit). PF_Init() calls this with the configured size; a
	program can call it again later to resize the pool, but only while
	no page is in the buffer (all files closed). On any error the old
	pool is kept.

RETURN VALUE:
	PFE_OK		if ok
	PFE_INVALIDBUFS	if nbufs < PF_MIN_BUFS
	PFE_BUFINUSE	if some page is still in the buffer
	PFE_NOMEM	if no memory
*****************************************************************************/
{
	if (nbufs < PF_MIN_BUFS){
		PFerrno = PFE_INVALIDBUFS;
		return(PFerrno);
	}
	return(PFbufInit(nbufs));
}

PF_CreateFile(fname)
char *fname;	/* name of file to create */
/****************************************************************************
SPECIFICATIONS:
	Create a paged file called "fname". The file should not have
	already existed before.

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if OK
	PF error code if error.
*****************************************************************************/
{
int fd;	/* unix file descripotr */
PFhdr_str hdr;	/* file header */
int error;

	/* create file for exclusive use */
	if ((fd=open(fname,O_CREAT|O_EXCL|O_WRONLY,0664))<0){
		/* unix error on open */
		PFerrno = PFE_UNIX;
		return(PFE_UNIX);
	}

	/* write out the file header */
	hdr.firstfree = PF_PAGE_LIST_END;	/* no free pag yet */
	hdr.numpages = 0;
	if ((error=write(fd,(char *)&hdr,sizeof(hdr))) != sizeof(hdr)){
		/* error while writing. Abort everything. */
		if (error < 0)
			PFerrno = PFE_UNIX;
		else PFerrno = PFE_HDRWRITE;
		close(fd);
		unlink(fname);
		return(PFerrno);
	}

	if ((error=close(fd)) == -1){
		PFerrno = PFE_UNIX;
		return(PFerrno);
	}

	return(PFE_OK);
}


PF_DestroyFile(fname)
char *fname;		/* file name to destroy */
/****************************************************************************
SPECIFICATIONS:
	Destroy the paged file whose name is "fname". The file should
	exist, and should not be already open.

AUTHOR:
	clc

RETURN VALUE:
	PFE_OK 	if success
	PF error codes if error
*****************************************************************************/
{
int error;

	if (PFtabFindFname(fname)!= -1){
		/* file is open */
		PFerrno = PFE_FILEOPEN;
		return(PFerrno);
	}

	if ((error =unlink(fname))!= 0){
		/* unix error */
		PFerrno = PFE_UNIX;
		return(PFerrno);
	}

	/* success */
	return(PFE_OK);
}


/* CHANGED [P3]: was PF_OpenFile; the same code, plus a replacement policy */
PF_OpenFileEx(fname,policy)
char *fname;		/* name of the file to open */
int policy;		/* PF_LRU or PF_MRU */
/****************************************************************************
SPECIFICATIONS:
	CHANGED [P3]: like PF_OpenFile, but the file gets its own
	replacement policy, "policy" (PF_LRU or PF_MRU), kept in its file
	table entry.
	The buffer pool is global and shared by all open files. When a
	page has to be brought in and the pool is full, the policy of the
	file the INCOMING page belongs to picks the victim, and the victim
	can be a page of any file:
		PF_LRU: the unfixed page nearest the tail of the used list
			(least recently used, the old behaviour).
		PF_MRU: the first unfixed page from the head (most recently
			used).
	A page goes to the head of the used list when it is brought into
	the buffer and every time it is unfixed. Fixing a page that is
	already in the buffer does not move it. Fixed pages are never
	victims; if every page is fixed the fix fails with PFE_NOBUF.

	Old spec of PF_OpenFile:
	Open the paged file whose name is fname.  It is possible to open
	a file more than once. Warning: Openinging a file more than once for 
	write operations is not prevented. The possible consequence is
	the corruption of the file structure, which will crash
	the Paged File functions. On the other hand, opening a file
	more than once for reading is OK.

AUTHOR: clc

RETURN VALUE:
	The file descriptor, which is >= 0, if no error.
	PFE_INVALIDPOLICY if policy is not PF_LRU or PF_MRU (CHANGED [P3]).
	PF error codes otherwise.

IMPLEMENTATION NOTES:
	A file opened more than once will have different file descriptors
	returned. Separate buffers are used.
*****************************************************************************/
{
int count;	/* # of bytes in read */
int fd; /* file descriptor */

	/* CHANGED [P3]: check the policy before taking a table entry */
	if (policy != PF_LRU && policy != PF_MRU){
		PFerrno = PFE_INVALIDPOLICY;
		return(PFerrno);
	}

	/* find a free entry in the file table */
	if ((fd=PFftabFindFree())< 0){
		/* file table full */
		PFerrno = PFE_FTABFULL;
		return(PFerrno);
	}

	/* open the file */
	if ((PFftab[fd].unixfd = open(fname,O_RDWR))< 0){
		/* can't open the file */
		PFerrno = PFE_UNIX;
		return(PFerrno);
	}

	/* Read the file header */
	if ((count=read(PFftab[fd].unixfd,(char *)&PFftab[fd].hdr,PF_HDR_SIZE))
				!= PF_HDR_SIZE){
		if (count < 0)
			/* unix error */
			PFerrno = PFE_UNIX;
		else	/* not enough bytes in file */
			PFerrno = PFE_HDRREAD;
		close(PFftab[fd].unixfd);
		return(PFerrno);
	}
	/* set file header to be not changed */
	PFftab[fd].hdrchanged = FALSE;

	/* CHANGED [P3]: remember the replacement policy of this file */
	PFftab[fd].policy = policy;

	/* save the file name */
	if ((PFftab[fd].fname = savestr(fname)) == NULL){
		/* no memory */
		close(PFftab[fd].unixfd);
		PFerrno = PFE_NOMEM;
		return(PFerrno);
	}

	return(fd);
}

/* CHANGED [P4]: PF_OpenFile is now a wrapper, so the AM layer and the
   provided tests need no edits */
PF_OpenFile(fname)
char *fname;		/* name of the file to open */
/****************************************************************************
SPECIFICATIONS:
	Open the paged file "fname" with the default replacement policy
	(default_policy in the config, read by PF_Init; LRU if none).
	See PF_OpenFileEx().

RETURN VALUE:
	Same as PF_OpenFileEx().
*****************************************************************************/
{
	return(PF_OpenFileEx(fname, PFdefaultPolicy));
}

/* CHANGED [P3]: new internal function, used by buf.c to pick a victim */
PFftabGetPolicy(fd)
int fd;		/* file descriptor */
/****************************************************************************
SPECIFICATIONS:
	Return the replacement policy of the open file "fd". The callers
	in buf.c only get fds that pf.c has already checked; for an invalid
	fd PF_LRU (the old behaviour) is returned.

RETURN VALUE:
	PF_LRU or PF_MRU.
*****************************************************************************/
{
	if (PFinvalidFd(fd))
		return(PF_LRU);
	return(PFftab[fd].policy);
}

PF_CloseFile(fd)
int fd;		/* file descriptor to close */
/****************************************************************************
SPECIFICATIONS:
	Close the file indexed by file descriptor fd. The file should have
	been opened with PFopen(). It is an error to close a file
	with pages still fixed in the buffer.

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if OK
	PF error code if error.

*****************************************************************************/
{
int error;

	if (PFinvalidFd(fd)){
		/* invalid file descriptor */
		PFerrno = PFE_FD;
		return(PFerrno);
	}
	

	/* Flush all buffers for this file */
	if ( (error=PFbufReleaseFile(fd,PFwritefcn)) != PFE_OK)
		return(error);

	if (PFftab[fd].hdrchanged){
		/* write the header back to the file */
		/* First seek to the appropriate place */
		if ((error=lseek(PFftab[fd].unixfd,(unsigned)0,L_SET)) == -1){
			/* seek error */
			PFerrno = PFE_UNIX;
			return(PFerrno);
		}

		/* write header*/
		if((error=write(PFftab[fd].unixfd, (char *)&PFftab[fd].hdr,
				PF_HDR_SIZE))!=PF_HDR_SIZE){
			if (error <0)
				PFerrno = PFE_UNIX;
			else	PFerrno = PFE_HDRWRITE;
			return(PFerrno);
		}
		PFftab[fd].hdrchanged = FALSE;
	}


		
	/* close the file */
	if ((error=close(PFftab[fd].unixfd))== -1){
		PFerrno = PFE_UNIX;
		return(PFerrno);
	}

	/* free the file name space */
	free((char *)PFftab[fd].fname);
	PFftab[fd].fname = NULL;

	return(PFE_OK);
}


PF_GetFirstPage(fd,pagenum,pagebuf)
int fd;	/* file descriptor */
int *pagenum;	/* page number of first page */
char **pagebuf;	/* pointer to the pointer to buffer */
/****************************************************************************
SPECIFICATIONS:
	Read the first page into memory and set *pagebuf to point to it.
	Set *pagenum to the page number of the page read.
	The page read is fixed in the buffer until it is unixed with
	PFunfix().

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if no error.
	PFE_EOF	if end of file reached.(meaning there is no first page. )
	other PF error code if other error.

*****************************************************************************/
{

	*pagenum = -1;
	return(PF_GetNextPage(fd,pagenum,pagebuf));
}


PF_GetNextPage(fd,pagenum,pagebuf)
int fd;	/* file descriptor of the file */
int *pagenum;	/* old page number on input, new page number on output */
char **pagebuf;	/* pointer to pointer to buffer of page data */
/****************************************************************************
SPECIFICATIONS:
	Read the next valid page after *pagenum, the current page number,
	and set *pagebuf to point to the page data. Set *pagenum
	to be the new page number. The new page is fixed in memory 
	until PFunfix() is called.
	Note that PF_GetNextPage() with *pagenum == -1 will return the 
	first valid page. PFgetFirst() is just a short hand for this.

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if success
	PFE_EOF	if end of file reached without encountering
		any used page data. 
	PFE_INVALIDPAGE  if page number is invalid.
	other PF errors code for other error.

*****************************************************************************/
{
int temppage;	/* page number to scan for next valid page */
int error;	/* error code */
PFfpage *fpage;	/* pointer to file page */

	if (PFinvalidFd(fd)){
		PFerrno = PFE_FD;
		return(PFerrno);
	}


	if (*pagenum < -1 || *pagenum >= PFftab[fd].hdr.numpages){
		PFerrno = PFE_INVALIDPAGE;
		return(PFerrno);
	}

	/* scan the file until a valid used page is found */
	for (temppage= *pagenum+1;temppage<PFftab[fd].hdr.numpages;temppage++){
		if ( (error=PFbufGet(fd,temppage,&fpage,PFreadfcn,
					PFwritefcn))!= PFE_OK)
			return(error);
		else if (fpage->nextfree == PF_PAGE_USED){
			/* found a used page */
			*pagenum = temppage;
			*pagebuf = (char *)fpage->pagebuf;
			return(PFE_OK);
		}

		/* page is free, unfix it */
		if ((error=PFbufUnfix(fd,temppage,FALSE))!= PFE_OK)
			return(error);
	}

	/* No valid used page found */
	PFerrno = PFE_EOF;
	return(PFerrno);

}

PF_GetThisPage(fd,pagenum,pagebuf)
int fd;		/* file descriptor */
int pagenum;	/* page number to read */
char **pagebuf;	/* pointer to pointer to page data */
/****************************************************************************
SPECIFICATIONS:
	Read the page specifeid by "pagenum" and set *pagebuf to point
	to the page data. The page number should be valid.

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if no error.
	PFE_INVALIDPAGE if invalid page number is specified.
	PFE_PAGEFIXED if page already fixed in memory. In this case,
		*pagebuf  is still set to point to the buffer that contains
		the page data.
	other PF error codes if other error encountered.
*****************************************************************************/
{
int error;
PFfpage *fpage;

	if (PFinvalidFd(fd)){
		PFerrno = PFE_FD;
		return(PFerrno);
	}

	if (PFinvalidPagenum(fd,pagenum)){
		PFerrno = PFE_INVALIDPAGE;
		return(PFerrno);
	}

	if ( (error=PFbufGet(fd,pagenum,&fpage,PFreadfcn,PFwritefcn))!= PFE_OK){
		if (error== PFE_PAGEFIXED)
			*pagebuf = fpage->pagebuf;
		return(error);
	}

	if (fpage->nextfree == PF_PAGE_USED){
		/* page is used*/
		*pagebuf = (char *)fpage->pagebuf;
		return(PFE_OK);
	}
	else {
		/* invalid page */
		if (PFbufUnfix(fd,pagenum,FALSE)!= PFE_OK){
			printf("internal error:PFgetThis()\n");
			exit(1);
		}
		PFerrno = PFE_INVALIDPAGE;
		return(PFerrno);
	}
}

PF_AllocPage(fd,pagenum,pagebuf)
int fd;		/* file descriptor */
int *pagenum;	/* page number */
char **pagebuf;	/* pointer to pointer to page buffer*/
/****************************************************************************
SPECIFICATIONS:
	Allocate a new, empty page for file "fd".
	set *pagenum to the new page number. 
	Set *pagebuf to point to the buffer for that page.
	The page allocated is fixed in the buffer.

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if ok
	PF error codes if not ok.

*****************************************************************************/
{
PFfpage *fpage;	/* pointer to file page */
int error;

	if (PFinvalidFd(fd)){
		PFerrno= PFE_FD;
		return(PFerrno);
	}

	if (PFftab[fd].hdr.firstfree != PF_PAGE_LIST_END){
		/* get a page from the free list */
		*pagenum = PFftab[fd].hdr.firstfree;
		if ((error=PFbufGet(fd,*pagenum,&fpage,PFreadfcn,
					PFwritefcn))!= PFE_OK)
			/* can't get the page */
			return(error);
		PFftab[fd].hdr.firstfree = fpage->nextfree;
		PFftab[fd].hdrchanged = TRUE;
	}
	else {
		/* Free list empty, allocate one more page from the file */
		*pagenum = PFftab[fd].hdr.numpages;
		if ((error=PFbufAlloc(fd,*pagenum,&fpage,PFwritefcn))!= PFE_OK)
			/* can't allocate a page */
			return(error);
	
		/* increment # of pages for this file */
		PFftab[fd].hdr.numpages++;
		PFftab[fd].hdrchanged = TRUE;

		/* mark this page dirty */
		if ((error=PFbufUsed(fd,*pagenum))!= PFE_OK){
			printf("internal error: PFalloc()\n");
			exit(1);
		}

	}

	/* zero out the page. Seems to be a nice thing to do,
	at least for debugging. */
	/*
	bzero(fpage->pagebuf,PF_PAGE_SIZE);
	*/

	/* Mark the new page used */
	fpage->nextfree = PF_PAGE_USED;

	/* set return value */
	*pagebuf = fpage->pagebuf;
	
	return(PFE_OK);
}

PF_DisposePage(fd,pagenum)
int fd;		/* file descriptor */
int pagenum;	/* page number */
/****************************************************************************
SPECIFICATIONS:
	Dispose the page numbered "pagenum" of the file "fd".
	Only a page that is not fixed in the buffer can be disposed.

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if no error.
	PF error code if error.

*****************************************************************************/
{
PFfpage *fpage;	/* pointer to file page */
int error;

	if (PFinvalidFd(fd)){
		PFerrno = PFE_FD;
		return(PFerrno);
	}

	if (PFinvalidPagenum(fd,pagenum)){
		PFerrno = PFE_INVALIDPAGE;
		return(PFerrno);
	}

	if ((error=PFbufGet(fd,pagenum,&fpage,PFreadfcn,PFwritefcn))!= PFE_OK)
		/* can't get this page */
		return(error);
	
	if (fpage->nextfree != PF_PAGE_USED){
		/* this page already freed */
		if (PFbufUnfix(fd,pagenum,FALSE)!= PFE_OK){
			printf("internal error: PFdispose()\n");
			exit(1);
		}
		PFerrno = PFE_PAGEFREE;
		return(PFerrno);
	}

	/* put this page into the free list */
	fpage->nextfree = PFftab[fd].hdr.firstfree;
	PFftab[fd].hdr.firstfree = pagenum;
	PFftab[fd].hdrchanged = TRUE;

	/* unfix this page */
	return(PFbufUnfix(fd,pagenum,TRUE));
}

PF_UnfixPage(fd,pagenum,dirty)
int fd;	/* file descriptor */
int pagenum;	/* page number */
int dirty;	/* true if file is dirty */
/****************************************************************************
SPECIFICATIONS:
	Tell the Paged File Interface that the page numbered "pagenum"
	of the file "fd" is no longer needed in the buffer.
	Set the variable "dirty" to TRUE if page has been modified.

AUTHOR: clc

RETURN VALUE:
	PFE_OK	if no error
	PF error code if error.

*****************************************************************************/
{

	if (PFinvalidFd(fd)){
		PFerrno = PFE_FD;
		return(PFerrno);
	}

	if (PFinvalidPagenum(fd,pagenum)){
		PFerrno = PFE_INVALIDPAGE;
		return(PFerrno);
	}

	return(PFbufUnfix(fd,pagenum,dirty));
}

/* CHANGED [P5]: new public call, sets the dirty bit only (declared in pf.h) */
PF_MarkDirty(fd,pagenum)
int fd;		/* file descriptor */
int pagenum;	/* page number */
/****************************************************************************
SPECIFICATIONS:
	Tell the Paged File Interface that page "pagenum" of file "fd"
	has been modified, so it is written back to the file when it
	leaves the buffer (eviction or PF_CloseFile).
	The page must be in the buffer pool. It can be fixed or unfixed.
	Only the dirty bit is set. Nothing else changes:
	- the page keeps its place in the used list, so LRU/MRU see no
	  access (PFbufUsed() would move it to the head, which is why it
	  is not used here),
	- the fix state is unchanged, so the caller still has to call
	  PF_UnfixPage() for a page it fixed,
	- no page is read or written.
	Calling it again on a page that is already dirty does nothing more.
	PF_UnfixPage(fd,pagenum,FALSE) after this does not clear the bit.
	A caller that changes the page data must hold the page fixed
	while doing it (and until after this call), otherwise the page
	may be evicted before it is marked and the change is lost.

RETURN VALUE:
	PFE_OK		if no error.
	PFE_FD		if fd is not an open file.
	PFE_INVALIDPAGE	if pagenum is not in 0..numpages-1.
	PFE_PAGENOTINBUF	if the page is not in the buffer pool.

*****************************************************************************/
{

	if (PFinvalidFd(fd)){
		PFerrno = PFE_FD;
		return(PFerrno);
	}

	if (PFinvalidPagenum(fd,pagenum)){
		PFerrno = PFE_INVALIDPAGE;
		return(PFerrno);
	}

	return(PFbufMarkDirty(fd,pagenum));
}

/* error messages */
static char *PFerrormsg[]={
"No error",
"No memory",
"No buffer space",
"Page already fixed in buffer",
"page to be unfixed is not in the buffer",
"unix error",
"incomplete read of page from file",
"incomplete write of page to file",
"incomplete read of header from file",
"incomplete write of header from file",
"invalid page number",
"file already open",
"file table full",
"invalid file descriptor",
"end of file",
"page already free",
"page already unfixed",
"new page to be allocated already in buffer",
"hash table entry not found",
"page already in hash table",
/* CHANGED [P1]: messages for PFE_INVALIDBUFS and PFE_BUFINUSE */
"buffer pool size below the minimum",
"buffer pool in use, can't resize",
/* CHANGED [P3]: message for PFE_INVALIDPOLICY */
"invalid replacement policy"
};

void PF_PrintError(s)
char *s;	/* string to write */
/****************************************************************************
SPECIFICATIONS:
	Write the string "s" onto stderr, then write the last
	error message from PF onto stderr.

AUTHOR: clc

RETURN VALUE: none

*****************************************************************************/
{

	fprintf(stderr,"%s",s);
	fprintf(stderr,":%s",PFerrormsg[-1*PFerrno]);
	if (PFerrno == PFE_UNIX)
		/* print the unix error message */
		perror(" ");
	else	fprintf(stderr,"\n");

}
