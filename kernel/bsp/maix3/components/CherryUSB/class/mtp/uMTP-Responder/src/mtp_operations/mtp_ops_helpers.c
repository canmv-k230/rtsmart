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
 * @file   mtp_ops_helpers.c
 * @brief  mtp operations helpers
 * @author Jean-Fran�ois DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#include "buildconf.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "mtp.h"
#include "mtp_helpers.h"
#include "mtp_constant.h"
#include "mtp_operations.h"
#include "usb_gadget_fct.h"
#include "inotify.h"

#include "logs_out.h"

uint32_t mtp_sync_folder(mtp_ctx * ctx, uint32_t storageid, uint32_t parent_handle)
{
	fs_entry * entry;
	char * storage_root;
	char * full_path;
	char * allocated_path;
	int ret;

	if( !ctx || !ctx->fs_db )
		return MTP_RESPONSE_SESSION_NOT_OPEN;

	storage_root = mtp_get_storage_root(ctx, storageid);
	if( !storage_root )
		return MTP_RESPONSE_INVALID_STORAGE_ID;

	entry = get_entry_by_handle_and_storageid(ctx->fs_db, parent_handle, storageid);
	if( !entry )
		return parent_handle ? MTP_RESPONSE_INVALID_OBJECT_HANDLE : MTP_RESPONSE_GENERAL_ERROR;
	if( !(entry->flags & ENTRY_IS_DIR) )
		return MTP_RESPONSE_INVALID_PARENT_OBJECT;

	allocated_path = NULL;
	if( parent_handle )
	{
		allocated_path = build_full_path(ctx->fs_db, storage_root, entry);
		full_path = allocated_path;
	}
	else
	{
		full_path = storage_root;
	}

	if( !full_path )
		return MTP_RESPONSE_GENERAL_ERROR;

	ret = 0;
	if( !ctx->fs_db_scan_cache || !fs_scan_cache_valid(ctx->fs_db, entry) )
	{
		ret = -1;
		if( !set_storage_giduid(ctx, storageid) )
			ret = scan_and_add_folder(ctx->fs_db, full_path, parent_handle, storageid);
		restore_giduid(ctx);
	}

	if( ret < 0 )
	{
		PRINT_WARN("MTP directory scan failed: %s", full_path);
		if( allocated_path )
			free(allocated_path);
		return MTP_RESPONSE_ACCESS_DENIED;
	}

	entry->watch_descriptor = inotify_handler_addwatch(ctx, full_path);
	if( allocated_path )
		free(allocated_path);

	return MTP_RESPONSE_OK;
}

mtp_size send_file_data( mtp_ctx * ctx, fs_entry * entry,mtp_offset offset, mtp_size maxsize )
{
	mtp_size actualsize;
	mtp_size j;
	int ofs;
	mtp_size blocksize;
	int file;
	int bytes_read;
	mtp_offset buf_index;
	int io_buffer_index;
	int first_part_size;
	unsigned char * usb_buffer_ptr;
	int transfer_failed;
	int transfer_cancelled;
	int write_size;
	int transfer_limit;
	unsigned char * read_buffer_ptr;

	if( !ctx || !entry || !ctx->wrbuffer || (ctx->usb_wr_buffer_max_size <= (int)sizeof(MTP_PACKET_HEADER)) || (ctx->read_file_buffer_size <= 0) || (ctx->read_file_buffer_size & (ctx->read_file_buffer_size - 1)) || (offset < 0) || (maxsize < 0) || (entry->size < 0) )
		return -1;

	transfer_limit = mtp_get_usb_bulk_transfer_limit(ctx);
	if( transfer_limit > ctx->usb_wr_buffer_max_size )
		transfer_limit = ctx->usb_wr_buffer_max_size;
	if( transfer_limit <= (int)sizeof(MTP_PACKET_HEADER) )
		return -1;

	if( !ctx->read_file_buffer )
	{
		ctx->read_file_buffer = malloc( ctx->read_file_buffer_size );
		if(!ctx->read_file_buffer)
			return -1;

		memset(ctx->read_file_buffer, 0, ctx->read_file_buffer_size);
	}

	usb_buffer_ptr = NULL;

	buf_index = -1;

	if( offset >= entry->size )
	{
		actualsize = 0;
	}
	else
	{
		if( maxsize > (entry->size - offset) )
			actualsize = entry->size - offset;
		else
			actualsize = maxsize;
	}
	if( actualsize > (mtp_size)(UINT32_MAX - sizeof(MTP_PACKET_HEADER)) )
		return -1;

	if( poke32(ctx->wrbuffer, 0, ctx->usb_wr_buffer_max_size, sizeof(MTP_PACKET_HEADER) + actualsize) < 0 )
		return -1;

	ofs = sizeof(MTP_PACKET_HEADER);

	PRINT_DEBUG("send_file_data : Offset 0x%"SIZEHEX" - Maxsize 0x%"SIZEHEX" - Size 0x%"SIZEHEX" - ActualSize 0x%"SIZEHEX, offset,maxsize,entry->size,actualsize);

	file = entry_open(ctx->fs_db, entry);
	if( file != -1 )
	{
		ctx->transferring_file_data = 1;
		transfer_failed = 0;
		transfer_cancelled = 0;

		j = 0;
		while( (j < actualsize) && !ctx->cancel_req )
		{
			if((j + ((mtp_size)transfer_limit - ofs)) < actualsize)
				blocksize = ((mtp_size)transfer_limit - ofs);
			else
				blocksize = actualsize - j;
			if( blocksize > ctx->read_file_buffer_size )
				blocksize = ctx->read_file_buffer_size;

			// Is the target page loaded ?
			if( buf_index != ((offset + j) & ~((mtp_offset)(ctx->read_file_buffer_size-1))) )
			{
				bytes_read = entry_read(ctx->fs_db, file, ctx->read_file_buffer, ((offset + j) & ~((mtp_offset)(ctx->read_file_buffer_size-1))) , (mtp_size)ctx->read_file_buffer_size);
				if( bytes_read < 0 )
				{
					transfer_failed = 1;
					break;
				}

				buf_index = ((offset + j) & ~((mtp_offset)(ctx->read_file_buffer_size-1)));
			}

			io_buffer_index = (offset + j) & (mtp_offset)(ctx->read_file_buffer_size-1);

			// Is a new page needed ?
			if( io_buffer_index + blocksize <= ctx->read_file_buffer_size )
			{
				// No, just use the io buffer
				if( (io_buffer_index + blocksize) > bytes_read )
				{
					transfer_failed = 1;
					break;
				}

				read_buffer_ptr = (unsigned char *)&ctx->read_file_buffer[io_buffer_index];
				if( !ofs && !((uintptr_t)read_buffer_ptr & 0x3U) )
				{
					usb_buffer_ptr = read_buffer_ptr;
				}
				else
				{
					memcpy(&ctx->wrbuffer[ofs], read_buffer_ptr, blocksize );
					usb_buffer_ptr = (unsigned char *)&ctx->wrbuffer[0];
				}
			}
			else
			{
				// Yes, new page needed. Get the first part in the io buffer and the load a new page to get the remaining data.
				first_part_size = blocksize - ( ( io_buffer_index + blocksize ) - (mtp_size)ctx->read_file_buffer_size);
				if( (io_buffer_index + first_part_size) > bytes_read )
				{
					transfer_failed = 1;
					break;
				}

				memcpy(&ctx->wrbuffer[ofs], &ctx->read_file_buffer[io_buffer_index], first_part_size  );

				buf_index += (mtp_offset)ctx->read_file_buffer_size;
				bytes_read = entry_read(ctx->fs_db, file, ctx->read_file_buffer, buf_index , ctx->read_file_buffer_size);
				if( bytes_read < 0 )
				{
					transfer_failed = 1;
					break;
				}
				if( bytes_read < (blocksize - first_part_size) )
				{
					transfer_failed = 1;
					break;
				}

				memcpy(&ctx->wrbuffer[ofs + first_part_size], &ctx->read_file_buffer[0], blocksize - first_part_size );

				usb_buffer_ptr = (unsigned char *)&ctx->wrbuffer[0];
			}

			j   += blocksize;
			ofs += blocksize;


				write_size = write_usb(ctx->usb_ctx, EP_DESCRIPTOR_IN, usb_buffer_ptr, ofs);
				if( write_size == -2 )
				{
					transfer_cancelled = 1;
					break;
				}
				if( write_size != ofs )
				{
					transfer_failed = 1;
					break;
				}

			ofs = 0;

		}

			if( !transfer_failed && !ctx->cancel_req && !actualsize )
			{
				write_size = write_usb(ctx->usb_ctx, EP_DESCRIPTOR_IN, ctx->wrbuffer, ofs);
				if( write_size == -2 )
					transfer_cancelled = 1;
				else if( write_size != ofs )
					transfer_failed = 1;
			}

		ctx->transferring_file_data = 0;

		entry_close( file );

			if( transfer_cancelled || ctx->cancel_req )
			{
				PRINT_DEBUG("send_file_data : Cancelled ! Aborded...");
				actualsize = -2;
			}
			else if( transfer_failed )
			{
				actualsize = -1;
			}
			else
			{
				PRINT_DEBUG("send_file_data : Full transfert done !");

				write_size = check_and_send_USB_ZLP(ctx , sizeof(MTP_PACKET_HEADER) + actualsize );
				if( write_size == -2 )
					actualsize = -2;
				else if( write_size < 0 )
					actualsize = -1;
		}

	}
	else
		actualsize = -1;

	return actualsize;
}

int delete_tree(mtp_ctx * ctx,uint32_t handle)
{
	int ret;
	fs_entry * entry;
	char * path;
	uint32_t storage_id;
	ret = -1;
	storage_id = 0;

	entry = get_entry_by_handle(ctx->fs_db, handle);
	if(entry)
	{
		storage_id = entry->storage_id;
		path = build_full_path(ctx->fs_db, mtp_get_storage_root(ctx, entry->storage_id), entry);

		if (path)
		{
			if(entry->flags & ENTRY_IS_DIR)
				{
					pthread_mutex_unlock(&ctx->inotify_mutex);
					ret = fs_remove_tree( path );
					pthread_mutex_lock(&ctx->inotify_mutex);
					entry = get_entry_by_handle_and_storageid(ctx->fs_db, handle, storage_id);

					if(!ret)
					{
						if( entry )
						{
							fs_mark_entry_deleted(ctx->fs_db, entry);
							fs_prune_deleted_entries(ctx->fs_db);
						}
						fs_invalidate_scan_cache(ctx->fs_db);
					}
					else
					{
						fs_invalidate_scan_cache(ctx->fs_db);
						if( entry )
						{
							scan_and_add_folder(ctx->fs_db, path, handle, entry->storage_id); // partially deleted ? update/sync the db.
						}
					}
				}
				else
				{
					ret = unlink(path);
					if(!ret)
					{
						fs_mark_entry_deleted(ctx->fs_db, entry);
						fs_prune_deleted_entries(ctx->fs_db);
						fs_invalidate_scan_cache(ctx->fs_db);
					}
			}

			free(path);
		}
	}

	return ret;
}

int umount_store(mtp_ctx * ctx, int store_index, int update_flag)
{
	fs_entry * entry;

	if(store_index >= 0 && store_index < MAX_STORAGE_NB )
	{
		if( ctx->storages[store_index].root_path )
		{
			for(entry = ctx->fs_db->entry_list; entry; entry = entry->next)
			{
				if( !(entry->flags & ENTRY_IS_DELETED) &&
					(entry->storage_id == ctx->storages[store_index].storage_id) )
				{
					fs_mark_entry_deleted(ctx->fs_db, entry);
				}
			}
			fs_prune_deleted_entries(ctx->fs_db);
			fs_invalidate_scan_cache(ctx->fs_db);

			if( update_flag )
				ctx->storages[store_index].flags |= UMTP_STORAGE_NOTMOUNTED;
		}
	}

	return 0;
}

int mount_store(mtp_ctx * ctx, int store_index, int update_flag)
{
	if(store_index >= 0 && store_index < MAX_STORAGE_NB )
	{
		if( ctx->storages[store_index].root_path )
		{
			if( ctx->storages[store_index].flags & UMTP_STORAGE_NOTMOUNTED )
			{
				alloc_root_entry(ctx->fs_db, ctx->storages[store_index].storage_id);
				fs_invalidate_scan_cache(ctx->fs_db);
			}

			if( update_flag )
				ctx->storages[store_index].flags &= ~UMTP_STORAGE_NOTMOUNTED;
		}
	}

	return 0;
}
