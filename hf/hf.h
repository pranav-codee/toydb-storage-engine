/* hf.h: frozen heap-file and static-baseline interfaces */
#ifndef HF_H
#define HF_H

#define HF_PAGE_SIZE 4096

#define HF_RID(page, slot) ((page) * HF_PAGE_SIZE + (slot))
#define HF_RID_PAGE(rid) ((rid) / HF_PAGE_SIZE)
#define HF_RID_SLOT(rid) ((rid) % HF_PAGE_SIZE)

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
 * Return zero on success or a negative heap-file error.
 */
int HF_CreateFile(char *fname);

/*
 * Open the slotted heap file named fname.
 * Return a non-negative file descriptor or a negative heap-file error.
 */
int HF_OpenFile(char *fname);

/*
 * Close fd and flush its heap-file state.
 * Return zero on success or a negative heap-file error.
 */
int HF_CloseFile(int fd);

/*
 * Insert len bytes from rec into fd and return its non-negative RID.
 * Return a negative heap-file error for invalid input or failure.
 */
int HF_Insert(int fd, char *rec, int len);

/*
 * Delete the record identified by rid in fd.
 * Return zero on success or a negative heap-file error.
 */
int HF_Delete(int fd, int rid);

/*
 * Copy the record identified by rid into buf and store its length in *len.
 * The caller supplies enough space in buf; return zero on success or a
 * negative heap-file error without modifying the record on failure.
 */
int HF_Get(int fd, int rid, char *buf, int *len);

/*
 * Open a sequential scan of fd in page and slot order.
 * Return a non-negative scan descriptor or a negative heap-file error.
 */
int HF_ScanOpen(int fd);

/*
 * Return the next live record in scanfd, copying it to buf, storing its
 * length in *len, and storing its RID in *rid.  Return zero for a record,
 * a designated EOF error at end of scan, or another negative error.
 */
int HF_ScanNext(int scanfd, char *buf, int *len, int *rid);

/*
 * Close scanfd and release its scan resources.
 * Return zero on success or a negative heap-file error.
 */
int HF_ScanClose(int scanfd);

/*
 * Fill out with slotted-file page, record, useful-byte, and utilization
 * statistics for fd.  Return zero on success or a negative heap-file error.
 */
int HF_GetSpaceStats(int fd, HFSpaceStats *out);

/*
 * Create an empty fixed-length static-baseline file named fname using
 * record_length bytes per slot.  Return zero or a negative error.
 */
int HF_StaticCreateFile(char *fname, int record_length);

/*
 * Open the fixed-length static-baseline file named fname.
 * Return a non-negative descriptor or a negative error.
 */
int HF_StaticOpenFile(char *fname);

/*
 * Close fd and flush its static-baseline state.
 * Return zero on success or a negative error.
 */
int HF_StaticCloseFile(int fd);

/*
 * Insert rec into the fixed-length file, truncating records longer than its
 * configured slot length and counting each truncation.  Return zero or a
 * negative error.
 */
int HF_StaticInsert(int fd, char *rec, int len);

/*
 * Fill out with static-baseline page, record, truncation, and utilization
 * statistics for fd.  Return zero on success or a negative error.
 */
int HF_StaticGetSpaceStats(int fd, HFSpaceStats *out);

#endif /* HF_H */
