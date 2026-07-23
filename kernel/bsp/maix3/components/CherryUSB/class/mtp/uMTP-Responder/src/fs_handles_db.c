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
 * @file   fs_handles_db.c
 * @brief  Local file system helpers and handles database management.
 * @author Jean-François DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#include "buildconf.h"

#include <inttypes.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "mtp.h"
#include "mtp_helpers.h"

#include "fs_handles_db.h"
#include "inotify.h"
#include "logs_out.h"
#include "dfs_posix.h"

int fs_remove_tree( char *folder )
{
	struct dirent *d;
	DIR * dir;
	char * tmpstr;
	int del_fail;

	del_fail = 0;

	dir = opendir (folder);
	if( dir )
	{
		do
		{
			d = readdir (dir);
			if( d )
			{
				tmpstr = malloc (strlen(folder) + strlen(d->d_name) + 4 );
				if( tmpstr )
				{
					strcpy(tmpstr,folder);
					strcat(tmpstr,"/");
					strcat(tmpstr,d->d_name);

					if( !strcmp(d->d_name, ".") || !strcmp(d->d_name, "..") )
					{
						free(tmpstr);
						continue;
					}

					/* Do not use stat() here: it follows symbolic links. */
					if( d->d_type == DT_DIR )
					{
						if( fs_remove_tree(tmpstr) )
							del_fail = 1;
					}
					else
					{
						if( unlink(tmpstr) )
							del_fail = 1;
					}

					free(tmpstr);
				}
				else
				{
					del_fail = 1;
				}
			}
		}while(d);

		closedir(dir);

		if( rmdir(folder) )
			del_fail = 1;
	}
	else
	{
		del_fail = 1;
	}

	return del_fail;
}

int fs_entry_stat(char *path, filefoundinfo* fileinfo)
{
	struct stat fileStat;
	int i;

	memset(&fileStat,0,sizeof(struct stat));
	if( !stat (path, &fileStat) )
	{
		if ( S_ISDIR ( fileStat.st_mode ) )
			fileinfo->isdirectory = 1;
		else
			fileinfo->isdirectory = 0;

		fileinfo->size = fileStat.st_size;
		if( fileStat.st_mtime > 0 && (uint64_t)fileStat.st_mtime <= UINT32_MAX )
			fileinfo->date = (uint32_t)fileStat.st_mtime;
		else
			fileinfo->date = 0;
		fileinfo->fat_date = 0;
		fileinfo->fat_time = 0;

		i = strlen(path);
		while( i )
		{
			if( path[i] == '/' )
			{
				i++;
				break;
			}
			i--;
		}

		strncpy(fileinfo->filename,&path[i],FS_HANDLE_MAX_FILENAME_SIZE);
		fileinfo->filename[FS_HANDLE_MAX_FILENAME_SIZE] = '\0';

		return 1;
	}

	return 0;
}

typedef struct fs_dir_iterator_
{
	DIR * dir;
	struct dirent * entries;
	int entries_size;
	int entry_offset;
	int buffer_size;
	int owns_entries;
}fs_dir_iterator;

static int fs_dir_read_buffer_size(fs_handles_db * db)
{
	mtp_ctx * ctx;
	uint32_t buffer_size;

	buffer_size = CONFIG_MTP_DIR_READ_BUFFER_SIZE;
	ctx = db ? (mtp_ctx *)db->mtp_ctx : NULL;
	if( ctx )
		buffer_size = ctx->fs_dir_read_buffer_size;

	if( (buffer_size < sizeof(struct dirent)) || (buffer_size > INT_MAX) )
		return 0;

	buffer_size -= buffer_size % sizeof(struct dirent);
	return (int)buffer_size;
}

static int fs_dir_next(fs_dir_iterator * iterator, struct dirent ** dirent)
{
	struct dfs_fd * fd;
	int ret;

	if( !iterator || !iterator->dir || !dirent )
		return -1;

	if( iterator->entries )
	{
		if( iterator->entry_offset >= iterator->entries_size )
		{
			fd = fd_get(iterator->dir->fd);
			if( !fd )
			{
				errno = EBADF;
				return -1;
			}

			ret = dfs_file_getdents(fd, iterator->entries, iterator->buffer_size);
			if( ret <= 0 )
			{
				if( ret < 0 )
					errno = -ret;
				return ret < 0 ? -1 : 0;
			}
			if( ret > iterator->buffer_size )
			{
				errno = EIO;
				return -1;
			}

			iterator->entries_size = ret;
			iterator->entry_offset = 0;
		}

		*dirent = (struct dirent *)((unsigned char *)iterator->entries + iterator->entry_offset);
		if( ((*dirent)->d_reclen < (offsetof(struct dirent, d_name) + 1)) ||
			((*dirent)->d_reclen > (iterator->entries_size - iterator->entry_offset)) ||
			((*dirent)->d_namlen >= ((*dirent)->d_reclen - offsetof(struct dirent, d_name))) ||
			((*dirent)->d_name[(*dirent)->d_namlen] != '\0') )
		{
			errno = EIO;
			return -1;
		}

		iterator->entry_offset += (*dirent)->d_reclen;
		return 1;
	}

	errno = 0;
	*dirent = readdir(iterator->dir);
	if( !*dirent )
		return errno ? -1 : 0;

	return 1;
}

static int fs_dirent_to_fileinfo(const struct dirent * d, char * folder, filefoundinfo * fileinfo)
{
	char * tmpstr;
#if !defined(MTP_USE_FILE_STAT_OPERATION) && !defined(CONFIG_MTP_USE_FILE_STAT_OPERATION)
	int len;
#endif

	if( !d || !fileinfo )
		return -1;

#if !defined(MTP_USE_FILE_STAT_OPERATION) && !defined(CONFIG_MTP_USE_FILE_STAT_OPERATION)
	if( d->d_info_flags & DFS_DIRENT_INFO_METADATA_VALID )
	{
		fileinfo->isdirectory = (d->d_type == DT_DIR) ? 1 : 0;
		fileinfo->size = d->fsize;
		fileinfo->date = d->mtime;
		fileinfo->fat_date = d->fdate;
		fileinfo->fat_time = d->ftime;

		len = d->d_namlen;
		if( len >= FS_HANDLE_MAX_FILENAME_SIZE )
			len = FS_HANDLE_MAX_FILENAME_SIZE;

		strncpy(fileinfo->filename, d->d_name, len);
		fileinfo->filename[len] = '\0';

		return 0;
	}
#endif

	tmpstr = malloc(strlen(folder) + strlen(d->d_name) + 4);
	if( !tmpstr )
	{
		errno = ENOMEM;
		return -1;
	}

	strcpy(tmpstr,folder);
	strcat(tmpstr,"/");
	strcat(tmpstr,d->d_name);

	if( !fs_entry_stat(tmpstr, fileinfo) )
	{
		free(tmpstr);
		return (errno == ENOENT) || (errno == -ENOENT) ? 1 : -1;
	}

	free(tmpstr);
	return 0;
}

static void fs_find_close(fs_dir_iterator * iterator)
{
	if( !iterator )
		return;

	if( iterator->dir )
		closedir(iterator->dir);
	if( iterator->owns_entries && iterator->entries )
		free(iterator->entries);
	memset(iterator, 0, sizeof(*iterator));
}

static int fs_find_first_file(fs_handles_db * db, char * folder, fs_dir_iterator * iterator, filefoundinfo* fileinfo, int * file_found)
{
	struct dirent *d;
	int ret;
	int buffer_size;

	if( !iterator || !file_found )
		return -1;

	*file_found = 0;
	memset(iterator, 0, sizeof(*iterator));

	iterator->dir = opendir(folder);
	if( !iterator->dir )
		return -1;

	if( db && db->dir_read_buffer && db->dir_read_buffer_size )
	{
		iterator->entries = db->dir_read_buffer;
		iterator->buffer_size = db->dir_read_buffer_size;
	}
	else
	{
		buffer_size = fs_dir_read_buffer_size(db);
		if( buffer_size )
		{
			iterator->entries = malloc(buffer_size);
			if( iterator->entries )
			{
				iterator->buffer_size = buffer_size;
				iterator->owns_entries = 1;
			}
		}
	}

	while( 1 )
	{
		ret = fs_dir_next(iterator, &d);
		/* An opened directory with no entries is a valid empty folder. */
		if( !ret )
			return 0;
		if( ret < 0 )
		{
			fs_find_close(iterator);
			return -1;
		}

		ret = fs_dirent_to_fileinfo(d, folder, fileinfo);
		if( !ret )
		{
			*file_found = 1;
			break;
		}
		if( ret < 0 )
		{
			fs_find_close(iterator);
			return -1;
		}
	}

	return 0;
}

static int fs_find_next_file(fs_dir_iterator * iterator, char *folder, filefoundinfo* fileinfo)
{
	struct dirent *d;
	int ret;

	if(!iterator)
		return 0;

	while( 1 )
	{
		ret = fs_dir_next(iterator, &d);
		if( ret <= 0 )
			return ret;

		ret = fs_dirent_to_fileinfo(d, folder, fileinfo);
		if( !ret )
			return 1;
		if( ret < 0 )
			return -1;
	}
}

static uint32_t fs_entry_hash(const char * name, uint32_t parent, uint32_t storage_id)
{
	uint32_t hash;
	const unsigned char * ptr;
	uint32_t value;
	int i;

	hash = 2166136261U;
	for(i = 0; i < 4; i++)
	{
		value = (parent >> (i * 8)) & 0xffU;
		hash = (hash ^ value) * 16777619U;
	}
	for(i = 0; i < 4; i++)
	{
		value = (storage_id >> (i * 8)) & 0xffU;
		hash = (hash ^ value) * 16777619U;
	}

	ptr = (const unsigned char *)name;
	while(*ptr)
	{
		hash = (hash ^ *ptr++) * 16777619U;
	}

	return hash;
}

static uint32_t fs_handle_hash(uint32_t handle)
{
	return handle * 2654435761U;
}

static void fs_cache_insert_entry(fs_handles_db * db, fs_entry * entry)
{
	uint32_t bucket;

	if( !db || !db->entry_cache || !db->entry_cache_bucket_count || !entry || !entry->name[0] || (entry->flags & ENTRY_IS_DELETED) )
		return;

	bucket = fs_entry_hash(entry->name, entry->parent, entry->storage_id) % db->entry_cache_bucket_count;
	entry->cache_next = db->entry_cache[bucket];
	db->entry_cache[bucket] = entry;
}

static void fs_handle_cache_insert_entry(fs_handles_db * db, fs_entry * entry)
{
	uint32_t bucket;

	if( !db || !db->handle_cache || !db->entry_cache_bucket_count || !entry || (entry->flags & ENTRY_IS_DELETED) )
		return;

	bucket = fs_handle_hash(entry->handle) % db->entry_cache_bucket_count;
	entry->handle_cache_next = db->handle_cache[bucket];
	db->handle_cache[bucket] = entry;
}

static void fs_cache_remove_entry(fs_handles_db * db, fs_entry * entry)
{
	fs_entry ** link;
	uint32_t bucket;

	if( !db || !entry )
		return;

	if( db->entry_cache && db->entry_cache_bucket_count && entry->name[0] )
	{
		bucket = fs_entry_hash(entry->name, entry->parent, entry->storage_id) % db->entry_cache_bucket_count;
		link = &db->entry_cache[bucket];
		while( *link )
		{
			if( *link == entry )
			{
				*link = entry->cache_next;
				break;
			}
			link = &(*link)->cache_next;
		}
	}
	entry->cache_next = NULL;

	if( db->handle_cache && db->entry_cache_bucket_count )
	{
		bucket = fs_handle_hash(entry->handle) % db->entry_cache_bucket_count;
		link = &db->handle_cache[bucket];
		while( *link )
		{
			if( *link == entry )
			{
				*link = entry->handle_cache_next;
				break;
			}
			link = &(*link)->handle_cache_next;
		}
	}
	entry->handle_cache_next = NULL;
}

void fs_rebuild_entry_cache(fs_handles_db * db)
{
	fs_entry * entry;

	if( !db )
		return;

	if( db->entry_cache && db->entry_cache_bucket_count )
		memset(db->entry_cache, 0, db->entry_cache_bucket_count * sizeof(*db->entry_cache));
	if( db->handle_cache && db->entry_cache_bucket_count )
		memset(db->handle_cache, 0, db->entry_cache_bucket_count * sizeof(*db->handle_cache));
	entry = db->entry_list;
	while(entry)
	{
		entry->cache_next = NULL;
		entry->handle_cache_next = NULL;
		fs_cache_insert_entry(db, entry);
		fs_handle_cache_insert_entry(db, entry);
		entry = entry->next;
	}
}

static fs_entry * fs_alloc_entry_storage(fs_handles_db * db)
{
	fs_entry * entry;

	if( !db )
		return NULL;

	if( db->free_entry_list )
	{
		entry = db->free_entry_list;
		db->free_entry_list = entry->next;
		memset(entry, 0, sizeof(*entry));
		entry->flags = ENTRY_FROM_DB_POOL;
		return entry;
	}

	if( db->entry_pool && (db->entry_pool_next < db->entry_pool_count) )
	{
		entry = &db->entry_pool[db->entry_pool_next++];
		memset(entry, 0, sizeof(*entry));
		entry->flags = ENTRY_FROM_DB_POOL;
		return entry;
	}

	entry = calloc(1, sizeof(*entry));
	if( entry && db->entry_pool && !db->entry_pool_exhausted )
	{
		PRINT_WARN("fs_handles_db : database cache buffer exhausted, using heap entries");
		db->entry_pool_exhausted = 1;
	}

	return entry;
}

static void fs_free_entry_storage(fs_entry * entry)
{
	if( entry && !(entry->flags & ENTRY_FROM_DB_POOL) )
		free(entry);
}

static void fs_release_entry_storage(fs_handles_db * db, fs_entry * entry)
{
	if( !entry )
		return;

	entry->cache_next = NULL;
	entry->handle_cache_next = NULL;

	if( entry->flags & ENTRY_FROM_DB_POOL )
	{
		entry->next = db->free_entry_list;
		db->free_entry_list = entry;
	}
	else
	{
		free(entry);
	}
}

fs_handles_db * init_fs_db(void * mtp_context)
{
	fs_handles_db * db;
	mtp_ctx * ctx;

	PRINT_DEBUG("init_fs_db called");

	db = (fs_handles_db *)malloc(sizeof(fs_handles_db));
	if( db )
	{
		memset(db,0,sizeof(fs_handles_db));
		db->next_handle = 0x00000001;
		db->mtp_ctx = mtp_context;

		ctx = (mtp_ctx *)mtp_context;
		if( ctx && ctx->fs_db_pool_size >= sizeof(*db->entry_pool) )
		{
			db->entry_pool = malloc(ctx->fs_db_pool_size);
			if( db->entry_pool )
			{
				db->entry_pool_count = ctx->fs_db_pool_size / sizeof(*db->entry_pool);
			}
			else
			{
				PRINT_WARN("init_fs_db : database cache buffer allocation failed, using heap entries");
			}
		}

		db->dir_read_buffer_size = fs_dir_read_buffer_size(db);
		if( db->dir_read_buffer_size )
		{
			db->dir_read_buffer = malloc(db->dir_read_buffer_size);
			if( !db->dir_read_buffer )
			{
				PRINT_WARN("init_fs_db : directory scan buffer allocation failed, using readdir fallback");
				db->dir_read_buffer_size = 0;
			}
		}

		if( ctx && ctx->fs_db_cache_buckets )
		{
			db->entry_cache_bucket_count = ctx->fs_db_cache_buckets;
			db->handle_cache = calloc(ctx->fs_db_cache_buckets, sizeof(*db->handle_cache));
			if( !db->handle_cache )
				PRINT_WARN("init_fs_db : handle cache allocation failed, using list lookup");

			db->entry_cache = calloc(ctx->fs_db_cache_buckets, sizeof(*db->entry_cache));
			if( !db->entry_cache )
				PRINT_WARN("init_fs_db : file-handle cache allocation failed, using list lookup");
		}
	}

	return db;
}

void deinit_fs_db(fs_handles_db * fsh)
{
	fs_entry * next_entry;

	PRINT_DEBUG("deinit_fs_db called");

	if( fsh )
	{
		while( fsh->entry_list )
		{
			next_entry = fsh->entry_list->next;

			if( fsh->entry_list->watch_descriptor != -1 )
			{
				// Disable the inotify watch point
				inotify_handler_rmwatch( fsh->mtp_ctx, fsh->entry_list->watch_descriptor );
				fsh->entry_list->watch_descriptor = -1;
			}

			fs_free_entry_storage(fsh->entry_list);

			fsh->entry_list = next_entry;
		}

		if( fsh->entry_cache )
			free(fsh->entry_cache);
		if( fsh->handle_cache )
			free(fsh->handle_cache);
		if( fsh->dir_read_buffer )
			free(fsh->dir_read_buffer);
		if( fsh->entry_pool )
			free(fsh->entry_pool);

		free(fsh);
	}

	return;
}

fs_entry * search_entry(fs_handles_db * db, filefoundinfo *fileinfo, uint32_t parent, uint32_t storage_id)
{
	fs_entry * entry_list;
	uint32_t bucket;

	if( !db )
		return NULL;

	if( db->entry_cache && db->entry_cache_bucket_count )
	{
		bucket = fs_entry_hash(fileinfo->filename, parent, storage_id) % db->entry_cache_bucket_count;
		entry_list = db->entry_cache[bucket];
		while( entry_list )
		{
			if( !( entry_list->flags & ENTRY_IS_DELETED ) && ( entry_list->parent == parent ) && ( entry_list->storage_id == storage_id ) && entry_list->name[0] )
			{
				if( !strcmp(entry_list->name,fileinfo->filename) )
					return entry_list;
			}

			entry_list = entry_list->cache_next;
		}

		return NULL;
	}

	entry_list = db->entry_list;

	while( entry_list )
	{
		if( !( entry_list->flags & ENTRY_IS_DELETED ) && ( entry_list->parent == parent ) && ( entry_list->storage_id == storage_id ) && entry_list->name[0] )
		{
			if( !strcmp(entry_list->name,fileinfo->filename) )
			{
				return entry_list;
			}
		}

		entry_list = entry_list->next;
	}

	return NULL;
}

fs_entry * alloc_entry(fs_handles_db * db, filefoundinfo *fileinfo, uint32_t parent, uint32_t storage_id)
{
	fs_entry * entry;

	entry = fs_alloc_entry_storage(db);
	if( entry )
	{
		entry->handle = db->next_handle;
		db->next_handle++;
		entry->parent = parent;
		entry->storage_id = storage_id;

		strncpy(entry->name, fileinfo->filename, FS_HANDLE_MAX_FILENAME_SIZE);
		entry->name[FS_HANDLE_MAX_FILENAME_SIZE] = '\0';

		entry->size = fileinfo->size;
		entry->date = fileinfo->date;
		entry->fat_date = fileinfo->fat_date;
		entry->fat_time = fileinfo->fat_time;

		entry->watch_descriptor = -1;

		if( fileinfo->isdirectory )
			entry->flags |= ENTRY_IS_DIR;

		entry->next = db->entry_list;

		db->entry_list = entry;
		fs_cache_insert_entry(db, entry);
		fs_handle_cache_insert_entry(db, entry);
	}

	return entry;
}

fs_entry * alloc_root_entry(fs_handles_db * db, uint32_t storage_id)
{
	fs_entry * entry;

	if( !db )
		return NULL;

	entry = fs_alloc_entry_storage(db);
	if( entry )
	{
		entry->handle = 0x00000000;
		entry->parent = 0x00000000;
		entry->storage_id = storage_id;

		strcpy(entry->name,"/");

		entry->size = 1;

		entry->watch_descriptor = -1;

		entry->flags |= ENTRY_IS_DIR;

		entry->next = db->entry_list;

		db->entry_list = entry;
		fs_cache_insert_entry(db, entry);
		fs_handle_cache_insert_entry(db, entry);
	}

	return entry;
}


fs_entry * add_entry(fs_handles_db * db, filefoundinfo *fileinfo, uint32_t parent, uint32_t storage_id)
{
	fs_entry * entry;

	entry = search_entry(db, fileinfo, parent, storage_id);
	if( entry )
	{
		entry->size = fileinfo->size;
		entry->date = fileinfo->date;
		entry->fat_date = fileinfo->fat_date;
		entry->fat_time = fileinfo->fat_time;
		if( fileinfo->isdirectory )
			entry->flags |= ENTRY_IS_DIR;
		else
			entry->flags &= ~ENTRY_IS_DIR;
	}
	else
	{
		entry = alloc_entry( db, fileinfo, parent, storage_id);
	}

	return entry;
}

static uint32_t fs_current_scan_cache_generation(fs_handles_db * db)
{
	mtp_ctx * ctx;

	ctx = db ? (mtp_ctx *)db->mtp_ctx : NULL;
	if( !ctx )
		return 1;

	return __atomic_load_n(&ctx->fs_db_change_generation, __ATOMIC_ACQUIRE);
}

void fs_invalidate_scan_cache_context(void * mtp_context)
{
	mtp_ctx * ctx;
	uint32_t generation;

	ctx = (mtp_ctx *)mtp_context;
	if( !ctx )
		return;

	do
	{
		generation = __atomic_add_fetch(&ctx->fs_db_change_generation, 1, __ATOMIC_RELEASE);
	}while( !generation );
}

void fs_invalidate_scan_cache(fs_handles_db * db)
{
	if( db )
		fs_invalidate_scan_cache_context(db->mtp_ctx);
}

int fs_scan_cache_valid(fs_handles_db * db, fs_entry * entry)
{
	if( !db || !entry || (entry->flags & ENTRY_IS_DELETED) || !(entry->flags & ENTRY_IS_DIR) )
		return 0;

	return entry->children_scan_generation &&
		(entry->children_scan_generation == fs_current_scan_cache_generation(db));
}

static void fs_mark_entry_deleted_recursive(fs_handles_db * db, fs_entry * entry)
{
	fs_entry * child;

	if( !db || !entry || (entry->flags & ENTRY_IS_DELETED) )
		return;

	entry->flags |= ENTRY_IS_DELETED;
	fs_cache_remove_entry(db, entry);
	entry->children_scan_generation = 0;
	entry->edit_session_id = 0;
	if( entry->watch_descriptor != -1 )
	{
		inotify_handler_rmwatch(db->mtp_ctx, entry->watch_descriptor);
		entry->watch_descriptor = -1;
	}

	for(child = db->entry_list; child; child = child->next)
	{
		if( (child != entry) && !(child->flags & ENTRY_IS_DELETED) &&
			(child->storage_id == entry->storage_id) &&
			(child->parent == entry->handle) && (child->handle != child->parent) )
		{
			fs_mark_entry_deleted_recursive(db, child);
		}
	}
}

void fs_mark_entry_deleted(fs_handles_db * db, fs_entry * entry)
{
	fs_mark_entry_deleted_recursive(db, entry);
}

void fs_prune_deleted_entries(fs_handles_db * db)
{
	fs_entry ** link;
	fs_entry * entry;
	int pruned;

	if( !db )
		return;

	pruned = 0;
	link = &db->entry_list;
	while( *link )
	{
		entry = *link;
		if( entry->flags & ENTRY_IS_DELETED )
		{
			*link = entry->next;
			fs_release_entry_storage(db, entry);
			pruned = 1;
		}
		else
		{
			link = &entry->next;
		}
	}

	if( pruned )
		fs_rebuild_entry_cache(db);
}

static uint32_t fs_next_scan_generation(fs_handles_db * db)
{
	db->scan_generation++;
	if( !db->scan_generation )
		db->scan_generation++;

	return db->scan_generation;
}

static int fs_entry_is_visible(fs_handles_db * db, fs_entry * entry)
{
	mtp_ctx * ctx;

	if( !entry || !entry->name[0] )
		return 0;

	ctx = db ? (mtp_ctx *)db->mtp_ctx : NULL;
	return !ctx || ctx->usb_cfg.show_hidden_files || (entry->name[0] != '.');
}

static void fs_remove_unseen_children(fs_handles_db * db, uint32_t parent, uint32_t storage_id, uint32_t scan_generation)
{
	fs_entry * entry;

	for(entry = db->entry_list; entry; entry = entry->next)
	{
		if( !(entry->flags & ENTRY_IS_DELETED) && (entry->parent == parent) &&
			(entry->storage_id == storage_id) && (entry->handle != entry->parent) &&
			(entry->last_seen_scan != scan_generation) && fs_entry_is_visible(db, entry) )
		{
			fs_mark_entry_deleted(db, entry);
		}
	}
}

int scan_and_add_folder(fs_handles_db * db, char * base, uint32_t parent, uint32_t storage_id)
{
	fs_entry * entry;
	fs_dir_iterator dir;
	mtp_ctx * ctx;
	uint32_t scan_generation;
	uint32_t cache_generation;
	int ret;
	int next_ret;
	int file_found;
	filefoundinfo fileinfo;

	if( !db || !base )
		return -1;

	PRINT_DEBUG("scan_and_add_folder : %s, Parent : 0x%.8X, Storage ID : 0x%.8X",base,parent,storage_id);

	ctx = (mtp_ctx *)db->mtp_ctx;
	cache_generation = fs_current_scan_cache_generation(db);
	scan_generation = fs_next_scan_generation(db);
	ret = 0;
	next_ret = 0;
	if( fs_find_first_file(db, base, &dir, &fileinfo, &file_found) )
		return -1;

	if( file_found )
	{
		do
		{
			if( strcmp(fileinfo.filename,"..") && strcmp(fileinfo.filename,".") && \
				(!ctx || ctx->usb_cfg.show_hidden_files || fileinfo.filename[0] != '.') )
			{
				entry = add_entry(db, &fileinfo, parent, storage_id);
				if( !entry )
				{
					ret = -1;
					break;
				}
				entry->last_seen_scan = scan_generation;
			}

			next_ret = fs_find_next_file(&dir, base, &fileinfo);
		}while(next_ret > 0);

		if( next_ret < 0 )
			ret = -1;

	}

	fs_find_close(&dir);
	if( ret )
		return ret;

	entry = get_entry_by_handle_and_storageid(db, parent, storage_id);
	if( entry && (entry->flags & ENTRY_IS_DIR) )
	{
		if( fs_current_scan_cache_generation(db) == cache_generation )
		{
			fs_remove_unseen_children(db, parent, storage_id, scan_generation);
			fs_prune_deleted_entries(db);
			entry->children_scan_generation = cache_generation;
		}
		else
			entry->children_scan_generation = 0;
	}

	return 0;
}

fs_entry * init_search_handle(fs_handles_db * db, uint32_t parent, uint32_t storage_id)
{
	db->search_entry = db->entry_list;
	db->handle_search = parent;
	db->storage_search = storage_id;

	return db->search_entry;
}

fs_entry * get_next_child_handle(fs_handles_db * db)
{
	fs_entry * entry_list;

	entry_list = db->search_entry;

	while( entry_list )
	{
		if( !( entry_list->flags & ENTRY_IS_DELETED ) && ( entry_list->parent == db->handle_search ) && ( entry_list->storage_id == db->storage_search ) && ( entry_list->handle != entry_list->parent ) )
		{
			db->search_entry = entry_list->next;

			return entry_list;
		}

		entry_list = entry_list->next;
	}

	db->search_entry = 0x00000000;

	return NULL;
}

fs_entry * get_entry_by_handle(fs_handles_db * db, uint32_t handle)
{
	fs_entry * entry_list;
	uint32_t bucket;

	if( !db )
		return NULL;

	if( db->handle_cache && db->entry_cache_bucket_count )
	{
		bucket = fs_handle_hash(handle) % db->entry_cache_bucket_count;
		entry_list = db->handle_cache[bucket];
		while( entry_list )
		{
			if( !(entry_list->flags & ENTRY_IS_DELETED) && (entry_list->handle == handle) &&
				mtp_get_storage_root(db->mtp_ctx, entry_list->storage_id) )
			{
				return entry_list;
			}
			entry_list = entry_list->handle_cache_next;
		}

		return NULL;
	}

	entry_list = db->entry_list;

	while( entry_list )
	{
		if( !( entry_list->flags & ENTRY_IS_DELETED ) && ( entry_list->handle == handle ) )
		{
			if( mtp_get_storage_root(db->mtp_ctx, entry_list->storage_id) )
			{
				return entry_list;
			}
		}

		entry_list = entry_list->next;
	}

	return 0;
}

fs_entry * get_entry_by_handle_and_storageid(fs_handles_db * db, uint32_t handle, uint32_t storage_id)
{
	fs_entry * entry_list;
	uint32_t bucket;

	if( !db )
		return NULL;

	if( db->handle_cache && db->entry_cache_bucket_count )
	{
		bucket = fs_handle_hash(handle) % db->entry_cache_bucket_count;
		entry_list = db->handle_cache[bucket];
		while( entry_list )
		{
			if( !(entry_list->flags & ENTRY_IS_DELETED) && (entry_list->handle == handle) &&
				(entry_list->storage_id == storage_id) )
			{
				return entry_list;
			}
			entry_list = entry_list->handle_cache_next;
		}

		return NULL;
	}

	entry_list = db->entry_list;

	while( entry_list )
	{
		if( !( entry_list->flags & ENTRY_IS_DELETED ) && ( entry_list->handle == handle ) && ( entry_list->storage_id == storage_id ) )
		{
			return entry_list;
		}
		entry_list = entry_list->next;
	}

	return NULL;
}

char * build_full_path(fs_handles_db * db,char * root_path,fs_entry * entry)
{
	int totallen,namelen;
	fs_entry * curentry;
	char * full_path;
	int full_path_offset;

	full_path = NULL;

	if( !entry )
		return full_path;

	curentry = entry;
	totallen = 0;
	do
	{
		if(curentry->name[0])
			totallen += strlen(curentry->name);

		totallen++; // '/'

		if( curentry->parent && ( curentry->parent != 0xFFFFFFFF ) )
		{
			curentry = get_entry_by_handle(db, curentry->parent);
		}
		else
		{
			curentry = NULL;
		}

	}while( curentry );

	if(root_path)
	{
		totallen += strlen(root_path);
	}

	full_path = malloc(totallen+1);
	if( full_path )
	{
		memset(full_path,0,totallen+1);
		full_path_offset = totallen;
		curentry = entry;

		do
		{
			if(curentry->name[0])
				namelen = strlen(curentry->name);
			else
				namelen = 0;

			full_path_offset -= namelen;

			if( namelen )
				memcpy(&full_path[full_path_offset],curentry->name,namelen);

			full_path_offset--;

			full_path[full_path_offset] = '/';

			if(curentry->parent && curentry->parent!=0xFFFFFFFF)
			{
				curentry = get_entry_by_handle(db, curentry->parent);
			}
			else
			{
				curentry = NULL;
			}
		}while(curentry);

		if(root_path)
		{
			memcpy(&full_path[0],root_path,strlen(root_path));
		}

		if(entry->name[0])
			PRINT_DEBUG("build_full_path : %s -> %s",entry->name, full_path);
	}

	return full_path;
}

int entry_open(fs_handles_db * db, fs_entry * entry)
{
	int file;
	char * full_path;

	file = -1;

	full_path = build_full_path(db,mtp_get_storage_root(db->mtp_ctx, entry->storage_id), entry);
	if( full_path )
	{
		file = -1;

		if(!set_storage_giduid(db->mtp_ctx, entry->storage_id))
		{
			file = open(full_path,O_RDONLY | O_LARGEFILE);
		}

		restore_giduid(db->mtp_ctx);

		if( file == -1 )
			PRINT_DEBUG("entry_open : Can't open %s !",full_path);

		free(full_path);
	}

	return file;
}

int entry_read(fs_handles_db * db, int file, unsigned char * buffer_out, mtp_offset offset, mtp_size size)
{
	int totalread;

	if( file != -1 )
	{
		lseek(file, offset, SEEK_SET);

		totalread = read( file, buffer_out, size );

		return totalread;
	}

	return 0;
}

void entry_close(int file)
{
	if( file != -1 )
		close(file);
}

fs_entry * get_entry_by_wd( fs_handles_db * db, int watch_descriptor, fs_entry * entry_list )
{
	if(!entry_list && db)
		entry_list = db->entry_list;

	while( entry_list )
	{
		if( !( entry_list->flags & ENTRY_IS_DELETED ) && ( entry_list->watch_descriptor == watch_descriptor ) )
		{
			return entry_list;
		}

		entry_list = entry_list->next;
	}

	return NULL;
}

fs_entry * get_entry_by_storageid( fs_handles_db * db, uint32_t storage_id, fs_entry * entry_list )
{
	if(!entry_list && db)
		entry_list = db->entry_list;

	while( entry_list )
	{
		if( !( entry_list->flags & ENTRY_IS_DELETED ) && ( entry_list->storage_id == storage_id ) )
		{
			return entry_list;
		}

		entry_list = entry_list->next;
	}

	return NULL;
}
