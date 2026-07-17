/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <fcntl.h>

#include <rthw.h>
#include <rtthread.h>

#include <msh.h>

#include <dfs_fs.h>
#include <dfs_posix.h>

#include <ioremap.h>
#include <riscv_io.h>

#include "board.h"

#include "./config.h"
#include "sdk_version.h"

#include "drv_gpio.h"

#include "sysctl_pwr.h"
#include "sysctl_boot.h"

#define K230_TRUSTED_PRELOAD_CMD "@preload"

#ifdef ENABLE_CHERRY_USB

#ifdef ENABLE_CHERRY_USB_OTG
#include "usbotg_core.h"
#endif // ENABLE_CHERRY_USB_OTG

#if defined(ENABLE_CHERRY_USB_DEVICE) || defined(ENABLE_CHERRY_USB_OTG)
#include "usbd_desc.h"
#endif // ENABLE_CHERRY_USB_DEVICE || ENABLE_CHERRY_USB_OTG

#if defined(ENABLE_CHERRY_USB_HOST) || defined(ENABLE_CHERRY_USB_OTG)
#include "usbh_core.h"
#include "drv_gpio.h"
#endif // ENABLE_CHERRY_USB_HOST || ENABLE_CHERRY_USB_OTG

#if defined(ENABLE_CHERRY_USB_DEVICE) && defined (ENABLE_CHERRY_USB_HOST) && !defined(ENABLE_CHERRY_USB_OTG)
  #if CHERRY_USB_DEVICE_USING_DEV + CHERRY_USB_HOST_USING_DEV != 1
    #error "Can not set same usb device as device and host"
  #endif
#endif

#endif // ENABLE_CHERRY_USB

bool g_fs_mount_data_succ = false;
bool g_fs_mount_sdcard_succ = false;

pid_t exec(char *filename, int debug, int argc, char **argv);

#if defined(ENABLE_CHERRY_USB) && !defined(ENABLE_CHERRY_USB_OTG) && defined(ENABLE_CHERRY_USB_HOST) && defined(ENABLE_CANMV_USB_HOST)
struct usbh_initialize_async_arg {
  uint8_t busid;
  uint32_t reg_base;
};

static struct usbh_initialize_async_arg usbh_initialize_async_args;

static void usbh_initialize_async_entry(void *parameter)
{
  struct usbh_initialize_async_arg *arg = (struct usbh_initialize_async_arg *)parameter;

  usbh_initialize(arg->busid, arg->reg_base);
}

static int usbh_initialize_async(uint8_t busid, uint32_t reg_base)
{
  rt_thread_t tid;

  usbh_initialize_async_args.busid = busid;
  usbh_initialize_async_args.reg_base = reg_base;

  tid = rt_thread_create("usbh_init",
                         usbh_initialize_async_entry,
                         &usbh_initialize_async_args,
                         4096,
                         RT_THREAD_PRIORITY_MAX - 1,
                         20);
  if (tid) {
    rt_thread_startup(tid);
    return 0;
  }

  return usbh_initialize(busid, reg_base);
}
#endif

static const struct dfs_mount_tbl* const auto_mount_table[SYSCTL_BOOT_MAX] = {
    (const struct dfs_mount_tbl[]) {
        /* Nor Flash */
        { 0 },
    },
    (const struct dfs_mount_tbl[]) {
        /* Nand Flash */
        { "nand0", "/bin", "uffs", 0, 0 },
#if CONFIG_RT_PARTITION_NUMBER >= 2
        { "nand1", "/sdcard", "uffs", 0, 0 },
#endif
        { 0 },
    },
    (const struct dfs_mount_tbl[]) {
        /* EMMC */
        { "sd00", "/bin", "elm", 0, 0 },
#if CONFIG_RT_PARTITION_NUMBER == 2
        { "sd01", "/data", "elm", 0, 0 },
#elif CONFIG_RT_PARTITION_NUMBER == 3
        { "sd01", "/sdcard", "elm", 0, 0 },
        { "sd02", "/data", "elm", 0, 0 },
#endif
        { 0 },
    },
    (const struct dfs_mount_tbl[]) {
        /* SdCard */
        { "sd10", "/bin", "elm", 0, 0 },
#if CONFIG_RT_PARTITION_NUMBER == 2
        { "sd11", "/data", "elm", 0, 0 },
#elif CONFIG_RT_PARTITION_NUMBER == 3
        { "sd11", "/sdcard", "elm", 0, 0 },
        { "sd12", "/data", "elm", 0, 0 },
#endif
        { 0 },
    },
};

static void mnt_mount_table(void)
{
    int ret;
    int err                     = 0;
    int fd                      = -1;
    int mkfs_for_data_partition = 0;
#if defined(CONFIG_RT_AUTO_RESIZE_PARTITION)
    bool format_data_partition  = false;
    bool auto_mkfs_marker       = false;
#endif

    sysctl_boot_mode_e          boot_mode;
    const struct dfs_mount_tbl* mnt_tbl = NULL;

    boot_mode = sysctl_boot_get_boot_mode();
    boot_mode &= 0x03;
    mnt_tbl = auto_mount_table[boot_mode];

    while (1) {
        if (!mnt_tbl->path) {
            break;
        }

        if (0x00
            == (ret = dfs_mount(mnt_tbl->device_name, mnt_tbl->path, mnt_tbl->filesystemtype, mnt_tbl->rwflag,
                                mnt_tbl->data))) {
            if (0x00 == rt_strcmp("/data", mnt_tbl->path)) {
                g_fs_mount_data_succ = true;
            } else if (0x00 == rt_strcmp("/sdcard", mnt_tbl->path)) {
                g_fs_mount_sdcard_succ = true;
            }
        } else {
            err = errno;
            rt_kprintf("mount fs[%s] on %s failed(%d), error %d.\n", mnt_tbl->filesystemtype, mnt_tbl->path, ret, err);

            if ((0x00 == rt_strncmp("/data", mnt_tbl->path, sizeof("/data") - 1))
                && (0x00 == rt_strncmp("elm", mnt_tbl->filesystemtype, sizeof("elm") - 1))) {

#if defined(CONFIG_RT_AUTO_RESIZE_PARTITION)
                if (0 <= (fd = open("/bin/auto_mkfs_data", O_RDONLY))) {
                    close(fd);
                    auto_mkfs_marker = true;
                    format_data_partition = true;
                }

#if defined(CONFIG_RT_AUTO_RESIZE_PARTITION_ALWAYS)
                format_data_partition = true;
#endif

                if (format_data_partition && (0x00 == mkfs_for_data_partition)) {
                    mkfs_for_data_partition = 1;

                    rt_kprintf("\033[31mStart format partition[2] to fat, it will took a long time, DO NOT POWEROFF "
                               "THE BOARD, PLEASE WAIT IT DONE\033[0m\n");
                    int mkfs_ret = dfs_mkfs("elm", mnt_tbl->device_name);
                    if (RT_EOK == mkfs_ret) {
                        if (auto_mkfs_marker &&
                            (0 != unlink("/bin/auto_mkfs_data"))) {
                            rt_kprintf("remove /bin/auto_mkfs_data failed\n");
                        }
                        rt_kprintf("\n\n\033[32mformat done.\033[0m\n");
                        mnt_tbl--;
                    } else {
                        rt_kprintf("\n\n\033[31mformat failed (%d), errno %d.\033[0m\n",
                                   mkfs_ret, errno);
                    }
                }
#endif

                if ((-19) == err && !format_data_partition) {
                    rt_kprintf("Please format the partition[2] to FAT32.\nRefer to "
                               "https://support.microsoft.com/zh-cn/windows/"
                               "%E5%88%9B%E5%BB%BA%E5%92%8C%E6%A0%BC%E5%BC%8F%E5%8C%96%E7%A1%AC%E7%9B%98%E5%88%86%E5%"
                               "8C%BA-bbb8e185-1bda-ecd1-3465-c9728f7d7d2e\n");
                }
            }
        }

        mnt_tbl++;
    }
}

#ifdef MOUNT_SECOND_CARD

extern void kd_sdhci_change(int id);

static void mount_second_card(void)
{
    int      ret;
    int      err         = 0;
    uint32_t wait_sd_cnt = 0;

    sysctl_boot_mode_e boot_mode;
    boot_mode = sysctl_boot_get_boot_mode();
    boot_mode &= 0x03;

    const char* device_name = (SYSCTL_BOOT_EMMC == boot_mode) ? "sd10" : "sd01";

    if (SYSCTL_BOOT_EMMC == boot_mode) {
        kd_sdhci_change(1);
    } else if (SYSCTL_BOOT_SDCARD == boot_mode) {
        kd_sdhci_change(0);
    }

    while (mmcsd_wait_cd_changed(100) != MMCSD_HOST_PLUGED) {
        if (++wait_sd_cnt > 5) {
            rt_kprintf("no second mmc device\n");
            break;
        }
    }

    if (0x00 != (ret = dfs_mount(device_name, "/ext_data", "elm", 0, 0))) {
        err = errno;
        rt_kprintf("mount fs[elm] on /ext_data failed(%d), error %d.\n", ret, err);
    }
}
#endif

static rt_bool_t auto_exec_cmd_is_trusted_preload(const char *cmd)
{
  rt_size_t leading = 0;
  rt_size_t cmd0_size = 0;

  while (cmd[leading] == ' ' || cmd[leading] == '\t') {
    leading++;
  }

  cmd += leading;

  while (cmd[cmd0_size] != '\0' && cmd[cmd0_size] != ' ' && cmd[cmd0_size] != '\t') {
    cmd0_size++;
  }

  return cmd0_size == (sizeof(K230_TRUSTED_PRELOAD_CMD) - 1)
      && rt_strncmp(cmd, K230_TRUSTED_PRELOAD_CMD, cmd0_size) == 0;
}

static void auto_exec_trusted_preload(const char *cmd_line)
{
  char *cmd_copy;
  char *cursor;
  char *argv[8];
  int argc = 0;

  cmd_copy = rt_strdup(cmd_line);
  if (cmd_copy == RT_NULL) {
    rt_kprintf("auto exec trusted preload failed: no memory\n");
    return;
  }

  rt_memset(argv, 0x00, sizeof(argv));
  cursor = cmd_copy;
  while (*cursor != '\0' && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
    while (*cursor == ' ' || *cursor == '\t') {
      *cursor = '\0';
      cursor++;
    }

    if (*cursor == '\0') {
      break;
    }

    argv[argc++] = cursor;

    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
      cursor++;
    }
  }

  if (argc <= 0 || exec(argv[0], 0, argc, argv) <= 0) {
    rt_kprintf("trusted preload image unavailable, enable fast boot and ensure U-Boot preloads rtapp\n");
  }

  rt_free(cmd_copy);
}

int main(void) {
  sysctl_pwr_off(SYSCTL_PD_CPU0);
  sysctl_pwr_off(SYSCTL_PD_DPU);

  rt_kprintf("\n#############SDK VERSION######################################\n");
  rt_kprintf("SDK   : %s\n", SDK_VERSION_);
#ifdef CONFIG_SDK_ENABLE_CANMV
  rt_kprintf("CanMV : %s\n", CANMV_VERSION_);
#endif //CONFIG_SDK_ENABLE_CANMV
  rt_kprintf("nncase: %s\n", NNCASE_VERSION_);
  rt_kprintf("##############################################################\n");

#ifdef RT_USING_SDIO
  uint32_t wait_sd_cnt = 0;
  while (mmcsd_wait_cd_changed(100) != MMCSD_HOST_PLUGED) {
    if(++wait_sd_cnt > 5) {
      rt_kprintf("no mmc device\n");
      break;
    }
  }
#endif //RT_USING_SDIO

  mnt_mount_table();
  excute_sdcard_config();

#if defined (RT_USING_SDIO) && defined (MOUNT_SECOND_CARD)
  mount_second_card();
#endif // MOUNT_SECOND_CARD

#if defined (RT_RECOVERY_MPY_AUTO_EXEC_PY)
  extern int check_delete_file_mark(void);
  check_delete_file_mark();
#endif

#ifdef ENABLE_CHERRY_USB
  void *usb_base;
  const void *usb_dev_addr[2] = {(void *)0x91500000UL, (void *)0x91540000UL};

#if defined (ENABLE_CHERRY_USB_OTG)
  usb_base = (void *)rt_ioremap((void *)usb_dev_addr[CHERRY_USB_OTG_USING_DEV], 0x10000);
  board_usb_device_register();
  usbotg_initialize(0, (uint32_t)(long)usb_base, board_usb_device_event_handler, USBOTG_MODE_OTG);
#else
  /* Strange BUG, ​​USB Host must be initialized first */
#if defined (ENABLE_CHERRY_USB_HOST) && defined (ENABLE_CANMV_USB_HOST)
  usb_base = (void *)rt_ioremap((void *)usb_dev_addr[CHERRY_USB_HOST_USING_DEV], 0x10000);
  usbh_initialize_async(0, (uint32_t)(long)usb_base);

#ifdef CANMV_USB_PWR_PIN
  int usb_host_pin = CANMV_USB_PWR_PIN;
  if(0 <= usb_host_pin) {
    kd_pin_mode(usb_host_pin, GPIO_DM_OUTPUT);
    kd_pin_write(usb_host_pin, CANMV_USB_PWR_PIN_VALID_VAL);
  }
#endif

#endif // ENABLE_CHERRY_USB_HOST

#if defined (ENABLE_CHERRY_USB_DEVICE)
  usb_base = (void *)rt_ioremap((void *)usb_dev_addr[CHERRY_USB_DEVICE_USING_DEV], 0x10000);
  board_usb_device_init(usb_base);
#endif // ENABLE_CHERRY_USB_DEVICE

#endif // ENABLE_CHERRY_USB_OTG
#endif //ENABLE_CHERRY_USB

#if defined CONFIG_BOARD_K230D_CANMV_LABPLUS_AI_CAMERA \
  || CONFIG_BOARD_K230D_CANMV_LABPLUS_AI_CAMERA_V2

  writel(0x8009900d, (volatile void *)(uintptr_t)(CMU_BASE_ADDR + 0x04UL));
  writel(0x8000040d, (volatile void *)(uintptr_t)(CMU_BASE_ADDR + 0x08UL));
  writel(0x80000c67, (volatile void *)(uintptr_t)(CMU_BASE_ADDR + 0x0CUL));
  writel(0x800002c2, (volatile void *)(uintptr_t)(CMU_BASE_ADDR + 0x80UL));

#endif

  {
    size_t cmd_length = rt_strlen(CONFIG_RTT_AUTO_EXEC_CMD);

    if(cmd_length) {
      if (auto_exec_cmd_is_trusted_preload(CONFIG_RTT_AUTO_EXEC_CMD)) {
        auto_exec_trusted_preload(CONFIG_RTT_AUTO_EXEC_CMD);
      }
#ifdef CONFIG_SECURE_BOOT_FIRMWARE_ENABLE
      else {
        rt_kprintf("secure boot requires CONFIG_RTT_AUTO_EXEC_CMD to start with @preload; skip auto exec\n");
      }
#else
      else {
        msh_exec(CONFIG_RTT_AUTO_EXEC_CMD, cmd_length);
      }
#endif
    }
  }

  return 0;
}
