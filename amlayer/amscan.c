
# include <stdio.h>
# include "am.h"
# include "pf.h"

/* The structure of the scan Table */
struct {
         int fileDesc;
         int op;
         int attrType;
         int pageNum;
         short index;
         short actindex;
         int nextpageNum;
         char nextvalue[AM_MAXATTRLENGTH];
         short nextIndex;
         short nextRecIdPtr;
         int lastpageNum;
         short lastIndex;
         int status;
       } AM_scanTable[MAXSCANS];


/* Opens an index scan */
AM_OpenIndexScan(fileDesc,attrType,attrLength,op,value)
int fileDesc; /* file Descriptor */

char attrType; /* 'i' or 'c' or 'f' */
int attrLength; /* 4 for 'i' or 'f' , 1-255 for 'c' */
int op; /* operator for comparison */
char *value; /* value for comparison */

{
int scanDesc; /* index into scan table */
int status; /* whether value is found or not in the tree */
int index; /* index of value in leaf */
int pageNum;/* page number of leaf page where value is found */
int recSize; /* size of key,ptr pair in leaf */
char *pageBuf; /* buffer for page */
int errVal; /* return value of functions */
AM_LEAFHEADER head,*header; /* local header */
int searchpageNum;
char *searchBuf; /* CHANGED [A5]: buffer of the leaf AM_Search found */



/* check the parameters */
if (fileDesc < 0)
  {
   AM_Errno = AME_FD;
   return(AME_FD);
  }

if ((attrType != 'i') && (attrType != 'c') && (attrType != 'f'))
  {
  AM_Errno = AME_INVALIDATTRTYPE;
  return(AME_INVALIDATTRTYPE);
  }

/* initialise header */
header = &head;

/* find a vacant place in the scan table */
for (scanDesc = 0; scanDesc <  MAXSCANS;scanDesc++)
  if (AM_scanTable[scanDesc].status == FREE) break;

/* scan table is full */
if (scanDesc > MAXSCANS - 1) 
 {
 AM_Errno = AME_SCAN_TAB_FULL;
 return(AME_SCAN_TAB_FULL);
 }

/* there is room */
AM_scanTable[scanDesc].status = FIRST;
AM_scanTable[scanDesc].attrType = attrType;

/* initialise AM_LeftPageNum */
AM_LeftPageNum = GetLeftPageNum(fileDesc);

/* scan of all keys */
if (value == NULL)
  {
   AM_scanTable[scanDesc].fileDesc = fileDesc;
   AM_scanTable[scanDesc].op = ALL;
   AM_scanTable[scanDesc].nextpageNum = AM_LeftPageNum;
   AM_scanTable[scanDesc].nextIndex = 1;
   AM_scanTable[scanDesc].actindex = 1;
   errVal = PF_GetThisPage(fileDesc,AM_LeftPageNum,&pageBuf);
   AM_Check;
   bcopy(pageBuf + AM_sl + attrLength,&AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
   errVal = PF_UnfixPage(fileDesc,AM_LeftPageNum,FALSE);
   AM_Check;
   return(scanDesc);
  }
  
/* search for the pagenumber and index of value */
status = AM_Search(fileDesc,attrType,attrLength,value,&pageNum,&pageBuf,&index);
searchpageNum = pageNum;
searchBuf = pageBuf; /* CHANGED [A5]: see the LT, LE and NE cases */
/* check for errors */
if (status < 0) 
  { AM_scanTable[scanDesc].status = FREE;
    AM_Errno = status;
    return(status);
  }

bcopy(pageBuf,header,AM_sl);
recSize = attrLength + AM_ss;
AM_scanTable[scanDesc].fileDesc = fileDesc;
AM_scanTable[scanDesc].op = op;

/* value is not in leaf but if inserted will have to be inserted after the last
key */
if (index > header->numKeys) 
  if (header->nextLeafPage != AM_NULL_PAGE)
  {
  /* CHANGED [A5]: pageNum is set to the next leaf before the bcopy, which
  overwrites header with that leaf's header. The unfix used
  header->nextLeafPage after the bcopy, i.e. the leaf after the next one,
  which is not fixed: it failed with AME_PF and left the search leaf and
  the next leaf fixed. Unfix pageNum, the page fixed here. */
  pageNum = header->nextLeafPage;
  errVal = PF_GetThisPage(fileDesc,pageNum,&pageBuf);
  AM_Check;
  bcopy(pageBuf,header,AM_sl);
  errVal = PF_UnfixPage(fileDesc,pageNum,FALSE);
  AM_Check;
  index = 1;
  }
  else 
   pageNum = AM_NULL_PAGE;

AM_scanTable[scanDesc].pageNum = pageNum;
AM_scanTable[scanDesc].index = index;



/* case on op */
switch(op) 
 {
  case EQUAL : 
               {
                /* value not in leaf - no match */
		if (status != AM_FOUND)
                   AM_scanTable[scanDesc].status = OVER;
                else
                 {
                  AM_scanTable[scanDesc].nextpageNum = pageNum;
                  AM_scanTable[scanDesc].nextIndex = index;
                  AM_scanTable[scanDesc].actindex = index;
                  bcopy(pageBuf + AM_sl + (index - 1)*recSize + attrLength,
                          &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
                  AM_scanTable[scanDesc].lastpageNum  = pageNum;
                  AM_scanTable[scanDesc].lastIndex  = index;
                 }
                break;
               }
  case LESS_THAN : 
               {
                AM_scanTable[scanDesc].nextpageNum = AM_LeftPageNum;
                AM_scanTable[scanDesc].nextIndex = 1;
                AM_scanTable[scanDesc].actindex = 1;
                if (searchpageNum != AM_LeftPageNum)
		 { errVal = PF_GetThisPage(fileDesc,AM_LeftPageNum,&pageBuf);
                  AM_Check;
                 }
                /* CHANGED [A5]: after a move to the next leaf above, pageBuf
                is that leaf, not the leftmost leaf the search found */
                else pageBuf = searchBuf;
                bcopy(pageBuf + AM_sl + attrLength,
                        &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
                if (searchpageNum != AM_LeftPageNum)
		 {
		  errVal = PF_UnfixPage(fileDesc,AM_LeftPageNum,FALSE);
                  AM_Check;
                 }
                AM_scanTable[scanDesc].lastpageNum  = pageNum;
                AM_scanTable[scanDesc].lastIndex  = index - 1 ;
                break;
               }
  case GREATER_THAN :
               {
                if (status == AM_FOUND)
                 if ((index + 1) <= (header->numKeys))
                  {
                   AM_scanTable[scanDesc].nextpageNum = pageNum;
                   AM_scanTable[scanDesc].nextIndex = index + 1;
                   AM_scanTable[scanDesc].actindex = index + 1;
                   bcopy(pageBuf + AM_sl + (index)*recSize + attrLength,
                             &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
                  }
                 else
                   /* got to start from next leaf page */
		   if (header->nextLeafPage != AM_NULL_PAGE)
                   {
                   AM_scanTable[scanDesc].nextpageNum = header->nextLeafPage;
                   AM_scanTable[scanDesc].nextIndex =  1;
                   AM_scanTable[scanDesc].actindex = 1;
                   errVal =PF_GetThisPage(fileDesc,header->nextLeafPage,&pageBuf);
                   AM_Check;
                   bcopy(pageBuf + AM_sl + attrLength,
                           &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
                   errVal = PF_UnfixPage(fileDesc,header->nextLeafPage,FALSE);
                   AM_Check;
                   }
                   else /* Nextleafpage is not last NULL page */
                     AM_scanTable[scanDesc].status = OVER;
                else /* status == AM_NOT_FOUND */ 
                 {
                   AM_scanTable[scanDesc].nextpageNum = pageNum;
                   AM_scanTable[scanDesc].nextIndex = index ;
                   AM_scanTable[scanDesc].actindex = index;
                   bcopy(pageBuf + AM_sl + (index - 1)*recSize + attrLength,
                              &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
                 }
                break;
               }
  case LESS_THAN_EQUAL :
               {
               AM_scanTable[scanDesc].nextpageNum = AM_LeftPageNum;
               AM_scanTable[scanDesc].nextIndex = 1;
               AM_scanTable[scanDesc].actindex = 1;
                if (searchpageNum != AM_LeftPageNum)
		 { errVal = PF_GetThisPage(fileDesc,AM_LeftPageNum,&pageBuf);
                  AM_Check;
                 }
                /* CHANGED [A5]: after a move to the next leaf above, pageBuf
                is that leaf, not the leftmost leaf the search found */
                else pageBuf = searchBuf;
               bcopy(pageBuf + AM_sl + attrLength,
                                 &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
               if (searchpageNum != AM_LeftPageNum)
		 {
		  errVal = PF_UnfixPage(fileDesc,AM_LeftPageNum,FALSE);
                  AM_Check;
                 }		   
               AM_scanTable[scanDesc].lastpageNum  = pageNum;
               if (status == AM_FOUND)
                AM_scanTable[scanDesc].lastIndex  = index ;
               else
                AM_scanTable[scanDesc].lastIndex  = index - 1;
               break;
               }
  case GREATER_THAN_EQUAL :
                
               {
                AM_scanTable[scanDesc].nextpageNum = pageNum;
                AM_scanTable[scanDesc].nextIndex = index;
                AM_scanTable[scanDesc].actindex = index;
                bcopy(pageBuf + AM_sl + (index - 1)*recSize + attrLength,
                        &AM_scanTable[scanDesc].nextRecIdPtr,   AM_ss);
                break;
               }
  case NOT_EQUAL :
               {
               /* CHANGED [A4]: this set-up used to run only if the value
               was found. For an absent value nextpageNum, nextIndex,
               actindex and nextRecIdPtr kept whatever the scan table
               slot held (zeros, or an old scan's cursor), so the scan
               read page 0 or a stale page as a leaf: garbage record ids
               or a crash. Every NE scan starts at the leftmost leaf; an
               absent value just has nothing to skip (pageNum below). */
                {
                AM_scanTable[scanDesc].nextpageNum = AM_LeftPageNum;
                AM_scanTable[scanDesc].nextIndex = 1;
                AM_scanTable[scanDesc].actindex = 1;
                if (searchpageNum != AM_LeftPageNum)
		  {
		  errVal = PF_GetThisPage(fileDesc,AM_LeftPageNum,&pageBuf);
                  AM_Check;
		  }
                /* CHANGED [A5]: as in the LT case */
                else pageBuf = searchBuf;
                bcopy(pageBuf + AM_sl + attrLength,
                              &AM_scanTable[scanDesc].nextRecIdPtr,   AM_ss);
                if (searchpageNum != AM_LeftPageNum)
		 { errVal = PF_UnfixPage(fileDesc,AM_LeftPageNum,FALSE);
                  AM_Check;
                 }
                }
               if (status != AM_FOUND) /* CHANGED [A4]: was "else" */
                AM_scanTable[scanDesc].pageNum = AM_NULL_PAGE;
               break;
               }
  default : {
             AM_scanTable[scanDesc].status = FREE;
	     AM_Errno = AME_INVALID_OP_TO_SCAN;
	     return(AME_INVALID_OP_TO_SCAN);
             break;
            }
 }

errVal = PF_UnfixPage(fileDesc,searchpageNum,FALSE);
AM_Check;
return(scanDesc);
}

/* returns the record id of the next record that satisfies the conditions
specified for index scan associated with scanDesc */
AM_FindNextEntry(scanDesc)
int scanDesc;/* index scan descriptor */

{
int recId; /* recordId to be returned */
char *pageBuf;/* buffer for page */
int errVal;/* return value for functions */
AM_LEAFHEADER head,*header; /* local header */
int recSize;/* size of key,ptr pair for leaf */
int compareVal; /* value returned by compare routine */


/* check if scanDesc is valid */
if ((scanDesc < 0) || (scanDesc > MAXSCANS - 1))
  {
   AM_Errno = AME_INVALID_SCANDESC;
   return(AME_INVALID_SCANDESC);
  }

/* check if scan is over */
if (AM_scanTable[scanDesc].status == OVER)
      return(AME_EOF);

if (AM_scanTable[scanDesc].nextpageNum == AM_NULL_PAGE)
 {
  AM_scanTable[scanDesc].status = OVER;
  return(AME_EOF);
 }

header = &head;
errVal = PF_GetThisPage(AM_scanTable[scanDesc].fileDesc
                        ,AM_scanTable[scanDesc].nextpageNum,&pageBuf);
AM_Check;

bcopy(pageBuf,header,AM_sl);
recSize = header->attrLength + AM_ss;

errVal = PF_UnfixPage(AM_scanTable[scanDesc].fileDesc
           ,AM_scanTable[scanDesc].nextpageNum,FALSE);
AM_Check;

/* Get next non empty leaf page */
while(header->numKeys == 0)
  if(header->nextLeafPage == AM_NULL_PAGE)
   {
    AM_scanTable[scanDesc].status = OVER; 
    return(AME_EOF);
   }
  else
   {
    errVal = PF_GetThisPage(AM_scanTable[scanDesc].fileDesc,header->nextLeafPage,&pageBuf);
    AM_Check;
    errVal = PF_UnfixPage(AM_scanTable[scanDesc].fileDesc,header->nextLeafPage,FALSE);
    AM_Check;
    AM_scanTable[scanDesc].nextpageNum = header->nextLeafPage;
    AM_scanTable[scanDesc].nextIndex = 1;
    AM_scanTable[scanDesc].actindex = 1;
    bcopy(pageBuf,header,AM_sl);
    bcopy(pageBuf + AM_sl +  (AM_scanTable[scanDesc].nextIndex-1)*recSize
    + header->attrLength, &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
    AM_scanTable[scanDesc].status = FIRST;
   }

/* if op is < or <= check if you are done - might have overshot while scanning
empty pages*/
if ((AM_scanTable[scanDesc].op == LESS_THAN) || 
  (AM_scanTable[scanDesc].op == LESS_THAN_EQUAL))
 /* CHANGED [A7]: was lastpageNum <= nextpageNum. Page numbers are not
 in leaf order (in a tree built by inserts the leftmost leaf is page 2 and
 the next one is often page 1), so an LT or LE scan whose stop position is
 index 0 of a later leaf (value = first key of that leaf, or a value in
 the gap before it) stopped at the first leaf whose page number was at
 least lastpageNum, often the leftmost leaf, with no records. Index 0 of
 lastpageNum means the scan is over only once it is on that very leaf. */
 if ((AM_scanTable[scanDesc].lastpageNum == AM_scanTable[scanDesc].nextpageNum)
 && (AM_scanTable[scanDesc].lastIndex == 0))
 {
  AM_scanTable[scanDesc].status = OVER;
  return(AME_EOF);
 }

/* if op is not equal then check if we have to skip this value */
if (AM_scanTable[scanDesc].op == NOT_EQUAL)
  if ((AM_scanTable[scanDesc].pageNum == AM_scanTable[scanDesc].nextpageNum)
    && (AM_scanTable[scanDesc].index == AM_scanTable[scanDesc].actindex))
       
       /*skip this value */
       /* CHANGED [A3]: the skip moved the cursor to the next key but left
       nextvalue holding the skipped key. The deleted-record check below
       then saw a different key at nextIndex, took it for a deleted
       record, moved back with nextIndex-- and returned the skipped key
       after all (with every one of its record ids). The skip now updates
       nextvalue like the normal advance further down does, so the check
       sees an unchanged key. */
       if ((AM_scanTable[scanDesc].nextIndex + 1) <= (header->numKeys))
         {
          AM_scanTable[scanDesc].nextIndex++;
          AM_scanTable[scanDesc].actindex++;
          bcopy(pageBuf + AM_sl +  (AM_scanTable[scanDesc].nextIndex-1)*recSize
          + header->attrLength, &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
          /* CHANGED [A3]: keep nextvalue in step with nextIndex */
          bcopy(pageBuf + AM_sl + (AM_scanTable[scanDesc].nextIndex-1)*recSize,
          AM_scanTable[scanDesc].nextvalue,header->attrLength);
         }
       else
          if (header->nextLeafPage == AM_NULL_PAGE)
            return(AME_EOF); 
          else
           {
            AM_scanTable[scanDesc].nextpageNum = header->nextLeafPage;
            AM_scanTable[scanDesc].nextIndex =  1;
            AM_scanTable[scanDesc].actindex = 1;
            errVal =PF_GetThisPage(AM_scanTable[scanDesc].fileDesc,
                  header->nextLeafPage,&pageBuf);
            AM_Check;
            bcopy(pageBuf + AM_sl + header->attrLength,
               &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
            bcopy(pageBuf,header,AM_sl);
            /* CHANGED [A3]: header is already the new leaf's header here,
            so header->nextLeafPage named the leaf after it, which is not
            fixed: the unfix failed (AME_PF) and the new leaf stayed fixed.
            Unfix the page fixed above, nextpageNum. */
            errVal = PF_UnfixPage(AM_scanTable[scanDesc].fileDesc
                ,AM_scanTable[scanDesc].nextpageNum,FALSE);
            AM_Check;
            /* CHANGED [A3]: keep nextvalue in step, as in the same-leaf
            case above */
            bcopy(pageBuf + AM_sl,AM_scanTable[scanDesc].nextvalue,
               header->attrLength);
           }
/* if not the first call to findnextentry , check if previous record has 
been deleted */
if (AM_scanTable[scanDesc].status != FIRST)
 {
  compareVal = AM_Compare(pageBuf + (AM_scanTable[scanDesc].nextIndex - 1)
                *recSize + AM_sl,AM_scanTable[scanDesc].attrType,
		header->attrLength,AM_scanTable[scanDesc].nextvalue);
  if (compareVal != 0)
   {
    /* prev record deleted */
    AM_scanTable[scanDesc].nextIndex--;
    bcopy(pageBuf + AM_sl + (AM_scanTable[scanDesc].nextIndex -1)*recSize + 
    header->attrLength, &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
   }
 }
else 
  /* make the status busy - no more the first call */
  { AM_scanTable[scanDesc].status = BUSY;
    bcopy(pageBuf + AM_sl + (AM_scanTable[scanDesc].nextIndex - 1)*recSize,
    AM_scanTable[scanDesc].nextvalue, header->attrLength);
  }

/* copy the recId to be returned */
bcopy(pageBuf + AM_scanTable[scanDesc].nextRecIdPtr,&recId,AM_si);

/* copy the place for next recId */
bcopy(pageBuf + AM_scanTable[scanDesc].nextRecIdPtr + AM_si,
          &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);


/* check if this keys list is over */
if (AM_scanTable[scanDesc].nextRecIdPtr == (short)0)
   if ((AM_scanTable[scanDesc].nextIndex + 1) <= (header->numKeys))
    {
     AM_scanTable[scanDesc].nextIndex++;
     AM_scanTable[scanDesc].actindex++;
     bcopy(pageBuf + AM_sl + (AM_scanTable[scanDesc].nextIndex - 1)*recSize + 
     header->attrLength , &AM_scanTable[scanDesc].nextRecIdPtr, AM_ss);
     bcopy(pageBuf + AM_sl + (AM_scanTable[scanDesc].nextIndex -1 )*recSize,
     AM_scanTable[scanDesc].nextvalue,header->attrLength); 
    }
   else
    /* got to go to next page */
    if (header->nextLeafPage == AM_NULL_PAGE)
      AM_scanTable[scanDesc].status = OVER;
    else
     {
      AM_scanTable[scanDesc].nextpageNum = header->nextLeafPage;
      AM_scanTable[scanDesc].nextIndex =  1;
      AM_scanTable[scanDesc].actindex = 1;
      errVal =PF_GetThisPage(AM_scanTable[scanDesc].fileDesc,
        header->nextLeafPage,&pageBuf);
      AM_Check;
      bcopy(pageBuf + AM_sl + header->attrLength,
         &AM_scanTable[scanDesc].nextRecIdPtr,AM_ss);
      errVal = PF_UnfixPage(AM_scanTable[scanDesc].fileDesc
                ,header->nextLeafPage,FALSE);
      AM_Check;
      bcopy(pageBuf + AM_sl + (AM_scanTable[scanDesc].nextIndex -1 )*recSize,
      AM_scanTable[scanDesc].nextvalue,header->attrLength); 
      bcopy(pageBuf,header,AM_sl);
     }

/* If op is equal then see if you are done */
if (AM_scanTable[scanDesc].op == EQUAL)
  if ((AM_scanTable[scanDesc].pageNum != AM_scanTable[scanDesc].nextpageNum)
    || (AM_scanTable[scanDesc].index != AM_scanTable[scanDesc].actindex))
     AM_scanTable[scanDesc].status = OVER;

/* see if you are at the last record if op is < or <= */
/* CHANGED [A6]: not once the scan is OVER. At the end of the last key's
record ids in the last leaf the code above sets OVER, but for an LE scan on
the largest key lastpageNum/lastIndex still match the cursor and the test
below turned OVER back into LAST, so the next call went on past the end of
the leaf and returned header bytes as record ids. */
if (((AM_scanTable[scanDesc].op == LESS_THAN) || 
  (AM_scanTable[scanDesc].op == LESS_THAN_EQUAL))
  && (AM_scanTable[scanDesc].status != OVER))
   if ((AM_scanTable[scanDesc].lastpageNum == AM_scanTable[scanDesc].nextpageNum)
    && (AM_scanTable[scanDesc].lastIndex == AM_scanTable[scanDesc].actindex))
       AM_scanTable[scanDesc].status = LAST;
  else  if ((AM_scanTable[scanDesc].lastpageNum == AM_scanTable[scanDesc].nextpageNum)
    && (AM_scanTable[scanDesc].lastIndex  <  AM_scanTable[scanDesc].actindex))
        AM_scanTable[scanDesc].status = OVER;
    else
      if (AM_scanTable[scanDesc].status == LAST)
        AM_scanTable[scanDesc].status = OVER;
        

return(recId);
}


/* terminates an index scan */
AM_CloseIndexScan(scanDesc)
int scanDesc;/* scan Descriptor*/

{
if ((scanDesc < 0) || (scanDesc > MAXSCANS - 1))
  {
   AM_Errno = AME_INVALID_SCANDESC;
   return(AME_INVALID_SCANDESC);
  }
AM_scanTable[scanDesc].status = FREE;
return(AME_OK);
}


GetLeftPageNum(fileDesc)
int fileDesc;

{
char *pageBuf;
int pageNum;
int errVal;
int child; /* CHANGED [A1]: leftmost child of an internal node */

errVal = PF_GetFirstPage(fileDesc,&pageNum,&pageBuf);
AM_Check;
/* CHANGED [A1]: this used to return page 2 whenever the root was not a
leaf. That only holds for trees built by inserts (the first root split
puts the copy of the left half in page 2). In a tree with any other page
layout, e.g. a bulk-loaded one with its leaves in pages 1..k, full, LT,
LE and NE scans started at the wrong page: the first leaf was skipped,
or an internal node was read as a leaf. Now we follow the leftmost child
pointer p0 (the int right after the internal header, as AM_FillRootPage
writes it) from the root down until we reach a leaf. Each page is
unfixed before its child is fixed, so only the leaf is fixed after the
loop, and the unfix below releases it. For an insert-built tree this
still ends at page 2. */
while (*pageBuf != 'l')
  {
  bcopy(pageBuf + AM_sint,&child,AM_si);
  errVal = PF_UnfixPage(fileDesc,pageNum,FALSE);
  AM_Check;
  pageNum = child;
  errVal = PF_GetThisPage(fileDesc,pageNum,&pageBuf);
  AM_Check;
  }
AM_LeftPageNum = pageNum;
errVal = PF_UnfixPage(fileDesc,pageNum,FALSE);
AM_Check;
return(AM_LeftPageNum);

}
