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
 * @file   inotify.c
 * @brief  inotify file system events handler.
 * @author Jean-François DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#include "buildconf.h"

#include <inttypes.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "mtp.h"
#include "mtp_helpers.h"
#include "mtp_constant.h"
#include "mtp_datasets.h"

#include "usbd_core.h"
#include "rtthread.h"

#include "usb_gadget_fct.h"
#include "fs_handles_db.h"
#include "inotify.h"
#include "logs_out.h"

typedef struct {
	int type;
	uint32_t session_generation;
	char *path;
} file_chg_msg_t;

#define NTY_FILE_STOP 0

static rt_mq_t mtp_inty_mq;
static rt_thread_t mtp_inty_tid;
static rt_sem_t mtp_inty_exit_sem;
static mtp_ctx * mtp_inotify_ctx;
static struct rt_mutex mtp_inty_lock;
static int mtp_inty_lock_ready;

static int inotify_find_storage(mtp_ctx * ctx, const char * path, uint32_t * storage_id, const char ** relative_path)
{
	int i;
	size_t root_len;
	size_t path_len;
	const char * root;

	if( !ctx || !path )
		return -1;

	path_len = strlen(path);
	for(i = 0; i < MAX_STORAGE_NB; i++)
	{
		root = ctx->storages[i].root_path;
		if( !root || !root[0] )
			continue;

		root_len = strlen(root);
		while( (root_len > 1) && (root[root_len - 1] == '/') )
			root_len--;

		if( (path_len < root_len) || strncmp(path, root, root_len) ||
			((path_len > root_len) && (root_len != 1) && (path[root_len] != '/')) )
		{
			continue;
		}

		if( storage_id )
			*storage_id = ctx->storages[i].storage_id;
		if( relative_path )
		{
			*relative_path = path + root_len;
			while( **relative_path == '/' )
				(*relative_path)++;
		}
		return 0;
	}

	return -1;
}

static fs_entry * inotify_find_parent(fs_handles_db * db, uint32_t storage_id, char * relative_path, char ** leaf_name)
{
	fs_entry * parent;
	fs_entry * entry;
	filefoundinfo fileinfo;
	uint32_t parent_handle;
	char * segment;
	char * next_segment;
	char * saveptr;

	if( !db || !relative_path || !leaf_name || !relative_path[0] )
		return NULL;

	parent = get_entry_by_handle_and_storageid(db, 0, storage_id);
	if( !parent || !(parent->flags & ENTRY_IS_DIR) )
		return NULL;

	parent_handle = 0;
	saveptr = NULL;
	segment = strtok_r(relative_path, "/", &saveptr);
	while( segment )
	{
		next_segment = strtok_r(NULL, "/", &saveptr);
		if( !next_segment )
		{
			*leaf_name = segment;
			return parent;
		}

		memset(&fileinfo, 0, sizeof(fileinfo));
		strncpy(fileinfo.filename, segment, FS_HANDLE_MAX_FILENAME_SIZE);
		fileinfo.filename[FS_HANDLE_MAX_FILENAME_SIZE] = '\0';
		entry = search_entry(db, &fileinfo, parent_handle, storage_id);
		if( !entry || !(entry->flags & ENTRY_IS_DIR) )
			return NULL;

		parent = entry;
		parent_handle = entry->handle;
		segment = next_segment;
	}

	return NULL;
}

static void inotify_process_message(mtp_ctx * ctx, file_chg_msg_t * msg)
{
	fs_handles_db * db;
	fs_entry * parent;
	fs_entry * entry;
	filefoundinfo fileinfo;
	filefoundinfo lookup;
	uint32_t storage_id;
	uint32_t handle;
	uint16_t event;
	const char * relative_path;
	char * leaf_name;

	if( !ctx || !msg || !msg->path ||
		(msg->session_generation != mtp_fs_db_session_get(ctx)) )
		return;

	if( (msg->type == NTY_FILE_ADD) || (msg->type == NTY_FILE_CHG) )
	{
		if( !fs_entry_stat(msg->path, &fileinfo) )
			return;
	}
	else
	{
		memset(&fileinfo, 0, sizeof(fileinfo));
	}

	if( pthread_mutex_lock(&ctx->inotify_mutex) )
		return;

	event = 0;
	handle = 0;
	if( msg->session_generation != mtp_fs_db_session_get(ctx) )
		goto out;
	db = ctx->fs_db;
	if( !db || inotify_find_storage(ctx, msg->path, &storage_id, &relative_path) )
		goto out;

	parent = inotify_find_parent(db, storage_id, (char *)relative_path, &leaf_name);
	if( !parent || (parent->watch_descriptor == -1) )
		goto out;

	memset(&lookup, 0, sizeof(lookup));
	strncpy(lookup.filename, leaf_name, FS_HANDLE_MAX_FILENAME_SIZE);
	lookup.filename[FS_HANDLE_MAX_FILENAME_SIZE] = '\0';
	entry = search_entry(db, &lookup, parent->handle, storage_id);

	switch(msg->type)
	{
		case NTY_FILE_ADD:
			if( !entry )
			{
				entry = add_entry(db, &fileinfo, parent->handle, storage_id);
				if( entry )
				{
					handle = entry->handle;
					event = MTP_EVENT_OBJECT_ADDED;
				}
			}
		break;

		case NTY_FILE_CHG:
			if( entry )
			{
				entry->size = fileinfo.size;
				entry->date = fileinfo.date;
				entry->fat_date = fileinfo.fat_date;
				entry->fat_time = fileinfo.fat_time;
				if( fileinfo.isdirectory )
					entry->flags |= ENTRY_IS_DIR;
				else
					entry->flags &= ~ENTRY_IS_DIR;
				handle = entry->handle;
				event = MTP_EVENT_OBJECT_INFO_CHANGED;
			}
			else
			{
				entry = add_entry(db, &fileinfo, parent->handle, storage_id);
				if( entry )
				{
					handle = entry->handle;
					event = MTP_EVENT_OBJECT_ADDED;
				}
			}
		break;

		case NTY_FILE_RM:
			if( entry )
			{
				handle = entry->handle;
				fs_mark_entry_deleted(db, entry);
				fs_prune_deleted_entries(db);
				event = MTP_EVENT_OBJECT_REMOVED;
			}
		break;

		default:
		break;
	}
out:
	pthread_mutex_unlock(&ctx->inotify_mutex);
	if( event )
		mtp_push_event(ctx, event, 1, &handle);
}

void inotify_thread(void* arg)
{
	mtp_ctx * ctx;
	file_chg_msg_t msg;

	ctx = arg;
	while( 1 )
	{
		if( RT_EOK != rt_mq_recv(mtp_inty_mq, &msg, sizeof(msg), RT_WAITING_FOREVER) )
			continue;

		if( msg.type == NTY_FILE_STOP )
		{
			if( mtp_inty_exit_sem )
				rt_sem_release(mtp_inty_exit_sem);
			return;
		}

		inotify_process_message(ctx, &msg);
		if( msg.path )
			rt_free(msg.path);
	}
}

int inotify_handler_init( mtp_ctx * ctx )
{
	rt_mq_t mq;
	rt_sem_t exit_sem;
	rt_thread_t tid;
	rt_err_t ret;

	if( !ctx )
		return -RT_EINVAL;
	if( !mtp_inty_lock_ready )
	{
		if( rt_mutex_init(&mtp_inty_lock, "mtp_inty", RT_IPC_FLAG_FIFO) != RT_EOK )
			return -RT_ERROR;
		mtp_inty_lock_ready = 1;
	}

	if( rt_mutex_take(&mtp_inty_lock, RT_WAITING_FOREVER) != RT_EOK )
		return -RT_ERROR;
	if( mtp_inotify_ctx || mtp_inty_mq )
	{
		rt_mutex_release(&mtp_inty_lock);
		return -RT_EBUSY;
	}

	mtp_inty_mq = rt_mq_create("mtp_inty", sizeof(file_chg_msg_t), 64, RT_IPC_FLAG_FIFO);
	if( !mtp_inty_mq )
	{
		rt_mutex_release(&mtp_inty_lock);
		return -RT_ENOMEM;
	}

	exit_sem = rt_sem_create("mtp_exit", 0, RT_IPC_FLAG_FIFO);
	if( !exit_sem )
	{
		rt_mq_delete(mtp_inty_mq);
		mtp_inty_mq = NULL;
		rt_mutex_release(&mtp_inty_lock);
		return -RT_ENOMEM;
	}

	tid = rt_thread_create("mtp_inty", inotify_thread, ctx, CONFIG_USBDEV_MTP_STACKSIZE, CONFIG_USBDEV_MTP_PRIO, 10);
	if( !tid )
	{
		rt_sem_delete(exit_sem);
		rt_mq_delete(mtp_inty_mq);
		mtp_inty_mq = NULL;
		rt_mutex_release(&mtp_inty_lock);
		return -RT_ENOMEM;
	}

	mtp_inty_tid = tid;
	mtp_inty_exit_sem = exit_sem;
	mtp_inotify_ctx = ctx;
	rt_mutex_release(&mtp_inty_lock);

	ret = rt_thread_startup(tid);
	if( ret != RT_EOK )
	{
		rt_mutex_take(&mtp_inty_lock, RT_WAITING_FOREVER);
		mq = mtp_inty_mq;
		exit_sem = mtp_inty_exit_sem;
		mtp_inotify_ctx = NULL;
		mtp_inty_tid = NULL;
		mtp_inty_exit_sem = NULL;
		mtp_inty_mq = NULL;
		rt_mutex_release(&mtp_inty_lock);
		rt_thread_delete(tid);
		rt_sem_delete(exit_sem);
		rt_mq_delete(mq);
		return -RT_ERROR;
	}

	return 0;
}

int inotify_handler_deinit( mtp_ctx * ctx )
{
	rt_mq_t mq;
	rt_sem_t exit_sem;
	rt_thread_t tid;
	file_chg_msg_t msg;
	rt_err_t ret;

	if( !mtp_inty_lock_ready )
		return 0;
	if( rt_mutex_take(&mtp_inty_lock, RT_WAITING_FOREVER) != RT_EOK )
		return -RT_ERROR;
	if( mtp_inotify_ctx != ctx )
	{
		rt_mutex_release(&mtp_inty_lock);
		return 0;
	}

	mtp_inotify_ctx = NULL;
	tid = mtp_inty_tid;
	mq = mtp_inty_mq;
	exit_sem = mtp_inty_exit_sem;
	mtp_inty_tid = NULL;

	if( !tid || !mq || !exit_sem )
	{
		mtp_inty_mq = NULL;
		mtp_inty_exit_sem = NULL;
		rt_mutex_release(&mtp_inty_lock);
		if( exit_sem )
			rt_sem_delete(exit_sem);
		if( mq )
			rt_mq_delete(mq);
		return 0;
	}

	while( rt_mq_recv(mq, &msg, sizeof(msg), RT_WAITING_NO) == RT_EOK )
	{
		if( msg.path )
			rt_free(msg.path);
	}

	msg.type = NTY_FILE_STOP;
	msg.session_generation = 0;
	msg.path = NULL;
	ret = rt_mq_urgent(mq, &msg, sizeof(msg));
	rt_mutex_release(&mtp_inty_lock);

	if( ret != RT_EOK )
		return ret;

	ret = rt_sem_take(exit_sem, RT_WAITING_FOREVER);
	if( ret != RT_EOK )
		return ret;

	rt_mutex_take(&mtp_inty_lock, RT_WAITING_FOREVER);
	if( mtp_inty_mq == mq )
		mtp_inty_mq = NULL;
	if( mtp_inty_exit_sem == exit_sem )
		mtp_inty_exit_sem = NULL;
	rt_mutex_release(&mtp_inty_lock);

	if( exit_sem )
		rt_sem_delete(exit_sem);
	if( mq )
	{
		rt_mq_delete(mq);
	}

	return 0;
}

int inotify_handler_addwatch( mtp_ctx * ctx, char * path )
{
	return 1;
}

int inotify_handler_rmwatch( mtp_ctx * ctx, int wd )
{
	return -1;
}

int inotify_handler_filechange(int type, const char *path)
{
	rt_err_t ret = RT_EOK;
	char *dup_path;
	file_chg_msg_t msg;
	mtp_ctx * ctx;
	uint32_t session_generation;

	if( !path || ((type != NTY_FILE_ADD) && (type != NTY_FILE_CHG) && (type != NTY_FILE_RM)) )
		return -RT_EINVAL;
	if( !mtp_inty_lock_ready )
		return 0;
	if( rt_mutex_take(&mtp_inty_lock, RT_WAITING_FOREVER) != RT_EOK )
		return -RT_ERROR;

	ctx = mtp_inotify_ctx;
	if( !ctx )
		goto out;

	/* Ignore mutations outside the configured MTP storage roots. */
	if( inotify_find_storage(ctx, path, NULL, NULL) )
		goto out;

	/* This can run while an MTP operation owns the database mutex. */
	fs_invalidate_scan_cache_context(ctx);

	session_generation = mtp_fs_db_session_get(ctx);
	if( !session_generation || !mtp_inty_mq )
		goto out;

	dup_path = rt_strdup(path);
	if (!dup_path)
	{
		ret = -RT_ENOMEM;
		goto out;
	}

	msg.type = type;
	msg.session_generation = session_generation;
	msg.path = dup_path;

	ret = rt_mq_send(mtp_inty_mq, &msg, sizeof(file_chg_msg_t));
	if (ret != RT_EOK)
	{
		rt_free(dup_path);
	}

	out:
	rt_mutex_release(&mtp_inty_lock);
	return ret;
}
