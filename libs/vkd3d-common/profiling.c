/*
 * Copyright 2020 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#include "vkd3d_profiling.h"
#include "vkd3d_threads.h"
#include "vkd3d_debug.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static pthread_once_t profiling_block_once = PTHREAD_ONCE_INIT;
static int profiling_fd = -1;
static unsigned int profiling_region_count;
static spinlock_t profiling_lock;

struct vkd3d_profiling_block
{
    uint64_t ticks_total;
    uint64_t iteration_total;
    char name[64 - 2 * sizeof(uint64_t)];
};

static struct vkd3d_profiling_block *mapped_blocks;

#define VKD3D_MAX_PROFILING_REGIONS 1024

static void vkd3d_init_profiling_once(void)
{
    char path_pid[PATH_MAX];
    const char *path = getenv("VKD3D_PROFILE_PATH");

    if (path)
    {
        snprintf(path_pid, sizeof(path_pid), "%s.%u", path, getpid());
        profiling_fd = open(path_pid, O_RDWR | O_CREAT, 0644);
        if (profiling_fd >= 0)
        {
            if (ftruncate(profiling_fd, VKD3D_MAX_PROFILING_REGIONS * sizeof(*mapped_blocks)) < 0)
            {
                ERR("Failed to resize profiling FD.\n");
                return;
            }
            mapped_blocks = mmap(NULL, VKD3D_MAX_PROFILING_REGIONS * sizeof(*mapped_blocks),
                  PROT_READ | PROT_WRITE, MAP_SHARED, profiling_fd, 0);
            if (!mapped_blocks)
            {
                ERR("Failed to map block.\n");
                return;
            }
            memset(mapped_blocks, 0, VKD3D_MAX_PROFILING_REGIONS * sizeof(*mapped_blocks));
        }
        else
        {
            ERR("Failed to open profiling FD.\n");
        }
    }
}

void vkd3d_init_profiling(void)
{
    pthread_once(&profiling_block_once, vkd3d_init_profiling_once);
}

unsigned int vkd3d_profiling_register_region(const char *name, spinlock_t *lock, spinlock_t *latch)
{
    unsigned int index;
    if (!mapped_blocks)
        return 0;

    spinlock_acquire(lock);

    if (*latch == 0)
    {
        spinlock_acquire(&profiling_lock);
        /* Begin at 1, 0 is reserved as a sentinel. */
        index = ++profiling_region_count;
        if (index <= VKD3D_MAX_PROFILING_REGIONS)
        {
            /* Not getting a terminating NUL is okay here, the parser will take care of it later. */
            strncpy(mapped_blocks[index - 1].name, name, sizeof(mapped_blocks[index - 1].name));
            /* Important to store with release semantics after we've initialized the block. */
            vkd3d_atomic_uint32_store_explicit(latch, index, vkd3d_memory_order_release);
        }
        else
        {
            ERR("Too many profiling regions!\n");
            index = 0;
        }
        spinlock_release(&profiling_lock);
    }
    else
        index = *latch;

    spinlock_release(lock);
    return index;
}

void vkd3d_profiling_notify_work(unsigned int index,
        uint64_t start_ticks, uint64_t end_ticks,
        unsigned int iteration_count)
{
    struct vkd3d_profiling_block *block;
    if (index == 0 || index > VKD3D_MAX_PROFILING_REGIONS || !mapped_blocks)
        return;
    block = &mapped_blocks[index - 1];

    /* A slight hazard if application terminates in the middle of these two atomics, but whatever. */
    /* Can use relaxed order since there are no data dependencies associated with these, they are plain counters
     * purely used for the purpose of lightweight profiling. */
    vkd3d_atomic_uint64_add_fetch_explicit(&block->iteration_total, iteration_count, vkd3d_memory_order_relaxed);
    vkd3d_atomic_uint64_add_fetch_explicit(&block->ticks_total, end_ticks - start_ticks, vkd3d_memory_order_relaxed);
}
