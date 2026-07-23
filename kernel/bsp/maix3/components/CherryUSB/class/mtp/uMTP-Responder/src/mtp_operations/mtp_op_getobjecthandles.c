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
 * @file   mtp_op_getobjecthandles.c
 * @brief  get object handles operation
 * @author Jean-Fran�ois DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#include "buildconf.h"

#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>

#include "mtp.h"
#include "mtp_helpers.h"
#include "mtp_constant.h"
#include "mtp_operations.h"
#include "mtp_ops_helpers.h"
#include "usb_gadget_fct.h"

#include "logs_out.h"

/* PTP uses this value when the caller wants handles from every storage. */
#define MTP_ALL_STORAGE_ID 0xFFFFFFFFU
#define MTP_ROOT_HANDLE    MTP_ALL_STORAGE_ID

static int mtp_handle_matches(fs_entry * entry, uint32_t storageid, uint32_t parent_handle, uint32_t object_format)
{
	uint16_t entry_format;

	if( (entry->flags & ENTRY_IS_DELETED) || (entry->handle == entry->parent) )
		return 0;

	if( (storageid != MTP_ALL_STORAGE_ID) && (entry->storage_id != storageid) )
		return 0;

	if( entry->parent != parent_handle )
		return 0;

	entry_format = (entry->flags & ENTRY_IS_DIR) ? MTP_FORMAT_ASSOCIATION : MTP_FORMAT_UNDEFINED;
	if( object_format && (object_format != entry_format) )
		return 0;

	return 1;
}


static uint32_t mtp_sync_requested_handles(mtp_ctx * ctx, uint32_t storageid, uint32_t parent_handle)
{
	fs_entry * entry;
	uint32_t response_code;
	int i;
	int storage_found;

	if( storageid != MTP_ALL_STORAGE_ID )
	{
		if( !mtp_get_storage_root(ctx, storageid) )
			return MTP_RESPONSE_INVALID_STORAGE_ID;

		return mtp_sync_folder(ctx, storageid, parent_handle);
	}

	if( parent_handle )
	{
		entry = get_entry_by_handle(ctx->fs_db, parent_handle);
		if( !entry )
			return MTP_RESPONSE_INVALID_OBJECT_HANDLE;

		return mtp_sync_folder(ctx, entry->storage_id, parent_handle);
	}

	storage_found = 0;
	for(i = 0; i < MAX_STORAGE_NB; i++)
	{
		if( !ctx->storages[i].root_path )
			continue;

		storage_found = 1;
		response_code = mtp_sync_folder(ctx, ctx->storages[i].storage_id, parent_handle);
		if( response_code != MTP_RESPONSE_OK )
			return response_code;
	}

	return storage_found ? MTP_RESPONSE_OK : MTP_RESPONSE_INVALID_STORAGE_ID;
}

uint32_t mtp_op_GetObjectHandles(mtp_ctx * ctx,MTP_PACKET_HEADER * mtp_packet_hdr, int * size,uint32_t * ret_params, int * ret_params_size)
{
	int ofs;
	uint32_t storageid;
	uint32_t parent_handle;
	uint32_t object_format;
	uint32_t nb_of_handles;
	uint32_t total_size;
	uint32_t response_code;
	fs_entry * entry;
	int transfer_limit;
	int sz;
	int transfer_result;

	if(!ctx->fs_db)
		return MTP_RESPONSE_SESSION_NOT_OPEN;

	if( pthread_mutex_lock( &ctx->inotify_mutex ) )
		return MTP_RESPONSE_GENERAL_ERROR;

	storageid = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 0, 4);        // Get param 1 - Storage ID
	object_format = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 4, 4);   // Get param 2 - Object Format
	parent_handle = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 8, 4);   // Get param 3 - parent handle

	sz = build_response(ctx, mtp_packet_hdr->tx_id, MTP_CONTAINER_TYPE_DATA, mtp_packet_hdr->code, ctx->wrbuffer, ctx->usb_wr_buffer_max_size, 0,0);
	if(sz < 0)
		goto error;

	if( parent_handle == MTP_ROOT_HANDLE )
		parent_handle = 0x00000000;

	PRINT_DEBUG("MTP_OPERATION_GET_OBJECT_HANDLES - Parent Handle 0x%.8x, Storage ID 0x%.8x, Format 0x%.8x", parent_handle, storageid, object_format);

	response_code = mtp_sync_requested_handles(ctx, storageid, parent_handle);
	if( response_code != MTP_RESPONSE_OK )
	{
		pthread_mutex_unlock( &ctx->inotify_mutex );
		return response_code;
	}

	nb_of_handles = 0;
	entry = ctx->fs_db->entry_list;
	while( entry )
	{
		if( mtp_handle_matches(entry, storageid, parent_handle, object_format) )
			nb_of_handles++;
		entry = entry->next;
	}

	if( nb_of_handles > ((UINT32_MAX - (uint32_t)sizeof(MTP_PACKET_HEADER) - (uint32_t)sizeof(uint32_t)) / (uint32_t)sizeof(uint32_t)) )
		goto error;

	total_size = (uint32_t)sizeof(MTP_PACKET_HEADER) + (uint32_t)sizeof(uint32_t) + (nb_of_handles * (uint32_t)sizeof(uint32_t));

	transfer_limit = mtp_get_usb_bulk_transfer_limit(ctx);
	if( transfer_limit <= 0 )
		goto error;
	if( transfer_limit > ctx->usb_wr_buffer_max_size )
		transfer_limit = ctx->usb_wr_buffer_max_size;
	transfer_limit -= transfer_limit % (int)ctx->max_packet_size;
	if( transfer_limit <= (int)(sizeof(MTP_PACKET_HEADER) + sizeof(uint32_t)) )
		goto error;

	if( poke32(ctx->wrbuffer, 0, ctx->usb_wr_buffer_max_size, total_size) < 0 )
		goto error;

	ofs = sizeof(MTP_PACKET_HEADER);
	ofs = poke32(ctx->wrbuffer, ofs, ctx->usb_wr_buffer_max_size, nb_of_handles);
	if( ofs < 0 )
		goto error;

	PRINT_DEBUG("MTP_OPERATION_GET_OBJECT_HANDLES - %u objects found", nb_of_handles);

	entry = ctx->fs_db->entry_list;
	while( entry )
	{
		if( mtp_handle_matches(entry, storageid, parent_handle, object_format) )
		{
			/* Keep intermediate writes MPS aligned and within the staging buffer. */
			if( ofs + (int)sizeof(uint32_t) > transfer_limit )
			{
				transfer_result = write_usb(ctx->usb_ctx, EP_DESCRIPTOR_IN, ctx->wrbuffer, ofs);
				if( transfer_result == -2 )
				{
					pthread_mutex_unlock( &ctx->inotify_mutex );
					return MTP_RESPONSE_NO_RESPONSE;
				}
				if( transfer_result != ofs )
					goto error;
				ofs = 0;
			}

			ofs = poke32(ctx->wrbuffer, ofs, ctx->usb_wr_buffer_max_size, entry->handle);
			if( ofs < 0 )
				goto error;
		}

		entry = entry->next;
	}

	if( ofs )
	{
		transfer_result = write_usb(ctx->usb_ctx, EP_DESCRIPTOR_IN, ctx->wrbuffer, ofs);
		if( transfer_result == -2 )
		{
			pthread_mutex_unlock( &ctx->inotify_mutex );
			return MTP_RESPONSE_NO_RESPONSE;
		}
		if( transfer_result != ofs )
			goto error;
	}

	transfer_result = check_and_send_USB_ZLP(ctx, total_size);
	if( transfer_result == -2 )
	{
		pthread_mutex_unlock( &ctx->inotify_mutex );
		return MTP_RESPONSE_NO_RESPONSE;
	}
	if( transfer_result < 0 )
		goto error;

	*size = (total_size <= INT_MAX) ? (int)total_size : 0;

	pthread_mutex_unlock( &ctx->inotify_mutex );

	return MTP_RESPONSE_OK;

error:
	pthread_mutex_unlock( &ctx->inotify_mutex );

	return MTP_RESPONSE_GENERAL_ERROR;
}
