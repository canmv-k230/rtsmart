/*
 * uMTP Responder
 * Copyright (c) 2018 - 2021 Viveris Technologies
 *
 * uMTP Responder is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
 *
 * uMTP Responder is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 3 for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with uMTP Responder; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file   mtp.h
 * @brief  Main MTP protocol functions.
 * @author Jean-François DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#ifndef _INC_MTP_H_
#define _INC_MTP_H_

#define MAX_STORAGE_NB 4
#define MAX_CFG_STRING_SIZE 128

/* DWC2 bulk endpoint transfer limits. */
#define MTP_USB_BULK_MAX_PACKET_COUNT   1023U
#define MTP_USB_BULK_MAX_TRANSFER_SIZE  0x7FFFFU

#pragma pack(1)

typedef struct _MTP_PACKET_HEADER
{
	uint32_t length;
	uint16_t operation;
	uint16_t code;
	uint32_t tx_id;
}MTP_PACKET_HEADER;

#pragma pack()

#include "fs_handles_db.h"
#include "rtthread.h"

#ifndef MTP_FS_DB_POOL_SIZE
#ifdef CONFIG_MTP_FS_DB_POOL_SIZE
#define MTP_FS_DB_POOL_SIZE CONFIG_MTP_FS_DB_POOL_SIZE
#else
#define MTP_FS_DB_POOL_SIZE CONFIG_FS_DB_POOL_SIZE
#endif
#endif

#define pthread_mutex_t struct rt_mutex

typedef struct mtp_usb_cfg_
{
	uint16_t usb_vendor_id;
	uint16_t usb_product_id;
	uint8_t  usb_class;
	uint8_t  usb_subclass;
	uint8_t  usb_protocol;
	uint16_t usb_dev_version;
	uint16_t usb_max_packet_size;
	uint8_t  usb_functionfs_mode;

	char usb_string_manufacturer[MAX_CFG_STRING_SIZE + 1];
	char usb_string_product[MAX_CFG_STRING_SIZE + 1];
	char usb_string_serial[MAX_CFG_STRING_SIZE + 1];
	char usb_string_version[MAX_CFG_STRING_SIZE + 1];

	int wait_connection;
	int loop_on_disconnect;

	int show_hidden_files;

	int val_umask;

}mtp_usb_cfg;

typedef struct mtp_storage_
{
	char * root_path;
	char * description;
	uint32_t storage_id;
	uint32_t flags;
	int uid;
	int gid;
}mtp_storage;

#define UMTP_STORAGE_LOCKED      0x00000010
#define UMTP_STORAGE_LOCKABLE    0x00000008
#define UMTP_STORAGE_REMOVABLE   0x00000004
#define UMTP_STORAGE_NOTMOUNTED  0x00000002
#define UMTP_STORAGE_READONLY    0x00000001
#define UMTP_STORAGE_READWRITE   0x00000000

typedef struct mtp_ctx_
{
	uint32_t session_id;

	mtp_usb_cfg usb_cfg;

	void * usb_ctx;

	unsigned char * wrbuffer;
	int usb_wr_buffer_max_size;

	unsigned char * rdbuffer;
	unsigned char * rdbuffer2;
	int usb_rd_buffer_max_size;

	unsigned char * read_file_buffer;
	int read_file_buffer_size;
	uint32_t fs_db_cache_buckets;
	uint32_t fs_db_pool_size;
	uint32_t fs_dir_read_buffer_size;
	int fs_db_scan_cache;
	volatile uint32_t fs_db_change_generation;
	volatile uint32_t fs_db_session_generation;
	uint32_t fs_db_session_counter;

	uint32_t *temp_array;

	fs_handles_db * fs_db;

	uint32_t SendObjInfoHandle;
	mtp_size SendObjInfoSize;
	mtp_offset SendObjInfoOffset;

	uint32_t SetObjectPropValue_Handle;
	uint32_t SetObjectPropValue_PropCode;

	uint32_t max_packet_size;

	mtp_storage storages[MAX_STORAGE_NB];

	int inotify_fd;
	pthread_t inotify_thread;
	pthread_mutexattr_t inotify_mutex_attr;
	pthread_mutex_t inotify_mutex;

	int msgqueue_id;
	pthread_t msgqueue_thread;

	int no_inotify;

	int uid,euid;
	int gid,egid;

	int default_uid;
	int default_gid;

	volatile int cancel_req;
	volatile int cancel_status_pending;
	volatile int reset_req;
	volatile int transferring_file_data;
	volatile int transaction_active;
	volatile uint32_t active_transaction_id;
	uint16_t pending_data_operation;
	uint32_t pending_data_transaction_id;
}mtp_ctx;

mtp_ctx * mtp_init_responder();

int  mtp_incoming_packet(mtp_ctx * ctx);
void mtp_set_usb_handle(mtp_ctx * ctx, void * handle, uint32_t max_packet_size);

int mtp_load_config_file(mtp_ctx * context, const char * conffile);

uint32_t mtp_add_storage(mtp_ctx * ctx, char * path, char * description, int uid, int gid, uint32_t flags);
int mtp_remove_storage(mtp_ctx * ctx, char * name);
int mtp_get_storage_index_by_name(mtp_ctx * ctx, char * name);
int mtp_get_storage_index_by_id(mtp_ctx * ctx, uint32_t storage_id);
uint32_t mtp_get_storage_id_by_name(mtp_ctx * ctx, char * name);
char * mtp_get_storage_description(mtp_ctx * ctx, uint32_t storage_id);
char * mtp_get_storage_root(mtp_ctx * ctx, uint32_t storage_id);
uint32_t mtp_get_storage_flags(mtp_ctx * ctx, uint32_t storage_id);

int check_handle_access( mtp_ctx * ctx, fs_entry * entry, uint32_t handle, int wraccess, uint32_t * response);
int begin_edit_object(mtp_ctx * ctx, uint32_t handle, uint32_t * response);
int end_edit_object(mtp_ctx * ctx, uint32_t handle, uint32_t * response);
void clear_edit_locks(fs_handles_db * db);

int mtp_push_event(mtp_ctx * ctx, uint32_t event, int nbparams, uint32_t * parameters );

void mtp_deinit_responder(mtp_ctx * ctx);
uint32_t mtp_fs_db_session_begin(mtp_ctx * ctx);
void mtp_fs_db_session_end(mtp_ctx * ctx);
uint32_t mtp_fs_db_session_get(mtp_ctx * ctx);

int build_response(mtp_ctx * ctx, uint32_t tx_id, uint16_t type, uint16_t status, void * buffer, int maxsize, void * datain,int size);
int check_and_send_USB_ZLP(mtp_ctx * ctx , int size);
int mtp_get_usb_bulk_transfer_limit(mtp_ctx * ctx);
int mtp_send_data_response(mtp_ctx * ctx, int size);
int parse_incomming_dataset(mtp_ctx * ctx,void * datain,int size,uint32_t * newhandle, uint32_t parent_handle, uint32_t storage_id);

#define APP_VERSION "v1.6.6"

#endif
