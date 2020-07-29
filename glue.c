/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Hugo Lefeuvre <hugo.lefeuvre@neclab.eu>
 *
 * Copyright (c) 2020, NEC Laboratories Europe GmbH, NEC Corporation,
 *                     All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <uk/mimalloc_impl.h>
#include <uk/mimalloc.h>
#include <mimalloc.h>
#include <mimalloc-internal.h> // _mi_options_init()
#include <uk/alloc_impl.h>
#include <uk/print.h>
#include <uk/allocregion.h>
#include <uk/thread.h>	// uk_thread_current()
#include <uk/page.h>	// round_pgup()
#include <uk/sched.h>	// uk_mimalloc_init_internal()

/* Notes of Unikraft's Mimalloc port:
 *
 * 1. Minimum heap size required: 256MiB, which is the size of an arena.
 *
 * 2. Maximum number of Mimalloc instances:
 *    Mimalloc's current code base relies strongly on static variables. Instead
 *    of heavily patching it (and maintaining the patches) we simply restrict
 *    the maximum number of Mimalloc instances to one.
 *
 * 3. Early boot time allocator:
 *    Mimalloc can only be initialized after pthread. However the early boot
 *    process including pthread's initialization itself requires a functioning
 *    memory allocator. We solve this problem by relying on ukallocregion during
 *    the early boot time. The transition to Mimalloc is triggered as soon as
 *    _tls_ready() returns true. We expect that this won't cause significant
 *    memory leak since memory allocated during EBT is typically not freed.
 *
 * 4. Transition EBT allocator -> Mimalloc:
 *    We transition as soon as the TLS has been allocated and the %fs register
 *    set. This is checked at every EBT allocation by inspecting
 *    uk_thread_current()->prv which typically points to the thread local
 *    storage. Since memory allocations might happen during Mimalloc's
 *    initialization itself (e.g. calls to malloc() by pthread) the early boot
 *    time allocator continues to satisfy requests until Mimalloc is ready
 *    (after mi_process_load() returned).
 */

/* Minimum heap size (size of an arena)
 * TODO: can Mimalloc be reconfigured/patched to lower/remove this limitation?
 */
#define MIN_HEAP_SIZE 268435456L

/* Rely on ukallocregion to satisfy boot-time allocations */
static struct uk_alloc *__region_alloc;

/* make sure that the transition from boot-time allocation to Mimalloc is done
 * only once: calls to malloc() during Mimalloc initialization should be
 * satisfied using the boot-time allocator.
 */
static int __initialized;

static inline int _tls_ready(void)
{
	/* Is the thread local storage ready? */
	struct uk_thread *current = uk_thread_current();

	return current && current->prv != NULL;
}

/* boot-time malloc interface */

static void uk_mimalloc_init_internal(struct uk_alloc *a);

/* NOTE: not static, this is used in the mimalloc code base to request memory
 * "from the OS"
 */
void *uk_mimalloc_region_malloc(struct uk_alloc *a, size_t size)
{
	/* detect call from main thread to leave boot time mode */
	if (_tls_ready() && !__initialized) {
		uk_pr_debug("%s: leaving early boot-time allocation mode\n",
			   uk_thread_current()->name);
		__initialized = 1;
		uk_mimalloc_init_internal(a);

		/* satisfy request using new malloc interface */
		return uk_malloc(a, size);
	}

	uk_pr_debug("allocating %zu from region allocator\n", size);

	return uk_malloc(__region_alloc, size);
}

static int uk_mimalloc_region_posix_memalign(struct uk_alloc *a __unused,
					     void **memptr, size_t align,
					     size_t size)
{
	uk_pr_debug("allocating %zu aligned at %zu from region allocator\n",
			size, align);

	return uk_posix_memalign(__region_alloc, memptr, align, size);
}

static void uk_mimalloc_region_free(struct uk_alloc *a __unused, void *ptr)
{
	uk_pr_info("attempt to free memory during early boot time\n");

	return uk_free(__region_alloc, ptr);
}

/* malloc interface */

static void *uk_mimalloc_malloc(struct uk_alloc *a __unused, size_t size)
{
	return mi_malloc(size);
}

static void uk_mimalloc_free(struct uk_alloc *a __unused, void *ptr)
{
	mi_free(ptr);
}

static void *uk_mimalloc_realloc(struct uk_alloc *a __unused, void *ptr,
				 size_t size)
{
	return mi_realloc(ptr, size);
}

static void *uk_mimalloc_calloc(struct uk_alloc *a __unused, size_t nelem,
				size_t elem_size)
{
	return mi_calloc(nelem, elem_size);
}

static int uk_mimalloc_posix_memalign(struct uk_alloc *a __unused, void **p,
				      size_t alignment, size_t size)
{
	return mi_posix_memalign(p, alignment, size);
}

static void *uk_mimalloc_memalign(struct uk_alloc *a __unused, size_t alignment,
				  size_t size)
{
	return mi_memalign(alignment, size);
}

static void uk_mimalloc_init_internal(struct uk_alloc *a)
{
	mi_process_load();

	/* rebind interface to actual malloc interface */
	(a)->malloc         = uk_mimalloc_malloc;
	(a)->calloc         = uk_mimalloc_calloc;
	(a)->realloc        = uk_mimalloc_realloc;
	(a)->posix_memalign = uk_mimalloc_posix_memalign;
	(a)->memalign       = uk_mimalloc_memalign;
	(a)->free           = uk_mimalloc_free;

	/* delay this after interface rebinding to avoid using early boot
	 * time memory.
	 */
	_mi_options_init();

	uk_pr_info("Successfully initialized Mimalloc\n");
}

struct uk_alloc *uk_mimalloc_init(void *base, size_t len)
{
	struct uk_alloc *a;
	size_t metalen;

	/* TODO: This Mimalloc port does not support multiple memory regions
	 * yet. Because of the multiboot layout, the first region might be a
	 * single page, so we simply ignore it.
	 */
	if (len <= __PAGE_SIZE)
		return NULL;

	if (__region_alloc) {
		uk_pr_err("mimalloc already initialized "
			  "(max number of instances: 1)\n");
		return NULL;
	}

	metalen = round_pgup(sizeof(*a));

	/* enough space for allocator available? */
	if (metalen > len) {
		uk_pr_err("Not enough space for allocator: %" __PRIsz
			  " B required but only %" __PRIuptr" B usable\n",
			  metalen, len);
		return NULL;
	}

	/* enough space to allocate arena? */
	if (len < MIN_HEAP_SIZE) {
		/* Note: we don't exit, but calls to malloc will return NULL. */
		uk_pr_err("Not enough space to allocate arena: %lu bytes "
			  "required but only %" __PRIsz" bytes usable\n",
			  268435456L, len);
	}

	/* store allocator metadata on the heap, just before the memory pool */
	a = (struct uk_alloc *)base;
	uk_pr_info("Initialize mimalloc allocator (early boot time mode) @ 0x%"
		   __PRIuptr ", len %"__PRIsz"\n", (uintptr_t)a, len);

	/* register mimalloc *before* initializing the region allocator: in all
	 * cases we want Mimalloc to be the default allocator.
	 * FIXME: add uk_allocregion_init_noregister() that initializes a region
	 * allocator without registering it.
	 */
	uk_alloc_init_malloc(a, uk_mimalloc_region_malloc, uk_calloc_compat,
				uk_realloc_compat, uk_mimalloc_region_free,
				uk_mimalloc_region_posix_memalign,
				uk_memalign_compat, NULL);

	__region_alloc = uk_allocregion_init((void *)((uintptr_t) base +
						metalen), len - metalen);

	return a;
}
