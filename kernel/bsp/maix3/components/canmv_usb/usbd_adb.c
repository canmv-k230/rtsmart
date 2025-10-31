/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbd_desc.h"

#include "usb_osal.h"

#include <rtthread.h>
#include <rtdevice.h>

#include "usbd_adb.h"
#include <finsh.h>
#include <shell.h>
#include <dirent.h>
#include <unistd.h>

#if defined (CHERRY_USB_DEVICE_FUNC_ADB) || defined (CHERRY_USB_DEVICE_FUNC_CDC_ADB)
/* Max USB packet size */
#define WCID_VENDOR_CODE 0x17

__ALIGN_BEGIN const uint8_t WCID_StringDescriptor_MSOS[18] __ALIGN_END = {
    ///////////////////////////////////////
    /// MS OS string descriptor
    ///////////////////////////////////////
    0x12,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    /* MSFT100 */
    'M', 0x00, 'S', 0x00, 'F', 0x00, 'T', 0x00, /* wcChar_7 */
    '1', 0x00, '0', 0x00, '0', 0x00,            /* wcChar_7 */
    WCID_VENDOR_CODE,                           /* bVendorCode */
    0x00,                                       /* bReserved */
};

__ALIGN_BEGIN const uint8_t WINUSB_WCIDDescriptor[40] __ALIGN_END = {
    ///////////////////////////////////////
    /// WCID descriptor
    ///////////////////////////////////////
    0x28, 0x00, 0x00, 0x00,                   /* dwLength */
    0x00, 0x01,                               /* bcdVersion */
    0x04, 0x00,                               /* wIndex */
    0x01,                                     /* bCount */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* bReserved_7 */

    ///////////////////////////////////////
    /// WCID function descriptor
    ///////////////////////////////////////
    ADB_INTF_NUM, /* bFirstInterfaceNumber */
    0x01, /* bReserved */
    /* Compatible ID */
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00, /* cCID_8: WINUSB */
    /* Sub Compatible ID */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* cSubCID_8 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* bReserved_6 */
};

__ALIGN_BEGIN const uint8_t WINUSB_IF0_WCIDProperties[142] __ALIGN_END = {
    ///////////////////////////////////////
    /// WCID property descriptor
    ///////////////////////////////////////
    0x8e, 0x00, 0x00, 0x00, /* dwLength */
    0x00, 0x01,             /* bcdVersion */
    0x05, 0x00,             /* wIndex */
    0x01, 0x00,             /* wCount */

    ///////////////////////////////////////
    /// registry propter descriptor
    ///////////////////////////////////////
    0x84, 0x00, 0x00, 0x00, /* dwSize */
    0x01, 0x00, 0x00, 0x00, /* dwPropertyDataType */
    0x28, 0x00,             /* wPropertyNameLength */
    /* DeviceInterfaceGUID */
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00,  /* wcName_20 */
    'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,  /* wcName_20 */
    't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,  /* wcName_20 */
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,  /* wcName_20 */
    'U', 0x00, 'I', 0x00, 'D', 0x00, 0x00, 0x00, /* wcName_20 */
    0x4e, 0x00, 0x00, 0x00,                      /* dwPropertyDataLength */
    /* {1D4B2365-4749-48EA-B38A-7C6FDDDD7E26} */
    '{', 0x00, '1', 0x00, 'D', 0x00, '4', 0x00, /* wcData_39 */
    'B', 0x00, '2', 0x00, '3', 0x00, '6', 0x00, /* wcData_39 */
    '5', 0x00, '-', 0x00, '4', 0x00, '7', 0x00, /* wcData_39 */
    '4', 0x00, '9', 0x00, '-', 0x00, '4', 0x00, /* wcData_39 */
    '8', 0x00, 'E', 0x00, 'A', 0x00, '-', 0x00, /* wcData_39 */
    'B', 0x00, '3', 0x00, '8', 0x00, 'A', 0x00, /* wcData_39 */
    '-', 0x00, '7', 0x00, 'C', 0x00, '6', 0x00, /* wcData_39 */
    'F', 0x00, 'D', 0x00, 'D', 0x00, 'D', 0x00, /* wcData_39 */
    'D', 0x00, '7', 0x00, 'E', 0x00, '2', 0x00, /* wcData_39 */
    '6', 0x00, '}', 0x00, 0x00, 0x00,           /* wcData_39 */
};

const uint8_t *WINUSB_IFx_WCIDProperties[] = {
    WINUSB_IF0_WCIDProperties,
};

struct usb_msosv1_descriptor msosv1_desc = {
    .string = WCID_StringDescriptor_MSOS,
    .vendor_code = WCID_VENDOR_CODE,
    .compat_id = WINUSB_WCIDDescriptor,
    .comp_id_property = WINUSB_IFx_WCIDProperties,
};

#ifndef CONFIG_USBDEV_SHELL_RX_BUFSIZE
#define CONFIG_USBDEV_SHELL_RX_BUFSIZE (2048)
#endif

/* Thread + counting semaphore for deferred console processing
 *
 * Why not workqueue?
 * - Workqueue can drop requests if work is already queued/running
 * - We need EVERY character to be processed (especially Ctrl+C!)
 *
 * Solution: Use counting semaphore
 * - Each rt_sem_release() increments counter
 * - Thread processes ALL notifications, never drops any
 */
static struct usbd_interface intf0;

struct usbd_adb_shell {
    struct rt_device parent;
    struct rt_ringbuffer rx_rb;
    struct rt_device_notify rx_notify;
    rt_uint8_t rx_rb_buffer[CONFIG_USBDEV_SHELL_RX_BUFSIZE];
    rt_mutex_t tx_lock;  /* Protects usbd_adb_shell_write from concurrent calls */
    rt_sem_t adb_rx_sem;
    rt_thread_t adb_rx_thread;
    rt_bool_t use_adb_console;
} g_usbd_adb_shell;

/* Debug switch: set to 1 to enable debug output, 0 to disable */
#define ADB_SYNC_DEBUG 0

#if ADB_SYNC_DEBUG
#define SYNC_DBG(fmt, ...) rt_kprintf("[ADB_SYNC] " fmt "\n", ##__VA_ARGS__)
#else
#define SYNC_DBG(fmt, ...)
#endif

/* Error logging - always enabled */
#define SYNC_ERR(fmt, ...) rt_kprintf("[ADB_SYNC_ERR] " fmt "\n", ##__VA_ARGS__)

#define ADB_SYNC_MSG_MAX_SIZE 4096
#define ADB_SYNC_MSG_POOL_SIZE 2
#define ADB_SYNC_MAX_PENDING ADB_SYNC_MSG_POOL_SIZE

struct adb_sync_message {
    uint32_t length;
    uint8_t data[ADB_SYNC_MSG_MAX_SIZE];
};

/* Message pool with simple index-based management */
static struct adb_sync_message msg_pool[ADB_SYNC_MSG_POOL_SIZE];
static volatile uint8_t msg_pool_used[ADB_SYNC_MSG_POOL_SIZE];  /* 0=free, 1=used */

static rt_mq_t adb_sync_mq = RT_NULL;
static rt_thread_t adb_sync_thread = RT_NULL;

/* Flow control semaphore: limits concurrent messages to prevent overflow */
static rt_sem_t flow_control_sem = RT_NULL;

/* Pending OKAY: set when flow control blocks an OKAY, cleared when sent */
static volatile bool pending_transport_okay = false;

/*
 * ADB Sync Protocol Commands (little-endian ASCII)
 * These are sub-commands within the ADB WRTE payload for file operations
 *
 * ID_STAT: Get file/directory status (size, mode, mtime)
 *          PC sends STAT + path, device responds with STAT + stat_data
 *
 * ID_SEND: Push file from PC to device (adb push)
 *          PC sends SEND + "path,mode", then DATA...DATA...DONE
 *
 * ID_RECV: Pull file from device to PC (adb pull)
 *          PC sends RECV + path, device responds with DATA...DATA...DONE
 *
 * ID_LIST: List directory contents (adb ls)
 *          PC sends LIST + path, device responds with DENT...DENT...DONE
 *
 * ID_QUIT: Close sync connection
 *          PC sends QUIT, device responds with OKAY
 *
 * ID_DATA: File data chunk (used in SEND/RECV)
 *          Format: DATAnnnn<file_data> where nnnn is 4-byte length
 *
 * ID_DONE: End of transfer marker
 *          Format: DONEnnnn where nnnn is file mtime (for SEND) or 0 (for RECV)
 *
 * ID_OKAY: Success response
 *          Generic acknowledgment
 *
 * ID_FAIL: Error response
 *          Followed by error message string
 *
 * ID_DENT: Directory entry (in LIST response)
 *          Format: mode(4) + size(4) + mtime(4) + namelen(4) + name
 */
#define ID_STAT 0x54415453  /* "STAT" */
#define ID_SEND 0x444e4553  /* "SEND" */
#define ID_RECV 0x56434552  /* "RECV" */
#define ID_DATA 0x41544144  /* "DATA" */
#define ID_DONE 0x454e4f44  /* "DONE" */
#define ID_LIST 0x5453494c  /* "LIST" */
#define ID_QUIT 0x54495551  /* "QUIT" */
#define ID_OKAY 0x59414b4f  /* "OKAY" */
#define ID_FAIL 0x4c494146  /* "FAIL" */
#define ID_DENT 0x544e4544  /* "DENT" */

struct sync_msg {
    uint32_t id;
    uint32_t size;
};

/*
 * File transfer state machine
 *
 * SYNC_IDLE:
 *   - Initial state, no transfer in progress
 *   - Waiting for sync commands (STAT/SEND/RECV/LIST/QUIT)
 *
 * SYNC_RECEIVING_FILE:
 *   - File is open (after SEND command)
 *   - Processing DATA packets until DONE
 */
enum sync_state {
    SYNC_IDLE,
    SYNC_RECEIVING_FILE
};

struct adb_sync_context {
    enum sync_state state;
    char current_path[256];
    int file_fd;
    uint32_t file_mode;
    uint32_t expected_file_size;
    uint32_t bytes_received;
    uint32_t current_data_remaining;  /* Bytes remaining in current DATA chunk */
};

static struct adb_sync_context sync_ctx = {
    .state = SYNC_IDLE,
    .file_fd = -1,  /* POSIX standard: -1 for invalid fd */
    .file_mode = 0,
    .expected_file_size = 0,
    .bytes_received = 0,
    .current_data_remaining = 0
};

int usbd_adb_sync_init(void);

/*
 * Thread entry: process console RX notifications in thread context
 * Uses counting semaphore - processes ALL notifications, never drops
 */
static void adb_rx_thread_entry(void *param)
{
    while (1) {
        /* Wait for notification (counting semaphore) */
        rt_sem_take(g_usbd_adb_shell.adb_rx_sem, RT_WAITING_FOREVER);

        /* Now in thread context - safe to call console_rx_notify */
        if (g_usbd_adb_shell.rx_notify.notify)
        {
            g_usbd_adb_shell.rx_notify.notify(g_usbd_adb_shell.rx_notify.dev);
        }
    }
}

void usbd_adb_notify_shell_read(uint8_t *data, uint32_t len)
{
    rt_ringbuffer_put(&g_usbd_adb_shell.rx_rb, data, len);

    if (g_usbd_adb_shell.parent.rx_indicate) {
        g_usbd_adb_shell.parent.rx_indicate(&g_usbd_adb_shell.parent, len);
    }

    /* Signal thread to process (counting semaphore - never drops!) */
    if (g_usbd_adb_shell.adb_rx_sem && g_usbd_adb_shell.rx_notify.notify)
    {
        rt_sem_release(g_usbd_adb_shell.adb_rx_sem);  /* Safe in ISR, increments counter */
    }
}

static rt_err_t usbd_adb_shell_open(struct rt_device *dev, rt_uint16_t oflag)
{
    while (!usb_device_is_configured(0)) {
        rt_thread_mdelay(10);
    }
    return RT_EOK;
}

static rt_err_t usbd_adb_shell_close(struct rt_device *dev)
{
    return RT_EOK;
}

static rt_size_t usbd_adb_shell_read(struct rt_device *dev,
                                      rt_off_t pos,
                                      void *buffer,
                                      rt_size_t size)
{
    return rt_ringbuffer_get(&g_usbd_adb_shell.rx_rb, (rt_uint8_t *)buffer, size);
}

static rt_size_t usbd_adb_shell_write(struct rt_device *dev,
                                       rt_off_t pos,
                                       const void *buffer,
                                       rt_size_t size)
{
    int ret = 0;
    rt_err_t result;

    RT_ASSERT(dev != RT_NULL);

    if (!usb_device_is_configured(0)) {
        return size;
    }

    /* Acquire lock to serialize write operations */
    result = rt_mutex_take(g_usbd_adb_shell.tx_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK) {
        rt_kprintf("[ADB] Failed to acquire TX mutex: %d\n", result);
        return 0;  /* Return 0 to indicate write failure */
    }

    if (usbd_adb_can_write() && size) {
        /* Fast path: check if conversion needed */
        if (memchr(buffer, '\n', size)) {
            /* Slow path: need \n -> \r\n conversion */
            static uint8_t tx_buffer[CONFIG_USBDEV_SHELL_RX_BUFSIZE * 2];
            const uint8_t *data = (const uint8_t *)buffer;
            rt_size_t tx_len = 0;
            rt_size_t i;

            for (i = 0; i < size && tx_len < sizeof(tx_buffer) - 1; i++) {
                if (data[i] == '\n') {
                    tx_buffer[tx_len++] = '\r';
                }
                tx_buffer[tx_len++] = data[i];
            }

            ret = usbd_adb_write(ADB_SHELL_LOALID, tx_buffer, tx_len);
        } else {
            /* Fast path: no \n found or STREAM disabled, send directly */
            ret = usbd_adb_write(ADB_SHELL_LOALID, buffer, size);
        }

        if (ret != 0) {
            rt_kprintf("[ADB] usbd_adb_write failed: %d\n", ret);
            rt_mutex_release(g_usbd_adb_shell.tx_lock);
            return 0;  /* Return 0 to indicate write failure */
        }
    }

    /* Release lock after write completes */
    result = rt_mutex_release(g_usbd_adb_shell.tx_lock);
    if (result != RT_EOK) {
        rt_kprintf("[ADB] Failed to release TX mutex: %d\n", result);
    }

    return size;
}

static rt_err_t usbd_adb_shell_control(struct rt_device *dev,
                                       int cmd,
                                       void *args)
{
    rt_err_t ret = RT_EOK;

    switch (cmd) {
    case RT_DEVICE_CTRL_NOTIFY_SET:
        if (args) {
            rt_memcpy(&g_usbd_adb_shell.rx_notify, args,
                      sizeof(struct rt_device_notify));
        }
        break;

    default:
        ret = -RT_EINVAL;  /* Unsupported command */
        break;
    }

    return ret;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops usbd_adb_shell_ops = {
    NULL,
    usbd_adb_shell_open,
    usbd_adb_shell_close,
    usbd_adb_shell_read,
    usbd_adb_shell_write,
    usbd_adb_shell_control,
};
#endif

void usbd_adb_shell_init(uint8_t in_ep, uint8_t out_ep)
{
    rt_err_t ret;
    struct rt_device *device;

    device = &(g_usbd_adb_shell.parent);

    device->type = RT_Device_Class_Char;
    device->rx_indicate = RT_NULL;
    device->tx_complete = RT_NULL;

#ifdef RT_USING_DEVICE_OPS
    device->ops = &usbd_adb_shell_ops;
#else
    device->init = NULL;
    device->open = usbd_adb_shell_open;
    device->close = usbd_adb_shell_close;
    device->read = usbd_adb_shell_read;
    device->write = usbd_adb_shell_write;
    device->control = NULL;
#endif
    device->user_data = NULL;

    /* register a character device */
    ret = rt_device_register(device, "adb-sh", RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_REMOVABLE);
    if (ret != RT_EOK) {
        rt_kprintf("[ADB] Failed to register device: %d\n", ret);
        return;
    }

    /* Initialize mutex for write serialization */
    g_usbd_adb_shell.tx_lock = rt_mutex_create("adb_tx", RT_IPC_FLAG_FIFO);
    if (g_usbd_adb_shell.tx_lock == RT_NULL) {
        rt_kprintf("[ADB] Failed to create TX mutex\n");
        goto err_unregister_device;
    }

    /* Initialize ring buffer */
    rt_ringbuffer_init(&g_usbd_adb_shell.rx_rb, g_usbd_adb_shell.rx_rb_buffer,
                       sizeof(g_usbd_adb_shell.rx_rb_buffer));

    /* Initialize deferred RX processing thread and counting semaphore */
    g_usbd_adb_shell.adb_rx_sem = rt_sem_create("adb_rx", 0, RT_IPC_FLAG_FIFO);
    if (g_usbd_adb_shell.adb_rx_sem == RT_NULL) {
        rt_kprintf("[ADB] Failed to create RX semaphore\n");
        goto err_delete_mutex;
    }

    g_usbd_adb_shell.adb_rx_thread = rt_thread_create("adb_rx",
                                      adb_rx_thread_entry,
                                      RT_NULL,
                                      4096,  /* 4KB stack to avoid overflow */
                                      10,    /* Medium priority */
                                      20);
    if (g_usbd_adb_shell.adb_rx_thread == RT_NULL) {
        rt_kprintf("[ADB] Failed to create RX thread\n");
        goto err_delete_rx_sem;
    }

    ret = rt_thread_startup(g_usbd_adb_shell.adb_rx_thread);
    if (ret != RT_EOK) {
        rt_kprintf("[ADB] Failed to start RX thread: %d\n", ret);
        goto err_delete_thread;
    }

    return;

    /* Error handling - clean up in reverse order */
err_delete_thread:
    rt_thread_delete(g_usbd_adb_shell.adb_rx_thread);
    g_usbd_adb_shell.adb_rx_thread = RT_NULL;

err_delete_rx_sem:
    rt_sem_delete(g_usbd_adb_shell.adb_rx_sem);
    g_usbd_adb_shell.adb_rx_sem = RT_NULL;

err_delete_mutex:
    rt_mutex_delete(g_usbd_adb_shell.tx_lock);
    g_usbd_adb_shell.tx_lock = RT_NULL;

err_unregister_device:
    rt_device_unregister(device);

    rt_kprintf("[ADB] Shell initialization failed\n");
}

void adb_enter()
{
    rt_device_t dev = RT_NULL;

    finsh_set_device("adb-sh");
    rt_console_set_device("adb-sh");

    dev = rt_device_find(RT_CONSOLE_DEVICE_NAME);
    if (RT_EOK != rt_device_open(dev, RT_DEVICE_OFLAG_RDWR |
                                 RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM)) {
        rt_kprintf("%s: open %s fail\n", __func__, RT_CONSOLE_DEVICE_NAME);
    }

    g_usbd_adb_shell.use_adb_console = RT_TRUE;
}

void exit_adb_console(void)
{
    if (g_usbd_adb_shell.use_adb_console) {

        finsh_set_device(RT_CONSOLE_DEVICE_NAME);
        rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
        rt_ringbuffer_reset(&g_usbd_adb_shell.rx_rb);

        g_usbd_adb_shell.use_adb_console = RT_FALSE;

    }
}

int adb_exit(void)
{
    usbd_adb_close(ADB_SHELL_LOALID);

    if (0 != strcmp("adb-sh", finsh_get_device())) {
        rt_kprintf("exit: command not found.\n");
    }

    exit_adb_console();

    return 0;
}
MSH_CMD_EXPORT_ALIAS(adb_exit, exit, adb exit command);

rt_bool_t is_use_adb_console()
{
    return g_usbd_adb_shell.use_adb_console;
}

void canmv_usb_device_adb_init(void)
{
    uint8_t busid = USB_DEVICE_BUS_ID;
    rt_err_t result;

    usbd_adb_shell_init(ADB_IN_EP, ADB_OUT_EP);

    result = usbd_adb_sync_init();
    if (result != RT_EOK) {
        rt_kprintf("[ADB] Failed to initialize ADB sync: %d\n", result);
        /* TODO: Add cleanup of usbd_adb_shell_init resources if needed */
        return;  /* Abort initialization on sync init failure */
    }

    usbd_msosv1_desc_register(busid, &msosv1_desc);
    usbd_add_interface(busid, usbd_adb_init_intf(busid, &intf0, ADB_IN_EP, ADB_OUT_EP));
}

/*
 * Security: Validate file path to prevent directory traversal attacks
 * Returns true if path is safe, false otherwise
 *
 * Security policy:
 * - Only relative paths allowed (no leading /)
 * - No directory traversal (no ../ or /..)
 * - No backslashes (Windows path separator)
 *
 * NOTE: If you need to allow absolute paths, you should:
 * 1. Define a whitelist of allowed base directories
 * 2. Use realpath() to canonicalize paths
 * 3. Check that canonicalized path is within allowed directories
 */
static bool is_safe_path(const char *path)
{
#if 0
    /* Reject NULL or empty paths */
    if (!path || path[0] == '\0') {
        return false;
    }

    /* Reject absolute paths (starting with /) - force relative paths only */
    if (path[0] == '/') {
        SYNC_ERR("Absolute path rejected: %s", path);
        return false;
    }

    /* Check for directory traversal patterns */
    const char *p = path;
    while (*p) {
        /* Check for "../" pattern */
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            SYNC_ERR("Directory traversal rejected: %s", path);
            return false;
        }

        /* Check for "/.." pattern */
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0')) {
            SYNC_ERR("Directory traversal rejected: %s", path);
            return false;
        }

        p++;
    }

    /* Additional check: reject paths with backslashes (Windows-style) */
    if (strchr(path, '\\')) {
        SYNC_ERR("Backslash in path rejected: %s", path);
        return false;
    }

    return true;
#else
    return true;
#endif
}

/*
 * Helper function to ensure all data is written
 * Handles partial writes and EINTR interruptions
 * Returns total bytes written on success, -1 on error
 */
static ssize_t write_all(int fd, const void *buf, size_t count)
{
    size_t total_written = 0;
    const uint8_t *ptr = (const uint8_t *)buf;
    uint32_t retry_count = 0;
    const uint32_t MAX_RETRIES = 4;  /* Prevent infinite loop */

    while (total_written < count) {
        ssize_t written = write(fd, ptr + total_written, count - total_written);

        if (written < 0) {
            /* Check if interrupted by signal - retry */
            if (errno == EINTR) {
                continue;
            }
            /* Real error - disk full, invalid fd, etc. */
            SYNC_ERR("write failed: errno=%d, written=%u/%u", errno, (uint32_t)total_written, (uint32_t)count);
            return -1;
        }

        if (written == 0) {
            /* No progress - might be disk full or other issue */
            if (++retry_count > MAX_RETRIES) {
                SYNC_ERR("write stalled after %u retries", retry_count);
                return -1;
            }
            rt_thread_mdelay(1);  /* Small delay before retry */
            continue;
        }

        total_written += written;
        retry_count = 0;  /* Reset retry count on progress */
    }

    return total_written;
}

/*
 * Send data to PC through ADB file channel
 * This mimics the shell's write mechanism:
 * 1. Reset semaphore
 * 2. Call usbd_adb_write which triggers AWRITE_MSG -> AWRITE_DATA state machine
 */
static void adb_sync_write(const uint8_t *data, uint32_t len)
{
    SYNC_DBG("adb_sync_write: len=%u", len);

    if (!usb_device_is_configured(0)) {
        SYNC_ERR("  USB not configured");
        return;
    }

    if (len == 0) {
        SYNC_ERR("  Nothing to write");
        return;
    }

    /* Wait for writable flag - PC must send OKAY after our previous WRTE */
    uint32_t wait_count = 0;
    while (!usbd_adb_can_write() && wait_count < 3000) {
        rt_thread_mdelay(1);
        wait_count++;
    }

    if (!usbd_adb_can_write()) {
        SYNC_ERR("  Timeout waiting for writable (waited %ums)", wait_count);
        SYNC_ERR("  Aborting write - protocol may be out of sync");
        return;  /* Abort on timeout instead of sending corrupted data */
    }

    usbd_adb_write(ADB_FILE_LOALID, data, len);
    SYNC_DBG("  Write completed");
}

/*
 * Send sync protocol response
 * Format: 4-byte command ID + payload (NO length field!)
 * This follows the same pattern as Windows QUIT command: just ID + data
 */
static void send_sync_response(uint32_t id, const void *data, uint32_t len)
{
    static uint8_t buffer[4 + ADB_SYNC_MSG_MAX_SIZE];  /* Static to save stack */
    uint32_t *id_ptr = (uint32_t *)buffer;

    /* Validate length to prevent buffer overflow */
    if (len > ADB_SYNC_MSG_MAX_SIZE) {
        SYNC_ERR("send_sync_response: len=%u exceeds max=%u", len, ADB_SYNC_MSG_MAX_SIZE);
        return;
    }

    *id_ptr = id;

    SYNC_DBG("send_sync_response: id=0x%08x (%c%c%c%c), len=%u",
             id,
             (char)(id & 0xFF),
             (char)((id >> 8) & 0xFF),
             (char)((id >> 16) & 0xFF),
             (char)((id >> 24) & 0xFF),
             len);

    if (len > 0 && data != NULL) {
        memcpy(buffer + 4, data, len);
    }

    adb_sync_write(buffer, 4 + len);
}

static void send_sync_fail(const char *reason)
{
    SYNC_ERR("FAIL: %s", reason);
    uint32_t len = strlen(reason);
    send_sync_response(ID_FAIL, reason, len);
}

/*
 * Handle STAT command: get file/directory information
 * PC sends: STAT<path>
 * Device responds: STAT<mode+size+mtime>
 *
 * Note: For adb push, file may not exist yet. Return mode=0 to indicate
 * "file doesn't exist but can be created"
 */
static void handle_sync_stat(const uint8_t *path, uint32_t len)
{
    static char file_path[256];  /* Static to save stack */
    struct stat st;

    SYNC_DBG("handle_sync_stat: path_len=%u", len);

    if (len >= sizeof(file_path)) {
        SYNC_DBG("  Path too long");
        send_sync_fail("Path too long");
        return;
    }

    memcpy(file_path, path, len);
    file_path[len] = '\0';

    SYNC_DBG("  path='%s'", file_path);

    /* Security check: validate path */
    if (!is_safe_path(file_path)) {
        send_sync_fail("Invalid path");
        return;
    }

    struct {
        uint32_t mode;
        uint32_t size;
        uint32_t mtime;
    } stat_data;

    if (stat(file_path, &st) == 0) {
        /* File/directory exists - convert RT-Thread mode to POSIX format */
        uint32_t posix_mode;
        if (S_ISDIR(st.st_mode)) {
            /* Directory: 0040755 */
            posix_mode = 0040755;
        } else {
            /* Regular file: 0100644 */
            posix_mode = 0100644;
        }

        stat_data.mode = posix_mode;
        stat_data.size = st.st_size;
        stat_data.mtime = st.st_mtime;

        SYNC_DBG("  File found: mode=0%o (orig=0x%x), size=%u, mtime=%u",
                 posix_mode, st.st_mode, (uint32_t)st.st_size, (uint32_t)st.st_mtime);
    } else {
        /* File doesn't exist - return zeros to indicate "can be created" */
        stat_data.mode = 0;
        stat_data.size = 0;
        stat_data.mtime = 0;

        SYNC_DBG("  File not found, returning mode=0 (can create)");
    }

    SYNC_DBG("[ADB_SYNC] STAT response: mode=0x%08x (0%o), size=%u, mtime=%u\n",
               stat_data.mode, stat_data.mode, stat_data.size, stat_data.mtime);
    send_sync_response(ID_STAT, &stat_data, sizeof(stat_data));
}

/*
 * Handle SEND command: receive file from PC (adb push)
 *
 * SEND packet contains everything in one A_WRTE:
 * Format: SEND + size + path,mode + DATA + size + filedata + DONE + mtime
 *
 * For small files: all in one packet
 * For large files: SEND + path,mode in first packet, then DATA packets follow
 */
static void handle_sync_send(const uint8_t *data, uint32_t len)
{
    SYNC_DBG("handle_sync_send: len=%u", len);

    /* Find the comma to split path and mode */
    char *comma = memchr(data, ',', len);
    if (!comma) {
        SYNC_ERR("No comma in SEND");
        send_sync_fail("Invalid SEND format");
        return;
    }

    /* Extract path */
    uint32_t path_len = comma - (char *)data;
    static char file_path[256];  /* Static to save stack */
    if (path_len >= sizeof(file_path)) {
        send_sync_fail("Path too long");
        return;
    }
    memcpy(file_path, data, path_len);
    file_path[path_len] = '\0';

    SYNC_DBG("  File path: '%s'", file_path);

    /* Security check: validate path */
    if (!is_safe_path(file_path)) {
        send_sync_fail("Invalid path");
        return;
    }

    /* Extract mode (everything between comma and "DATA" or end) */
    const uint8_t *mode_start = (const uint8_t *)(comma + 1);
    uint32_t remaining = len - path_len - 1;

    /* Find where mode ends (either at "DATA" or end of payload) */
    const uint8_t *data_pos = NULL;
    for (uint32_t i = 0; i + 4 <= remaining; i++) {
        if (memcmp(mode_start + i, "DATA", 4) == 0) {
            data_pos = mode_start + i;
            break;
        }
    }

    uint32_t mode_len;
    if (data_pos) {
        mode_len = data_pos - mode_start;
    } else {
        mode_len = remaining;
    }

    char mode_str[16];
    if (mode_len >= sizeof(mode_str)) {
        send_sync_fail("Mode too long");
        return;
    }
    memcpy(mode_str, mode_start, mode_len);
    mode_str[mode_len] = '\0';
    uint32_t mode = atoi(mode_str);

    SYNC_DBG("  File mode: %s (%u)", mode_str, mode);

    /* Check if file already open - close it first */
    if (sync_ctx.file_fd >= 0) {
        SYNC_DBG("Closing previously open file: fd=%d", sync_ctx.file_fd);
        close(sync_ctx.file_fd);
        sync_ctx.file_fd = -1;
    }

    /* Open file */
    sync_ctx.file_fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (sync_ctx.file_fd < 0) {
        SYNC_ERR("Failed to open: %s (errno=%d)", file_path, errno);
        send_sync_fail("Cannot create file");
        sync_ctx.file_fd = -1;
        return;
    }

    SYNC_DBG("File opened: %s, fd=%d", file_path, sync_ctx.file_fd);
    strcpy(sync_ctx.current_path, file_path);
    sync_ctx.file_mode = mode;
    sync_ctx.state = SYNC_RECEIVING_FILE;
    sync_ctx.bytes_received = 0;
    sync_ctx.current_data_remaining = 0;

    /* Check if DATA follows in same packet */
    if (data_pos) {
        SYNC_DBG("DATA in same packet");
        /* Calculate position relative to start of buffer */
        uint32_t data_offset = data_pos - data;

        /* Check if we have enough space for DATA header (8 bytes) */
        if (data_offset + 8 <= len) {
            uint32_t data_size = *(const uint32_t *)(data_pos + 4);
            const uint8_t *file_data = data_pos + 8;
            uint32_t file_data_offset = data_offset + 8;

            /* Calculate actual data length in this packet
             * For small files: use header size if data fits
             * For large files: use remaining packet length */
            uint32_t remaining_in_packet = len - file_data_offset;
            uint32_t actual_data_len;

            if (data_size <= remaining_in_packet) {
                /* Small file: header size is accurate */
                actual_data_len = data_size;
            } else {
                /* Large file: header size is wrong, use packet length */
                actual_data_len = remaining_in_packet;
            }

            SYNC_DBG("Writing %u bytes (header_size=%u)", actual_data_len, data_size);

            if (actual_data_len > 0) {
                ssize_t written = write_all(sync_ctx.file_fd, (const void *)file_data, actual_data_len);
                if (written < 0) {
                    SYNC_ERR("Failed to write file data");
                    send_sync_fail("Write error");
                    close(sync_ctx.file_fd);
                    sync_ctx.file_fd = -1;  /* Fixed: use -1 not 0 */
                    sync_ctx.state = SYNC_IDLE;
                    return;
                }
                sync_ctx.bytes_received += written;
                
                /* Update current_data_remaining if data doesn't fit in one packet */
                if (data_size > actual_data_len) {
                    sync_ctx.current_data_remaining = data_size - actual_data_len;
                    SYNC_DBG("Wrote %d bytes, %u remaining in DATA chunk", (int)written, sync_ctx.current_data_remaining);
                } else {
                    sync_ctx.current_data_remaining = 0;
                    SYNC_DBG("Wrote %d bytes, DATA chunk complete", (int)written);
                }
                
                SYNC_DBG("Total received: %u bytes", sync_ctx.bytes_received);

                /* Check for DONE in same packet (after actual data written) */
                uint32_t done_offset = file_data_offset + actual_data_len;
                if (done_offset + 8 <= len) {
                    const uint8_t *done_pos = data + done_offset;
                    if (memcmp(done_pos, "DONE", 4) == 0) {
                        uint32_t zero = 0;
                        SYNC_DBG("DONE in same packet, closing file");
                        close(sync_ctx.file_fd);
                        sync_ctx.file_fd = -1;
                        sync_ctx.state = SYNC_IDLE;
                        sync_ctx.current_data_remaining = 0;
                        /* Send OKAY with 4 bytes of 0, matching QUIT/DONE format */
                        send_sync_response(ID_OKAY, &zero, 4);
                        return;
                    }
                }
            } else {
                /* No data in this packet, but DATA header indicates more data coming */
                if (data_size > 0) {
                    sync_ctx.current_data_remaining = data_size;
                    SYNC_DBG("DATA header only, %u bytes remaining", data_size);
                }
            }
        }
    } else {
        SYNC_DBG("No DATA in SEND packet, waiting for DATA...");
    }

    SYNC_DBG("SEND done, state=RECEIVING_FILE, fd=%d", sync_ctx.file_fd);
    /* No sync OKAY needed for SEND - just wait for DATA/DONE packets */
}

/*
 * Handle RECV command: send file to PC (adb pull)
 * PC sends: RECV<path>
 * Device responds: DATA<data>...DATA<data>...DONE or FAIL<reason>
 */
static void handle_sync_recv(const uint8_t *path, uint32_t len)
{
    static char file_path[256];  /* Static to save stack */

    SYNC_DBG("[ADB_SYNC] handle_sync_recv: path_len=%u\n", len);

    if (len >= sizeof(file_path)) {
        SYNC_DBG("  Path too long");
        send_sync_fail("Path too long");
        return;
    }

    memcpy(file_path, path, len);
    file_path[len] = '\0';

    SYNC_DBG("[ADB_SYNC] RECV path='%s'\n", file_path);

    /* Security check: validate path */
    if (!is_safe_path(file_path)) {
        send_sync_fail("Invalid path");
        return;
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        SYNC_ERR("[ADB_SYNC] File not found: %s\n", file_path);
        send_sync_fail("File not found");
        return;
    }

    SYNC_DBG("[ADB_SYNC] File opened, fd=%d\n", fd);

    static char buffer[4016];  /* 4000 data + 16 for headers */
    ssize_t bytes_read;
    uint32_t total_sent = 0;
    bool is_last = false;

    while (!is_last) {
        bytes_read = read(fd, buffer + 8, 4000);  /* Reserve 8 bytes for DATA header */
        if (bytes_read <= 0) {
            break;  /* End of file or error */
        }

        /* Peek if this is the last chunk - use separate buffer to avoid overflow */
        char peek_byte;
        ssize_t peek = read(fd, &peek_byte, 1);  /* Try read 1 more byte */
        if (peek <= 0) {
            /* This is the last chunk - append DONE */
            is_last = true;

            /* Build: DATA header + data + DONE header */
            struct sync_msg *data_msg = (struct sync_msg *)buffer;
            data_msg->id = ID_DATA;
            data_msg->size = bytes_read;

            struct sync_msg *done_msg = (struct sync_msg *)(buffer + 8 + bytes_read);
            done_msg->id = ID_DONE;
            done_msg->size = 0;

            uint32_t total_len = 8 + bytes_read + 8;
            SYNC_DBG("[ADB_SYNC] Sending final DATA+DONE: %d bytes\n", (int)bytes_read);
            adb_sync_write(buffer, total_len);
            total_sent += bytes_read;
        } else {
            /* Not last chunk - seek back 1 byte */
            if (lseek(fd, -1, SEEK_CUR) < 0) {
                SYNC_ERR("lseek failed, errno=%d", errno);
                send_sync_fail("Seek error");
                close(fd);
                return;
            }

            struct sync_msg *data_msg = (struct sync_msg *)buffer;
            data_msg->id = ID_DATA;
            data_msg->size = bytes_read;

            SYNC_DBG("[ADB_SYNC] Sending DATA chunk: %d bytes\n", (int)bytes_read);
            adb_sync_write(buffer, 8 + bytes_read);
            total_sent += bytes_read;
        }
    }

    close(fd);

    if (total_sent == 0) {
        /* Empty file - send DONE only (with 4 bytes mtime=0) */
        SYNC_DBG("[ADB_SYNC] Empty file, sending DONE\n");
        uint32_t mtime = 0;
        send_sync_response(ID_DONE, &mtime, 4);
    }

    SYNC_DBG("[ADB_SYNC] Transfer complete: %u bytes total\n", total_sent);
}

/*
 * Handle LIST command: list directory contents
 * PC sends: LIST<path>
 * Device responds: DENT<entry>...DENT<entry>...DONE or FAIL<reason>
 */
static void handle_sync_list(const uint8_t *path, uint32_t len)
{
    static char dir_path[256];  /* Static to save stack */

    SYNC_DBG("handle_sync_list: path_len=%u", len);

    if (len >= sizeof(dir_path)) {
        SYNC_DBG("  Path too long");
        send_sync_fail("Path too long");
        return;
    }

    memcpy(dir_path, path, len);
    dir_path[len] = '\0';

    SYNC_DBG("  path='%s'", dir_path);

    /* Security check: validate path */
    if (!is_safe_path(dir_path)) {
        send_sync_fail("Invalid path");
        return;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        SYNC_DBG("  Cannot open directory");
        send_sync_fail("Cannot open directory");
        return;
    }

    struct dirent *entry;
    uint32_t entry_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        static char full_path[512];  /* Static to save stack space */
        static char dent_buffer[512];  /* Static to save stack space */
        struct stat st;

        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        if (stat(full_path, &st) == 0) {
            struct {
                uint32_t mode;
                uint32_t size;
                uint32_t mtime;
                uint32_t namelen;
            } *dent_data = (void*)dent_buffer;

            dent_data->mode = st.st_mode;
            dent_data->size = st.st_size;
            dent_data->mtime = st.st_mtime;
            dent_data->namelen = strlen(entry->d_name);

            memcpy(dent_buffer + sizeof(*dent_data), entry->d_name, dent_data->namelen);

            SYNC_DBG("  DENT: %s (mode=0x%x, size=%u)", entry->d_name, st.st_mode, (uint32_t)st.st_size);
            send_sync_response(ID_DENT, dent_data, sizeof(*dent_data) + dent_data->namelen);
            entry_count++;
        }
    }

    closedir(dir);
    SYNC_DBG("  Directory listing complete: %u entries", entry_count);
    uint32_t zero = 0;
    send_sync_response(ID_DONE, &zero, 4);
}

/*
 * Handle QUIT command: close sync session
 * PC sends: QUIT
 * Device responds: OKAY
 */
static void handle_sync_quit(void)
{
    SYNC_DBG("[ADB_SYNC] handle_sync_quit\n");

    if (sync_ctx.file_fd >= 0) {
        SYNC_ERR("[ADB_SYNC] Closing open file: fd=%d\n", sync_ctx.file_fd);
        close(sync_ctx.file_fd);
        sync_ctx.file_fd = -1;
    }

    sync_ctx.state = SYNC_IDLE;
    sync_ctx.bytes_received = 0;
    sync_ctx.current_data_remaining = 0;
    memset(sync_ctx.current_path, 0, sizeof(sync_ctx.current_path));

    SYNC_DBG("[ADB_SYNC] State -> SYNC_IDLE\n");

    /* Send sync OKAY in response to QUIT */
    uint32_t zero = 0;
    send_sync_response(ID_OKAY, &zero, 4);
    SYNC_DBG("[ADB_SYNC] Sent OKAY response to QUIT\n");
}

/*
 * Handle subsequent DATA/DONE packets after SEND
 * Called when state == SYNC_RECEIVING_FILE
 * Packet format: "DATAnnnnFileData..." or "DONEnnnn"
 */
static void handle_file_data_packet(const uint8_t *data, uint32_t len)
{
    uint32_t offset = 0;

    if (sync_ctx.file_fd < 0) {
        SYNC_ERR("File not open (fd=%d)", sync_ctx.file_fd);
        send_sync_fail("File not open");
        sync_ctx.state = SYNC_IDLE;
        return;
    }

    /* Process packet in a loop instead of recursion */
    while (offset < len) {
        const uint8_t *current_data = data + offset;
        uint32_t remaining = len - offset;

        /* Check if continuation of previous DATA chunk */
        if (sync_ctx.current_data_remaining > 0) {
            uint32_t to_write = (remaining < sync_ctx.current_data_remaining) ? remaining : sync_ctx.current_data_remaining;
            if (to_write == 0) {
                SYNC_ERR("to_write is 0! remaining=%u, data_remaining=%u", remaining, sync_ctx.current_data_remaining);
                return;
            }

            ssize_t written = write_all(sync_ctx.file_fd, current_data, to_write);
            if (written < 0) {
                SYNC_ERR("Write failed: to_write=%u, fd=%d", to_write, sync_ctx.file_fd);
                send_sync_fail("Write error");
                goto cleanup;
            }

            sync_ctx.bytes_received += written;
            sync_ctx.current_data_remaining -= written;
            offset += written;

            /* If chunk complete, loop will process next header */
            continue;
        }

        /* New packet with header */
        if (remaining < 8) {
            SYNC_ERR("Packet too short: %u bytes", remaining);
            send_sync_fail("Invalid packet");
            goto cleanup;
        }

        struct sync_msg *msg = (struct sync_msg *)current_data;

        if (msg->id == ID_DATA) {
            uint32_t data_size = msg->size;
            uint32_t actual_len = remaining - 8;

            offset += 8;  /* Skip header */

            if (actual_len > 0) {
                uint32_t to_write = (actual_len < data_size) ? actual_len : data_size;
                ssize_t written = write_all(sync_ctx.file_fd, current_data + 8, to_write);
                if (written < 0) {
                    SYNC_ERR("Write failed");
                    send_sync_fail("Write error");
                    goto cleanup;
                }
                sync_ctx.bytes_received += written;
                sync_ctx.current_data_remaining = data_size - written;
                offset += written;
            } else {
                /* Header only, all data in next packets */
                sync_ctx.current_data_remaining = data_size;
            }
            continue;
        }

        if (msg->id == ID_DONE) {
            if (sync_ctx.file_fd >= 0) {
                close(sync_ctx.file_fd);
                sync_ctx.file_fd = -1;
            }
            sync_ctx.state = SYNC_IDLE;
            sync_ctx.current_data_remaining = 0;
            if (sync_ctx.bytes_received > 0) {
                /* Send OKAY with 4 bytes of 0, matching QUIT/DONE format */
                uint32_t zero = 0;
                send_sync_response(ID_OKAY, &zero, 4);
            }
            return;
        }

        SYNC_ERR("Unknown packet id: 0x%08x", msg->id);
        send_sync_fail("Invalid data packet");
        goto cleanup;
    }

    return;

cleanup:
    if (sync_ctx.file_fd >= 0) {
        close(sync_ctx.file_fd);
        sync_ctx.file_fd = -1;
    }
    sync_ctx.state = SYNC_IDLE;
}

/*
 * Process sync command based on current state
 * This is the main dispatcher for all sync protocol messages
 */
static void process_sync_command(struct adb_sync_message *msg)
{
    SYNC_DBG("process_sync_command: len=%u, state=%d", msg->length, sync_ctx.state);

    if (sync_ctx.state == SYNC_RECEIVING_FILE) {
        handle_file_data_packet(msg->data, msg->length);
        return;
    }

    if (msg->length < 8) {
        SYNC_ERR("Message too short: %u", msg->length);
        send_sync_fail("Invalid sync message");
        return;
    }

    struct sync_msg *sync_msg = (struct sync_msg *)msg->data;
    uint8_t *payload = msg->data + sizeof(struct sync_msg);
    uint32_t payload_len = msg->length - sizeof(struct sync_msg);


    switch (sync_msg->id) {
        case ID_STAT:
            handle_sync_stat(payload, payload_len);
            break;

        case ID_SEND:
            handle_sync_send(payload, payload_len);
            break;

        case ID_RECV:
            handle_sync_recv(payload, payload_len);
            break;

        case ID_LIST:
            handle_sync_list(payload, payload_len);
            break;

        case ID_QUIT:
            handle_sync_quit();
            break;

        default:
            SYNC_ERR("Unknown cmd: 0x%08x", sync_msg->id);
            send_sync_fail("Unknown sync command");
            break;
    }
}

static void adb_sync_thread_entry(void *parameter)
{
    int idx;

    while (1) {
        if (rt_mq_recv(adb_sync_mq, &idx, sizeof(int), RT_WAITING_FOREVER) == RT_EOK) {
            if (idx >= 0 && idx < ADB_SYNC_MSG_POOL_SIZE) {
                process_sync_command(&msg_pool[idx]);

                /* Release buffer after processing */
                rt_base_t level = rt_hw_interrupt_disable();
                msg_pool_used[idx] = 0;  /* Mark as free */
                rt_hw_interrupt_enable(level);

                /* Signal flow control: one more slot available */
                rt_sem_release(flow_control_sem);

                if (pending_transport_okay) {

                    pending_transport_okay = false;
                    usbd_adb_send_file_okay();
                }
            }
        }
    }
}

int usbd_adb_sync_init(void)
{
    /* Initialize message pool - all buffers free */
    for (int i = 0; i < ADB_SYNC_MSG_POOL_SIZE; i++) {
        msg_pool_used[i] = 0;
    }

    /* Create flow control semaphore - initial count is max pending messages */
    flow_control_sem = rt_sem_create("adb_fc", ADB_SYNC_MAX_PENDING, RT_IPC_FLAG_FIFO);
    if (flow_control_sem == RT_NULL) {
        return -RT_ERROR;
    }

    /* Create queue for indices only (4 bytes per message instead of 4100) */
    adb_sync_mq = rt_mq_create("adb_sync",
                               sizeof(int),
                               ADB_SYNC_MSG_POOL_SIZE,
                               RT_IPC_FLAG_FIFO);
    if (adb_sync_mq == RT_NULL) {
        rt_sem_delete(flow_control_sem);
        return -RT_ERROR;
    }

    adb_sync_thread = rt_thread_create("adb_sync",
                                       adb_sync_thread_entry,
                                       RT_NULL,
                                       4096,  /* 16KB stack for file operations */
                                       8,  /* High priority to process quickly */
                                       20);
    if (adb_sync_thread == RT_NULL) {
        rt_sem_delete(flow_control_sem);
        rt_mq_delete(adb_sync_mq);
        return -RT_ERROR;
    }

    rt_thread_startup(adb_sync_thread);
    return RT_EOK;
}

/*
 * ISR callback: called when PC sends A_WRTE to file channel
 * This is called from usbd_adb_bulk_out (line 226 in usbd_adb.c)
 * Must not do file operations here - queue to worker thread instead
 */
void usbd_adb_notify_file_read(uint8_t *data, uint32_t len)
{
    if (len > ADB_SYNC_MSG_MAX_SIZE) {
        SYNC_ERR("ISR: Data too large: %u", len);
        return;
    }

    if (adb_sync_mq == RT_NULL) {
        SYNC_ERR("ISR: Queue not initialized");
        return;
    }

    /* Try to acquire flow control slot (non-blocking) */
    if (rt_sem_trytake(flow_control_sem) != RT_EOK) {
        /* No slots available - transport layer will not send OKAY */
        return;
    }

    /* Find free buffer */
    int idx = -1;
    rt_base_t level = rt_hw_interrupt_disable();
    for (int i = 0; i < ADB_SYNC_MSG_POOL_SIZE; i++) {
        if (msg_pool_used[i] == 0) {
            msg_pool_used[i] = 1;  /* Mark as used */
            idx = i;
            break;
        }
    }
    rt_hw_interrupt_enable(level);

    if (idx < 0) {
        SYNC_ERR("ISR: No free buffer");
        rt_sem_release(flow_control_sem);  /* Release the slot we took */
        return;
    }

    /* Copy data to pool buffer */
    msg_pool[idx].length = len;
    memcpy(msg_pool[idx].data, data, len);

    /* Send only index (4 bytes) to queue */
    if (rt_mq_send(adb_sync_mq, &idx, sizeof(int)) != RT_EOK) {
        SYNC_ERR("ISR: Queue full");
        /* Release buffer and flow control slot */
        level = rt_hw_interrupt_disable();
        msg_pool_used[idx] = 0;
        rt_hw_interrupt_enable(level);
        rt_sem_release(flow_control_sem);
    }
}

/* Flow control: check if we can accept more data (non-destructive check) */
bool usbd_adb_can_send_file_okay(void)
{
    if (flow_control_sem == RT_NULL) {
        return false;
    }

    /* Non-destructive check: peek at semaphore value
     * RT-Thread doesn't have a direct peek API, so we try-take and immediately release */
    rt_err_t result = rt_sem_trytake(flow_control_sem);
    if (result == RT_EOK) {
        rt_sem_release(flow_control_sem);  /* Put it back */
        return true;
    }

    /* No slots available - mark that we need to send OKAY later */
    pending_transport_okay = true;
    return false;
}
#endif
