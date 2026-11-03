/* ambulk.c: bottom-up construction of AM-format B+ trees */
#include <stdio.h>
#include <string.h>

#include "am.h"
#include "ambulk.h"
#include "pf.h"
#include "config.h"

extern int AM_CreateIndex(char *fileName, int indexNo, int attrType,
			  int attrLength);
extern int AM_DestroyIndex(char *fileName, int indexNo);
extern int PF_OpenFile(char *fname);
extern int PF_CloseFile(int fd);
extern int PF_GetThisPage(int fd, int pagenum, char **pagebuf);
extern int PF_AllocPage(int fd, int *pagenum, char **pagebuf);
extern int PF_UnfixPage(int fd, int pagenum, int dirty);
extern int AM_InsertintoLeaf(char *pageBuf, int attrLength, char *value,
			     int recId, int index, int status);
extern int AM_FillRootPage(char *pageBuf, int pageNum1, int pageNum2,
			   char *value, int attrLength, int maxKeys);
extern int AM_AddtoIntPage(char *pageBuf, char *value, int pageNum,
			   AM_INTHEADER *header, int offset);

extern char *malloc();
extern char *realloc();
extern void free();

struct bulk_item {
	char *key;
	int rid;
};

struct bulk_group {
	char *key;
	int first;
	int count;
};

struct bulk_child {
	int page;
	char *key;
};

static void *
bulk_alloc(unsigned long size)
{
	void *p;

	p = malloc(size);
	return p;
}

static void
bulk_free(void *p)
{
	free(p);
}

static int
bulk_add_item(struct bulk_item **items, int *count, int *capacity,
	      const char *key, int attrLength, int rid)
{
	struct bulk_item *grown;
	char *copy;

	if (*count == *capacity) {
		*capacity *= 2;
		grown = (struct bulk_item *)realloc(*items,
			(unsigned long)*capacity * sizeof(**items));
		if (grown == NULL)
			return AME_INTERROR;
		*items = grown;
	}
	copy = (char *)bulk_alloc((unsigned long)attrLength);
	if (copy == NULL)
		return AME_INTERROR;
	memcpy(copy, key, (size_t)attrLength);
	(*items)[*count].key = copy;
	(*items)[*count].rid = rid;
	(*count)++;
	return AME_OK;
}

static void
bulk_free_items(struct bulk_item *items, int count)
{
	int i;

	for (i = 0; i < count; i++)
		bulk_free(items[i].key);
	bulk_free(items);
}

static int
bulk_group_items(struct bulk_item *items, int count, int attrLength,
		 struct bulk_group **groups, int *group_count)
{
	struct bulk_group *result;
	int capacity;
	int i;

	capacity = count > 0 ? count : 1;
	result = (struct bulk_group *)bulk_alloc(
		(unsigned long)capacity * sizeof(*result));
	if (result == NULL)
		return AME_INTERROR;
	*group_count = 0;
	for (i = 0; i < count; i++) {
		if (*group_count == 0 ||
		    memcmp(result[*group_count - 1].key, items[i].key,
			   (size_t)attrLength) != 0) {
			result[*group_count].key = items[i].key;
			result[*group_count].first = i;
			result[*group_count].count = 1;
			(*group_count)++;
		} else {
			result[*group_count - 1].count++;
		}
	}
	*groups = result;
	return AME_OK;
}

static int
bulk_alloc_page(int fd, char **page, int *page_number)
{
	int error;

	error = PF_AllocPage(fd, page_number, page);
	return error == PFE_OK ? AME_OK : AME_PF;
}

static int
bulk_write_internal(int fd, int page_number, struct bulk_child *children,
		    int first, int count, int attrLength, int maxKeys)
{
	char *page;
	AM_INTHEADER header;
	int i;
	int error;

	error = PF_GetThisPage(fd, page_number, &page);
	if (error != PFE_OK)
		return AME_PF;
	if (count < 2) {
		fprintf(stderr, "internal count=%d\n", count);
		PF_UnfixPage(fd, page_number, FALSE);
		return AME_INVALIDVALUE;
	}
	AM_FillRootPage(page, children[first].page, children[first + 1].page,
			children[first + 1].key, attrLength, maxKeys);
	for (i = 2; i < count; i++) {
		memcpy(&header, page, sizeof(header));
		AM_AddtoIntPage(page, children[first + i].key,
				children[first + i].page, &header, i - 1);
		memcpy(page, &header, sizeof(header));
	}
	if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK)
		return AME_PF;
	return AME_OK;
}

static int
bulk_target(int capacity, double factor)
{
	int target;

	target = (int)(capacity * factor);
	if (target < 2)
		target = 2;
	if (target > capacity)
		target = capacity;
	return target;
}

static int
bulk_build_leaves(int fd, AM_LEAFHEADER *template, int attrLength,
		  struct bulk_item *items, struct bulk_group *groups,
		  int group_count, double factor, struct bulk_child **children,
		  int *child_count)
{
	struct bulk_child *result;
	char leaf[PF_PAGE_SIZE];
	char *page;
	AM_LEAFHEADER header;
	int capacity;
	int target;
	int first;
	int group;
	int item;
	int page_count;
	int previous_page;
	int page_number;
	int replay;
	int replay_item;
	int status;

	capacity = template->maxKeys;
	target = bulk_target(capacity, factor);
	result = (struct bulk_child *)bulk_alloc(
		(unsigned long)(group_count + 1) * sizeof(*result));
	if (result == NULL)
		return AME_INTERROR;
	*child_count = 0;
	previous_page = AM_NULL_PAGE;
	group = 0;
	while (group < group_count) {
		first = group;
		header = *template;
		header.nextLeafPage = AM_NULL_PAGE;
		memcpy(leaf, &header, sizeof(header));
		page_count = 0;
		while (group < group_count && page_count < target) {
			for (item = 0; item < groups[group].count; item++) {
				status = item == 0 ? AM_NOT_FOUND : AM_FOUND;
				if (!AM_InsertintoLeaf(leaf, attrLength,
					groups[group].key,
					items[groups[group].first + item].rid,
					page_count + 1, status))
					break;
			}
			if (item != groups[group].count) {
				if (page_count == 0) {
					fprintf(stderr, "leaf failure group=%d items=%d item=%d target=%d max=%d\n",
						group, groups[group].count, item,
						target, capacity);
					bulk_free(result);
					return AME_INVALIDVALUE;
				}
				header = *template;
				header.nextLeafPage = AM_NULL_PAGE;
				memcpy(leaf, &header, sizeof(header));
				for (replay = first; replay < group; replay++)
					for (replay_item = 0;
					     replay_item < groups[replay].count;
					     replay_item++)
						AM_InsertintoLeaf(leaf, attrLength,
							groups[replay].key,
							items[groups[replay].first +
							      replay_item].rid,
							replay - first + 1,
							replay_item == 0 ?
							AM_NOT_FOUND : AM_FOUND);
				break;
			}
			page_count++;
			group++;
		}
		if (bulk_alloc_page(fd, &page, &page_number) != AME_OK) {
			bulk_free(result);
			return AME_PF;
		}
		memcpy(page, leaf, PF_PAGE_SIZE);
		if (PF_UnfixPage(fd, page_number, TRUE) != PFE_OK) {
			bulk_free(result);
			return AME_PF;
		}
		if (previous_page != AM_NULL_PAGE) {
			if (PF_GetThisPage(fd, previous_page, &page) != PFE_OK) {
				bulk_free(result);
				return AME_PF;
			}
			memcpy(&header, page, sizeof(header));
			header.nextLeafPage = page_number;
			memcpy(page, &header, sizeof(header));
			if (PF_UnfixPage(fd, previous_page, TRUE) != PFE_OK) {
				bulk_free(result);
				return AME_PF;
			}
		}
		result[*child_count].page = page_number;
		result[*child_count].key = groups[first].key;
		(*child_count)++;
		previous_page = page_number;
	}
	*children = result;
	return AME_OK;
}

int
AM_BulkLoad(char *relName, int indexNo, char attrType, int attrLength,
	    AMBulkNext next, void *context)
{
	struct bulk_item *items;
	struct bulk_group *groups;
	struct bulk_child *children;
	struct bulk_child *next_children;
	AM_LEAFHEADER template;
	char index_name[AM_MAX_FNAME_LENGTH];
	char key[AM_MAXATTRLENGTH];
	char *page;
	double factor;
	int fd;
	int item_count;
	int item_capacity;
	int group_count;
	int child_count;
	int next_child_count;
	int page_number;
	int maxKeys;
	int rid;
	int status;
	int error;
	int i;
	children = NULL;

	if (relName == NULL || next == NULL || attrLength < 1 ||
	    attrLength > AM_MAXATTRLENGTH ||
	    (attrType != 'c' && attrType != 'i' && attrType != 'f'))
		return AME_INVALIDVALUE;
	factor = Config_GetDouble("bulk_fill_factor", 1.0);
	if (factor <= 0.0 || factor > 1.0)
		return AME_INVALIDVALUE;
	items = (struct bulk_item *)bulk_alloc(sizeof(*items) * 16UL);
	if (items == NULL)
		return AME_INTERROR;
	item_count = 0;
	item_capacity = 16;
	status = AM_BULK_NEXT_ITEM;
	while (status == AM_BULK_NEXT_ITEM) {
		status = next(context, key, &rid);
		if (status == AM_BULK_NEXT_ITEM) {
			if (rid < 0 ||
			    (item_count > 0 &&
			     memcmp(items[item_count - 1].key, key,
				    (size_t)attrLength) > 0)) {
				bulk_free_items(items, item_count);
				return AME_INVALIDVALUE;
			}
			error = bulk_add_item(&items, &item_count, &item_capacity,
					      key, attrLength, rid);
			if (error != AME_OK) {
				bulk_free_items(items, item_count);
				return error;
			}
		} else if (status != AM_BULK_NEXT_EOF &&
			   status != AM_BULK_NEXT_ERROR) {
			bulk_free_items(items, item_count);
			return AME_INVALIDVALUE;
		}
	}
	if (status == AM_BULK_NEXT_ERROR) {
		bulk_free_items(items, item_count);
		return AME_INVALIDVALUE;
	}
	error = bulk_group_items(items, item_count, attrLength, &groups,
				 &group_count);
	if (error != AME_OK) {
		bulk_free_items(items, item_count);
		return error;
	}
	if (AM_CreateIndex(relName, indexNo, attrType, attrLength) != AME_OK) {
		bulk_free(groups);
		bulk_free_items(items, item_count);
		return AM_Errno;
	}
	sprintf(index_name, "%s.%d", relName, indexNo);
	fd = PF_OpenFile(index_name);
	if (fd < 0) {
		AM_DestroyIndex(relName, indexNo);
		bulk_free(groups);
		bulk_free_items(items, item_count);
		return AME_PF;
	}
	if (PF_GetThisPage(fd, 0, &page) != PFE_OK) {
		PF_CloseFile(fd);
		AM_DestroyIndex(relName, indexNo);
		bulk_free(groups);
		bulk_free_items(items, item_count);
		return AME_PF;
	}
	memcpy(&template, page, sizeof(template));
	if (PF_UnfixPage(fd, 0, FALSE) != PFE_OK) {
		PF_CloseFile(fd);
		AM_DestroyIndex(relName, indexNo);
		bulk_free(groups);
		bulk_free_items(items, item_count);
		return AME_PF;
	}
	maxKeys = template.maxKeys;
	if (group_count == 0) {
		if (PF_GetThisPage(fd, 0, &page) != PFE_OK)
			error = AME_PF;
		else {
			if (PF_UnfixPage(fd, 0, TRUE) != PFE_OK)
				error = AME_PF;
			else
				error = AME_OK;
		}
	} else {
		error = bulk_build_leaves(fd, &template, attrLength, items, groups,
					  group_count, factor, &children,
					  &child_count);
		while (error == AME_OK && child_count > 1) {
			next_children = (struct bulk_child *)bulk_alloc(
				(unsigned long)child_count * sizeof(*next_children));
			if (next_children == NULL) {
				error = AME_INTERROR;
				break;
			}
			next_child_count = 0;
			for (i = 0; i < child_count; ) {
				int count;
				count = child_count - i;
				if (count > bulk_target(maxKeys + 1, factor))
					count = bulk_target(maxKeys + 1, factor);
				if (child_count - i > count &&
				    child_count - i - count == 1 &&
				    count > 2)
					count--;
				if (count < 2) {
					fprintf(stderr, "internal remainder child_count=%d i=%d target=%d\n",
						child_count, i,
						bulk_target(maxKeys + 1, factor));
					error = AME_INVALIDVALUE;
					break;
				}
				if (bulk_alloc_page(fd, &page, &page_number) != AME_OK) {
					error = AME_PF;
					break;
				}
				PF_UnfixPage(fd, page_number, FALSE);
				error = bulk_write_internal(fd, page_number, children,
							    i, count, attrLength,
							    maxKeys);
				if (error != AME_OK)
					break;
				next_children[next_child_count].page = page_number;
				next_children[next_child_count].key =
					children[i].key;
				next_child_count++;
				i += count;
			}
			bulk_free(children);
			children = next_children;
			child_count = next_child_count;
		}
		if (error == AME_OK) {
			if (PF_GetThisPage(fd, 0, &page) != PFE_OK)
				error = AME_PF;
			else {
				if (child_count == 1) {
					char *source;
					if (PF_GetThisPage(fd, children[0].page,
							   &source) != PFE_OK)
						error = AME_PF;
					else {
						memcpy(page, source, PF_PAGE_SIZE);
						PF_UnfixPage(fd, children[0].page,
							     FALSE);
					}
				} else {
					if (PF_GetThisPage(fd, children[0].page,
							   &page) == PFE_OK)
						PF_UnfixPage(fd, children[0].page,
							     FALSE);
				}
				if (error == AME_OK &&
				    PF_UnfixPage(fd, 0, TRUE) != PFE_OK)
					error = AME_PF;
			}
		}
		bulk_free(children);
	}
	if (PF_CloseFile(fd) != PFE_OK)
		error = AME_PF;
	if (error != AME_OK)
		AM_DestroyIndex(relName, indexNo);
	bulk_free(groups);
	bulk_free_items(items, item_count);
	return error;
}
