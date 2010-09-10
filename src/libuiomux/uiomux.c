/*
 * UIOMux: a conflict manager for system resources, including UIO devices.
 * Copyright (C) 2009 Renesas Technology Corp.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include "uiomux/uiomux.h"
#include "uiomux_private.h"
#include "uio.h"

/* #define DEBUG */

static int init_done = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t uio_mutex[UIOMUX_BLOCK_MAX];

struct uiomux *uiomux_open(void)
{
	struct uiomux *uiomux;
	const char *name = NULL;
	int i;

	pthread_mutex_lock(&mutex);
	if (!init_done) {
		pthread_mutex_t temp_mutex = PTHREAD_MUTEX_INITIALIZER;

		for (i = 0; i < UIOMUX_BLOCK_MAX; i++)
			memcpy(&uio_mutex[i], &temp_mutex, sizeof(temp_mutex));

		init_done = 1;
	}
	pthread_mutex_unlock(&mutex);

	uiomux = (struct uiomux *) calloc(1, sizeof(*uiomux));
	if (!uiomux)
		return NULL;

	/* Open handles to all hardware blocks */
	for (i = 0; i < UIOMUX_BLOCK_MAX; i++) {
		if ((name = uiomux_name(1 << i)) != NULL) {
			uiomux->uios[i] = uio_open(name);
		}
	}

	return uiomux;
}

static void uiomux_delete(struct uiomux *uiomux)
{
	struct uio *uio;
	int i;

	for (i = 0; i < UIOMUX_BLOCK_MAX; i++) {
		uio = uiomux->uios[i];
		if (uio != NULL) {
			uio_close(uio);
		}
	}

	free(uiomux);
}

int uiomux_close(struct uiomux *uiomux)
{
	if (uiomux == NULL)
		return -1;

#ifdef DEBUG
	fprintf(stderr, "%s: IN\n", __func__);
#endif

	uiomux_delete(uiomux);

	return 0;
}

int uiomux_system_reset(struct uiomux *uiomux)
{
	/* Locks by other tasks cannot be invalidated. */
	return -1;
}

int uiomux_system_destroy(struct uiomux *uiomux)
{
	uiomux_delete(uiomux);

	return 0;
}

int uiomux_lock(struct uiomux *uiomux, uiomux_resource_t blockmask)
{
	unsigned long *reg_base;
	int i, k, ret = 0;
	struct uio *uio;

	for (i = 0; i < UIOMUX_BLOCK_MAX; i++) {
		if (blockmask & (1 << i)) {
			uio = uiomux->uios[i];
			if (!uio) {
				fprintf(stderr, "No uio exists.\n");
				goto undo_locks;
			}

			/* Lock uio within this process. This is required because the
			   fcntl()'s advisory lock is only valid between processes, not
			   within a process. */
			ret = pthread_mutex_lock(&uio_mutex[i]);
			if (ret != 0) {
				perror("pthread_mutex_lock failed");
				goto undo_locks;
			}

			ret = flock(uio->dev.fd, LOCK_EX);
			if (ret < 0) {
				perror("flock failed");
				goto undo_locks;
			}
			uiomux->locked_resources |= 1U << i;
			uio_read_nonblocking(uio);
		}
	}

	return 0;

undo_locks:
	{
		int save_errno = errno;
		uiomux_unlock(uiomux, uiomux->locked_resources);
		errno = save_errno;
	}

	return ret;
}

int uiomux_unlock(struct uiomux *uiomux, uiomux_resource_t blockmask)
{
	unsigned long *reg_base;
	int i, k, ret;
	struct uio *uio;

	for (i = UIOMUX_BLOCK_MAX - 1; i >= 0; i--) {
		if (blockmask & (1 << i)) {
			uio = uiomux->uios[i];
			if (uio) {
				ret = flock(uio->dev.fd, LOCK_UN);
				if (ret < 0)
					perror("flock failed");

				ret = pthread_mutex_unlock(&uio_mutex[i]);
				if (ret != 0)
					perror("pthread_mutex_unlock failed");
			}
		}
	}

	return 0;
}

#define MULTI_BIT(x) (((long)x)&(((long)x)-1))

static int
uiomux_get_block_index(struct uiomux *uiomux, uiomux_resource_t blockmask)
{
	int i;

	/* Invalid if multiple bits are set */
	if (MULTI_BIT(blockmask)) {
#ifdef DEBUG
		fprintf(stderr, "%s: Multiple blocks specified\n", __func__);
#endif
		return -1;
	}

	for (i = 0; i < UIOMUX_BLOCK_MAX; i++) {
		if (blockmask & (1 << i)) {
			return i;
		}
	}

	return -1;
}

int uiomux_sleep(struct uiomux *uiomux, uiomux_resource_t blockmask)
{
	struct uio *uio;
	int ret = 0;
	int i;

	/* Invalid if multiple bits are set, or block not found */
	if ((i = uiomux_get_block_index(uiomux, blockmask)) == -1)
		return -1;

	uio = uiomux->uios[i];

	if (uio) {
#ifdef DEBUG
		fprintf(stderr, "%s: Waiting for block %d\n", __func__, i);
#endif
		ret = uio_sleep(uio, NULL);
	}

	return ret;
}

int uiomux_wakeup(struct uiomux *uiomux, uiomux_resource_t blockmask)
{
	struct uio *uio;
	int ret = 0;
	int i;

	/* Invalid if multiple bits are set, or block not found */
	if ((i = uiomux_get_block_index(uiomux, blockmask)) == -1)
		return -1;

	uio = uiomux->uios[i];

	if (uio) {
#ifdef DEBUG
		fprintf(stderr, "%s: Waking up block %d\n", __func__, i);
#endif
		write (uio->exit_sleep_pipe[1], "UIOX", 4);
	}
	return 0;
}

void *uiomux_malloc(struct uiomux *uiomux, uiomux_resource_t blockmask,
		    size_t size, int align)
{
	struct uio *uio;
	void *ret = NULL;
	int i;

	/* Invalid if multiple bits are set, or block not found */
	if ((i = uiomux_get_block_index(uiomux, blockmask)) == -1)
		return NULL;

	uio = uiomux->uios[i];

	if (uio) {
#ifdef DEBUG
		fprintf(stderr, "%s: Allocating %d bytes for block %d\n",
			__func__, size, i);
#endif
		ret = uio_malloc(uio, i, size, align, 0);
	}

	return ret;
}

void *uiomux_malloc_shared(struct uiomux *uiomux, uiomux_resource_t blockmask,
		    size_t size, int align)
{
	struct uio *uio;
	void *ret = NULL;
	int i;

	/* Invalid if multiple bits are set, or block not found */
	if ((i = uiomux_get_block_index(uiomux, blockmask)) == -1)
		return NULL;

	uio = uiomux->uios[i];

	if (uio) {
#ifdef DEBUG
		fprintf(stderr, "%s: Allocating %d bytes shm for block %d\n",
			__func__, size, i);
#endif
		ret = uio_malloc(uio, i, size, align, 1);
	}

	return ret;
}

void
uiomux_free(struct uiomux *uiomux, uiomux_resource_t blockmask,
	    void *address, size_t size)
{
	struct uio *uio;
	int i;

	/* Invalid if multiple bits are set, or block not found */
	if ((i = uiomux_get_block_index(uiomux, blockmask)) == -1)
		return;

	uio = uiomux->uios[i];

	if (uio) {
#ifdef DEBUG
		fprintf(stderr, "%s: Freeing memory for block %d\n",
			__func__, i);
#endif
		uio_free(uio, i, address, size);
	}
}

unsigned long
uiomux_get_mmio(struct uiomux *uiomux, uiomux_resource_t blockmask,
		unsigned long *address, unsigned long *size, void **iomem)
{
	struct uio *uio;
	int i;

	/* Invalid if multiple bits are set, or block not found */
	if ((i = uiomux_get_block_index(uiomux, blockmask)) == -1)
		return 0;

	uio = uiomux->uios[i];

	/* Invalid if no uio associated with it */
	if (uio == NULL)
		return 0;

	if (address)
		*address = uio->mmio.address;
	if (size)
		*size = uio->mmio.size;
	if (iomem)
		*iomem = uio->mmio.iomem;

	return uio->mmio.address;
}

unsigned long
uiomux_get_mem(struct uiomux *uiomux, uiomux_resource_t blockmask,
	       unsigned long *address, unsigned long *size, void **iomem)
{
	struct uio *uio;
	int i;

	/* Invalid if multiple bits are set, or block not found */
	if ((i = uiomux_get_block_index(uiomux, blockmask)) == -1)
		return 0;

	uio = uiomux->uios[i];

	/* Invalid if no uio associated with it */
	if (uio == NULL)
		return 0;

	if (address)
		*address = uio->mem.address;
	if (size)
		*size = uio->mem.size;
	if (iomem)
		*iomem = uio->mem.iomem;

	return uio->mem.address;
}

static unsigned long
uio_map_virt_to_phys(struct uio_map *map, void *virt_address)
{
	if ((virt_address >= map->iomem)
	    && (((unsigned long)virt_address -
		 (unsigned long)map->iomem) < map->size))
		return map->address + ((unsigned long)virt_address -
				       (unsigned long)map->iomem);

	return (unsigned long) -1;
}

static void *
uio_map_phys_to_virt(struct uio_map *map, unsigned long phys_address)
{
	if ((phys_address >= map->address)
	    && ((phys_address - map->address) < map->size))
		return (void *)((unsigned long)map->iomem +
				(phys_address - map->address));

	return NULL;
}

unsigned long
uiomux_virt_to_phys(struct uiomux *uiomux, uiomux_resource_t blockmask,
		    void *virt_address)
{
	struct uio *uio;
	unsigned long ret;
	int i;

	/* Invalid if multiple bits are set, or block not found */
	if ((i = uiomux_get_block_index(uiomux, blockmask)) == -1)
		return 0;

	uio = uiomux->uios[i];

	/* Invalid if no uio associated with it */
	if (uio == NULL)
		return 0;

	if ((ret =
	     uio_map_virt_to_phys(&uio->mem,
				  virt_address)) != (unsigned long) -1)
		return ret;

	if ((ret =
	     uio_map_virt_to_phys(&uio->mmio,
				  virt_address)) != (unsigned long) -1)
		return ret;

	return 0;
}

void *
uiomux_phys_to_virt(struct uiomux *uiomux, uiomux_resource_t blockmask,
		    unsigned long phys_address)
{
	struct uio *uio;
	void * ret;
	int i;

	/* Invalid if multiple bits are set, or block not found */
	if ((i = uiomux_get_block_index(uiomux, blockmask)) == -1)
		return NULL;

	uio = uiomux->uios[i];

	/* Invalid if no uio associated with it */
	if (uio == NULL)
		return NULL;

	if ((ret =
	     uio_map_phys_to_virt(&uio->mem,
				  phys_address)) != NULL)
		return ret;

	if ((ret =
	     uio_map_phys_to_virt(&uio->mmio,
				  phys_address)) != NULL)
		return ret;

	return NULL;
}

const char *uiomux_name(uiomux_resource_t block)
{
	switch (block) {
	case UIOMUX_SH_BEU:
		return "BEU";
		break;
	case UIOMUX_SH_CEU:
		return "CEU";
		break;
	case UIOMUX_SH_JPU:
		return "JPU";
		break;
	case UIOMUX_SH_VEU:
		return "VEU";
		break;
	case UIOMUX_SH_VPU:
		return "VPU5";
		break;
	default:
		return NULL;
	}
}

uiomux_resource_t uiomux_query(void)
{
	uiomux_resource_t blocks = UIOMUX_NONE;
	struct uio *uio;
	const char *name = NULL;
	int i;

	for (i = 0; i < UIOMUX_BLOCK_MAX; i++) {
		if ((name = uiomux_name(1 << i)) != NULL) {
			if ((uio = uio_open(name)) != NULL) {
				blocks |= (1 << i);
				uio_close(uio);
			}
		}
	}

	return blocks;
}

static int uiomux_showversion(struct uiomux *uiomux)
{
	printf("uiomux\n" VERSION);
	fflush(stdout);

	return 0;
}

int uiomux_info(struct uiomux *uiomux)
{
	uiomux_resource_t blocks = UIOMUX_NONE;
	struct uio *uio;
	int i;
	long pagesize;

	uiomux_showversion(uiomux);

	pagesize = sysconf(_SC_PAGESIZE);

	for (i = 0; i < UIOMUX_BLOCK_MAX; i++) {
		uio = uiomux->uios[i];
		if (uio != NULL) {
			printf("%s: %s", uio->dev.path,
			       uio->dev.name);
			printf
			    ("\tmmio\t0x%8lx\t0x%8lx bytes (%ld pages)\n\tmem\t0x%8lx\t0x%8lx bytes (%ld pages)\n",
			     uio->mmio.address,
			     uio->mmio.size,
			     (uio->mmio.size + pagesize - 1)/ pagesize,
			     uio->mem.address, uio->mem.size,
			     (uio->mem.size + pagesize - 1)/ pagesize);
		}
	}

	return 0;
}

int uiomux_meminfo(struct uiomux *uiomux)
{
	uiomux_resource_t blocks = UIOMUX_NONE;
	struct uio *uio;
	int i;
	long pagesize;

	uiomux_showversion(uiomux);

	pagesize = sysconf(_SC_PAGESIZE);

	for (i = 0; i < UIOMUX_BLOCK_MAX; i++) {
		uio = uiomux->uios[i];
		if (uio != NULL) {
			printf("%s: %s", uio->dev.path,
			       uio->dev.name);
			uio_meminfo(uio);
		}
	}

	return 0;
}
