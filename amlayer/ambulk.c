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
/* amsearch.c, K&R: the char attrType is passed promoted to int */
extern int AM_Compare(char *bufPtr, int attrType, int attrLength,
		      char *valPtr);

extern char *malloc();
extern char *realloc();
extern void free();

/* a tree has far fewer levels than this; the plan stops with an error */
#define BULK_MAX_LEVELS 32

struct bulk_item {
	char *key;
	int rid;
};

/* one distinct key and the run of items (record ids) that carry it */
struct bulk_group {
	char *key;
	int first;
	int count;
};

/*
 * One planned page. For a leaf, first/count select groups; for an internal
 * node they select nodes of the level below. key is the smallest key under
 * the node, which is the separator its parent stores in front of it.
 */
struct bulk_node {
	int first;
	int count;
	int page;
	char *key;
};

struct bulk_level {
	struct bulk_node *nodes;
	int count;
};

/* pages this load holds fixed, so an error path can unfix all of them */
struct bulk_pins {
	int fd;
	int pages[2];
	int count;
};

static void *
bulk_malloc(unsigned long size)
{
	return (void *)malloc(size);
}

static void
bulk_free(void *p)
{
	if (p != NULL)
		free(p);
}

static int
bulk_add_item(struct bulk_item **items, int *count, int *capacity,
	      const char *key, int attrLength, int rid)
{
	struct bulk_item *grown;
	char *copy;

	if (*count == *capacity) {
		grown = (struct bulk_item *)realloc(*items,
			(unsigned long)*capacity * 2UL * sizeof(**items));
		if (grown == NULL)
			return AME_INTERROR;
		*items = grown;
		*capacity *= 2;
	}
	copy = (char *)bulk_malloc((unsigned long)attrLength);
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

	if (items == NULL)
		return;
	for (i = 0; i < count; i++)
		bulk_free(items[i].key);
	bulk_free(items);
}

/* keys that AM_Compare calls equal are one AM key with a list of rids */
static int
bulk_group_items(struct bulk_item *items, int count, char attrType,
		 int attrLength, struct bulk_group **groups, int *group_count)
{
	struct bulk_group *result;
	int i;

	result = (struct bulk_group *)bulk_malloc(
		(unsigned long)(count > 0 ? count : 1) * sizeof(*result));
	if (result == NULL)
		return AME_INTERROR;
	*group_count = 0;
	for (i = 0; i < count; i++) {
		if (*group_count == 0 ||
		    AM_Compare(result[*group_count - 1].key, (int)attrType,
			       attrLength, items[i].key) != 0) {
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

/* target = floor(factor * capacity), clamped to [minimum, capacity] */
static int
bulk_target(int capacity, double factor, int minimum)
{
	int target;

	target = (int)(factor * (double)capacity);
	if (target < minimum)
		target = minimum;
	if (target > capacity)
		target = capacity;
	return target;
}

/*
 * Split the groups into leaves. A leaf takes groups until it holds target
 * keys or the next group's bytes do not fit. A new key costs attrLength +
 * AM_ss (key and list head) + AM_si + AM_ss (rid and next pointer), each
 * further rid AM_si + AM_ss, and a fresh leaf has PF_PAGE_SIZE - AM_sl
 * free bytes. These are the checks AM_InsertintoLeaf makes, so every
 * planned leaf can be filled. A duplicate key never spans two leaves.
 */
static int
bulk_plan_leaves(struct bulk_group *groups, int group_count, int attrLength,
		 int target, struct bulk_level *level)
{
	int available;
	int used;
	int need;
	int keys;
	int group;

	level->nodes = (struct bulk_node *)bulk_malloc(
		(unsigned long)group_count * sizeof(*level->nodes));
	if (level->nodes == NULL)
		return AME_INTERROR;
	level->count = 0;
	available = PF_PAGE_SIZE - (int)AM_sl;
	group = 0;
	while (group < group_count) {
		level->nodes[level->count].first = group;
		level->nodes[level->count].key = groups[group].key;
		level->nodes[level->count].page = AM_NULL_PAGE;
		used = 0;
		keys = 0;
		while (group < group_count && keys < target) {
			need = attrLength + (int)AM_ss +
			       groups[group].count * (int)(AM_si + AM_ss);
			if (used + need > available)
				break;
			used += need;
			keys++;
			group++;
		}
		/* cannot happen: every group was checked against available */
		if (keys == 0)
			return AME_INTERROR;
		level->nodes[level->count].count = keys;
		level->count++;
	}
	return AME_OK;
}

/*
 * Group the nodes of one level under ceil(n / target) parents and spread
 * the children evenly, so with target >= 3 and n >= 2 every parent gets
 * at least 2 children (an AM internal node needs 1 key = 2 children).
 */
static int
bulk_plan_internal(struct bulk_level *below, int target,
		   struct bulk_level *level)
{
	int parents;
	int base;
	int extra;
	int first;
	int i;

	parents = (below->count + target - 1) / target;
	level->nodes = (struct bulk_node *)bulk_malloc(
		(unsigned long)parents * sizeof(*level->nodes));
	if (level->nodes == NULL)
		return AME_INTERROR;
	level->count = parents;
	base = below->count / parents;
	extra = below->count % parents;
	first = 0;
	for (i = 0; i < parents; i++) {
		level->nodes[i].first = first;
		level->nodes[i].count = base + (i < extra ? 1 : 0);
		level->nodes[i].key = below->nodes[first].key;
		level->nodes[i].page = AM_NULL_PAGE;
		first += level->nodes[i].count;
	}
	return AME_OK;
}

static void
bulk_pin(struct bulk_pins *pins, int page)
{
	pins->pages[pins->count++] = page;
}

/* fix page 0, or allocate a new page, and remember that it is fixed */
static int
bulk_page(struct bulk_pins *pins, int root, int *page_number, char **page)
{
	if (pins->count == 2)
		return AME_INTERROR;
	if (root) {
		*page_number = 0;
		if (PF_GetThisPage(pins->fd, 0, page) != PFE_OK)
			return AME_PF;
	} else if (PF_AllocPage(pins->fd, page_number, page) != PFE_OK) {
		return AME_PF;
	}
	bulk_pin(pins, *page_number);
	return AME_OK;
}

/* forget the page first, so a failed unfix is not tried again later */
static int
bulk_unfix(struct bulk_pins *pins, int page, int dirty)
{
	int i;

	for (i = 0; i < pins->count; i++)
		if (pins->pages[i] == page) {
			pins->pages[i] = pins->pages[--pins->count];
			break;
		}
	return PF_UnfixPage(pins->fd, page, dirty) == PFE_OK ? AME_OK : AME_PF;
}

static void
bulk_unfix_all(struct bulk_pins *pins)
{
	while (pins->count > 0)
		(void)bulk_unfix(pins, pins->pages[pins->count - 1], FALSE);
}

/*
 * Write the leaves in key order and chain them. A leaf stays fixed until
 * the next one has its page number, so at most two pages are fixed. A tree
 * of one leaf is written straight into page 0, the root.
 */
static int
bulk_write_leaves(struct bulk_pins *pins, AM_LEAFHEADER *template,
		  int attrLength, struct bulk_item *items,
		  struct bulk_group *groups, struct bulk_level *leaves)
{
	AM_LEAFHEADER header;
	struct bulk_node *node;
	char *page;
	char *previous_page;
	int previous;
	int error;
	int key_index;
	int group;
	int item;
	int i;

	previous = AM_NULL_PAGE;
	previous_page = NULL;
	for (i = 0; i < leaves->count; i++) {
		node = &leaves->nodes[i];
		error = bulk_page(pins, leaves->count == 1, &node->page, &page);
		if (error != AME_OK)
			return error;
		memset(page, 0, PF_PAGE_SIZE);
		header = *template;
		header.nextLeafPage = AM_NULL_PAGE;
		memcpy(page, &header, sizeof(header));
		for (key_index = 1; key_index <= node->count; key_index++) {
			group = node->first + key_index - 1;
			for (item = 0; item < groups[group].count; item++)
				if (AM_InsertintoLeaf(page, attrLength,
					groups[group].key,
					items[groups[group].first + item].rid,
					key_index,
					item == 0 ? AM_NOT_FOUND : AM_FOUND)
				    != TRUE)
					return AME_INTERROR;
		}
		if (previous != AM_NULL_PAGE) {
			memcpy(&header, previous_page, sizeof(header));
			header.nextLeafPage = node->page;
			memcpy(previous_page, &header, sizeof(header));
			error = bulk_unfix(pins, previous, TRUE);
			if (error != AME_OK)
				return error;
		}
		previous = node->page;
		previous_page = page;
	}
	return bulk_unfix(pins, previous, TRUE);
}

/*
 * Write one internal level with the AM's own page functions. Child 0 goes
 * in front of key 1, child c (c >= 1) after key c, and key c is the
 * smallest key under child c. A level of one node is the root: page 0.
 */
static int
bulk_write_internal(struct bulk_pins *pins, struct bulk_level *below,
		    struct bulk_level *level, int attrLength, int maxKeys)
{
	AM_INTHEADER header;
	struct bulk_node *children;
	struct bulk_node *node;
	char *page;
	int error;
	int i;
	int c;

	for (i = 0; i < level->count; i++) {
		node = &level->nodes[i];
		children = below->nodes + node->first;
		if (node->count < 2 || node->count > maxKeys + 1)
			return AME_INTERROR;
		error = bulk_page(pins, level->count == 1, &node->page, &page);
		if (error != AME_OK)
			return error;
		memset(page, 0, PF_PAGE_SIZE);
		AM_FillRootPage(page, children[0].page, children[1].page,
				children[1].key, attrLength, maxKeys);
		for (c = 2; c < node->count; c++) {
			memcpy(&header, page, sizeof(header));
			AM_AddtoIntPage(page, children[c].key, children[c].page,
					&header, c - 1);
			memcpy(page, &header, sizeof(header));
		}
		error = bulk_unfix(pins, node->page, TRUE);
		if (error != AME_OK)
			return error;
	}
	return AME_OK;
}

/****************************************************************************
AM_BulkLoad(relName, indexNo, attrType, attrLength, next, context)

SPECIFICATIONS:
	Create index relName.indexNo and fill it bottom-up from the stream
	that next() supplies: (key, rid) pairs in AM comparison order
	(AM_Compare: numeric for 'i' and 'f', strncmp for 'c'), equal keys
	next to each other. Keys that compare equal become one AM key with a
	list of rids. The whole stream is read and checked before the file
	is created.

	Shape. With fill factor f from toydb.conf (bulk_fill_factor,
	default 1.0, valid range (0, 1]) and key length L:
	  leaf capacity      = floor((PF_PAGE_SIZE - sizeof(AM_LEAFHEADER))
	                             / (L + 8)),
	      the number of keys with one rid each that fit in a leaf
	      (L+2 for key and list head, 6 for the rid and its next
	      pointer); 254 for char(8)
	  leaf target        = floor(f * leaf capacity), at least 2
	  internal capacity  = maxKeys + 1 children, maxKeys as set by
	      AM_CreateIndex; 341 for char(8)
	  internal target    = floor(f * internal capacity), at least 3
	Both targets are at most their capacity. Leaves are packed in key
	order up to the leaf target, closing early when a key's rid list does
	not fit (a key never spans two leaves). Each upper level has
	ceil(n / internal target) nodes with the children spread evenly. The
	root is written into page 0, where the AM expects it; a tree of one
	leaf is just page 0. So the file has exactly 1 + leaves + internal
	pages (root not counted twice), all reachable from page 0, and an
	empty stream gives the single empty leaf of AM_CreateIndex.

	Record-id limit. All rids of one key must fit in one leaf:
	  floor((PF_PAGE_SIZE - sizeof(AM_LEAFHEADER) - (L + 2)) / 6)
	rids per key, 677 for char(8).

	On any failure every page the load fixed is unfixed, the file is
	closed and destroyed, so no half-built index is left behind. Nothing
	is printed.

RETURN VALUE (also stored in AM_Errno):
	AME_OK			the index was built
	AME_INVALIDATTRTYPE	invalid argument: relName or next NULL, name
				too long, attrType not 'c'/'i'/'f', attrLength
				not in 1..255 or not 4 for 'i'/'f'; or the
				fill factor is not in (0, 1]
	AME_INVALIDVALUE	unsorted input (a key smaller than the one
				before it) or a negative rid
	AME_INVALIDATTRLENGTH	a key has more rids than the record-id limit
	AME_PF			a PF call failed (this includes an index file
				that already exists)
	AME_INTERROR		the load could not complete for a reason
				outside the input order and arguments: next()
				returned AM_BULK_NEXT_ERROR or an unknown
				status, or memory ran out
*****************************************************************************/
int
AM_BulkLoad(char *relName, int indexNo, char attrType, int attrLength,
	    AMBulkNext next, void *context)
{
	struct bulk_item *items;
	struct bulk_group *groups;
	struct bulk_level levels[BULK_MAX_LEVELS];
	struct bulk_pins pins;
	AM_LEAFHEADER template;
	char index_name[AM_MAX_FNAME_LENGTH];
	char key[AM_MAXATTRLENGTH];
	char *page;
	double factor;
	int item_count;
	int item_capacity;
	int group_count;
	int level_count;
	int leaf_target;
	int internal_target;
	int rid_limit;
	int maxKeys;
	int created;
	int rid;
	int status;
	int error;
	int i;

	items = NULL;
	groups = NULL;
	item_count = 0;
	group_count = 0;
	level_count = 0;
	created = 0;
	pins.fd = -1;
	pins.count = 0;
	error = AME_OK;

	factor = Config_GetDouble("bulk_fill_factor", 1.0);
	if (relName == NULL || next == NULL ||
	    strlen(relName) + 12 >= sizeof(index_name) ||
	    (attrType != 'c' && attrType != 'i' && attrType != 'f') ||
	    attrLength < 1 || attrLength > 255 ||
	    (attrType != 'c' && attrLength != 4) ||
	    !(factor > 0.0 && factor <= 1.0)) {
		error = AME_INVALIDATTRTYPE;
		goto done;
	}

	/* read and check the whole stream */
	item_capacity = 16;
	items = (struct bulk_item *)bulk_malloc(
		(unsigned long)item_capacity * sizeof(*items));
	if (items == NULL) {
		error = AME_INTERROR;
		goto done;
	}
	for (;;) {
		status = next(context, key, &rid);
		if (status == AM_BULK_NEXT_EOF)
			break;
		if (status != AM_BULK_NEXT_ITEM) {
			error = AME_INTERROR;
			goto done;
		}
		if (rid < 0 || (item_count > 0 &&
		    AM_Compare(items[item_count - 1].key, (int)attrType,
			       attrLength, key) < 0)) {
			error = AME_INVALIDVALUE;
			goto done;
		}
		error = bulk_add_item(&items, &item_count, &item_capacity,
				      key, attrLength, rid);
		if (error != AME_OK)
			goto done;
	}
	error = bulk_group_items(items, item_count, attrType, attrLength,
				 &groups, &group_count);
	if (error != AME_OK)
		goto done;
	rid_limit = (PF_PAGE_SIZE - (int)AM_sl - (attrLength + (int)AM_ss)) /
		    (int)(AM_si + AM_ss);
	for (i = 0; i < group_count; i++)
		if (groups[i].count > rid_limit) {
			error = AME_INVALIDATTRLENGTH;
			goto done;
		}

	if (AM_CreateIndex(relName, indexNo, attrType, attrLength) != AME_OK) {
		error = AME_PF;
		goto done;
	}
	created = 1;
	if (group_count == 0)
		goto done;
	sprintf(index_name, "%s.%d", relName, indexNo);
	pins.fd = PF_OpenFile(index_name);
	if (pins.fd < 0) {
		error = AME_PF;
		goto done;
	}
	/* page 0 is the empty leaf of AM_CreateIndex: the header template */
	if (PF_GetThisPage(pins.fd, 0, &page) != PFE_OK) {
		error = AME_PF;
		goto done;
	}
	bulk_pin(&pins, 0);
	memcpy(&template, page, sizeof(template));
	error = bulk_unfix(&pins, 0, FALSE);
	if (error != AME_OK)
		goto done;
	maxKeys = template.maxKeys;

	/* plan every level before writing a page */
	leaf_target = bulk_target((PF_PAGE_SIZE - (int)AM_sl) /
				  (attrLength + (int)AM_ss + (int)(AM_si + AM_ss)),
				  factor, 2);
	internal_target = bulk_target(maxKeys + 1, factor, 3);
	error = bulk_plan_leaves(groups, group_count, attrLength, leaf_target,
				 &levels[0]);
	level_count = 1;
	while (error == AME_OK && levels[level_count - 1].count > 1) {
		if (level_count == BULK_MAX_LEVELS) {
			error = AME_INTERROR;
			break;
		}
		error = bulk_plan_internal(&levels[level_count - 1],
					   internal_target, &levels[level_count]);
		level_count++;
	}
	if (error != AME_OK)
		goto done;

	/* leaves first, then each level up; the root (page 0) comes last */
	error = bulk_write_leaves(&pins, &template, attrLength, items, groups,
				  &levels[0]);
	for (i = 1; error == AME_OK && i < level_count; i++)
		error = bulk_write_internal(&pins, &levels[i - 1], &levels[i],
					    attrLength, maxKeys);

done:
	bulk_unfix_all(&pins);
	if (pins.fd >= 0 && PF_CloseFile(pins.fd) != PFE_OK &&
	    error == AME_OK)
		error = AME_PF;
	if (error != AME_OK && created)
		(void)AM_DestroyIndex(relName, indexNo);
	for (i = 0; i < level_count; i++)
		bulk_free(levels[i].nodes);
	bulk_free(groups);
	bulk_free_items(items, item_count);
	AM_Errno = error;
	return error;
}
