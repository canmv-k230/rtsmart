/****************************************************************************
*
*    The MIT License (MIT)
*
*    Copyright (c) 2014 - 2022 Vivante Corporation
*
*    Permission is hereby granted, free of charge, to any person obtaining a
*    copy of this software and associated documentation files (the "Software"),
*    to deal in the Software without restriction, including without limitation
*    the rights to use, copy, modify, merge, publish, distribute, sublicense,
*    and/or sell copies of the Software, and to permit persons to whom the
*    Software is furnished to do so, subject to the following conditions:
*
*    The above copyright notice and this permission notice shall be included in
*    all copies or substantial portions of the Software.
*
*    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
*    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
*    DEALINGS IN THE SOFTWARE.
*
*****************************************************************************
*
*    The GPL License (GPL)
*
*    Copyright (C) 2014 - 2022 Vivante Corporation
*
*    This program is free software; you can redistribute it and/or
*    modify it under the terms of the GNU General Public License
*    as published by the Free Software Foundation; either version 2
*    of the License, or (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*****************************************************************************
*
*    Note: This software is released under dual MIT and GPL licenses. A
*    recipient may use this file under the terms of either the MIT license or
*    GPL License. If you wish to use only one license not the other, you can
*    indicate your decision by deleting one of the above license notices in your
*    version of this file.
*
*****************************************************************************/
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <dfs_file.h>
#include <rtdevice.h>
#include <rtdbg.h>
#include <rtdef.h>
#include <rthw.h>
#include <rtthread.h>

#include "board.h"
#include "ioremap.h"
#include "lwp_mm.h"
#include "lwp_mm_area.h"
#include "lwp_user_mm.h"
#include "mmu.h"
#include "page.h"
#include "sysctl_clk.h"
#include "sysctl_pwr.h"
#include "sysctl_rst.h"
#include "tick.h"
#include "vg_lite_platform.h"
#include "vg_lite_ioctl.h"
#include "vg_lite_kernel.h"
#include "vg_lite_hal.h"
#include "vg_lite_hw.h"

static void sleep(uint32_t msec)
{
    rt_thread_mdelay(msec);
}

#define rt_kprintf(...) do {} while (0)

static const uint32_t registerMemBase = (uint32_t)TAAH_GPU_BASE_ADDR;
static const uint32_t irq_num = 135;

/* If bit31 is activated this indicates a bus error */
#define IS_AXI_BUS_ERR(x) ((x)&(1U << 31))
#define VG_LITE_WAIT_SLICE_MS          1
#define VG_LITE_WAIT_FALLBACK_US       100
#define VG_LITE_WAIT_IDLE_TIMEOUT_US   100000
#define VG_LITE_CLOSE_IDLE_TIMEOUT_US  20000

void __attribute__((weak)) vg_lite_bus_error_handler();
void vg_lite_kernel_force_close(void);
int vg_lite_kernel_has_reference(void);

struct mapped_memory {
    void *pages;
    void *klogical;
    struct rt_lwp *owner;
    uint32_t page_bits;
    uint8_t user_mapped;
    uint8_t owns_pages;
    uint8_t cacheable;
    vg_lite_kernel_map_memory_t map;
    struct mapped_memory *next;
};

struct vg_lite_device {
    /* void * gpu; */
    uint32_t register_base;    /* Always use physical for register access in RTOS. */
    /* struct page * pages; */
    void * virtual;
    uint32_t physical;
    uint32_t size;
    int irq_enabled;
    volatile uint32_t int_flags;
    struct rt_semaphore int_queue;
    struct rt_mutex open_lock;
    struct rt_mutex alloc_lock;
    void * device;
    int registered;
    int open_count;
    int major;
    struct class * class;
    int created;
};

struct client_data {
    struct vg_lite_device * device;
    struct vm_area_struct * vm;
    void * contiguous_mapped;
};

static struct vg_lite_device Device, * device;
static struct mapped_memory *s_contiguous_list;

static void tracked_contiguous_add(struct mapped_memory *heap)
{
    if (heap == NULL)
        return;

    rt_mutex_take(&device->alloc_lock, RT_WAITING_FOREVER);
    heap->next = s_contiguous_list;
    s_contiguous_list = heap;
    rt_mutex_release(&device->alloc_lock);
}

static void tracked_contiguous_remove(struct mapped_memory *heap)
{
    struct mapped_memory **link;

    if (heap == NULL)
        return;

    rt_mutex_take(&device->alloc_lock, RT_WAITING_FOREVER);
    link = &s_contiguous_list;
    while (*link != NULL) {
        if (*link == heap) {
            *link = heap->next;
            heap->next = NULL;
            break;
        }
        link = &(*link)->next;
    }
    rt_mutex_release(&device->alloc_lock);
}

static void mapped_memory_release(struct mapped_memory *heap)
{
    if (heap == NULL)
        return;

    if (heap->user_mapped && heap->owner != NULL && heap->map.logical != NULL)
        lwp_unmap_user_phy(heap->owner, heap->map.logical);

    if (heap->owns_pages) {
        if (heap->klogical != NULL)
            rt_iounmap(heap->klogical);
        if (heap->pages != NULL)
            rt_pages_free(heap->pages, heap->page_bits);
    }

    rt_free(heap);
}

void vg_lite_hal_free_os_heap(void)
{
    struct mapped_memory *heap;

    if (device == NULL)
        return;

    rt_mutex_take(&device->alloc_lock, RT_WAITING_FOREVER);
    heap = s_contiguous_list;
    s_contiguous_list = NULL;
    rt_mutex_release(&device->alloc_lock);

    while (heap != NULL) {
        struct mapped_memory *next = heap->next;

        mapped_memory_release(heap);
        heap = next;
    }
}

static void clear_software_interrupt_state(void)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    device->int_flags = 0;
    rt_hw_interrupt_enable(level);

    while (rt_sem_take(&device->int_queue, RT_WAITING_NO) == RT_EOK) {
    }
}

static void clear_interrupt_state(void)
{
    (void)vg_lite_hal_peek(VG_LITE_INTR_STATUS);
    clear_software_interrupt_state();
}

static uint32_t take_interrupt_flags(uint32_t mask)
{
    uint32_t flags;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    flags = device->int_flags & mask;
    device->int_flags &= ~flags;
    rt_hw_interrupt_enable(level);

    if (flags == 0) {
        flags = vg_lite_hal_peek(VG_LITE_INTR_STATUS) & mask;
    }

    return flags;
}

void * vg_lite_hal_alloc(unsigned long size)
{
    return rt_malloc(size);
}

void vg_lite_hal_free(void * memory)
{
    rt_free(memory);
}

void vg_lite_hal_delay(uint32_t ms)
{
    rt_thread_mdelay(ms);
}

void vg_lite_hal_barrier(void)
{
    asm volatile("fence rw, rw" ::: "memory");
}

void vg_lite_hal_initialize(void) {}

void vg_lite_hal_deinitialize(void) {}

vg_lite_error_t vg_lite_hal_allocate_contiguous(unsigned long size, void **logical,
                                                     void **klogical, uint32_t *physical,
                                                     void **node)
{
    struct mapped_memory *heap;
    struct rt_lwp *owner = lwp_self();
    size_t aligned_size;
    size_t allocation_size;
    size_t page_bits;
    uintptr_t physical_addr;

    if (size == 0 || size > MAX_CONTIGUOUS_SIZE || owner == NULL ||
        logical == NULL || klogical == NULL || physical == NULL || node == NULL)
        return VG_LITE_INVALID_ARGUMENT;

    if (size > ULONG_MAX - 63UL)
        return VG_LITE_INVALID_ARGUMENT;

    aligned_size = (size + 63UL) & ~63UL;
    page_bits = rt_page_bits(aligned_size);
    if (page_bits >= sizeof(size_t) * CHAR_BIT - ARCH_PAGE_SHIFT)
        return VG_LITE_OUT_OF_MEMORY;

    allocation_size = (size_t)ARCH_PAGE_SIZE << page_bits;
    heap = rt_calloc(1, sizeof(*heap));
    if (heap == NULL)
        return VG_LITE_OUT_OF_MEMORY;

    heap->pages = rt_pages_alloc((uint32_t)page_bits);
    if (heap->pages == NULL) {
        rt_free(heap);
        return VG_LITE_OUT_OF_MEMORY;
    }

    physical_addr = (uintptr_t)heap->pages + (uintptr_t)PV_OFFSET;
    if (physical_addr > UINT32_MAX || allocation_size - 1 > UINT32_MAX - physical_addr) {
        rt_pages_free(heap->pages, (uint32_t)page_bits);
        rt_free(heap);
        return VG_LITE_OUT_OF_RESOURCES;
    }

    rt_memset(heap->pages, 0, allocation_size);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, heap->pages, allocation_size);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, heap->pages, allocation_size);

    heap->klogical = rt_ioremap_nocache((void *)physical_addr, allocation_size);
    if (heap->klogical == NULL) {
        rt_pages_free(heap->pages, (uint32_t)page_bits);
        rt_free(heap);
        return VG_LITE_OUT_OF_RESOURCES;
    }

    heap->owner = owner;
    heap->page_bits = (uint32_t)page_bits;
    heap->owns_pages = 1;
    heap->map.physical = (uint32_t)physical_addr;
    heap->map.bytes = (uint32_t)aligned_size;
    heap->map.logical = lwp_map_user_phy(owner, RT_NULL, (void *)physical_addr,
                                         aligned_size, 0);
    if (heap->map.logical == NULL) {
        mapped_memory_release(heap);
        return VG_LITE_OUT_OF_RESOURCES;
    }

    heap->user_mapped = 1;
    *logical = heap->map.logical;
    *klogical = heap->klogical;
    *physical = heap->map.physical;
    *node = heap;
    tracked_contiguous_add(heap);

    return VG_LITE_SUCCESS;
}

void vg_lite_hal_free_contiguous(void *memory_handle)
{
    struct mapped_memory *heap = memory_handle;

    if (heap == NULL)
        return;

    tracked_contiguous_remove(heap);
    mapped_memory_release(heap);
}

/* Portable: read register value. */
uint32_t vg_lite_hal_peek(uint32_t address)
{
    uint32_t val;

	asm volatile("lw %0, 0(%1)" : "=r" (val) : "r" (device->virtual + address));
	return val;
}

/* Portable: write register. */
void vg_lite_hal_poke(uint32_t address, uint32_t data)
{
    asm volatile("sw %0, 0(%1)" : : "r" (data), "r" (device->virtual + address));
}

vg_lite_error_t vg_lite_hal_query_mem(vg_lite_kernel_mem_t *mem)
{
    size_t total_pages;
    size_t free_pages;
    size_t free_bytes;

    if (mem == NULL)
        return VG_LITE_INVALID_ARGUMENT;

    rt_page_get_info(&total_pages, &free_pages);
    (void)total_pages;
    free_bytes = free_pages * ARCH_PAGE_SIZE;
    mem->bytes = free_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)free_bytes;
    return VG_LITE_SUCCESS;
}

vg_lite_error_t vg_lite_hal_map_memory(vg_lite_kernel_map_memory_t *node)
{
    struct rt_lwp *owner = lwp_self();

    if (node == NULL || owner == NULL || node->physical == 0 || node->bytes == 0)
        return VG_LITE_INVALID_ARGUMENT;

    node->logical = lwp_map_user_phy(owner, RT_NULL,
                                     (void *)(uintptr_t)node->physical,
                                     node->bytes, 0);
    if (node->logical == NULL)
        return VG_LITE_OUT_OF_RESOURCES;

    return VG_LITE_SUCCESS;
}

vg_lite_error_t vg_lite_hal_unmap_memory(vg_lite_kernel_unmap_memory_t *node)
{
    struct rt_lwp *owner = lwp_self();

    if (node == NULL || owner == NULL || node->logical == NULL)
        return VG_LITE_INVALID_ARGUMENT;

    if (lwp_unmap_user_phy(owner, node->logical))
        return VG_LITE_INVALID_ARGUMENT;

    return VG_LITE_SUCCESS;
}

void __attribute__((weak)) vg_lite_bus_error_handler()
{
    /*
     * Default implementation of the bus error handler does nothing. Application
     * should override this handler if it requires to be notified when a bus
     * error event occurs.
     */
     return;
}

static void vg_lite_IRQHandler(int irq, void *param)
{
    uint32_t flags = vg_lite_hal_peek(VG_LITE_INTR_STATUS);

    if (flags) {
        /* Combine with current interrupt flags. */
        device->int_flags |= flags;

        /* Wake up any waiters. */
        rt_sem_release(&device->int_queue);
    }
}

int32_t vg_lite_hal_wait_interrupt(uint32_t timeout, uint32_t mask, uint32_t * value)
{
    rt_tick_t wait_ticks;
    rt_tick_t start_ticks;
    rt_tick_t left_ticks;
    rt_tick_t slice_ticks;
    uint32_t flags;
    uint64_t idle_start_us;
    uint64_t wait_start_us;
    int wait_forever;

    if (value == NULL) {
        return 0;
    }

    *value = 0;
    wait_forever = (timeout == VG_LITE_INFINITE);
    if (wait_forever) {
        wait_ticks = RT_WAITING_FOREVER;
    }
    else {
        wait_ticks = rt_tick_from_millisecond((rt_int32_t)timeout);
    }
    start_ticks = rt_tick_get();
    left_ticks = wait_ticks;
    slice_ticks = rt_tick_from_millisecond(VG_LITE_WAIT_SLICE_MS);
    if (slice_ticks == 0) {
        slice_ticks = 1;
    }
    wait_start_us = cpu_ticks_us();

    for (;;) {
        rt_tick_t take_ticks;
        flags = take_interrupt_flags(mask);
        if (flags != 0) {
            break;
        }

        if ((cpu_ticks_us() - wait_start_us) >= VG_LITE_WAIT_FALLBACK_US &&
            ((vg_lite_hal_peek(VG_LITE_HW_IDLE) & VG_LITE_HW_IDLE_STATE) == VG_LITE_HW_IDLE_STATE)) {
            flags = take_interrupt_flags(mask);
            *value = flags;
            if (IS_AXI_BUS_ERR(*value)) {
                vg_lite_bus_error_handler();
            }
            clear_interrupt_state();
            vg_lite_hal_barrier();
            return 1;
        }

        if (!wait_forever) {
            rt_tick_t elapsed = rt_tick_get() - start_ticks;
            if (elapsed >= wait_ticks) {
                return 0;
            }
            left_ticks = wait_ticks - elapsed;
        }

        take_ticks = wait_forever ? slice_ticks : (left_ticks < slice_ticks ? left_ticks : slice_ticks);
        if (rt_sem_take_interruptible(&device->int_queue, take_ticks) == -RT_EINTR) {
            if ((vg_lite_hal_peek(VG_LITE_HW_IDLE) & VG_LITE_HW_IDLE_STATE) == VG_LITE_HW_IDLE_STATE) {
                clear_interrupt_state();
                vg_lite_hal_barrier();
                return 1;
            }
            return 0;
        }
    }

    *value = flags;
    if (IS_AXI_BUS_ERR(*value)) {
        vg_lite_bus_error_handler();
    }

    idle_start_us = cpu_ticks_us();
    while (1) {
        if ((vg_lite_hal_peek(VG_LITE_HW_IDLE) & VG_LITE_HW_IDLE_STATE) == VG_LITE_HW_IDLE_STATE) {
            vg_lite_hal_barrier();
            return 1;
        }
        if ((cpu_ticks_us() - idle_start_us) >= VG_LITE_WAIT_IDLE_TIMEOUT_US) {
            return 0;
        }
        if ((cpu_ticks_us() - idle_start_us) < 1000) {
            cpu_ticks_delay_us(10);
        } else {
            vg_lite_hal_delay(1);
        }
    }
}

vg_lite_error_t vg_lite_hal_memory_export(int32_t *fd)
{
    return VG_LITE_NOT_SUPPORT;
}


static bool inspect_user_mapping(struct rt_lwp *owner, void *logical,
                                 uint32_t bytes, uint32_t *physical,
                                 void **klogical, uint8_t *cacheable)
{
    struct lwp_avl_struct *area_node;
    struct rt_mm_area_struct *area;
    uintptr_t start = (uintptr_t)logical;
    uintptr_t physical_start;
    uintptr_t requested_physical;
    size_t offset = 0;
    bool valid = false;

    if (owner == NULL || logical == NULL || physical == NULL ||
        klogical == NULL || cacheable == NULL || bytes == 0 ||
        start > UINTPTR_MAX - (bytes - 1U))
        return false;

    requested_physical = *physical;

    rt_mm_lock();
    area_node = lwp_map_find(owner->map_area, start);
    if (area_node == NULL)
        goto out;

    area = area_node->data;
    if (start < area->addr || start - area->addr >= area->size ||
        bytes > area->size - (start - area->addr))
        goto out;

    physical_start = (uintptr_t)rt_hw_mmu_v2p(&owner->mmu_info, logical);
    if (physical_start == 0 || physical_start > UINT32_MAX ||
        bytes - 1U > UINT32_MAX - physical_start ||
        physical_start < (uintptr_t)PV_OFFSET)
        goto out;

    if (requested_physical != 0 && requested_physical != physical_start)
        goto out;

    while (offset < bytes) {
        uintptr_t current = start + offset;
        uintptr_t page_physical = (uintptr_t)rt_hw_mmu_v2p(&owner->mmu_info,
                                                            (void *)current);
        size_t step = ARCH_PAGE_SIZE - (current & ARCH_PAGE_MASK);

        if (page_physical != physical_start + offset)
            goto out;
        if (step > bytes - offset)
            step = bytes - offset;
        offset += step;
    }

    *physical = (uint32_t)physical_start;
    *cacheable = area->type == MM_AREA_TYPE_PHY_CACHED ||
                 area->type == MM_AREA_TYPE_SHM ||
                 area->type == MM_AREA_TYPE_DATA ||
                 area->type == MM_AREA_TYPE_TEXT;
    *klogical = (void *)(physical_start - (uintptr_t)PV_OFFSET);
    valid = true;

out:
    rt_mm_unlock();
    return valid;
}

void *vg_lite_hal_map(uint32_t flags, uint32_t bytes, void *logical,
                      uint32_t physical, int32_t dma_buf_fd, uint32_t *gpu)
{
    struct mapped_memory *heap;
    struct rt_lwp *owner = lwp_self();
    uint32_t mapped_physical = physical;

    (void)dma_buf_fd;

    if (flags != VG_LITE_HAL_MAP_USER_MEMORY || bytes == 0 ||
        (logical == NULL && physical == 0) ||
        (logical == NULL && bytes - 1U > UINT32_MAX - physical))
        return NULL;

    heap = rt_calloc(1, sizeof(*heap));
    if (heap == NULL)
        return NULL;

    if (logical != NULL &&
        !inspect_user_mapping(owner, logical, bytes, &mapped_physical,
                              &heap->klogical, &heap->cacheable)) {
        rt_free(heap);
        return NULL;
    }

    heap->owner = owner;
    heap->map.physical = mapped_physical;
    heap->map.bytes = bytes;
    heap->map.logical = logical;

    if (gpu != NULL)
        *gpu = mapped_physical;

    return heap;
}

uint32_t vg_lite_hal_is_cacheable(void *memory_handle)
{
    struct mapped_memory *heap = memory_handle;

    return heap != NULL && heap->cacheable;
}

void vg_lite_hal_unmap(void *handle)
{
    mapped_memory_release(handle);
}

static vg_lite_error_t operate_cache_range(void *logical, uint32_t bytes,
                                           vg_lite_cache_op_t cache_op)
{
    if (logical == NULL || bytes == 0)
        return VG_LITE_INVALID_ARGUMENT;

    vg_lite_hal_barrier();
    if (cache_op == VG_LITE_CACHE_CLEAN) {
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, logical, bytes);
    } else if (cache_op == VG_LITE_CACHE_INVALIDATE) {
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, logical, bytes);
    } else if (cache_op == VG_LITE_CACHE_FLUSH) {
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, logical, bytes);
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, logical, bytes);
    } else {
        return VG_LITE_INVALID_ARGUMENT;
    }
    vg_lite_hal_barrier();

    return VG_LITE_SUCCESS;
}

vg_lite_error_t vg_lite_hal_operation_cache(void *handle, vg_lite_cache_op_t cache_op)
{
    struct mapped_memory *heap = handle;

    if (heap == NULL || heap->map.bytes == 0)
        return VG_LITE_INVALID_ARGUMENT;

    if (!heap->cacheable)
        return VG_LITE_SUCCESS;

    return operate_cache_range(heap->klogical, heap->map.bytes, cache_op);
}

vg_lite_error_t vg_lite_hal_user_memory(vg_lite_kernel_user_memory_t *memory)
{
    struct rt_lwp *owner = lwp_self();
    void *klogical = NULL;
    uint8_t cacheable = 0;
    uint32_t physical = 0;
    vg_lite_cache_op_t cache_op;

    if (memory == NULL || memory->logical == NULL || memory->bytes == 0)
        return VG_LITE_INVALID_ARGUMENT;

    if (!inspect_user_mapping(owner, memory->logical, memory->bytes, &physical,
                              &klogical, &cacheable))
        return VG_LITE_OUT_OF_RESOURCES;

    memory->physical = physical;
    memory->cacheable = cacheable;

    if (memory->operation == VG_LITE_USER_MEMORY_QUERY)
        return VG_LITE_SUCCESS;

    if (memory->operation == VG_LITE_USER_MEMORY_CLEAN) {
        cache_op = VG_LITE_CACHE_CLEAN;
    } else if (memory->operation == VG_LITE_USER_MEMORY_INVALIDATE) {
        cache_op = VG_LITE_CACHE_INVALIDATE;
    } else if (memory->operation == VG_LITE_USER_MEMORY_FLUSH) {
        cache_op = VG_LITE_CACHE_FLUSH;
    } else {
        return VG_LITE_INVALID_ARGUMENT;
    }

    if (!cacheable)
        return VG_LITE_SUCCESS;

    return operate_cache_range(klogical, memory->bytes, cache_op);
}

static struct rt_device g_gpu_device;

static size_t gpu_ioctl_command_size(vg_lite_kernel_command_t command)
{
    static const size_t command_sizes[] = {
        [VG_LITE_INITIALIZE] = sizeof(vg_lite_kernel_initialize_t),
        [VG_LITE_TERMINATE] = sizeof(vg_lite_kernel_terminate_t),
        [VG_LITE_ALLOCATE] = sizeof(vg_lite_kernel_allocate_t),
        [VG_LITE_FREE] = sizeof(vg_lite_kernel_free_t),
        [VG_LITE_SUBMIT] = sizeof(vg_lite_kernel_submit_t),
        [VG_LITE_WAIT] = sizeof(vg_lite_kernel_wait_t),
        [VG_LITE_RESET] = sizeof(vg_lite_kernel_reset_t),
        [VG_LITE_DEBUG] = sizeof(vg_lite_kernel_debug_t),
        [VG_LITE_MAP] = sizeof(vg_lite_kernel_map_t),
        [VG_LITE_UNMAP] = sizeof(vg_lite_kernel_unmap_t),
        [VG_LITE_CHECK] = sizeof(vg_lite_kernel_info_t),
        [VG_LITE_QUERY_MEM] = sizeof(vg_lite_kernel_mem_t),
        [VG_LITE_FLEXA_DISABLE] = sizeof(vg_lite_kernel_flexa_info_t),
        [VG_LITE_FLEXA_ENABLE] = sizeof(vg_lite_kernel_flexa_info_t),
        [VG_LITE_FLEXA_STOP_FRAME] = sizeof(vg_lite_kernel_flexa_info_t),
        [VG_LITE_FLEXA_SET_BACKGROUND_ADDRESS] = sizeof(vg_lite_kernel_flexa_info_t),
        [VG_LITE_MAP_MEMORY] = sizeof(vg_lite_kernel_map_memory_t),
        [VG_LITE_UNMAP_MEMORY] = sizeof(vg_lite_kernel_unmap_memory_t),
        [VG_LITE_CLOSE] = sizeof(vg_lite_kernel_close_t),
        [VG_LITE_CACHE] = sizeof(vg_lite_kernel_cache_t),
        [VG_LITE_EXPORT_MEMORY] = sizeof(vg_lite_kernel_export_memory_t),
        [VG_LITE_SUBMIT_EX] = sizeof(vg_lite_kernel_submit_ex_t),
        [VG_LITE_USER_MEMORY] = sizeof(vg_lite_kernel_user_memory_t),
    };

    if ((unsigned int)command >= sizeof(command_sizes) / sizeof(command_sizes[0]))
        return 0;

    return command_sizes[command];
}

static int gpu_open(struct dfs_fd *file) {
    rt_kprintf("vg_lite open\n");
    if (rt_mutex_take(&device->open_lock, RT_WAITING_FOREVER) != RT_EOK) {
        return -EINTR;
    }

    if (device->open_count == 0) {
        /* Turn on the power. */
        sysctl_pwr_up(SYSCTL_PD_DISP);
        /* Turn on the clock. */
        sysctl_clk_set_disp_gpu_en(true);
        sysctl_reset(SYSCTL_RESET_GPU);
        clear_interrupt_state();
        rt_hw_interrupt_umask(irq_num);
    }
    device->open_count++;
    rt_mutex_release(&device->open_lock);
    return 0;
}

static int gpu_close(struct dfs_fd *file)
{
    int has_reference;
    int last_close = 0;

    if (rt_mutex_take(&device->open_lock, RT_WAITING_FOREVER) != RT_EOK) {
        return -EINTR;
    }

    if (device->open_count > 0) {
        device->open_count--;
    }
    if (device->open_count == 0) {
        last_close = 1;
    }

    if (last_close) {
        has_reference = vg_lite_kernel_has_reference();
        if (has_reference) {
            uint64_t close_start_us = cpu_ticks_us();

            while ((vg_lite_hal_peek(VG_LITE_HW_IDLE) & VG_LITE_HW_IDLE_STATE) != VG_LITE_HW_IDLE_STATE) {
                if ((cpu_ticks_us() - close_start_us) >= VG_LITE_CLOSE_IDLE_TIMEOUT_US) {
                    sysctl_reset(SYSCTL_RESET_GPU);
                    vg_lite_hal_delay(1);
                    break;
                }
                cpu_ticks_delay_us(10);
            }
        }

        rt_hw_interrupt_mask(irq_num);
        if (has_reference) {
            clear_interrupt_state();
            vg_lite_kernel_force_close();
        } else {
            clear_software_interrupt_state();
        }
        sysctl_clk_set_disp_gpu_en(false);
        sysctl_pwr_off(SYSCTL_PD_DISP);
    }

    rt_mutex_release(&device->open_lock);
    return 0;
}

static int gpu_ioctl(struct dfs_fd *file, int cmd, void *args) {
    struct ioctl_data arguments;
    union {
        unsigned long align;
        rt_uint8_t bytes[256];
    } stack_data;
    void *data;
    int use_heap = 0;
    size_t expected_bytes;

    if (cmd != VG_LITE_IOCTL)
        return -1;

    if (!args)
        return -1;

    if (lwp_get_from_user(&arguments, (void *)args, sizeof(arguments)) != sizeof(arguments))
        return -EFAULT;
    expected_bytes = gpu_ioctl_command_size(arguments.command);
    if (expected_bytes == 0 || arguments.bytes != expected_bytes || arguments.buffer == NULL)
        return -EINVAL;

    if (arguments.bytes <= sizeof(stack_data.bytes)) {
        data = stack_data.bytes;
    } else {
        data = rt_malloc(arguments.bytes);
        if (!data)
            return -ENOMEM;
        use_heap = 1;
    }

    if (lwp_get_from_user(data, arguments.buffer, arguments.bytes) != arguments.bytes) {
        if (use_heap)
            rt_free(data);
        return -EFAULT;
    }

    arguments.error = vg_lite_kernel(arguments.command, data);

    lwp_put_to_user(arguments.buffer, data, arguments.bytes);
    if (use_heap)
        rt_free(data);
    lwp_put_to_user((void *)args, &arguments, sizeof(arguments));

    return 0;
}

static struct dfs_file_ops gpu_ops = {
    .open = gpu_open,
    .close = gpu_close,
    .ioctl = gpu_ioctl,
};

static int vg_lite_driver_init(void)
{
    /* Initialize memory and objects ***************************************/
    /* Create device structure. */
    rt_kprintf("VGLite built %s %s\n", __DATE__, __TIME__);
    device = &Device;

    /* Zero out the enture structure. */
    memset(device, 0, sizeof(struct vg_lite_device));

    /* Setup register memory. **********************************************/
    device->register_base = registerMemBase;
    device->virtual = rt_ioremap_nocache((void *)(rt_size_t)device->register_base,
                                          TAAH_GPU_IO_SIZE);
    if (device->virtual == NULL) {
        return -1;
    }
    rt_hw_interrupt_mask(irq_num);
    rt_hw_interrupt_install(irq_num, vg_lite_IRQHandler, NULL, "gpu");

    rt_err_t err = rt_device_register(&g_gpu_device, "vg_lite", RT_DEVICE_OFLAG_RDWR);
    if (err != RT_EOK) {
        rt_iounmap(device->virtual);
        return -1;
    }
    g_gpu_device.fops = &gpu_ops;

    rt_sem_init(&device->int_queue, "gpu", 0, RT_IPC_FLAG_FIFO);
    rt_mutex_init(&device->open_lock, "gpuopen", RT_IPC_FLAG_FIFO);
    rt_mutex_init(&device->alloc_lock, "gpualloc", RT_IPC_FLAG_FIFO);
    device->int_flags = 0;

    /* Success. */
    return 0;
}
INIT_DEVICE_EXPORT(vg_lite_driver_init);
