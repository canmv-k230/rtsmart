/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <rthw.h>
#include <rtthread.h>

#include <dfs_fs.h>
#include <ioremap.h>
#include "riscv_mmu.h"

#include <msh.h>

#include "./config.h"
#include "dfs_posix.h"
#include "sdk_version.h"

#ifdef CONFIG_BOARD_K230_CANMV_LCKFB
#include "drv_gpio.h"
#endif // CONFIG_BOARD_K230_CANMV_LCKFB

#include "sysctl_pwr.h"
#include "sysctl_boot.h"

#ifdef ENABLE_CHERRY_USB

#ifdef ENABLE_CHERRY_USB_DEVICE
#include "usbd_desc.h"
#endif // ENABLE_CHERRY_USB_DEVICE

#ifdef ENABLE_CHERRY_USB_HOST
#include "usbh_core.h"
#include "drv_gpio.h"
#endif // ENABLE_CHERRY_USB_HOST

#if defined(ENABLE_CHERRY_USB_DEVICE) && defined (ENABLE_CHERRY_USB_HOST)
  #if CHERRY_USB_DEVICE_USING_DEV + CHERRY_USB_HOST_USING_DEV != 1
    #error "Can not set same usb device as device and host"
  #endif
#endif

#endif // ENABLE_CHERRY_USB

bool g_fs_mount_data_succ = false;
bool g_fs_mount_sdcard_succ = false;

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
#endif
#if CONFIG_RT_PARTITION_NUMBER == 3
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
#endif
#if CONFIG_RT_PARTITION_NUMBER == 3
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
                    unlink("/bin/auto_mkfs_data");
                    fd = 0x1234;
                }

                if ((0x1234 == fd) && (0x00 == mkfs_for_data_partition)) {
                    mkfs_for_data_partition = 1;

                    rt_kprintf("\033[31mStart format partition[2] to fat, it will took a long time, DO NOT POWEROFF "
                               "THE BOARD, PLEASE WAIT IT DONE\033[0m\n");
                    dfs_mkfs("elm", mnt_tbl->device_name);
                    rt_kprintf("\n\n\033[32mformat done.\033[0m\n");

                    mnt_tbl--;
                }
#endif

                if ((-19) == err) {
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

static void check_bank_voltage(void)
{
#define MAP_SIZE    PAGE_SIZE
#define MAP_MASK    (MAP_SIZE - 1)

  const rt_ubase_t target = 0x91213418UL;
  void *map_base = rt_ioremap_nocache((void *)(target & ~MAP_MASK), MAP_SIZE);
  volatile void *virt_addr = map_base + (target & MAP_MASK);
  volatile rt_uint32_t read_result = *((rt_uint32_t *) virt_addr);

  if(0x00 == read_result) {
#if !defined (CONFIG_BOARD_K230_CANMV) && !defined (CONFIG_BOARD_K230_CANMV_DONGSHANPI) && !defined (CONFIG_BOARD_K230_CANMV_RTT_EVB)
    rt_kprintf("\n\n\033[31mTHIS BOARD MAYBE NOT CONFIGURE BANK VOLTAGE!!!\n\n\033[0m");
#endif

#ifdef CONFIG_BOARD_K230_CANMV_LCKFB
    kd_pin_mode(62, GPIO_DM_OUTPUT);
    kd_pin_write(62, GPIO_PV_LOW);
#endif
  }

  rt_iounmap(map_base);
}

int main(void) {
  sysctl_pwr_off(SYSCTL_PD_CPU0);
  sysctl_pwr_off(SYSCTL_PD_DPU);

  check_bank_voltage();

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

#if defined (RT_RECOVERY_MPY_AUTO_EXEC_PY)
  extern int check_delete_file_mark(void);
  check_delete_file_mark();
#endif

#ifdef ENABLE_CHERRY_USB
  void *usb_base;
  const void *usb_dev_addr[2] = {(void *)0x91500000UL, (void *)0x91540000UL};

  /* Strange BUG, ​​USB Host must be initialized first */
#if defined (ENABLE_CHERRY_USB_HOST) && defined (ENABLE_CANMV_USB_HOST)
  usb_base = (void *)rt_ioremap((void *)usb_dev_addr[CHERRY_USB_HOST_USING_DEV], 0x10000);
  usbh_initialize(0, (uint32_t)(long)usb_base);

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

#endif //ENABLE_CHERRY_USB

  {
    size_t cmd_length = rt_strlen(CONFIG_RTT_AUTO_EXEC_CMD);

    if(cmd_length) {
      msh_exec(CONFIG_RTT_AUTO_EXEC_CMD, cmd_length);
    }
  }

  return 0;
}
