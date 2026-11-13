// SPDX-License-Identifier: GPL-2.0
/*
 * f2fs extent cache support
 *
 * Copyright (c) 2015 Motorola Mobility
 * Copyright (c) 2015 Samsung Electronics
 * Authors: Jaegeuk Kim <jaegeuk@kernel.org>
 *          Chao Yu <chao2.yu@samsung.com>
 */

#include <linux/fs.h>
#include <linux/f2fs_fs.h>

#include "f2fs.h"
#include "node.h"
#include <trace/events/f2fs.h>

static struct rb_entry *__lookup_rb_tree_fast(struct rb_entry *cached_re,
							unsigned int ofs)
{
	if (cached_re) {
		if (cached_re->ofs <= ofs &&
				cached_re->ofs + cached_re->len > ofs) {
			return cached_re;
		}
	}
	return NULL;
}

static struct rb_entry *__lookup_rb_tree_slow(struct rb_root *root,
							unsigned int ofs)
{
	struct rb_node *node = root->rb_node;
	struct rb_entry *re;

	while (node) {
		re = rb_entry(node, struct rb_entry, rb_node);

		if (ofs < re->ofs)
			node = node->rb_left;
		else if (ofs >= re->ofs + re->len)
			node = node->rb_right;
		else
			return re;
	}
	return NULL;
}

struct rb_entry *f2fs_lookup_rb_tree(struct rb_root *root,
				struct rb_entry *cached_re, unsigned int ofs)
{
	struct rb_entry *re;

	re = __lookup_rb_tree_fast(cached_re, ofs);
	if (!re)
		return __lookup_rb_tree_slow(root, ofs);

	return re;
}

struct rb_node **f2fs_lookup_rb_tree_for_insert(struct f2fs_sb_info *sbi,
				struct rb_root *root, struct rb_node **parent,
				unsigned int ofs)
{
	struct rb_node **p = &root->rb_node;
	struct rb_entry *re;

	while (*p) {
		*parent = *p;
		re = rb_entry(*parent, struct rb_entry, rb_node);

		if (ofs < re->ofs)
			p = &(*p)->rb_left;
		else if (ofs >= re->ofs + re->len)
			p = &(*p)->rb_right;
		else
			f2fs_bug_on(sbi, 1);
	}

	return p;
}

/*
 * lookup rb entry in position of @ofs in rb-tree,
 * if hit, return the entry, otherwise, return NULL
 * @prev_ex: extent before ofs
 * @next_ex: extent after ofs
 * @insert_p: insert point for new extent at ofs
 * in order to simpfy the insertion after.
 * tree must stay unchanged between lookup and insertion.
 */
struct rb_entry *f2fs_lookup_rb_tree_ret(struct rb_root *root,
				struct rb_entry *cached_re,
				unsigned int ofs,
				struct rb_entry **prev_entry,
				struct rb_entry **next_entry,
				struct rb_node ***insert_p,
				struct rb_node **insert_parent,
				bool force)
{
	struct rb_node **pnode = &root->rb_node;
	struct rb_node *parent = NULL, *tmp_node;
	struct rb_entry *re = cached_re;

	*insert_p = NULL;
	*insert_parent = NULL;
	*prev_entry = NULL;
	*next_entry = NULL;

	if (RB_EMPTY_ROOT(root))
		return NULL;

	if (re) {
		if (re->ofs <= ofs && re->ofs + re->len > ofs)
			goto lookup_neighbors;
	}

	while (*pnode) {
		parent = *pnode;
		re = rb_entry(*pnode, struct rb_entry, rb_node);

		if (ofs < re->ofs)
			pnode = &(*pnode)->rb_left;
		else if (ofs >= re->ofs + re->len)
			pnode = &(*pnode)->rb_right;
		else
			goto lookup_neighbors;
	}

	*insert_p = pnode;
	*insert_parent = parent;

	re = rb_entry(parent, struct rb_entry, rb_node);
	tmp_node = parent;
	if (parent && ofs > re->ofs)
		tmp_node = rb_next(parent);
	*next_entry = rb_entry_safe(tmp_node, struct rb_entry, rb_node);

	tmp_node = parent;
	if (parent && ofs < re->ofs)
		tmp_node = rb_prev(parent);
	*prev_entry = rb_entry_safe(tmp_node, struct rb_entry, rb_node);
	return NULL;

lookup_neighbors:
	if (ofs == re->ofs || force) {
		/* lookup prev node for merging backward later */
		tmp_node = rb_prev(&re->rb_node);
		*prev_entry = rb_entry_safe(tmp_node, struct rb_entry, rb_node);
	}
	if (ofs == re->ofs + re->len - 1 || force) {
		/* lookup next node for merging frontward later */
		tmp_node = rb_next(&re->rb_node);
		*next_entry = rb_entry_safe(tmp_node, struct rb_entry, rb_node);
	}
	return re;
}

bool f2fs_check_rb_tree_consistence(struct f2fs_sb_info *sbi,
						struct rb_root *root)
{
#ifdef CONFIG_F2FS_CHECK_FS
	struct rb_node *cur = rb_first(root), *next;
	struct rb_entry *cur_re, *next_re;

	if (!cur)
		return true;

	while (cur) {
		next = rb_next(cur);
		if (!next)
			return true;

		cur_re = rb_entry(cur, struct rb_entry, rb_node);
		next_re = rb_entry(next, struct rb_entry, rb_node);

		if (cur_re->ofs + cur_re->len > next_re->ofs) {
			f2fs_info(sbi, "inconsistent rbtree, cur(%u, %u) next(%u, %u)",
				  cur_re->ofs, cur_re->len,
				  next_re->ofs, next_re->len);
			return false;
		}

		cur = next;
	}
#endif
	return true;
}

static struct kmem_cache *extent_tree_slab;
static struct kmem_cache *extent_node_slab;

static struct extent_node *__attach_extent_node(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei,
				struct rb_node *parent, struct rb_node **p)
{
	struct extent_node *en;

	en = kmem_cache_alloc(extent_node_slab, GFP_ATOMIC);
	if (!en)
		return NULL;

	en->ei = *ei;
	INIT_LIST_HEAD(&en->list);
	en->et = et;

	rb_link_node(&en->rb_node, parent, p);
	rb_insert_color(&en->rb_node, &et->root);
	atomic_inc(&et->node_cnt);
	atomic_inc(&sbi->extent_tree[et->type].total_ext_node);
	return en;
}

static void __detach_extent_node(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_node *en)
{
	rb_erase(&en->rb_node, &et->root);
	atomic_dec(&et->node_cnt);
	atomic_dec(&sbi->extent_tree[et->type].total_ext_node);

	if (et->cached_en == en)
		et->cached_en = NULL;
	kmem_cache_free(extent_node_slab, en);
}

/*
 * Flow to release an extent_node:
 * 1. list_del_init
 * 2. __detach_extent_node
 * 3. kmem_cache_free.
 */
static void __release_extent_node(struct f2fs_sb_info *sbi,
			struct extent_tree *et, struct extent_node *en)
{
	spin_lock(&sbi->extent_tree[et->type].extent_lock);
	f2fs_bug_on(sbi, list_empty(&en->list));
	list_del_init(&en->list);
	spin_unlock(&sbi->extent_tree[et->type].extent_lock);

	__detach_extent_node(sbi, et, en);
}

static struct extent_tree *__grab_extent_tree(struct inode *inode,
					 enum extent_type type)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et;
	nid_t ino = inode->i_ino;

	mutex_lock(&sbi->extent_tree[type].extent_tree_lock);
	et = radix_tree_lookup(&sbi->extent_tree[type].extent_tree_root, ino);
	if (!et) {
		et = f2fs_kmem_cache_alloc(extent_tree_slab, GFP_NOFS);
		f2fs_radix_tree_insert(&sbi->extent_tree[type].extent_tree_root, ino, et);
		memset(et, 0, sizeof(struct extent_tree));
		et->ino = ino;
		et->type = type;
		et->root = RB_ROOT;
		et->cached_en = NULL;
		rwlock_init(&et->lock);
		INIT_LIST_HEAD(&et->list);
		atomic_set(&et->node_cnt, 0);
		atomic_inc(&sbi->extent_tree[type].total_ext_tree);
	} else {
		atomic_dec(&sbi->extent_tree[type].total_zombie_tree);
		list_del_init(&et->list);
	}
	mutex_unlock(&sbi->extent_tree[type].extent_tree_lock);

	/* never died until evict_inode */
	F2FS_I(inode)->extent_tree[type] = et;

	return et;
}

static struct extent_node *__init_extent_tree(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei)
{
	struct rb_node **p = &et->root.rb_node;
	struct extent_node *en;

	en = __attach_extent_node(sbi, et, ei, NULL, p);
	if (!en)
		return NULL;

	et->largest = en->ei;
	et->cached_en = en;
	return en;
}

static unsigned int __free_extent_tree(struct f2fs_sb_info *sbi,
					struct extent_tree *et)
{
	struct rb_node *node, *next;
	struct extent_node *en;
	unsigned int count = atomic_read(&et->node_cnt);

	node = rb_first(&et->root);
	while (node) {
		next = rb_next(node);
		en = rb_entry(node, struct extent_node, rb_node);
		__release_extent_node(sbi, et, en);
		node = next;
	}

	return count - atomic_read(&et->node_cnt);
}

static void __drop_largest_extent(struct extent_tree *et,
					pgoff_t fofs, unsigned int len)
{
	if (fofs < et->largest.fofs + et->largest.len &&
			fofs + len > et->largest.fofs) {
		et->largest.len = 0;
		et->largest_updated = true;
	}
}

/* return true, if inode page is changed */
static bool __f2fs_init_extent_tree(struct inode *inode, struct f2fs_extent *i_ext)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et;
	struct extent_node *en;
	struct extent_info ei;

	if (!f2fs_may_extent_tree(inode)) {
		/* drop largest extent */
		if (i_ext && i_ext->len) {
			i_ext->len = 0;
			return true;
		}
		return false;
	}

	et = __grab_extent_tree(inode, EX_READ);

	if (!i_ext || !i_ext->len)
		return false;

	get_extent_info(&ei, i_ext);

	write_lock(&et->lock);
	if (atomic_read(&et->node_cnt))
		goto out;

	en = __init_extent_tree(sbi, et, &ei);
	if (en) {
		spin_lock(&sbi->extent_tree[EX_READ].extent_lock);
		list_add_tail(&en->list, &sbi->extent_tree[EX_READ].extent_list);
		spin_unlock(&sbi->extent_tree[EX_READ].extent_lock);
	}
out:
	write_unlock(&et->lock);
	return false;
}

bool f2fs_init_extent_tree(struct inode *inode, struct f2fs_extent *i_ext)
{
	bool ret =  __f2fs_init_extent_tree(inode, i_ext);

	if (!F2FS_I(inode)->extent_tree[EX_READ])
		set_inode_flag(inode, FI_NO_EXTENT);

	f2fs_init_age_extent_tree(inode);
	return ret;
}

static bool f2fs_lookup_extent_tree(struct inode *inode, pgoff_t pgofs,
							struct extent_info *ei)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree[EX_READ];
	struct extent_node *en;
	bool ret = false;

	f2fs_bug_on(sbi, !et);

	trace_f2fs_lookup_extent_tree_start(inode, pgofs);

	read_lock(&et->lock);

	if (et->largest.fofs <= pgofs &&
			et->largest.fofs + et->largest.len > pgofs) {
		*ei = et->largest;
		ret = true;
		stat_inc_largest_node_hit(sbi);
		goto out;
	}

	en = (struct extent_node *)f2fs_lookup_rb_tree(&et->root,
				(struct rb_entry *)et->cached_en, pgofs);
	if (!en)
		goto out;

	if (en == et->cached_en)
		stat_inc_cached_node_hit(sbi);
	else
		stat_inc_rbtree_node_hit(sbi);

	*ei = en->ei;
	spin_lock(&sbi->extent_tree[EX_READ].extent_lock);
	if (!list_empty(&en->list)) {
		list_move_tail(&en->list, &sbi->extent_tree[EX_READ].extent_list);
		et->cached_en = en;
	}
	spin_unlock(&sbi->extent_tree[EX_READ].extent_lock);
	ret = true;
out:
	stat_inc_total_hit(sbi);
	read_unlock(&et->lock);

	trace_f2fs_lookup_extent_tree_end(inode, pgofs, ret ? ei : NULL);
	return ret;
}

static struct extent_node *__try_merge_extent_node(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei,
				struct extent_node *prev_ex,
				struct extent_node *next_ex)
{
	struct extent_node *en = NULL;

	if (prev_ex && __is_back_mergeable(ei, &prev_ex->ei, et->type)) {
		prev_ex->ei.len += ei->len;
		ei = &prev_ex->ei;
		en = prev_ex;
	}

	if (next_ex && __is_front_mergeable(ei, &next_ex->ei, et->type)) {
		next_ex->ei.fofs = ei->fofs;
		if (et->type == EX_READ)
			next_ex->ei.blk = ei->blk;
		else {
			next_ex->ei.age = ei->age;
			next_ex->ei.last_blocks = ei->last_blocks;
		}
		next_ex->ei.len += ei->len;
		if (en)
			__release_extent_node(sbi, et, prev_ex);

		en = next_ex;
	}

	if (!en)
		return NULL;

	__try_update_largest_extent(et, en);

	spin_lock(&sbi->extent_tree[et->type].extent_lock);
	if (!list_empty(&en->list)) {
		list_move_tail(&en->list, &sbi->extent_tree[et->type].extent_list);
		et->cached_en = en;
	}
	spin_unlock(&sbi->extent_tree[et->type].extent_lock);
	return en;
}

static struct extent_node *__insert_extent_tree(struct f2fs_sb_info *sbi,
				struct extent_tree *et, struct extent_info *ei,
				struct rb_node **insert_p,
				struct rb_node *insert_parent)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct extent_node *en = NULL;

	if (insert_p && insert_parent) {
		parent = insert_parent;
		p = insert_p;
		goto do_insert;
	}

	p = f2fs_lookup_rb_tree_for_insert(sbi, &et->root, &parent, ei->fofs);
do_insert:
	en = __attach_extent_node(sbi, et, ei, parent, p);
	if (!en)
		return NULL;

	__try_update_largest_extent(et, en);

	/* update in global extent list */
	spin_lock(&sbi->extent_tree[et->type].extent_lock);
	list_add_tail(&en->list, &sbi->extent_tree[et->type].extent_list);
	et->cached_en = en;
	spin_unlock(&sbi->extent_tree[et->type].extent_lock);
	return en;
}

static void f2fs_update_extent_tree_range(struct inode *inode,
				struct extent_info *tei, enum extent_type type, bool invalidate)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree[type];
	struct extent_node *en = NULL, *en1 = NULL;
	struct extent_node *prev_en = NULL, *next_en = NULL;
	struct extent_info ei, dei, prev;
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	unsigned int fofs = tei->fofs, len = tei->len;
	unsigned int end = fofs + len;
	unsigned int pos = (unsigned int)fofs;
	bool updated = false;

	if (!et || !len)
		return;

	if (type == EX_READ)
		trace_f2fs_update_extent_tree_range(inode, fofs, tei->blk, len);
	else
		trace_f2fs_update_age_extent_tree_range(inode, fofs, len,
			tei->age, tei->last_blocks, invalidate);

	write_lock(&et->lock);

	if (type == EX_READ && is_inode_flag_set(inode, FI_NO_EXTENT)) {
		write_unlock(&et->lock);
		return;
	}

	prev = et->largest;
	dei.len = 0;

	/*
	 * drop largest extent before lookup, in case it's already
	 * been shrunk from extent tree
	 */
	if (type == EX_READ)
		__drop_largest_extent(et, fofs, len);

	/* 1. lookup first extent node in range [fofs, fofs + len - 1] */
	en = (struct extent_node *)f2fs_lookup_rb_tree_ret(&et->root,
					(struct rb_entry *)et->cached_en, fofs,
					(struct rb_entry **)&prev_en,
					(struct rb_entry **)&next_en,
					&insert_p, &insert_parent, false);
	if (!en)
		en = next_en;

	/* 2. invlidate all extent nodes in range [fofs, fofs + len - 1] */
	while (en && en->ei.fofs < end) {
		unsigned int org_end;
		int parts = 0;	/* # of parts current extent split into */

		next_en = en1 = NULL;

		dei = en->ei;
		org_end = dei.fofs + dei.len;
		f2fs_bug_on(sbi, pos >= org_end);

		if (pos > dei.fofs &&	(type != EX_READ || pos - dei.fofs >= F2FS_MIN_EXTENT_LEN)) {
			en->ei.len = pos - en->ei.fofs;
			prev_en = en;
			parts = 1;
		}

		if (end < org_end && (type != EX_READ || org_end - end >= F2FS_MIN_EXTENT_LEN)) {
			if (parts) {
				ei = dei;
					ei.fofs = end;
					ei.len = org_end - end;
					if (type == EX_READ)
						ei.blk += end - dei.fofs;
				en1 = __insert_extent_tree(sbi, et, &ei,
							NULL, NULL);
				next_en = en1;
			} else {
				en->ei.fofs = end;
				if (type == EX_READ)
					en->ei.blk += end - dei.fofs;
				en->ei.len -= end - dei.fofs;
				next_en = en;
			}
			parts++;
		}

		if (!next_en) {
			struct rb_node *node = rb_next(&en->rb_node);

			next_en = rb_entry_safe(node, struct extent_node,
						rb_node);
		}

		if (parts)
			__try_update_largest_extent(et, en);
		else
			__release_extent_node(sbi, et, en);

		/*
		 * if original extent is split into zero or two parts, extent
		 * tree has been altered by deletion or insertion, therefore
		 * invalidate pointers regard to tree.
		 */
		if (parts != 1) {
			insert_p = NULL;
			insert_parent = NULL;
		}
		en = next_en;
	}

	/* 3. update extent in extent cache */
	if (!invalidate) {

		ei = *tei;
		if (!__try_merge_extent_node(sbi, et, &ei, prev_en, next_en))
			__insert_extent_tree(sbi, et, &ei,
						insert_p, insert_parent);

		/* give up extent_cache, if split and small updates happen */
		if (type == EX_READ && dei.len >= 1 &&
				prev.len < F2FS_MIN_EXTENT_LEN &&
				et->largest.len < F2FS_MIN_EXTENT_LEN) {
			et->largest.len = 0;
			et->largest_updated = true;
			set_inode_flag(inode, FI_NO_EXTENT);
		}
	}

	if (type == EX_READ && is_inode_flag_set(inode, FI_NO_EXTENT))
		__free_extent_tree(sbi, et);

	if (et->largest_updated) {
		et->largest_updated = false;
		updated = true;
	}

	write_unlock(&et->lock);

	if (updated)
		f2fs_mark_inode_dirty_sync(inode, true);
}

static unsigned int __shrink_extent_tree(struct f2fs_sb_info *sbi,
				int nr_shrink, enum extent_type type)
{
	struct extent_tree *et, *next;
	struct extent_node *en;
	unsigned int node_cnt = 0, tree_cnt = 0;
	int remained;

	if (!atomic_read(&sbi->extent_tree[type].total_zombie_tree))
		goto free_node;

	if (!mutex_trylock(&sbi->extent_tree[type].extent_tree_lock))
		goto out;

	/* 1. remove unreferenced extent tree */
	list_for_each_entry_safe(et, next, &sbi->extent_tree[type].zombie_list, list) {
		if (atomic_read(&et->node_cnt)) {
			write_lock(&et->lock);
			node_cnt += __free_extent_tree(sbi, et);
			write_unlock(&et->lock);
		}
		f2fs_bug_on(sbi, atomic_read(&et->node_cnt));
		list_del_init(&et->list);
		radix_tree_delete(&sbi->extent_tree[type].extent_tree_root, et->ino);
		kmem_cache_free(extent_tree_slab, et);
		atomic_dec(&sbi->extent_tree[type].total_ext_tree);
		atomic_dec(&sbi->extent_tree[type].total_zombie_tree);
		tree_cnt++;

		if (node_cnt + tree_cnt >= nr_shrink)
			goto unlock_out;
		cond_resched();
	}
	mutex_unlock(&sbi->extent_tree[type].extent_tree_lock);

free_node:
	/* 2. remove LRU extent entries */
	if (!mutex_trylock(&sbi->extent_tree[type].extent_tree_lock))
		goto out;

	remained = nr_shrink - (node_cnt + tree_cnt);

	spin_lock(&sbi->extent_tree[type].extent_lock);
	for (; remained > 0; remained--) {
		if (list_empty(&sbi->extent_tree[type].extent_list))
			break;
		en = list_first_entry(&sbi->extent_tree[type].extent_list,
					struct extent_node, list);
		et = en->et;
		if (!write_trylock(&et->lock)) {
			/* refresh this extent node's position in extent list */
			list_move_tail(&en->list, &sbi->extent_tree[type].extent_list);
			continue;
		}

		list_del_init(&en->list);
		spin_unlock(&sbi->extent_tree[type].extent_lock);

		__detach_extent_node(sbi, et, en);

		write_unlock(&et->lock);
		node_cnt++;
		spin_lock(&sbi->extent_tree[type].extent_lock);
	}
	spin_unlock(&sbi->extent_tree[type].extent_lock);

unlock_out:
	mutex_unlock(&sbi->extent_tree[type].extent_tree_lock);
out:
	trace_f2fs_shrink_extent_tree(sbi, node_cnt, tree_cnt);

	return node_cnt + tree_cnt;
}

unsigned int f2fs_shrink_extent_tree(struct f2fs_sb_info *sbi, int nr_shrink)
{
	unsigned int freed;

	freed = __shrink_extent_tree(sbi, nr_shrink, EX_BLOCK_AGE);
	if (freed < nr_shrink)
		freed += __shrink_extent_tree(sbi, nr_shrink - freed, EX_READ);
	return freed;
}

static unsigned int __destroy_extent_node(struct inode *inode,
					 enum extent_type type)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree[type];
	unsigned int node_cnt = 0;

	if (!et || !atomic_read(&et->node_cnt))
		return 0;

	write_lock(&et->lock);
	node_cnt = __free_extent_tree(sbi, et);
	write_unlock(&et->lock);

	return node_cnt;
}

unsigned int f2fs_destroy_extent_node(struct inode *inode)
{
	return __destroy_extent_node(inode, EX_READ) +
		__destroy_extent_node(inode, EX_BLOCK_AGE);
}

void f2fs_drop_extent_tree(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree[EX_READ];
	bool updated = false;

	__destroy_extent_node(inode, EX_BLOCK_AGE);
	if (!et || !f2fs_may_extent_tree(inode))
		return;

	write_lock(&et->lock);
	set_inode_flag(inode, FI_NO_EXTENT);
	__free_extent_tree(sbi, et);
	if (et->largest.len) {
		et->largest.len = 0;
		updated = true;
	}
	write_unlock(&et->lock);
	if (updated)
		f2fs_mark_inode_dirty_sync(inode, true);
}

static void __destroy_extent_tree(struct inode *inode, enum extent_type type)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree[type];
	unsigned int node_cnt = 0;

	if (!et)
		return;

	if (inode->i_nlink && !is_bad_inode(inode) &&
					atomic_read(&et->node_cnt)) {
		mutex_lock(&sbi->extent_tree[type].extent_tree_lock);
		list_add_tail(&et->list, &sbi->extent_tree[type].zombie_list);
		atomic_inc(&sbi->extent_tree[type].total_zombie_tree);
		F2FS_I(inode)->extent_tree[type] = NULL;
		mutex_unlock(&sbi->extent_tree[type].extent_tree_lock);
		return;
	}

	/* free all extent info belong to this extent tree */
	node_cnt = __destroy_extent_node(inode, type);

	/* delete extent tree entry in radix tree */
	mutex_lock(&sbi->extent_tree[type].extent_tree_lock);
	f2fs_bug_on(sbi, atomic_read(&et->node_cnt));
	radix_tree_delete(&sbi->extent_tree[type].extent_tree_root, inode->i_ino);
	kmem_cache_free(extent_tree_slab, et);
	atomic_dec(&sbi->extent_tree[type].total_ext_tree);
	mutex_unlock(&sbi->extent_tree[type].extent_tree_lock);

	F2FS_I(inode)->extent_tree[type] = NULL;

	trace_f2fs_destroy_extent_tree(inode, node_cnt);
}

void f2fs_destroy_extent_tree(struct inode *inode)
{
	__destroy_extent_tree(inode, EX_READ);
	__destroy_extent_tree(inode, EX_BLOCK_AGE);
}

bool f2fs_lookup_extent_cache(struct inode *inode, pgoff_t pgofs,
					struct extent_info *ei)
{
	if (!f2fs_may_extent_tree(inode))
		return false;

	return f2fs_lookup_extent_tree(inode, pgofs, ei);
}

void f2fs_update_extent_cache(struct dnode_of_data *dn)
{
	pgoff_t fofs;
	block_t blkaddr;

	if (!f2fs_may_extent_tree(dn->inode))
		return;

	if (dn->data_blkaddr == NEW_ADDR)
		blkaddr = NULL_ADDR;
	else
		blkaddr = dn->data_blkaddr;

	fofs = f2fs_start_bidx_of_node(ofs_of_node(dn->node_page), dn->inode) +
								dn->ofs_in_node;
	{
		struct extent_info ei = { .fofs = fofs, .blk = blkaddr, .len = 1 };

		f2fs_update_extent_tree_range(dn->inode, &ei, EX_READ, !blkaddr);
	}
}

void f2fs_update_extent_cache_range(struct dnode_of_data *dn,
				pgoff_t fofs, block_t blkaddr, unsigned int len)

{
	if (!f2fs_may_extent_tree(dn->inode))
		return;

	{
		struct extent_info ei = { .fofs = fofs, .blk = blkaddr, .len = len };

		f2fs_update_extent_tree_range(dn->inode, &ei, EX_READ, !blkaddr);
	}
}

/* Age trees contain runtime information only, never the on-disk extent. */
static bool may_age_extent_tree(struct inode *inode)
{
	return test_opt(F2FS_I_SB(inode), AGE_EXTENT_CACHE) &&
		(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)) &&
		!file_is_cold(inode);
}

void f2fs_init_age_extent_tree(struct inode *inode)
{
	if (may_age_extent_tree(inode))
		__grab_extent_tree(inode, EX_BLOCK_AGE);
}

bool f2fs_lookup_age_extent_cache(struct inode *inode, pgoff_t pgofs,
				struct extent_info *ei)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_tree *et = F2FS_I(inode)->extent_tree[EX_BLOCK_AGE];
	struct extent_node *en;
	bool found = false;

	if (!may_age_extent_tree(inode) || !et)
		return false;
	read_lock(&et->lock);
	en = (struct extent_node *)f2fs_lookup_rb_tree(&et->root,
			(struct rb_entry *)et->cached_en, pgofs);
	if (en) {
		*ei = en->ei;
		spin_lock(&sbi->extent_tree[EX_BLOCK_AGE].extent_lock);
		if (!list_empty(&en->list)) {
			list_move_tail(&en->list,
				&sbi->extent_tree[EX_BLOCK_AGE].extent_list);
			et->cached_en = en;
		}
		spin_unlock(&sbi->extent_tree[EX_BLOCK_AGE].extent_lock);
		found = true;
	}
	read_unlock(&et->lock);
	trace_f2fs_lookup_age_extent_tree_end(inode, pgofs, found ? ei : NULL);
	return found;
}

static unsigned long long __calculate_block_age(unsigned long long new,
					unsigned long long old)
{
	unsigned int rem_old, rem_new;
	unsigned long long res;

	/* Divide before multiplying: neither weighted term can overflow. */
	res = div_u64_rem(new, 100, &rem_new) * (100 - LAST_AGE_WEIGHT)
		+ div_u64_rem(old, 100, &rem_old) * LAST_AGE_WEIGHT;
	res += rem_new * (100 - LAST_AGE_WEIGHT) / 100;
	res += rem_old * LAST_AGE_WEIGHT / 100;
	return res;
}

void f2fs_update_age_extent_cache(struct dnode_of_data *dn)
{
	struct inode *inode = dn->inode;
	struct extent_info ei = {}, tei;
	unsigned long long cur_blocks;
	loff_t size = i_size_read(inode);

	if (!may_age_extent_tree(inode) ||
			!F2FS_I(inode)->extent_tree[EX_BLOCK_AGE])
		return;
	ei.fofs = f2fs_start_bidx_of_node(ofs_of_node(dn->node_page), inode) +
			dn->ofs_in_node;
	ei.len = 1;
	/* Do not mistake sequential writes to a partial last page for rewrites. */
	if ((size >> PAGE_SHIFT) == ei.fofs && (size & (PAGE_SIZE - 1)) &&
			dn->data_blkaddr == NEW_ADDR)
		return;
	cur_blocks = atomic64_read(&F2FS_I_SB(inode)->allocated_data_blocks);
	if (f2fs_lookup_age_extent_cache(inode, ei.fofs, &tei)) {
		/* Unsigned subtraction includes the wrap from ULLONG_MAX to zero. */
		unsigned long long age = cur_blocks - tei.last_blocks;

		ei.age = tei.age ? __calculate_block_age(age, tei.age) : age;
	}
	ei.last_blocks = cur_blocks;
	f2fs_update_extent_tree_range(inode, &ei, EX_BLOCK_AGE, false);
}

void f2fs_invalidate_age_extent_cache_range(struct inode *inode,
				pgoff_t fofs, unsigned int len)
{
	struct extent_info ei = { .fofs = fofs, .len = len };

	/* Invalidate even if a runtime cold hint now excludes this inode. */
	f2fs_update_extent_tree_range(inode, &ei, EX_BLOCK_AGE, true);
}

static void __init_extent_cache_info(struct extent_tree_info *eti)
{
	INIT_RADIX_TREE(&eti->extent_tree_root, GFP_NOIO);
	mutex_init(&eti->extent_tree_lock);
	INIT_LIST_HEAD(&eti->extent_list);
	spin_lock_init(&eti->extent_lock);
	atomic_set(&eti->total_ext_tree, 0);
	INIT_LIST_HEAD(&eti->zombie_list);
	atomic_set(&eti->total_zombie_tree, 0);
	atomic_set(&eti->total_ext_node, 0);
}

void f2fs_init_extent_cache_info(struct f2fs_sb_info *sbi)
{
	int type;

	atomic64_set(&sbi->allocated_data_blocks, 0);
	spin_lock_init(&sbi->age_threshold_lock);
	sbi->hot_data_age_threshold = DEF_HOT_DATA_AGE_THRESHOLD;
	sbi->warm_data_age_threshold = DEF_WARM_DATA_AGE_THRESHOLD;
	for (type = 0; type < NR_EXTENT_CACHES; type++)
		__init_extent_cache_info(&sbi->extent_tree[type]);
}

int __init f2fs_create_extent_cache(void)
{
	extent_tree_slab = f2fs_kmem_cache_create("f2fs_extent_tree",
			sizeof(struct extent_tree));
	if (!extent_tree_slab)
		return -ENOMEM;
	extent_node_slab = f2fs_kmem_cache_create("f2fs_extent_node",
			sizeof(struct extent_node));
	if (!extent_node_slab) {
		kmem_cache_destroy(extent_tree_slab);
		return -ENOMEM;
	}
	return 0;
}

void f2fs_destroy_extent_cache(void)
{
	kmem_cache_destroy(extent_node_slab);
	kmem_cache_destroy(extent_tree_slab);
}
