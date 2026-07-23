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
 * @file   fs_handles_db.h
 * @brief  Local file system helpers and handles database management.
 * @author Jean-François DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#ifndef _INC_FS_HANDLES_DB_H_
#define _INC_FS_HANDLES_DB_H_

typedef struct fs_entry fs_entry;

typedef int64_t mtp_size;
typedef int64_t mtp_offset;

#define FS_HANDLE_MAX_FILENAME_SIZE 256

struct fs_entry
{
	uint32_t handle;
	uint32_t parent;
	uint32_t storage_id;
	char name[FS_HANDLE_MAX_FILENAME_SIZE + 1];
	uint32_t flags;
	uint32_t edit_session_id;
	mtp_size size;
	uint32_t date;
	uint16_t fat_date;
	uint16_t fat_time;
	uint32_t last_seen_scan;
	uint32_t children_scan_generation;

	int watch_descriptor;

	fs_entry * next;
	fs_entry * cache_next;
	fs_entry * handle_cache_next;
};

#define ENTRY_IS_DIR 0x00000001
#define ENTRY_IS_DELETED 0x00000002
#define ENTRY_FROM_DB_POOL 0x00000004

#define _DEF_FS_HANDLES_ 1

typedef struct fs_handles_db_
{
	fs_entry * entry_list;
	uint32_t next_handle;

	fs_entry * search_entry;
	uint32_t handle_search;
	uint32_t storage_search;

	fs_entry ** entry_cache;
	fs_entry ** handle_cache;
	uint32_t entry_cache_bucket_count;

	fs_entry * entry_pool;
	uint32_t entry_pool_count;
	uint32_t entry_pool_next;
	int entry_pool_exhausted;
	fs_entry * free_entry_list;

	void * dir_read_buffer;
	int dir_read_buffer_size;

	uint32_t scan_generation;

	void * mtp_ctx;
}fs_handles_db;


typedef struct filefoundinfo_
{
	int isdirectory;
	char filename[FS_HANDLE_MAX_FILENAME_SIZE + 1];
	mtp_size size;
	uint32_t date;
	uint16_t fat_date;
	uint16_t fat_time;
}filefoundinfo;


fs_handles_db * init_fs_db(void * mtp_ctx);
void deinit_fs_db(fs_handles_db * fsh);
int scan_and_add_folder(fs_handles_db * db, char * base, uint32_t parent, uint32_t storage_id);
fs_entry * init_search_handle(fs_handles_db * db, uint32_t parent, uint32_t storage_id);
fs_entry * get_next_child_handle(fs_handles_db * db);
fs_entry * get_entry_by_handle(fs_handles_db * db, uint32_t handle);
fs_entry * get_entry_by_handle_and_storageid(fs_handles_db * db, uint32_t handle, uint32_t storage_id);
fs_entry * get_entry_by_wd(fs_handles_db * db, int watch_descriptor, fs_entry * entry_list);
fs_entry * get_entry_by_storageid( fs_handles_db * db, uint32_t storage_id, fs_entry * entry_list );
fs_entry * add_entry(fs_handles_db * db, filefoundinfo *fileinfo, uint32_t parent, uint32_t storage_id);
fs_entry * search_entry(fs_handles_db * db, filefoundinfo *fileinfo, uint32_t parent, uint32_t storage_id);
fs_entry * alloc_root_entry(fs_handles_db * db, uint32_t storage_id);
void fs_rebuild_entry_cache(fs_handles_db * db);
void fs_invalidate_scan_cache(fs_handles_db * db);
void fs_invalidate_scan_cache_context(void * mtp_ctx);
int fs_scan_cache_valid(fs_handles_db * db, fs_entry * entry);
void fs_mark_entry_deleted(fs_handles_db * db, fs_entry * entry);
void fs_prune_deleted_entries(fs_handles_db * db);

int entry_open(fs_handles_db * db, fs_entry * entry);
int entry_read(fs_handles_db * db, int file, unsigned char * buffer_out, mtp_offset offset, mtp_size size);
void entry_close(int file);

char * build_full_path(fs_handles_db * db,char * root_path,fs_entry * entry);

int fs_remove_tree( char *folder );

int fs_entry_stat(char *path, filefoundinfo* fileinfo);

#endif
