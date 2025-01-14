/*	$NetBSD: interval_tree.h,v 1.12 2021/12/19 11:00:18 riastradh Exp $	*/

/*-
 * Copyright (c) 2018 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Taylor R. Campbell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * XXX WARNING: This does not actually implement interval trees -- it
 * only implements trees of intervals.  In particular, it does not
 * support finding all intervals that contain a given point, or that
 * intersect with a given interval.  Another way to look at it is that
 * this is an interval tree restricted to nonoverlapping intervals.
 */

#ifndef	_LINUX_INTERVAL_TREE_H_
#define	_LINUX_INTERVAL_TREE_H_

#include <linux/rbtree.h>

struct interval_tree_node {
	struct rb_node	itn_node;
	unsigned long	start;	/* inclusive */
	unsigned long	last;	/* inclusive */
};

static inline int
interval_tree_compare_nodes(void *cookie, const void *va, const void *vb)
{
	const struct interval_tree_node *na = va;
	const struct interval_tree_node *nb = vb;

	if (na->start < nb->start)
		return -1;
	if (na->start > nb->start)
		return +1;
	if (na->last < nb->last)
		return -1;
	if (na->last > nb->last)
		return +1;
	return 0;
}

static inline int
interval_tree_compare_key(void *cookie, const void *vn, const void *vk)
{
	const struct interval_tree_node *n = vn;
	const unsigned long *k = vk;

	if (n->start < *k)
		return -1;
	if (*k < n->start)
		return +1;
	return 0;
}

static const rb_tree_ops_t interval_tree_ops = {
	.rbto_compare_nodes = interval_tree_compare_nodes,
	.rbto_compare_key = interval_tree_compare_key,
	.rbto_node_offset = offsetof(struct interval_tree_node, itn_node),
};

static inline void
interval_tree_init(struct rb_root_cached *root)
{

	rb_tree_init(&root->rb_root.rbr_tree, &interval_tree_ops);
}

static inline void
interval_tree_insert(struct interval_tree_node *node,
    struct rb_root_cached *root)
{
	struct interval_tree_node *collision __diagused;

	collision = rb_tree_insert_node(&root->rb_root.rbr_tree, node);
	KASSERT(collision == node);
}

static inline void
interval_tree_remove(struct interval_tree_node *node,
    struct rb_root_cached *root)
{

	rb_tree_remove_node(&root->rb_root.rbr_tree, node);
}

static inline struct interval_tree_node *
interval_tree_iter_first(struct rb_root_cached *root, unsigned long start,
    unsigned long last)
{
	struct interval_tree_node *node;

	node = rb_tree_find_node_geq(&root->rb_root.rbr_tree, &start);
	if (node == NULL)
		return NULL;
	if (last < node->start)
		return NULL;
	KASSERT(node->start <= last && node->last >= start);

	return node;
}

/*
 * XXX Linux's interval_tree_iter_next doesn't take the root as an
 * argument, which makes this difficult.  So we'll just patch those
 * uses.
 */
static inline struct interval_tree_node *
interval_tree_iter_next(struct rb_root_cached *root,
    struct interval_tree_node *node, unsigned long start, unsigned long last)
{
	struct interval_tree_node *next;

	KASSERT(node != NULL);
	next = rb_tree_iterate(&root->rb_root.rbr_tree, node, RB_DIR_RIGHT);
	if (next == NULL)
		return NULL;
	if (last < next->start)
		return NULL;
	KASSERT(next->start <= last && next->last >= start);

	return next;
}

#endif	/* _LINUX_INTERVAL_TREE_H_ */
