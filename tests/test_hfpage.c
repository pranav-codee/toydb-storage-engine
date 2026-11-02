/* test_hfpage.c: hand-checked tests for the pure slotted-page layout */
#include "hfpage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "test_hfpage: %s\n", message);
		exit(1);
	}
}

int
main(void)
{
	unsigned char page[PF_PAGE_SIZE];
	char record[HF_MAX_RECLEN];
	char output[HF_MAX_RECLEN];
	int slot;
	int length;
	int expected_slots;
	int expected_free;
	int i;

	memset(record, 'R', sizeof(record));
	HFPage_Init(page);
	check(HFPage_FreeSpace(page) == PF_PAGE_SIZE - HF_PAGE_HEADER_SIZE,
	      "initial free space");

	expected_slots = 0;
	expected_free = PF_PAGE_SIZE - HF_PAGE_HEADER_SIZE;
	for (i = 0; i < 10; i++) {
		slot = HFPage_Insert(page, record, 100);
		check(slot == i, "fixed-size slot number");
		expected_slots++;
		expected_free -= HF_PAGE_SLOT_SIZE + 100;
		check(HFPage_FreeSpace(page) == expected_free,
		      "fixed-size free space");
	}
	check(expected_slots == 10, "fixed-size record count");

	HFPage_Init(page);
	expected_free = PF_PAGE_SIZE - HF_PAGE_HEADER_SIZE;
	slot = HFPage_Insert(page, record, 0);
	check(slot == 0 && HFPage_FreeSpace(page) ==
	      PF_PAGE_SIZE - HF_PAGE_HEADER_SIZE - HF_PAGE_SLOT_SIZE,
	      "zero-length record");
	slot = HFPage_Insert(page, record, HF_MAX_RECLEN);
	check(slot == 1, "maximum-length record");
	expected_free -= 2 * HF_PAGE_SLOT_SIZE + HF_MAX_RECLEN;
	check(HFPage_FreeSpace(page) == expected_free, "maximum record space");
	check(HFPage_Insert(page, record, HF_MAX_RECLEN + 1) == HFE_TOOLONG,
	      "maximum-length boundary");

	HFPage_Init(page);
	for (i = 0; i < 39; i++)
		check(HFPage_Insert(page, record, 100) == i,
		      "exact-fit filling records");
	check(HFPage_Insert(page, record, 32) == 39, "exact-fit final record");
	check(HFPage_FreeSpace(page) == 0, "exact-fit free space");
	check(HFPage_Insert(page, record, 1) == HF_PAGE_NOSPACE,
	      "one-byte-too-large record");

	HFPage_Init(page);
	check(HFPage_Insert(page, "zero", 4) == 0, "first record");
	check(HFPage_Insert(page, "one", 3) == 1, "second record");
	check(HFPage_Delete(page, 0) == HF_OK, "delete first record");
	check(HFPage_Delete(page, 0) == HFE_NOTFOUND,
	      "repeat delete rejected");
	check(HFPage_Read(page, 1, output, &length) == HF_OK &&
	      length == 3 && memcmp(output, "one", 3) == 0,
	      "other slot remains readable");
	check(HFPage_Insert(page, "new", 3) == 0, "deleted slot reused");
	check(HFPage_Read(page, 0, output, &length) == HF_OK &&
	      length == 3 && memcmp(output, "new", 3) == 0,
	      "reused slot readable");
	check(HFPage_Insert(page, "third", 5) == 2, "slot numbers remain stable");

	printf("test_hfpage done!\n");
	return 0;
}
