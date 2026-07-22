/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2019-10-28     Jesven       first version
 * 2021-02-06     lizhirui     fixed fixed vtable size problem
 * 2021-02-12     lizhirui     add 64-bit support for lwp_brk
 * 2021-02-19     lizhirui     add riscv64 support for lwp_user_accessable and lwp_get_from_user
 * 2021-06-07     lizhirui     modify user space bound check
 */

#include <rtthread.h>
#include <rthw.h>

#ifdef RT_USING_USERSPACE

#include <mmu.h>
#include <page.h>
#include <lwp_mm_area.h>
#include <lwp_user_mm.h>
#include <lwp_arch.h>
#include <lwp_mm.h>

#include "rvv_ops.h"

static int lwp_user_range_valid(const void *addr, size_t size)
{
    rt_ubase_t start = (rt_ubase_t)addr;
    rt_ubase_t top = (rt_ubase_t)USER_VADDR_TOP;

    if (size == 0)
    {
        return 1;
    }
    if (!addr || start < (rt_ubase_t)USER_VADDR_START || start >= top)
    {
        return 0;
    }
    return size <= top - start;
}

int lwp_user_space_init(struct rt_lwp *lwp)
{
    return arch_user_space_init(lwp);
}

#ifdef LWP_ENABLE_ASID
void rt_hw_mmu_switch(void *mtable, unsigned int pid, unsigned int asid);
#else
void rt_hw_mmu_switch(void *mtable);
#endif
void *rt_hw_mmu_tbl_get(void);
void lwp_mmu_switch(struct rt_thread *thread)
{
    struct rt_lwp *l = RT_NULL;
    void *pre_mmu_table = RT_NULL, *new_mmu_table = RT_NULL;

    if (thread->lwp)
    {
        l = (struct rt_lwp *)thread->lwp;
        new_mmu_table = (void *)((char *)l->mmu_info.vtable + l->mmu_info.pv_off);
    }
    else
    {
        new_mmu_table = arch_kernel_mmu_table_get();
    }

    pre_mmu_table = rt_hw_mmu_tbl_get();
    if (pre_mmu_table != new_mmu_table)
    {
#ifdef LWP_ENABLE_ASID
        rt_hw_mmu_switch(new_mmu_table, l ? l->pid : 0, arch_get_asid(l));
#else
        rt_hw_mmu_switch(new_mmu_table);
#endif
    }
}

static void unmap_range(struct rt_lwp *lwp, void *addr, size_t size, int pa_need_free)
{
    void *va, *pa;
    size_t bytes;

    for (va = addr; size; va += bytes, size -= bytes)
    {
        bytes = ARCH_PAGE_SIZE - ((size_t)va & (ARCH_PAGE_SIZE - 1));
        bytes = bytes > size ? size : bytes;
        pa = rt_hw_mmu_v2p(&lwp->mmu_info, va);
        if (pa)
        {
            rt_hw_mmu_unmap(&lwp->mmu_info, va, bytes);
            if (pa_need_free)
            {
                rt_pages_free((void *)((char *)pa - PV_OFFSET), 0);
            }
        }
    }
}

static struct lwp_avl_struct *lwp_map_area_node_create(size_t addr, size_t size, int type,
                                                        size_t unmap_all_addr)
{
    struct lwp_avl_struct *node;
    struct rt_mm_area_struct *ma;

    ma = (struct rt_mm_area_struct *)rt_malloc(sizeof(*ma));
    if (!ma)
    {
        return RT_NULL;
    }

    node = (struct lwp_avl_struct *)rt_malloc(sizeof(*node));
    if (!node)
    {
        rt_free(ma);
        return RT_NULL;
    }

    ma->addr = addr;
    ma->size = size;
    ma->type = type;
    ma->unmap_all_addr = unmap_all_addr;
    rt_memset(node, 0, sizeof(*node));
    node->avl_key = addr;
    node->data = ma;
    return node;
}

static void lwp_map_area_node_free(struct lwp_avl_struct *node)
{
    if (node)
    {
        rt_free(node->data);
        rt_free(node);
    }
}

static struct lwp_avl_struct *lwp_map_find_next(struct lwp_avl_struct *tree, size_t addr)
{
    struct lwp_avl_struct *next = RT_NULL;

    while (tree)
    {
        if (tree->avl_key > addr)
        {
            next = tree;
            tree = tree->avl_left;
        }
        else
        {
            tree = tree->avl_right;
        }
    }
    return next;
}

static void lwp_map_area_set_unmap_all_addr(struct rt_lwp *lwp, void *map_va,
                                             void *unmap_all_va)
{
    struct lwp_avl_struct *node;

    node = lwp_map_find(lwp->map_area, (size_t)map_va);
    if (node)
    {
        struct rt_mm_area_struct *ma = (struct rt_mm_area_struct *)node->data;

        ma->unmap_all_addr = (size_t)unmap_all_va;
    }
}

void lwp_unmap_user_space(struct rt_lwp *lwp)
{
    struct lwp_avl_struct *node = RT_NULL;

    while ((node = lwp_map_find_first(lwp->map_area)) != 0)
    {
        struct rt_mm_area_struct *ma = (struct rt_mm_area_struct *)node->data;
        int pa_need_free = 0;

        RT_ASSERT(ma->type < MM_AREA_TYPE_UNKNOW);

        switch (ma->type)
        {
            case MM_AREA_TYPE_DATA:
            case MM_AREA_TYPE_TEXT:
                pa_need_free = 1;
                break;
            case MM_AREA_TYPE_SHM:
                lwp_shm_ref_dec(lwp, (void *)ma->addr);
                break;
        }
        unmap_range(lwp, (void *)ma->addr, ma->size, pa_need_free);
        lwp_map_area_remove(&lwp->map_area, ma->addr);
    }

    arch_user_space_vtable_free(lwp);
}

static void *_lwp_map_user(struct rt_lwp *lwp, void *map_va, size_t map_size, int text)
{
    void *va = RT_NULL;
    int ret = 0;
    rt_mmu_info *m_info = &lwp->mmu_info;
    int area_type;

    va = rt_hw_mmu_map_auto(m_info, map_va, map_size, MMU_MAP_U_RWCB);
    if (!va)
    {
        rt_kprintf("Memory exhaustion!\r\n");
        // sys_exit(-1);
        return 0;
    }

    area_type = text ? MM_AREA_TYPE_TEXT : MM_AREA_TYPE_DATA;
    ret = lwp_map_area_insert(&lwp->map_area, (size_t)va, map_size, area_type);
    if (ret != 0)
    {
        unmap_range(lwp, va, map_size, 1);
        return 0;
    }
    return va;
}

static int _lwp_unmap_user(struct rt_lwp *lwp, void *va)
{
    struct lwp_avl_struct *ma_avl_node = RT_NULL;
    struct rt_mm_area_struct *ma = RT_NULL;
    int pa_need_free = 0;

    if (!lwp || !va)
    {
        return -EINVAL;
    }
    va = (void *)((size_t)va & ~ARCH_PAGE_MASK);
    ma_avl_node = lwp_map_find(lwp->map_area, (size_t)va);
    if (!ma_avl_node)
    {
        return -1;
    }
    ma = (struct rt_mm_area_struct *)ma_avl_node->data;

    RT_ASSERT(ma->type < MM_AREA_TYPE_UNKNOW);
    if ((ma->type == MM_AREA_TYPE_DATA) || (ma->type == MM_AREA_TYPE_TEXT))
    {
        pa_need_free = 1;
    }
    unmap_range(lwp, (void *)ma->addr, ma->size, pa_need_free);
    lwp_map_area_remove(&lwp->map_area, ma->addr);

    return 0;
}

static int lwp_unmap_user_range_prepare(struct rt_lwp *lwp, size_t start, size_t end,
                                        struct lwp_avl_struct **tail)
{
    size_t current = start;

    *tail = RT_NULL;
    while (current < end)
    {
        struct lwp_avl_struct *node;
        struct rt_mm_area_struct *ma;
        size_t area_end;
        size_t segment_end;
        size_t offset;

        node = lwp_map_find(lwp->map_area, current);
        if (!node)
        {
            node = lwp_map_find_next(lwp->map_area, current);
            if (!node || node->avl_key >= end)
            {
                break;
            }
            current = node->avl_key;
            continue;
        }

        ma = (struct rt_mm_area_struct *)node->data;
        area_end = ma->addr + ma->size;
        if (area_end <= current)
        {
            return -EINVAL;
        }
        segment_end = area_end < end ? area_end : end;
        offset = current - ma->addr;

        if (ma->type == MM_AREA_TYPE_SHM &&
            (offset != 0 || segment_end != area_end))
        {
            return -EINVAL;
        }

        if (offset != 0 && segment_end < area_end)
        {
            if (*tail)
            {
                return -EINVAL;
            }
            *tail = lwp_map_area_node_create(segment_end, area_end - segment_end,
                                              ma->type, 0);
            if (!*tail)
            {
                return -ENOMEM;
            }
        }

        current = segment_end;
    }

    return 0;
}

static void lwp_unmap_user_area(struct rt_lwp *lwp, struct lwp_avl_struct *node,
                                size_t start, size_t end,
                                struct lwp_avl_struct *tail)
{
    struct rt_mm_area_struct *ma = (struct rt_mm_area_struct *)node->data;
    size_t offset = start - ma->addr;
    size_t length = end - start;
    size_t unmap_all_addr = ma->unmap_all_addr;
    int pa_need_free = 0;

    RT_ASSERT(ma->type < MM_AREA_TYPE_UNKNOW);
    if ((ma->type == MM_AREA_TYPE_DATA) || (ma->type == MM_AREA_TYPE_TEXT))
    {
        pa_need_free = 1;
    }
    else if (ma->type == MM_AREA_TYPE_SHM)
    {
        lwp_shm_ref_dec(lwp, (void *)start);
    }
    unmap_range(lwp, (void *)start, length, pa_need_free);

    if (offset == 0 && length == ma->size)
    {
        lwp_map_area_remove(&lwp->map_area, ma->addr);
    }
    else if (offset == 0)
    {
        lwp_avl_remove(node, &lwp->map_area);
        ma->addr += length;
        ma->size -= length;
        if (ma->unmap_all_addr && ma->unmap_all_addr < ma->addr)
        {
            ma->unmap_all_addr = 0;
        }
        node->avl_key = ma->addr;
        lwp_avl_insert(node, &lwp->map_area);
    }
    else
    {
        ma->size = offset;
        if (ma->unmap_all_addr && ma->unmap_all_addr >= ma->addr + ma->size)
        {
            ma->unmap_all_addr = 0;
        }
        if (tail)
        {
            struct rt_mm_area_struct *tail_ma =
                (struct rt_mm_area_struct *)tail->data;

            if (unmap_all_addr >= tail_ma->addr &&
                unmap_all_addr < tail_ma->addr + tail_ma->size)
            {
                tail_ma->unmap_all_addr = unmap_all_addr;
            }
            lwp_avl_insert(tail, &lwp->map_area);
        }
    }
}

static int _lwp_unmap_user_range(struct rt_lwp *lwp, void *va, size_t length)
{
    size_t current;
    size_t end;
    struct lwp_avl_struct *tail;
    int ret;

    if (!lwp || !va || ((size_t)va & ARCH_PAGE_MASK) || length == 0 ||
        (size_t)va > (size_t)-1 - length)
    {
        return -EINVAL;
    }

    current = (size_t)va;
    end = current + length;
    ret = lwp_unmap_user_range_prepare(lwp, current, end, &tail);
    if (ret != 0)
    {
        lwp_map_area_node_free(tail);
        return ret;
    }

    while (current < end)
    {
        struct lwp_avl_struct *node;
        struct rt_mm_area_struct *ma;
        size_t area_end;
        size_t segment_end;
        struct lwp_avl_struct *area_tail = RT_NULL;

        node = lwp_map_find(lwp->map_area, current);
        if (!node)
        {
            node = lwp_map_find_next(lwp->map_area, current);
            if (!node || node->avl_key >= end)
            {
                break;
            }
            current = node->avl_key;
            continue;
        }

        ma = (struct rt_mm_area_struct *)node->data;
        area_end = ma->addr + ma->size;
        segment_end = area_end < end ? area_end : end;
        if (current != ma->addr && segment_end < area_end)
        {
            area_tail = tail;
            tail = RT_NULL;
        }
        lwp_unmap_user_area(lwp, node, current, segment_end, area_tail);
        current = segment_end;
    }

    lwp_map_area_node_free(tail);
    return 0;
}

int lwp_unmap_user(struct rt_lwp *lwp, void *va)
{
    int ret;

    rt_mm_lock();
    ret = _lwp_unmap_user(lwp, va);
    rt_mm_unlock();
    return ret;
}

int lwp_dup_user(struct lwp_avl_struct *ptree, void *arg)
{
    struct rt_lwp *self_lwp = lwp_self();
    struct rt_lwp *new_lwp = (struct rt_lwp *)arg;
    struct rt_mm_area_struct *ma = (struct rt_mm_area_struct *)ptree->data;
    void *pa = RT_NULL;
    void *va = RT_NULL;

    switch (ma->type)
    {
        case MM_AREA_TYPE_PHY:
            pa = rt_hw_mmu_v2p(&self_lwp->mmu_info, (void *)ma->addr);
            va = lwp_map_user_type(new_lwp, (void *)ma->addr, pa, ma->size, 0, MM_AREA_TYPE_PHY);
            break;
        case MM_AREA_TYPE_PHY_CACHED:
            pa = rt_hw_mmu_v2p(&self_lwp->mmu_info, (void *)ma->addr);
            va = lwp_map_user_type(new_lwp, (void *)ma->addr, pa, ma->size, 0, MM_AREA_TYPE_PHY_CACHED);
            break;
        case MM_AREA_TYPE_SHM:
            va = (void *)ma->addr;
            if (lwp_shm_ref_inc(self_lwp, va) > 0)
            {
                pa = rt_hw_mmu_v2p(&self_lwp->mmu_info, va);
                va = lwp_map_user_type(new_lwp, va, pa, ma->size, 1, MM_AREA_TYPE_SHM);
            }
            break;
        case MM_AREA_TYPE_DATA:
            va = lwp_map_user(new_lwp, (void *)ma->addr, ma->size, 0);
            if (va == (void *)ma->addr)
            {
                lwp_data_put(&new_lwp->mmu_info, va, va, ma->size);
            }
            break;
        case MM_AREA_TYPE_TEXT:
            {
                char *addr = (char *)ma->addr;
                size_t size = ma->size;

                while (size)
                {
                    pa = rt_hw_mmu_v2p(&self_lwp->mmu_info, (void *)addr);
                    rt_page_ref_inc((char *)pa - self_lwp->mmu_info.pv_off, 0);
                    va = lwp_map_user_type(new_lwp, addr, pa, ARCH_PAGE_SIZE, 1, MM_AREA_TYPE_TEXT);
                    if (va != addr)
                    {
                        return -1;
                    }
                    addr += ARCH_PAGE_SIZE;
                    size -= ARCH_PAGE_SIZE;
                }
                va = (void *)ma->addr;
            }
            break;
        default:
            RT_ASSERT(0);
            break;
    }
    if (va != (void *)ma->addr)
    {
        return -1;
    }
    if (ma->unmap_all_addr)
    {
        rt_mm_lock();
        lwp_map_area_set_unmap_all_addr(new_lwp, va, (void *)ma->unmap_all_addr);
        rt_mm_unlock();
    }
    return 0;
}

int lwp_unmap_user_phy(struct rt_lwp *lwp, void *va)
{
    return lwp_unmap_user(lwp, va);
}

int lwp_unmap_user_type(struct rt_lwp *lwp, void *va)
{
    return lwp_unmap_user(lwp, va);
}

void *lwp_map_user(struct rt_lwp *lwp, void *map_va, size_t map_size, int text)
{
    void *ret = RT_NULL;
    size_t offset = 0;

    if (!map_size)
    {
        return 0;
    }
    offset = (size_t)map_va & ARCH_PAGE_MASK;
    map_size += (offset + ARCH_PAGE_SIZE - 1);
    map_size &= ~ARCH_PAGE_MASK;
    map_va = (void *)((size_t)map_va & ~ARCH_PAGE_MASK);

    rt_mm_lock();
    ret = _lwp_map_user(lwp, map_va, map_size, text);
    rt_mm_unlock();
    if (ret)
    {
        ret = (void *)((char *)ret + offset);
    }
    return ret;
}

static void *_lwp_map_user_type(struct rt_lwp *lwp, void *map_va, void *map_pa, size_t map_size, int cached, int type)
{
    void *va = RT_NULL;
    rt_mmu_info *m_info = &lwp->mmu_info;
    size_t attr = 0;
    int ret = 0;

    if (cached)
    {
        attr = MMU_MAP_U_RWCB;
        if (type == MM_AREA_TYPE_PHY)
        {
            type = MM_AREA_TYPE_PHY_CACHED;
        }
    }
    else
    {
        attr = MMU_MAP_U_RW;
    }

    va = rt_hw_mmu_map(m_info, map_va, map_pa, map_size, attr);
    if (va)
    {
        ret = lwp_map_area_insert(&lwp->map_area, (size_t)va, map_size, type);
        if (ret != 0)
        {
            unmap_range(lwp, va, map_size, 0);
            return 0;
        }
    }
    return va;
}

void *lwp_map_user_type(struct rt_lwp *lwp, void *map_va, void *map_pa, size_t map_size, int cached, int type)
{
    void *ret = RT_NULL;
    size_t offset = 0;

    if (!map_size)
    {
        return 0;
    }
    if (map_va)
    {
        if (((size_t)map_va & ARCH_PAGE_MASK) != ((size_t)map_pa & ARCH_PAGE_MASK))
        {
            return 0;
        }
    }
    offset = (size_t)map_pa & ARCH_PAGE_MASK;
    map_size += (offset + ARCH_PAGE_SIZE - 1);
    map_size &= ~ARCH_PAGE_MASK;
    map_pa = (void *)((size_t)map_pa & ~ARCH_PAGE_MASK);

    rt_mm_lock();
    ret = _lwp_map_user_type(lwp, map_va, map_pa, map_size, cached, type);
    rt_mm_unlock();
    if (ret)
    {
        ret = (void *)((char *)ret + offset);
    }
    return ret;
}

void *lwp_map_user_phy(struct rt_lwp *lwp, void *map_va, void *map_pa, size_t map_size, int cached)
{
    return lwp_map_user_type(lwp, map_va, map_pa, map_size, cached, MM_AREA_TYPE_PHY);
}

rt_base_t lwp_brk(void *addr)
{
    rt_base_t ret = -1;
    struct rt_lwp *lwp = RT_NULL;

    rt_mm_lock();
    lwp = rt_thread_self()->lwp;

    if ((size_t)addr <= lwp->end_heap)
    {
        ret = (rt_base_t)lwp->end_heap;
    }
    else
    {
        size_t size = 0;
        void *va = RT_NULL;

        if ((size_t)addr <= USER_HEAP_VEND)
        {
            size = (((size_t)addr - lwp->end_heap) + ARCH_PAGE_SIZE - 1) & ~ARCH_PAGE_MASK;
            va = lwp_map_user(lwp, (void *)lwp->end_heap, size, 0);
        }
        if (va)
        {
            lwp->end_heap += size;
            ret = lwp->end_heap;
        }
    }
    rt_mm_unlock();
    return ret;
}

#define MAP_ANONYMOUS  0x20
#define LWP_TLS_WORKAROUND_LENGTH_0 0x1b0
#define LWP_TLS_WORKAROUND_LENGTH_1 0xb70

void* lwp_mmap2(void *addr, size_t length, int prot,
        int flags, int fd, off_t pgoffset)
{
    void *ret = (void *)-1;
    void *map_va;

    if (fd == -1)
    {
        rt_mm_lock();
        if (length == LWP_TLS_WORKAROUND_LENGTH_0 ||
            length == LWP_TLS_WORKAROUND_LENGTH_1)  /* TODO: workround for tls bug, by aozima, need fix asap. */
        {
            ret = lwp_map_user(lwp_self(), addr, length + ARCH_PAGE_SIZE, 0);
            if (ret)
            {
                map_va = ret;
                ret = (void *)((char *)ret + ARCH_PAGE_SIZE);
                lwp_map_area_set_unmap_all_addr(lwp_self(), map_va, ret);
            }
        }
        else
        {
            ret = lwp_map_user(lwp_self(), addr, length, 0);
        }
        rt_mm_unlock();

        if (ret)
        {
            if ((flags & MAP_ANONYMOUS) != 0)
            {
                rvv_memset(ret, 0, length);
            }
        }
        else
        {
            ret = (void *)-1;
        }
    }
    else
    {
        struct dfs_fd *d;

        d = fd_get(fd);
        if (d && d->fnode->type == FT_DEVICE)
        {
            struct dfs_mmap2_args mmap2;
        
            mmap2.addr = addr;
            mmap2.length = length;
            mmap2.prot = prot;
            mmap2.flags = flags;
            mmap2.pgoffset = pgoffset;
            mmap2.ret = (void*) -1;

            if (dfs_file_mmap2(d, &mmap2) == 0)
            {
                ret = mmap2.ret;
            }
        }
    }

    return ret;
}

int lwp_munmap(void *addr, size_t length)
{
    struct rt_lwp *lwp;
    struct lwp_avl_struct *node;
    int ret;

    if (!addr || length == 0 || length > (size_t)-1 - ARCH_PAGE_MASK)
    {
        return -EINVAL;
    }
    length = (length + ARCH_PAGE_MASK) & ~ARCH_PAGE_MASK;
    if (!lwp_user_range_valid(addr, length))
    {
        return -EINVAL;
    }

    rt_mm_lock();
    lwp = lwp_self();
    if (!lwp)
    {
        ret = -EINVAL;
    }
    else
    {
        node = lwp_map_find(lwp->map_area, (size_t)addr);
        if (node)
        {
            struct rt_mm_area_struct *ma = (struct rt_mm_area_struct *)node->data;

            if (ma->unmap_all_addr == (size_t)addr)
            {
                addr = (void *)ma->addr;
                length = ma->size;
            }
        }
        ret = _lwp_unmap_user_range(lwp, addr, length);
    }
    rt_mm_unlock();

    return ret;
}

size_t lwp_get_from_user(void *dst, void *src, size_t size)
{
    struct rt_lwp *lwp = RT_NULL;
    rt_mmu_info *m_info = RT_NULL;

    if (!lwp_user_range_valid(src, size))
    {
        return 0;
    }

    lwp = lwp_self();
    if (!lwp)
    {
        return 0;
    }
    m_info = &lwp->mmu_info;

    return lwp_data_get(m_info, dst, src, size);
}

int lwp_get_from_user_ex(void *dst, void *src, size_t size)
{
    if (size == 0)
    {
        return 0;
    }
    if (!dst || !src)
    {
        return -1;
    }
    if (lwp_self() != NULL)
    {
        if (!lwp_user_accessable(src, size) ||
            size != lwp_get_from_user(dst, src, size))
        {
            return -1;
        }
        return 0;
    }

    rvv_memcpy(dst, src, size);

    return 0;
}

size_t lwp_put_to_user(void *dst, void *src, size_t size)
{
    struct rt_lwp *lwp = RT_NULL;
    rt_mmu_info *m_info = RT_NULL;

    if (!lwp_user_range_valid(dst, size))
    {
        return 0;
    }

    lwp = lwp_self();
    if (!lwp)
    {
        return 0;
    }
    m_info = &lwp->mmu_info;
    return lwp_data_put(m_info, dst, src, size);
}
 
int lwp_put_to_user_ex(void *dst, void *src, size_t size)
{
    if (size == 0)
    {
        return 0;
    }
    if (!dst || !src)
    {
        return -1;
    }
    if (lwp_self() != NULL)
    {
        if (!lwp_user_accessable(dst, size) ||
            size != lwp_put_to_user(dst, src, size))
        {
            return -1;
        }
        return 0;
    }

    rvv_memcpy(dst, src, size);

    return 0;
}

int lwp_user_accessable(void *addr, size_t size)
{
    void *addr_start = RT_NULL, *addr_end = RT_NULL, *next_page = RT_NULL;
    void *tmp_addr = RT_NULL;
    struct rt_lwp *lwp = lwp_self();
    rt_mmu_info *mmu_info = RT_NULL;

    if (!lwp)
    {
        return 0;
    }
    if (!lwp_user_range_valid(addr, size))
    {
        return 0;
    }
    addr_start = addr;
    addr_end = (void *)((char *)addr + size);

    mmu_info = &lwp->mmu_info;
    next_page = (void *)(((size_t)addr_start + ARCH_PAGE_SIZE) & ~(ARCH_PAGE_SIZE - 1));
    while (addr_start < addr_end)
    {
        size_t len = (char *)next_page - (char *)addr_start;

        if (size < len)
        {
            len = size;
        }
        tmp_addr = rt_hw_mmu_v2p(mmu_info, addr_start);
        if (!tmp_addr)
        {
            return 0;
        }
        addr_start = (void *)((char *)addr_start + len);
        size -= len;
        if (addr_start < addr_end)
        {
            next_page = (void *)((char *)next_page + ARCH_PAGE_SIZE);
        }
    }
    return 1;
}

/* src is in mmu_info space, dst is in current thread space */
size_t lwp_data_get(rt_mmu_info *mmu_info, void *dst, void *src, size_t size)
{
    size_t copy_len = 0;
    void *addr_start = RT_NULL, *addr_end = RT_NULL, *next_page = RT_NULL;
    void *tmp_dst = RT_NULL, *tmp_src = RT_NULL;

    if (!size || !dst)
    {
        return 0;
    }
    tmp_dst = dst;
    addr_start = src;
    addr_end = (void *)((char *)src + size);
    next_page = (void *)(((size_t)addr_start + ARCH_PAGE_SIZE) & ~(ARCH_PAGE_SIZE - 1));
    do
    {
        size_t len = (char *)next_page - (char *)addr_start;

        if (size < len)
        {
            len = size;
        }
        tmp_src = rt_hw_mmu_v2p(mmu_info, addr_start);
        if (!tmp_src)
        {
            break;
        }
        tmp_src = (void *)((char *)tmp_src - PV_OFFSET);
        rvv_memcpy(tmp_dst, tmp_src, len);
        tmp_dst = (void *)((char *)tmp_dst + len);
        addr_start = (void *)((char *)addr_start + len);
        size -= len;
        if (addr_start < addr_end)
        {
            next_page = (void *)((char *)next_page + ARCH_PAGE_SIZE);
        }
        copy_len += len;
    } while (addr_start < addr_end);
    return copy_len;
}

/* dst is in mmu_info space, src is in current thread space */
size_t lwp_data_put(rt_mmu_info *mmu_info, void *dst, void *src, size_t size)
{
    size_t copy_len = 0;
    void *addr_start = RT_NULL, *addr_end = RT_NULL, *next_page = RT_NULL;
    void *tmp_dst = RT_NULL, *tmp_src = RT_NULL;

    if (!size || !dst)
    {
        return 0;
    }
    tmp_src = src;
    addr_start = dst;
    addr_end = (void *)((char *)dst + size);
    next_page = (void *)(((size_t)addr_start + ARCH_PAGE_SIZE) & ~(ARCH_PAGE_SIZE - 1));
    do
    {
        size_t len = (char *)next_page - (char *)addr_start;

        if (size < len)
        {
            len = size;
        }
        tmp_dst = rt_hw_mmu_v2p(mmu_info, addr_start);
        if (!tmp_dst)
        {
            break;
        }
        tmp_dst = (void *)((char *)tmp_dst - PV_OFFSET);
        rvv_memcpy(tmp_dst, tmp_src, len);
        tmp_src = (void *)((char *)tmp_src + len);
        addr_start = (void *)((char *)addr_start + len);
        size -= len;
        if (addr_start < addr_end)
        {
            next_page = (void *)((char *)next_page + ARCH_PAGE_SIZE);
        }
        copy_len += len;
    } while (addr_start < addr_end);
    return copy_len;
}

void lwp_data_cache_flush(rt_mmu_info *mmu_info, void *vaddr, size_t size)
{
    void *paddr = RT_NULL;

    paddr = rt_hw_mmu_v2p(mmu_info, vaddr);
    paddr = (void *)((char *)paddr - PV_OFFSET);

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, paddr, size);
}
#endif
