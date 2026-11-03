/* hf.h: frozen heap-file and static-baseline interfaces */
#ifndef HF_H
#define HF_H

#define HF_OK        0
#define HFE_EOF     (-1)   /* end of scan, not a failure */
#define HFE_INVALID (-2)   /* invalid argument or file descriptor */
#define HFE_NOTFOUND (-3)  /* rid does not name a live record */
#define HFE_TOOLONG (-4)   /* record longer than HF_MAX_RECLEN or too big for a page */
#define HFE_NOMEM   (-5)
#define HFE_PF      (-6)   /* underlying PF layer error; PFerrno holds the detail */

/*
 * HF_RID_STRIDE is only the multiplier of the RID encoding and has nothing
 * to do with PF_PAGE_SIZE.  A slot number must stay below it, and RIDs must
 * stay non-negative because the B+ tree stores them as ints.
 */
#define HF_RID_STRIDE 4096

#define HF_RID(page, slot) ((page) * HF_RID_STRIDE + (slot))
#define HF_RID_PAGE(rid) ((rid) / HF_RID_STRIDE)
#define HF_RID_SLOT(rid) ((rid) % HF_RID_STRIDE)

/*
 * Longest record the heap file accepts.  Caller buffers for HF_Get and
 * HF_ScanNext must be at least this big; longer records are rejected with
 * HFE_TOOLONG.
 */
#define HF_MAX_RECLEN 1024

/*
 * Space statistics, with the same definitions for both layouts:
 * pages            = data pages in the file, excluding any header page
 * records          = live records
 * useful_bytes     = sum of the stored lengths of live records (after
 *                    truncation for the static layout), without
 *                    slot-directory or length-word overhead
 * truncated        = live records that were cut to the fixed length at
 *                    insert (static layout only, 0 otherwise); deleting
 *                    such a record lowers it
 * records_per_page = records / pages
 * utilization      = useful_bytes / (pages * PF_PAGE_SIZE)
 */
typedef struct {
	long pages;
	long records;
	long useful_bytes;
	long truncated;
	long records_per_page;
	double utilization;
} HFSpaceStats;

/*
 * Create an empty slotted heap file named fname.
 * Return HF_OK on success or a negative heap-file error.
 */
int HF_CreateFile(char *fname);

/*
 * Open the slotted heap file named fname.
 * Return a non-negative file descriptor or a negative heap-file error.
 */
int HF_OpenFile(char *fname);

/*
 * Close fd and flush its heap-file state.
 * Return HF_OK on success or a negative heap-file error.
 */
int HF_CloseFile(int fd);

/*
 * Insert len bytes from rec into fd and return its non-negative RID.
 * Return a negative heap-file error for invalid input or failure.
 */
int HF_Insert(int fd, char *rec, int len);

/*
 * Delete the record identified by rid in fd.
 * Return HF_OK on success or a negative heap-file error.
 */
int HF_Delete(int fd, int rid);

/*
 * Copy the record identified by rid into buf and store its length in *len.
 * The caller supplies at least HF_MAX_RECLEN bytes in buf; return HF_OK on
 * success or a negative heap-file error without modifying the record on
 * failure.
 */
int HF_Get(int fd, int rid, char *buf, int *len);

/*
 * Open a sequential scan of fd in page and slot order.
 * Return a non-negative scan descriptor or a negative heap-file error.
 */
int HF_ScanOpen(int fd);

/*
 * Return the next live record in scanfd, copying it to buf (at least
 * HF_MAX_RECLEN bytes), storing its length in *len, and storing its RID in
 * *rid.  Return HF_OK for a record, HFE_EOF at end of scan, or another
 * negative error.
 */
int HF_ScanNext(int scanfd, char *buf, int *len, int *rid);

/*
 * Close scanfd and release its scan resources.
 * Return HF_OK on success or a negative heap-file error.
 */
int HF_ScanClose(int scanfd);

/*
 * Fill out with slotted-file page, record, useful-byte, and utilization
 * statistics for fd.  Return HF_OK on success or a negative heap-file error.
 */
int HF_GetSpaceStats(int fd, HFSpaceStats *out);

/*
 * Create an empty fixed-length static-baseline file named fname using
 * record_length bytes per slot.  Return HF_OK or a negative error.
 */
int HF_StaticCreateFile(char *fname, int record_length);

/*
 * Open the fixed-length static-baseline file named fname.
 * Return a non-negative descriptor or a negative error.
 */
int HF_StaticOpenFile(char *fname);

/*
 * Close fd and flush its static-baseline state.
 * Return HF_OK on success or a negative error.
 */
int HF_StaticCloseFile(int fd);

/*
 * Insert rec into the fixed-length file.  A record longer than the slot
 * length (but at most HF_MAX_RECLEN) is truncated to it and flagged as
 * truncated; a longer one is rejected with HFE_TOOLONG.  Return HF_OK or
 * a negative error.
 */
int HF_StaticInsert(int fd, char *rec, int len);

/*
 * Fill out with static-baseline page, record, truncation, and utilization
 * statistics for fd.  Return HF_OK on success or a negative error.
 */
int HF_StaticGetSpaceStats(int fd, HFSpaceStats *out);

/*
 * Static baseline record access and scans use the same RID encoding.
 * HF_StaticGet and HF_StaticScanNext store the record's stored length
 * (at most the slot length) in *len.  Closing a static file makes its
 * open scans return HFE_INVALID; HF_StaticScanClose still releases them.
 */
int HF_StaticGet(int fd, int rid, char *buf, int *len);
int HF_StaticDelete(int fd, int rid);
int HF_StaticScanOpen(int fd);
int HF_StaticScanNext(int scanfd, char *buf, int *len, int *rid);
int HF_StaticScanClose(int scanfd);

#endif /* HF_H */
