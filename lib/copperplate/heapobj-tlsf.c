/*
 * Copyright (C) 2008 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include "copperplate/wrappers.h"
#include "copperplate/init.h"
#include "copperplate/heapobj.h"
#include "copperplate/debug.h"
#include "tlsf/tlsf.h"

/* XXX: depends on the implementation included from tlsf/, YMMV. */
#define TLSF_BLOCK_OVERHEAD  8

static int tlsf_pool_overhead;

void __heapobj_destroy(struct heapobj *hobj)
{
	destroy_memory_pool(hobj->pool);
	__RT(pthread_mutex_destroy(&hobj->lock));
}

int __heapobj_extend(struct heapobj *hobj, size_t size, void *mem)
{
	__RT(pthread_mutex_lock(&hobj->lock));
	hobj->size = add_new_area(hobj->pool, size, mem);
	__RT(pthread_mutex_unlock(&hobj->lock));
	if (hobj->size == (size_t)-1)
		return __bt(-EINVAL);

	return 0;
}

void *__heapobj_alloc(struct heapobj *hobj, size_t size)
{
	void *p;

	__RT(pthread_mutex_lock(&hobj->lock));
	p = malloc_ex(size, hobj->pool);
	__RT(pthread_mutex_unlock(&hobj->lock));

	return p;
}

void __heapobj_free(struct heapobj *hobj, void *ptr)
{
	__RT(pthread_mutex_lock(&hobj->lock));
	free_ex(ptr, hobj->pool);
	__RT(pthread_mutex_unlock(&hobj->lock));
}

size_t __heapobj_inquire(struct heapobj *hobj, void *ptr)
{
	size_t size;

	__RT(pthread_mutex_lock(&hobj->lock));
	size = malloc_usable_size_ex(ptr, hobj->pool);
	__RT(pthread_mutex_unlock(&hobj->lock));

	return size;
}

#ifdef CONFIG_XENO_PSHARED

static struct heapobj_ops tlsf_ops = {
	.destroy = __heapobj_destroy,
	.extend = __heapobj_extend,
	.alloc = __heapobj_alloc,
	.free = __heapobj_free,
	.inquire = __heapobj_inquire,
};

#endif

int heapobj_init_private(struct heapobj *hobj, const char *name,
			 size_t size, void *mem)
{
	if (mem == NULL) {
		/*
		 * When the memory area is unspecified, obtain it from
		 * the main pool, accounting for the TLSF overhead.
		 */
		size += tlsf_pool_overhead;
		mem = tlsf_malloc(size);
		if (mem == NULL)
			return __bt(-ENOMEM);
	}

	if (name)
		snprintf(hobj->name, sizeof(hobj->name), "%s", name);
	else
		snprintf(hobj->name, sizeof(hobj->name), "%p", hobj);
#ifdef CONFIG_XENO_PSHARED
	hobj->ops = &tlsf_ops;
#endif
	hobj->pool = mem;
	/* Make sure to wipe out tlsf's signature. */
	memset(mem, 0, size);
	hobj->size = init_memory_pool(size, mem);
	if (hobj->size == (size_t)-1)
		return __bt(-EINVAL);

	/*
	 * TLSF does not lock around so-called extended calls aimed at
	 * specific pools, which is definitely braindamage. So DIY.
	 */
	__RT(pthread_mutex_init(&hobj->lock, NULL));

	return 0;
}

int heapobj_init_array_private(struct heapobj *hobj, const char *name,
			       size_t size, int elems)
{
	size += TLSF_BLOCK_OVERHEAD;
	if (size < 16)
		size = 16;

	return __bt(heapobj_init_private(hobj, name, size * elems, NULL));
}

void *pvmalloc(size_t size)
{
	return tlsf_malloc(size);
}

void pvfree(void *ptr)
{
	tlsf_free(ptr);
}

char *pvstrdup(const char *ptr)
{
	char *str;

	str = pvmalloc(strlen(ptr) + 1);
	if (str == NULL)
		return NULL;

	return strcpy(str, ptr);
}

int heapobj_pkg_init_private(void)
{
	size_t size;
	void *mem;

	/*
	 * We want to know how many bytes from a memory pool TLSF will
	 * use for its own internal use. We get the probe memory from
	 * tlsf_malloc(), so that the main pool will be set up in the
	 * same move.
	 */
	mem = tlsf_malloc(__this_node.mem_pool);
	size = init_memory_pool(__this_node.mem_pool, mem);
	if (size == (size_t)-1)
		panic("cannot initialize TLSF memory manager");

	destroy_memory_pool(mem);
	tlsf_pool_overhead = __this_node.mem_pool - size;
	tlsf_pool_overhead = (tlsf_pool_overhead + 15) & ~15;
	tlsf_free(mem);

	return 0;
}
