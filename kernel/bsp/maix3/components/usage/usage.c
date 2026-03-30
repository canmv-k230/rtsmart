/**
 * @file usage.c
 * @author  ()
 * @brief
 * @version 1.0
 * @date 2022-11-18
 *
 * @copyright Copyright (c) 2022 Canaan Inc.
 *
 */
#include "usage.h"
#include "rvv_ops.h"
#include "tick.h"
#include <lwp.h>
#include <rthw.h>
#include <rtthread.h>

#define USAGE_ACC_MAX_COUNT     10
#define USAGE_CAL_PERIOD        1000 /*ms*/
#define MAX_USAGE_CAL_PERIOD    2000 /*ms*/
#define MIN_USAGE_CAL_PERIOD    100 /*ms*/
#define THREAD_NBR_MAX          100 // max the thread number
#define INVALID_USAGE           101
#define USAGE_THREAD_STACK_SIZE 8192
#define USAGE_THREAD_TIMESLICE  10
#define USAGE_THREAD_PRIORITY   (RT_THREAD_PRIORITY_MAX - 2)

#ifdef RT_USING_SMP
static struct rt_spinlock g_lock;
#else
static rt_uint32_t g_lock;
#endif

typedef struct {
    char*       lwp_name;
    char*       thread_name; // thead name
    rt_thread_t thread;
    int         pid;
    int         tid;
    rt_uint8_t  priority;
    rt_ubase_t  time;
    rt_uint8_t  usage; // thread usage percent 100%
} thread_usage_info;

static rt_ubase_t        schedule_last_time;
static thread_usage_info thread_info[THREAD_NBR_MAX] = { 0 };
static rt_ubase_t        total_time_last             = 0;
static int               thread_info_index           = 0;
static rt_thread_t       g_usage_thread              = RT_NULL;
static rt_thread_t       g_idle_thread               = RT_NULL;
static rt_sem_t          g_usage_wakeup              = RT_NULL;
static rt_sem_t          g_usage_sample_done         = RT_NULL;
static int               g_usage_period              = 0; /*ms*/
static rt_bool_t         top_command_enable          = RT_FALSE;
static rt_bool_t         g_usage_sample_pending      = RT_FALSE;
static rt_bool_t         g_usage_sample_active       = RT_FALSE;
static rt_bool_t         g_usage_ready               = RT_FALSE;
static rt_uint8_t        g_cpu_usage                 = INVALID_USAGE;

static thread_usage_info* usage_snapshot_alloc(void)
{
    return (thread_usage_info*)rt_malloc(sizeof(thread_usage_info) * THREAD_NBR_MAX);
}

static rt_base_t usage_lock(void)
{
#ifdef RT_USING_SMP
    return rt_spin_lock_irqsave(&g_lock);
#else
    rt_enter_critical();
    return 0;
#endif
}

static void usage_unlock(rt_base_t level)
{
#ifdef RT_USING_SMP
    rt_spin_unlock_irqrestore(&g_lock, level);
#else
    (void)level;
    rt_exit_critical();
#endif
}

static int thread_stats_copy(thread_usage_info* snapshot, rt_size_t capacity)
{
    rt_base_t level;
    rt_size_t count;

    level = usage_lock();
    if (!g_usage_ready) {
        usage_unlock(level);
        return -1;
    }

    count = thread_info_index;
    if (count > capacity) {
        count = capacity;
    }

    rvv_memcpy(snapshot, thread_info, count * sizeof(*snapshot));
    usage_unlock(level);

    return (int)count;
}

static void thread_stats_print_snapshot(thread_usage_info* snapshot, int count)
{
    thread_usage_info* p_info;

    rt_kprintf("%-5s %-16s %-5s %-5s %-16s %s\n", "pid", "lwp", "tid", "prio", "thread", "usage");
    for (int i = 0; i < count; i++) {
        p_info = &snapshot[i];
        if (p_info->thread_name == RT_NULL) {
            continue;
        }

        rt_kprintf("%-5d %-16.16s %-5d %-5u %-16.16s ", p_info->pid, p_info->lwp_name, p_info->tid, p_info->priority,
                   p_info->thread_name);
        if (p_info->usage > 0) {
            rt_kprintf("%2u%%\n", p_info->usage);
        } else {
            rt_kprintf("<1%%\n");
        }
    }
}

static rt_thread_t usage_get_idle_thread(void)
{
    if (g_idle_thread == RT_NULL) {
        g_idle_thread = rt_thread_find("tidle0");
    }

    return g_idle_thread;
}

static rt_uint8_t usage_calculate_percent(rt_ubase_t time, rt_ubase_t total_time)
{
    if (total_time > 0) {
        total_time /= 100;
        if (total_time > 0) {
            return time / total_time;
        }
    }

    return 0;
}

static rt_uint8_t thread_snapshot_cpu_usage(thread_usage_info* snapshot, int count)
{
    for (int i = 0; i < count; i++) {
        if ((snapshot[i].thread_name != RT_NULL) && !strcmp(snapshot[i].thread_name, "tidle0")) {
            return 100 - snapshot[i].usage;
        }
    }

    return INVALID_USAGE;
}

static void usage_reset_protected(void)
{
    struct rt_list_node* node;
    struct rt_list_node* list;
    struct rt_thread*    thread;

    rvv_memset(thread_info, 0, sizeof(thread_info));
    thread_info_index = 0;
    g_usage_ready     = RT_FALSE;
    g_cpu_usage       = INVALID_USAGE;

    list = &(rt_object_get_information(RT_Object_Class_Thread)->object_list);
    for (node = list->next; node != list; node = node->next) {
        thread           = rt_list_entry(node, struct rt_thread, list);
        thread->run_tick = 0;
    }

    total_time_last    = cpu_ticks();
    schedule_last_time = total_time_last;
}

static int thread_collect_usage(thread_usage_info* snapshot, rt_size_t capacity, rt_ubase_t* total_time)
{
    rt_ubase_t           time;
    struct rt_list_node* node;
    struct rt_list_node* list;
    struct rt_thread*    thread;
    struct rt_thread*    cur_thread;
    rt_ubase_t           i;
    rt_base_t            level;

    level       = usage_lock();
    time        = cpu_ticks();
    *total_time = time - total_time_last;

    cur_thread = rt_thread_self();
    if ((cur_thread != RT_NULL) && (schedule_last_time != 0)) {
        cur_thread->run_tick += time - schedule_last_time;
    }

    list = &(rt_object_get_information(RT_Object_Class_Thread)->object_list);
    for (i = 0, node = list->next; (node != list) && (i < capacity); node = node->next, i++) {
        thread                  = rt_list_entry(node, struct rt_thread, list);
        snapshot[i].thread      = thread;
        snapshot[i].thread_name = thread->name;
        snapshot[i].lwp_name    = "(kernel)";
        snapshot[i].pid         = 0;
#ifdef RT_USING_USERSPACE
        {
            struct rt_lwp* lwp = thread->lwp;

            if (lwp != RT_NULL) {
                snapshot[i].lwp_name = lwp->cmd;
                snapshot[i].pid      = lwp->pid;
            }
        }
#endif
        snapshot[i].time     = thread->run_tick;
        snapshot[i].tid      = thread->tid;
        snapshot[i].priority = thread->current_priority;
        snapshot[i].usage    = 0;
        thread->run_tick     = 0;
    }

    total_time_last    = time;
    schedule_last_time = time;
    usage_unlock(level);

    return (int)i;
}

static rt_uint8_t thread_collect_cpu_usage(void)
{
    rt_ubase_t  time;
    rt_ubase_t  total_time;
    rt_ubase_t  idle_time;
    rt_base_t   level;
    rt_uint8_t  cpu_usage;
    rt_thread_t idle_thread;
    rt_thread_t cur_thread;

    cpu_usage   = INVALID_USAGE;
    idle_thread = usage_get_idle_thread();

    level      = usage_lock();
    time       = cpu_ticks();
    total_time = time - total_time_last;
    cur_thread = rt_thread_self();
    if ((cur_thread != RT_NULL) && (schedule_last_time != 0)) {
        cur_thread->run_tick += time - schedule_last_time;
    }

    if (idle_thread != RT_NULL) {
        idle_time             = idle_thread->run_tick;
        idle_thread->run_tick = 0;
        cpu_usage             = 100 - usage_calculate_percent(idle_time, total_time);
    }

    total_time_last    = time;
    schedule_last_time = time;
    g_cpu_usage        = cpu_usage;
    usage_unlock(level);

    return cpu_usage;
}

static void thread_calculate_usage(thread_usage_info* snapshot, int count, rt_ubase_t total_time)
{
    for (int i = 0; i < count; i++) {
        snapshot[i].usage = usage_calculate_percent(snapshot[i].time, total_time);
    }
}

static void thread_publish_usage(thread_usage_info* snapshot, int count)
{
    rt_base_t level;

    level = usage_lock();
    rvv_memcpy(thread_info, snapshot, count * sizeof(*snapshot));
    if (count < THREAD_NBR_MAX) {
        rvv_memset(&thread_info[count], 0, (THREAD_NBR_MAX - count) * sizeof(*snapshot));
    }
    thread_info_index = count;
    g_usage_ready     = RT_TRUE;
    usage_unlock(level);
}

void init_cal_usage_time(void) { schedule_last_time = cpu_ticks(); }

void thread_stats_scheduler_hook(struct rt_thread* from, struct rt_thread* to)
{
    rt_ubase_t time;

    (void)to;
    RT_ASSERT(schedule_last_time != 0);

#ifdef RT_USING_SMP
    rt_base_t level;

    level = rt_spin_lock_irqsave(&g_lock);
    time  = cpu_ticks();
    if (from != RT_NULL) {
        from->run_tick += time - schedule_last_time;
    }
    schedule_last_time = time;
    rt_spin_unlock_irqrestore(&g_lock, level);
#else
    time = cpu_ticks();
    if (from != RT_NULL) {
        from->run_tick += time - schedule_last_time;
    }
    schedule_last_time = time;
#endif
}

static void thread_stats_print(void)
{
    thread_usage_info* snapshot;
    int                count;

    snapshot = usage_snapshot_alloc();
    if (snapshot == RT_NULL) {
        rt_kprintf("usage snapshot alloc failed\n");
        return;
    }

    count = thread_stats_copy(snapshot, THREAD_NBR_MAX);
    if (count < 0) {
        rt_free(snapshot);
        rt_kprintf("usage data is not ready\n");
        return;
    }

    thread_stats_print_snapshot(snapshot, count);
    rt_free(snapshot);
}

static rt_err_t usage_wait_full_snapshot(void)
{
    rt_tick_t timeout;
    rt_base_t level;
    rt_bool_t top_enabled;
    rt_bool_t ready;

    while (rt_sem_take(g_usage_sample_done, RT_WAITING_NO) == RT_EOK) { }

    level       = usage_lock();
    top_enabled = top_command_enable;
    ready       = g_usage_ready;
    if (!top_enabled) {
        g_usage_sample_pending = RT_TRUE;
        g_usage_sample_active  = RT_FALSE;
        g_usage_ready          = RT_FALSE;
    }
    usage_unlock(level);

    if (top_enabled && ready) {
        return RT_EOK;
    }

    timeout = rt_tick_from_millisecond(g_usage_period * 2);
    if (timeout == 0) {
        timeout = 1;
    }

    rt_sem_release(g_usage_wakeup);
    if (rt_sem_take(g_usage_sample_done, timeout) != RT_EOK) {
        return RT_ETIMEOUT;
    }

    return RT_EOK;
}

static void usage_thread_entry(void* parameter)
{
    thread_usage_info* snapshot;

    (void)parameter;

    snapshot = usage_snapshot_alloc();
    if (snapshot == RT_NULL) {
        rt_kprintf("usage worker alloc failed\n");
        return;
    }

    while (1) {
        rt_tick_t timeout;
        rt_base_t level;
        rt_bool_t need_full_sample;

        timeout = rt_tick_from_millisecond(g_usage_period);
        rt_sem_take(g_usage_wakeup, timeout);

        level            = usage_lock();
        need_full_sample = top_command_enable || g_usage_sample_pending;
        usage_unlock(level);

        if (need_full_sample) {
            rt_ubase_t total_time;
            int        count;

            level = usage_lock();
            if (!g_usage_sample_active) {
                usage_reset_protected();
                g_usage_sample_active = RT_TRUE;
                usage_unlock(level);
                continue;
            }
            usage_unlock(level);

            count = thread_collect_usage(snapshot, THREAD_NBR_MAX, &total_time);
            thread_calculate_usage(snapshot, count, total_time);
            thread_publish_usage(snapshot, count);

            level       = usage_lock();
            g_cpu_usage = thread_snapshot_cpu_usage(snapshot, count);
            if (!top_command_enable) {
                g_usage_sample_pending = RT_FALSE;
                g_usage_sample_active  = RT_FALSE;
            }
            usage_unlock(level);

            rt_sem_release(g_usage_sample_done);

            if (top_command_enable) {
                rt_kprintf("\e[1;1H\e[2J");
                thread_stats_print_snapshot(snapshot, count);
            }
        } else {
            level                 = usage_lock();
            g_usage_sample_active = RT_FALSE;
            usage_unlock(level);
            thread_collect_cpu_usage();
        }
    }
}

rt_uint8_t sys_cpu_usage(rt_uint8_t cpu_id)
{
    rt_base_t  level;
    rt_uint8_t usage;

    if (cpu_id != 0) {
        return INVALID_USAGE;
    }

    level = usage_lock();
    usage = g_cpu_usage;
    usage_unlock(level);
    return usage;
}

rt_uint8_t sys_thread_usage(int tid)
{
    thread_usage_info* p_info;
    rt_base_t          level;

    level = usage_lock();
    if (!g_usage_ready) {
        usage_unlock(level);
        return INVALID_USAGE;
    }

    for (rt_ubase_t i = 0; i < thread_info_index; i++) {
        p_info = &thread_info[i];
        if (p_info->tid == tid) {
            usage_unlock(level);
            return p_info->usage;
        }
    }

    usage_unlock(level);
    return INVALID_USAGE;
}

static rt_uint8_t sys_kernel_thread_usage_by_name(const char* thread_name)
{
    thread_usage_info* p_info;
    rt_base_t          level;

    if ((thread_name == RT_NULL) || (thread_name[0] == '\0')) {
        return INVALID_USAGE;
    }

    level = usage_lock();
    if (!g_usage_ready) {
        usage_unlock(level);
        return INVALID_USAGE;
    }

    for (rt_ubase_t i = 0; i < thread_info_index; i++) {
        p_info = &thread_info[i];
        if ((p_info->pid == 0) && (p_info->thread_name != RT_NULL) && !strcmp(p_info->thread_name, thread_name)) {
            usage_unlock(level);
            return p_info->usage;
        }
    }

    usage_unlock(level);
    return INVALID_USAGE;
}

static rt_bool_t thread_usage_parse_tid(const char* arg, int* tid)
{
    int value;

    if ((arg == RT_NULL) || (arg[0] == '\0') || (tid == RT_NULL)) {
        return RT_FALSE;
    }

    value = 0;
    for (int i = 0; arg[i] != '\0'; i++) {
        char ch;

        ch = arg[i];
        if ((ch < '0') || (ch > '9')) {
            return RT_FALSE;
        }

        value = value * 10 + (ch - '0');
    }

    *tid = value;
    return RT_TRUE;
}

rt_err_t usage_set_period(int mill_sec)
{
    rt_base_t level;

    if (g_usage_thread && (mill_sec <= MAX_USAGE_CAL_PERIOD) && (mill_sec >= MIN_USAGE_CAL_PERIOD)) {
        g_usage_period         = mill_sec;
        level                  = usage_lock();
        g_usage_sample_pending = RT_FALSE;
        g_usage_sample_active  = RT_FALSE;
        usage_reset_protected();
        usage_unlock(level);
        rt_sem_release(g_usage_wakeup);
        rt_kprintf("%s  mill_sec:%d success\n", __func__, mill_sec);
        return RT_EOK;
    }
    rt_kprintf("%s  mill_sec:%d failed\n", __func__, mill_sec);
    return RT_ERROR;
}

rt_int32_t cpu_usage_init(void)
{
#ifdef RT_USING_SMP
    rt_spin_lock_init(&g_lock);
#endif

    rt_scheduler_sethook(thread_stats_scheduler_hook);
    g_usage_period     = USAGE_CAL_PERIOD;
    total_time_last    = cpu_ticks();
    schedule_last_time = total_time_last;

    g_usage_wakeup = rt_sem_create("usage_sem", 0, RT_IPC_FLAG_FIFO);
    RT_ASSERT(g_usage_wakeup != RT_NULL);

    g_usage_sample_done = rt_sem_create("usage_done", 0, RT_IPC_FLAG_FIFO);
    RT_ASSERT(g_usage_sample_done != RT_NULL);

    g_usage_thread = rt_thread_create("usage_thread", usage_thread_entry, RT_NULL, USAGE_THREAD_STACK_SIZE,
                                      USAGE_THREAD_PRIORITY, USAGE_THREAD_TIMESLICE);
    RT_ASSERT(g_usage_thread != RT_NULL);
    rt_thread_startup(g_usage_thread);

    return 0;
}
INIT_COMPONENT_EXPORT(cpu_usage_init);

int usage_show(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if (usage_wait_full_snapshot() != RT_EOK) {
        rt_kprintf("usage data collect timeout\n");
        return -RT_ERROR;
    }

    thread_stats_print();
    return 0;
}
MSH_CMD_EXPORT(usage_show, show all thread usage);

int top(void)
{
    rt_base_t level;

    level                 = usage_lock();
    top_command_enable    = RT_TRUE;
    g_usage_sample_active = RT_FALSE;
    g_usage_ready         = RT_FALSE;
    usage_unlock(level);
    rt_sem_release(g_usage_wakeup);
    return 0;
}
MSH_CMD_EXPORT(top, show all thread usage at set intervals);

int top_exit(void)
{
    rt_base_t level;

    level                 = usage_lock();
    top_command_enable    = RT_FALSE;
    g_usage_sample_active = RT_FALSE;
    usage_unlock(level);
    return 0;
}
MSH_CMD_EXPORT(top_exit, stop show all thread usage at set intervals);

void thread_usage(int argc, char** argv)
{
    int        tid;
    rt_uint8_t usage;
    rt_bool_t  use_tid;

    if (argc < 2) {
        rt_kprintf("please input thread tid or kernel thread name\n");
        return;
    }

    if (usage_wait_full_snapshot() != RT_EOK) {
        rt_kprintf("usage data collect timeout\n");
        return;
    }

    use_tid = thread_usage_parse_tid(argv[1], &tid);
    if (use_tid) {
        usage = sys_thread_usage(tid);
    } else {
        usage = sys_kernel_thread_usage_by_name(argv[1]);
    }

    if (usage == INVALID_USAGE) {
        if (use_tid) {
            rt_kprintf("thread tid:%d can not get usage\n", tid);
        } else {
            rt_kprintf("kernel thread name:%s can not get usage\n", argv[1]);
        }
    } else if (usage > 0) {
        if (use_tid) {
            rt_kprintf("thread tid:%d usage:%d%%\n", tid, usage);
        } else {
            rt_kprintf("kernel thread name:%s usage:%d%%\n", argv[1], usage);
        }
    } else {
        if (use_tid) {
            rt_kprintf("thread tid:%d usage:<1%%\n", tid);
        } else {
            rt_kprintf("kernel thread name:%s usage:<1%%\n", argv[1]);
        }
    }
}
MSH_CMD_EXPORT(thread_usage, get thread usage by tid or kernel thread name);

void sys_usage(void)
{
    rt_uint8_t usage;

    usage = sys_cpu_usage(0);
    if (usage == INVALID_USAGE) {
        rt_kprintf("sys usage is not ready\n");
    } else if (usage > 0) {
        rt_kprintf("sys usage:%d%%\n", usage);
    } else {
        rt_kprintf("sys usage:<1%%\n", usage);
    }
}
MSH_CMD_EXPORT(sys_usage, get sys usage);

void set_usage_period(int argc, char** argv)
{
    rt_uint32_t mill_sec;
    if (argc < 2) {
        rt_kprintf("please input period(ms)<%d ~ %d>\n", MIN_USAGE_CAL_PERIOD, MAX_USAGE_CAL_PERIOD);
        return;
    }
    mill_sec = atoi(argv[1]);
    usage_set_period(mill_sec);
}
MSH_CMD_EXPORT(set_usage_period, set the period of the system usage ms);
