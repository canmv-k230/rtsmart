/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIBC_DIRENT_H__
#define LIBC_DIRENT_H__

#define DT_UNKNOWN           0x00
#define DT_FIFO              0x01
#define DT_DIR               0x04
#define DT_REG               0x08
/* musl's (Linux's) values, which getdents() hands to user space */
#ifndef DT_CHR
#define DT_CHR               0x02
#endif
#ifndef DT_BLK
#define DT_BLK               0x06
#endif
#ifndef DT_LNK
#define DT_LNK               0x0a
#endif
#ifndef DT_SOCK
#define DT_SOCK              0x0c
#endif

#endif
