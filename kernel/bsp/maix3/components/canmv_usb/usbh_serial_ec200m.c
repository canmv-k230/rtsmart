#include <dfs.h>
#include <dfs_file.h>
#include <dfs_private.h>
#include <unistd.h>

#include <rtthread.h>
#include "usb_osal.h"

#define SERIAL_MONITOR_MAX (4)
#define SERIAL_DEV_PATH_LEN (32)
#define READ_LEN (150)
#define SERIAL_MONITOR_STOP_WAIT_MS (200)

struct serial_monitor {
    int fd;
    bool running;
    usb_osal_thread_t thread;
    char dev_path[SERIAL_DEV_PATH_LEN];
};

static struct serial_monitor g_serial_monitors[SERIAL_MONITOR_MAX];
static bool g_serial_monitors_inited = false;
static int g_selected_monitor = -1;

static void serial_monitor_release(struct serial_monitor *monitor)
{
    if (monitor->fd >= 0) {
        close(monitor->fd);
        monitor->fd = -1;
    }

    monitor->running = false;
    monitor->dev_path[0] = '\0';
}

static void serial_monitor_init(void)
{
    if (g_serial_monitors_inited) {
        return;
    }

    for (int i = 0; i < SERIAL_MONITOR_MAX; i++) {
        g_serial_monitors[i].fd = -1;
    }

    g_serial_monitors_inited = true;
}

static bool serial_monitor_is_active(const struct serial_monitor *monitor)
{
    return monitor->running && (monitor->fd >= 0);
}

static bool serial_monitor_slot_available(const struct serial_monitor *monitor)
{
    return !monitor->running && (monitor->fd < 0);
}

static int serial_monitor_find_by_path(const char *dev_path)
{
    for (int i = 0; i < SERIAL_MONITOR_MAX; i++) {
        if ((g_serial_monitors[i].fd >= 0) &&
            (strncmp(g_serial_monitors[i].dev_path, dev_path, SERIAL_DEV_PATH_LEN) == 0)) {
            return i;
        }
    }

    return -1;
}

static int serial_monitor_find_free_slot(void)
{
    for (int i = 0; i < SERIAL_MONITOR_MAX; i++) {
        if (serial_monitor_slot_available(&g_serial_monitors[i])) {
            return i;
        }
    }

    return -1;
}

static bool serial_monitor_wait_stopped(struct serial_monitor *monitor)
{
    for (int elapsed = 0; elapsed < SERIAL_MONITOR_STOP_WAIT_MS; elapsed += 10) {
        if (serial_monitor_slot_available(monitor)) {
            return true;
        }

        rt_thread_mdelay(10);
    }

    return serial_monitor_slot_available(monitor);
}

static int serial_monitor_first_active(void)
{
    for (int i = 0; i < SERIAL_MONITOR_MAX; i++) {
        if (serial_monitor_is_active(&g_serial_monitors[i])) {
            return i;
        }
    }

    return -1;
}

static void serial_monitor_update_selected(void)
{
    if ((g_selected_monitor >= 0) &&
        (g_selected_monitor < SERIAL_MONITOR_MAX) &&
        serial_monitor_is_active(&g_serial_monitors[g_selected_monitor])) {
        return;
    }

    g_selected_monitor = serial_monitor_first_active();
}

static struct serial_monitor *serial_monitor_get_selected(void)
{
    serial_monitor_update_selected();
    if (g_selected_monitor < 0) {
        return RT_NULL;
    }

    return &g_serial_monitors[g_selected_monitor];
}

static void serial_monitor_stop_one(struct serial_monitor *monitor)
{
    if (serial_monitor_is_active(monitor)) {
        monitor->running = false;
        serial_monitor_release(monitor);
    }
}

static void serial_monitor_stop_all(void)
{
    for (int i = 0; i < SERIAL_MONITOR_MAX; i++) {
        serial_monitor_stop_one(&g_serial_monitors[i]);
    }
}

static void serial_monitor_list(void)
{
    int active = 0;

    serial_monitor_update_selected();
    for (int i = 0; i < SERIAL_MONITOR_MAX; i++) {
        if (!serial_monitor_is_active(&g_serial_monitors[i])) {
            continue;
        }

        rt_kprintf("%c %s\n", (i == g_selected_monitor) ? '*' : ' ', g_serial_monitors[i].dev_path);
        active++;
    }

    if (active == 0) {
        rt_kprintf("no active readers\n");
    }
}

static void rd_thread(void *argument)
{
    struct serial_monitor *monitor = (struct serial_monitor *)argument;
    char buf[READ_LEN];

    while (monitor->running) {
        int ret = read(monitor->fd, buf, sizeof(buf));

        if (ret > 0) {
            rt_kprintf("[%s] ", monitor->dev_path);
            for (int i = 0; i < ret; i++) {
                rt_kprintf("%c", buf[i]);
            }
            continue;
        }

        if (ret < 0) {
            rt_kprintf("[%s] reader stopped (%d)\n", monitor->dev_path, ret);
            break;
        }

        if (!monitor->running) {
            break;
        }

        rt_thread_mdelay(20);
    }

    serial_monitor_release(monitor);
    monitor->thread = RT_NULL;
    serial_monitor_update_selected();
}

void rd(char *dev_path)
{
    struct serial_monitor *monitor;
    usb_osal_thread_t thread;
    int fd;
    int monitor_idx;

    serial_monitor_init();

    if (strcmp(dev_path, "stop") == 0) {
        serial_monitor_stop_all();
        return;
    }

    if (strcmp(dev_path, "list") == 0) {
        serial_monitor_list();
        return;
    }

    if (strlen(dev_path) >= SERIAL_DEV_PATH_LEN) {
        rt_kprintf("device path is too long\n");
        return;
    }

    monitor_idx = serial_monitor_find_by_path(dev_path);
    if (monitor_idx >= 0) {
        monitor = &g_serial_monitors[monitor_idx];
        if (serial_monitor_is_active(monitor)) {
            g_selected_monitor = monitor_idx;
            rt_kprintf("monitoring %s\n", monitor->dev_path);
            return;
        }

        if (serial_monitor_wait_stopped(monitor)) {
            monitor_idx = -1;
        } else {
            rt_kprintf("reader on %s is stopping, retry shortly\n", dev_path);
            return;
        }
    }

    monitor_idx = serial_monitor_find_free_slot();
    if (monitor_idx < 0) {
        rt_kprintf("too many readers, run 'rd list' or 'rd stop'\n");
        return;
    }

    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        rt_kprintf("%s open dev fail\n", __func__);
        return;
    }

    monitor = &g_serial_monitors[monitor_idx];
    rt_memset(monitor, 0, sizeof(*monitor));
    monitor->fd = fd;
    monitor->running = true;
    rt_strncpy(monitor->dev_path, dev_path, SERIAL_DEV_PATH_LEN - 1);

    thread = usb_osal_thread_create("rd", 4096, 15, rd_thread, monitor);
    if (thread == NULL) {
        close(fd);
        monitor->fd = -1;
        monitor->running = false;
        monitor->dev_path[0] = '\0';
        rt_kprintf("%s fail to create thread\n", __func__);
        return;
    }

    monitor->thread = thread;
    g_selected_monitor = monitor_idx;
    rt_kprintf("monitoring %s\n", monitor->dev_path);
}
FINSH_FUNCTION_EXPORT(rd, rd data);

#define WRITE_BUF_LEN (256)

static int hex_to_val(char ch)
{
    if ((ch >= '0') && (ch <= '9')) {
        return ch - '0';
    }
    if ((ch >= 'a') && (ch <= 'f')) {
        return ch - 'a' + 10;
    }
    if ((ch >= 'A') && (ch <= 'F')) {
        return ch - 'A' + 10;
    }
    return -1;
}

static int decode_wr_data(const char *data, char *buf, int buf_len, bool *has_line_end)
{
    int src = 0;
    int dst = 0;

    *has_line_end = false;

    while (data[src] != '\0') {
        char ch = data[src++];

        if (dst >= buf_len) {
            return -RT_EFULL;
        }

        if ((ch == '\\') && (data[src] != '\0')) {
            switch (data[src]) {
            case 'r':
                ch = '\r';
                src++;
                *has_line_end = true;
                break;
            case 'n':
                ch = '\n';
                src++;
                *has_line_end = true;
                break;
            case 't':
                ch = '\t';
                src++;
                break;
            case '\\':
                ch = '\\';
                src++;
                break;
            case 'x': {
                int hi;
                int lo;

                hi = hex_to_val(data[src + 1]);
                lo = hex_to_val(data[src + 2]);
                if ((hi < 0) || (lo < 0)) {
                    return -RT_ERROR;
                }

                ch = (char)((hi << 4) | lo);
                src += 3;
                if ((ch == '\r') || (ch == '\n')) {
                    *has_line_end = true;
                }
                break;
            }
            default:
                break;
            }
        } else if ((ch == '\r') || (ch == '\n')) {
            *has_line_end = true;
        }

        buf[dst++] = ch;
    }

    return dst;
}

void wr(char *file_name, char *data)
{
    int fd;
    int len;
    bool has_line_end;
    int monitor_idx;
    static char buf[WRITE_BUF_LEN];

    serial_monitor_init();

    monitor_idx = serial_monitor_find_by_path(file_name);
    if ((monitor_idx >= 0) && serial_monitor_is_active(&g_serial_monitors[monitor_idx])) {
        g_selected_monitor = monitor_idx;
    }

    fd = open(file_name, O_RDWR);
    if (fd < 0) {
        rt_kprintf("open dev fail\n");
        goto out;
    }

    len = decode_wr_data(data, buf, WRITE_BUF_LEN - 1, &has_line_end);
    if (len < 0) {
        rt_kprintf("cmd is too long or escape is invalid\n");
        goto close_fd;
    }

    if (!has_line_end) {
        if (len >= (WRITE_BUF_LEN - 1)) {
            rt_kprintf("cmd is too long\n");
            goto close_fd;
        }
        buf[len++] = '\r';
    }

    write(fd, buf, len);

close_fd:
    close(fd);
out:
    return;
}
FINSH_FUNCTION_EXPORT(wr, wr data);






int cmd_rd(int argc, char **argv)
{
    serial_monitor_init();

    if ((argc == 2) && (strcmp(argv[1], "list") == 0))
    {
        serial_monitor_list();
        return 0;
    }

    if ((argc == 2) && (strcmp(argv[1], "stop") == 0))
    {
        serial_monitor_stop_all();
        return 0;
    }

    if ((argc == 3) && (strcmp(argv[1], "stop") == 0))
    {
        int monitor_idx = serial_monitor_find_by_path(argv[2]);

        if (monitor_idx < 0) {
            rt_kprintf("reader for %s not found\n", argv[2]);
            return 0;
        }

        serial_monitor_stop_one(&g_serial_monitors[monitor_idx]);
        return 0;
    }

    if (argc != 2)
    {
        rt_kprintf("Usage: rd /dev/ttyUSB1\n");
        rt_kprintf("       rd stop\n");
        rt_kprintf("       rd stop /dev/ttyUSB1\n");
        rt_kprintf("       rd list\n");
        return 0;
    }

    rd(argv[1]);

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_rd, rd, rd /dev/ttyUSB1);

int cmd_wr(int argc, char **argv)
{
    struct serial_monitor *monitor;

    serial_monitor_init();

    if (argc == 2)
    {
        monitor = serial_monitor_get_selected();
        if (monitor == RT_NULL) {
            rt_kprintf("no active reader, use 'rd /dev/ttyUSBx' first or pass a device path\n");
            return 0;
        }

        wr(monitor->dev_path, argv[1]);
        return 0;
    }

    if (argc != 3)
    {
        rt_kprintf("Usage: wr /dev/ttyUSB1 [data]\\n");
        rt_kprintf("       wr [data]    # use selected reader from rd\\n");
        rt_kprintf("       escapes: \\\\r \\\\n \\\\t \\\\xNN\\n");
        return 0;
    }

    wr(argv[1], argv[2]);

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wr, wr, wr /dev/ttyUSB1 [data]);

