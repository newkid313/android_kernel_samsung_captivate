/*
 * direct.c - NILFS direct block pointer.
 *
 * Copyright (C) 2006-2008 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Written by Koji Sato <koji@osrg.net>.
 */

#include <linux/errno.h>
#include "nilfs.h"
#include "page.h"
#include "direct.h"
#include "alloc.h"
#include "dat.h"

static inline __le64 *nilfs_direct_dptrs(const struct nilfs_direct *direct)
{
	return (__le64 *)
		((struct nilfs_direct_node *)direct->d_bmap.b_u.u_data + 1);
}

static inline __u64
nilfs_direct_get_ptr(const struct nilfs_direct *direct, __u64 key)
{
	return nilfs_bmap_dptr_to_ptr(*(nilfs_direct_dptrs(direct) + key));
}

static inline void nilfs_direct_set_ptr(struct nilfs_direct *direct,
					__u64 key, __u64 ptr)
{
	*(nilfs_direct_dptrs(direct) + key) = nilfs_bmap_ptr_to_dptr(ptr);
}

static int nilfs_direct_lookup(const struct nilfs_bmap *bmap,
			       __u64 key, int level, __u64 *ptrp)
{
	struct nilfs_direct *direct;
	__u64 ptr;

	direct = (struct nilfs_direct *)bmap;  /* XXX: use macro for level 1 */
	if (key > NILFS_DIRECT_KEY_MAX || level != 1)
		return -ENOENT;
	ptr = nilfs_direct_get_ptr(direct, key);
	if (ptr == NILFS_BMAP_INVALID_PTR)
		return -ENOENT;

	if (ptrp != NULL)
		*ptrp = ptr;
	return 0;
}

static int nilfs_direct_lookup_contig(const struct nilfs_bmap *bmap,
				      __u64 key, __u64 *ptrp,
				      unsigned maxblocks)
{
	struct nilfs_direct *direct = (struct nilfs_direct *)bmap;
	struct inode *dat = NULL;
	__u64 ptr, ptr2;
	sector_t blocknr;
	int ret, cnt;

	if (key > NILFS_DIRECT_KEY_MAX)
		return -ENOENT;
	ptr = nilfs_direct_get_ptr(direct, key);
	if (ptr == NILFS_BMAP_INVALID_PTR)
		return -ENOENT;

	if (NILFS_BMAP_USE_VBN(bmap)) {
		dat = nilfs_bmap_get_dat(bmap);
		ret = nilfs_dat_translate(dat, ptr, &blocknr);
		if (ret < 0)
			return ret;
		ptr = blocknr;
	}

	maxblocks = min_t(unsigned, maxblocks, NILFS_DIRECT_KEY_MAX - key + 1);
	for (cnt = 1; cnt < maxblocks &&
		     (ptr2 = nilfs_direct_get_ptr(direct, key + cnt)) !=
		     NILFS_BMAP_INVALID_PTR;
	     cnt++) {
		if (dat) {
			ret = nilfs_dat_translate(dat, ptr2, &blocknr);
			if (ret < 0)
				return ret;
			ptr2 = blocknr;
		}
		if (ptr2 != ptr + cnt)
			break;
	}
	*ptrp = ptr;
	return cnt;
}

static __u64
nilfs_direct_find_target_v(const struct nilfs_direct *direct, __u64 key)
{
	__u64 ptr;

	ptr = nilfs_bmap_find_target_seq(&direct->d_bmap, key);
	if (ptr != NILFS_BMAP_INVALID_PTR)
		/* sequential access */
		return ptr;
	else
		/* block group */
		return nilfs_bmap_find_target_in_group(&direct->d_bmap);
}

static void nilfs_direct_set_target_v(struct nilfs_direct *direct,
				      __u64 key, __u64 ptr)
{
	direct->d_bmap.b_last_allocated_key = key;
	direct->d_bmap.b_last_allocated_ptr = ptr;
}

static int nilfs_direct_prepare_insert(struct nilfs_direct *direct,
				       __u64 key,
				       union nilfs_bmap_ptr_req *req,
				       struct nilfs_bmap_stats *stats)
{
	int ret;

	if (NILFS_BMAP_USE_VBN(&direct->d_bmap))
		req->bpr_ptr = nilfs_direct_find_target_v(direct, key);
	ret = nilfs_bmap_prepare_alloc_ptr(&direct->d_bmap, req);
	if (ret < 0)
		return ret;

	stats->bs_nblocks = 1;
	return 0;
}

static void nilfs_direct_commit_insert(struct nilfs_direct *direct,
				       union nilfs_bmap_ptr_req *req,
				       __u64 key, __u64 ptr)
{
	struct buffer_head *bh;

	/* ptr must be a pointer to a buffer head. */
	bh = (struct buffer_head *)((unsigned long)ptr);
	set_buffer_nilfs_volatile(bh);

	nilfs_bmap_commit_alloc_ptr(&direct->d_bmap, req);
	nilfs_direct_set_ptr(direct, key, req->bpr_ptr);

	if (!nilfs_bmap_dirty(&direct->d_bmap))
		nilfs_bmap_set_dirty(&direct->d_bmap);

	if (NILFS_BMAP_USE_VBN(&direct->d_bmap))
		nilfs_direct_set_target_v(direct, key, req->bpr_ptr);
}

static int nilfs_direct_insert(struct nilfs_bmap *bmap, __u64 key, __u64 ptr)
{
	struct nilfs_direct *direct;
	union nilfs_bmap_ptr_req req;
	struct nilfs_bmap_stats stats;
	int ret;

	direct = (struct nilfs_direct *)bmap;
	if (key > NILFS_DIRECT_KEY_MAX)
		return -ENOENT;
	if (nilfs_direct_get_ptr(direct, key) != NILFS_BMAP_INVALID_PTR)
		return -EEXIST;

	ret = nilfs_direct_prepare_insert(direct, key, &req, &stats);
	if (ret < 0)
		return ret;
	nilfs_direct_commit_insert(direct, &req, key, ptr);
	nilfs_bmap_add_blocks(bmap, stats.bs_nblocks);

	return 0;
}

static int nilfs_direct_prepare_delete(struct nilfs_direct *direct,
				       union nilfs_bmap_ptr_req *req,
				       __u64 key,
				       struct nilfs_bmap_stats *stats)
{
	int ret;

	req->bpr_ptr = nilfs_direct_get_ptr(direct, key);
	ret = nilfs_bmap_prepare_end_ptr(&direct->d_bmap, req);
	if (!ret)
		stats->bs_nblocks = 1;
	return ret;
}

static void nilfs_direct_commit_delete(struct nilfs_direct *direct,
				       union nilfs_bmap_ptr_req *req,
				       __u64 key)
{
	nilfs_bmap_commit_end_ptr(&direct->d_bmap, req);
	nilfs_direct_set_ptr(direct, key, NILFS_BMAP_INVALID_PTR);
}

static int nilfs_direct_delete(struct nilfs_bmap *bmap, __u64 key)
{
	struct nilfs_direct *direct;
	union nilfs_bmap_ptr_req req;
	struct nilfs_bmap_stats stats;
	int ret;

	direct = (struct nilfs_direct *)bmap;
	if ((key > NILFS_DIRECT_KEY_MAX) ||
	    nilfs_direct_get_ptr(direct, key) == NILFS_BMAP_INVALID_PTR)
		return -ENOENT;

	ret = nilfs_direct_prepare_delete(direct, &req, key, &stats);
	if (ret < 0)
		return ret;
	nilfs_direct_commit_delete(direct, &req, key);
	nilfs_bmap_sub_blocks(bmap, stats.bs_nblocks);

	return 0;
}

static int nilfs_direct_last_key(const struct nilfs_bmap *bmap, __u64 *keyp)
{
	struct nilfs_direct *direct;
	__u64 key, lastkey;

	direct = (struct nilfs_direct *)bmap;
	lastkey = NILFS_DIRECT_KEY_MAX + 1;
	for (key = NILFS_DIRECT_KEY_MIN; key <= NILFS_DIRECT_KEY_MAX; key++)
		if (nilfs_direct_get_ptr(direct, key) !=
		    NILFS_BMAP_INVALID_PTR)
			lastkey = key;

	if (lastkey == NILFS_DIRECT_KEY_MAX + 1)
		return -ENOENT;

	*keyp = lastkey;

	return 0;
}

static int nilfs_direct_check_insert(const struct nilfs_bmap *bmap, __u64 key)
{
	return key > NILFS_DIRECT_KEY_MAX;
}

static int nilfs_direct_gather_data(struct nilfs_bmap *bmap,
				    __u64 *keys, __u64 *ptrs, int nitems)
{
	struct nilfs_direct *direct;
	__u64 key;
	__u64 ptr;
	int n;

	direct = (struct nilfs_direct *)bmap;
	if (nitems > NILFS_DIRECT_NBLOCKS)
		nitems = NILFS_DIRECT_NBLOCKS;
	n = 0;
	for (key = 0; key < nitems; key++) {
		ptr = nilfs_direct_get_ptr(direct, key);
		if (ptr != NILFS_BMAP_INVALID_PTR) {
			keys[n] = key;
			ptrs[n] = ptr;
			n++;
		}
	}
	return n;
}

int nilfs_direct_delete_and_convert(struct nilfs_bmap *bmap,
				    __u64 key, __u64 *keys, __u64 *ptrs, int n)
{
	struct nilfs_direct *direct;
	__le64 *dptrs;
	int ret, i, j;

	/* no need to allocate any resource for conversion */

	/* delete */
	ret = bmap->b_ops->bop_delete(bmap, key);
	if (ret < 0)
		return ret;

	/* free resources */
	if (bmap->b_ops->bop_clear != NULL)
		bmap->b_ops->bop_clear(bmap);

	/* convert */
	direct = (struct nilfs_direct *)bmap;
	dptrs = nilfs_direct_dptrs(direct);
	for (i = 0, j = 0; i < NILFS_DIRECT_NBLOCKS; i++) {
		if ((j < n) && (i == keys[j])) {
			dptrs[i] = (i != key) ?
				nilfs_bmap_ptr_to_dptr(ptrs[j]) :
				NILFS_BMAP_INVALID_PTR;
			j++;
		} else
			dptrs[i] = NILFS_BMAP_INVALID_PTR;
	}

	nilfs_direct_init(bmap);
	return 0;
}

static int nilfs_direct_propagate_v(struct nilfs_direct *direct,
				    struct buffer_head *bh)
{
	union nilfs_bmap_ptr_req oldreq, newreq;
	__u64 key;
	__u64 ptr;
	int ret;

	key = nilfs_bmap_data_get_key(&direct->d_bmap, bh);
	ptr = nilfs_direct_get_ptr(direct, key);
	if (!buffer_nilfs_volatile(bh)) {
		oldreq.bpr_ptr = ptr;
		newreq.bpr_ptr = ptr;
		ret = nilfs_bmap_prepare_update_v(&direct->d_bmap, &oldreq,
						  &newreq);
		if (ret < 0)
			return ret;
		nilfs_bmap_commit_update_v(&direct->d_bmap, &oldreq, &newreq);
		set_buffer_nilfs_volatile(bh);
		nilfs_direct_set_ptr(direct, key, newreq.bpr_ptr);
	} else
		ret = nilfs_bmap_mark_dirty(&direct->d_bmap, ptr);

	return ret;
}

static int nilfs_direct_propagate(const struct nilfs_bmap *bmap,
				  struct buffer_head *bh)
{
	struct nilfs_direct *direct = (struct nilfs_direct *)bmap;

	return NILFS_BMAP_USE_VBN(bmap) ?
		nilfs_direct_propagate_v(direct, bh) : 0;
}

static int nilfs_direct_assign_v(struct nilfs_direct *direct,
				 __u64 key, __u64 ptr,
				 struct buffer_head **bh,
				 sector_t blocknr,
				 union nilfs_binfo *binfo)
{
	union nilfs_bmap_ptr_req req;
	int ret;

	req.bpr_ptr = ptr;
	ret = nilfs_bmap_start_v(&direct->d_bmap, &req, blocknr);
	if (unlikely(ret < 0))
		return ret;

	binfo->bi_v.bi_vblocknr = nilfs_bmap_ptr_to_dptr(ptr);
	binfo->bi_v.bi_blkoff = nilfs_bmap_key_to_dkey(key);

	return 0;
}

static int nilfs_direct_assign_p(struct nilfs_direct *direct,
				 __u64 key, __u64 ptr,
				 struct buffer_head **bh,
				 sector_t blocknr,
				 union nilfs_binfo *binfo)
{
	nilfs_direct_set_ptr(direct, key, blocknr);

	binfo->bi_dat.bi_blkoff = nilfs_bmap_key_to_dkey(key);
	binfo->bi_dat.bi_level = 0;

	return 0;
}

static int nilfs_direct_assign(struct nilfs_bmap *bmap,
			       struct buffer_head **bh,
			       sector_t blocknr,
			       union nilfs_binfo *binfo)
{
	struct nilfs_direct *direct;
	__u64 key;
	__u64 ptr;

	direct = (struct nilfs_direct *)bmap;
	key = nilfs_bmap_data_get_key(bmap, *bh);
	if (unlikely(key > NILFS_DIRECT_KEY_MAX)) {
		printk(KERN_CRIT "%s: invalid key: %llu\n", __func__,
		       (unsigned long long)key);
		return -EINVAL;
	}
	ptr = nilfs_direct_get_ptr(direct, key);
	if (unlikely(ptr == NILFS_BMAP_INVALID_PTR)) {
		printk(KERN_CRIT "%s: invalid pointer: %llu\n", __func__,
		       (unsigned long long)ptr);
		return -EINVAL;
	}

	return NILFS_BMAP_USE_VBN(bmap) ?
		nilfs_direct_assign_v(direct, key, ptr, bh, blocknr, binfo) :
		nilfs_direct_assign_p(direct, key, ptr, bh, blocknr, binfo);
}

#ifdef CONFIG_NILFS_BMAP_DEBUG
static int nilfs_direct_verify(const struct nilfs_bmap *bmap)
{
	return 0;
}

static int nilfs_direct_print(const struct nilfs_bmap *bmap)
{
	const struct nilfs_direct *direct;
	__u64 i;

	direct = (const struct nilfs_direct *)bmap;
	printk(KERN_DEBUG "direct is %s\n",
	       nilfs_bmap_dirty(bmap) ? "dirty" : "clean");
	printk(KERN_DEBUG "flags = %d\n", bmap->b_u.u_flags);
	for (i = 0; i < NILFS_DIRECT_NBLOCKS; i++) {
		printk(KERN_DEBUG "key = %llu ptr = %llu\n",
		       (unsigned long long)i,
		       (unsigned long long)nilfs_direct_get_ptr(direct, i));
	}

	return 0;
}
#endif	/* CONFIG_NILFS_BMAP_DEBUG */

static const struct nilfs_bmap_operations nilfs_direct_ops = {
	.bop_lookup		=	nilfs_direct_lookup,
	.bop_lookup_contig	=	nilfs_direct_lookup_contig,
	.bop_insert		=	nilfs_direct_insert,
	.bop_delete		=	nilfs_direct_delete,
	.bop_clear		=	NULL,

	.bop_propagate		=	nilfs_direct_propagate,

	.bop_lookup_dirty_buffers	=	NULL,

	.bop_assign		=	nilfs_direct_assign,
	.bop_mark		=	NULL,

	.bop_last_key		=	nilfs_direct_last_key,
	.bop_check_insert	=	nilfs_direct_check_insert,
	.bop_check_delete	=	NULL,
	.bop_gather_data	=	nilfs_direct_gather_data,

#ifdef CONFIG_NILFS_BMAP_DEBUG
	.bop_verify		=	nilfs_direct_verify,
	.bop_print		=	nilfs_direct_print,
#endif	/* CONFIG_NILFS_BMAP_DEBUG */
};


int nilfs_direct_init(struct nilfs_bmap *bmap)
{
	bmap->b_ops = &nilfs_direct_ops;
	return 0;
}
