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
 * @file   mtp_op_getobjectproplist.c
 * @brief  get object prop list operation.
 * @author Jean-François DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#include "buildconf.h"

#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "mtp.h"
#include "mtp_helpers.h"
#include "mtp_constant.h"
#include "mtp_operations.h"
#include "mtp_ops_helpers.h"
#include "mtp_properties.h"
#include "usb_gadget_fct.h"

#include "logs_out.h"

#define MTP_ALL_OBJECT_HANDLES 0xFFFFFFFFU
#define MTP_OBJECTPROPLIST_ENTRY_BUFFER_SIZE 2048

typedef struct mtp_objectproplist_stream_
{
	mtp_ctx * ctx;
	int transfer_limit;
	int offset;
	int written;
}mtp_objectproplist_stream;

static int mtp_objectproplist_entry_matches(const fs_entry * entry, uint32_t handle, uint32_t format_id, uint32_t depth)
{
	uint16_t object_format;

	if( !entry || (entry->flags & ENTRY_IS_DELETED) || (entry->handle == entry->parent) )
		return 0;

	object_format = (entry->flags & ENTRY_IS_DIR) ? MTP_FORMAT_ASSOCIATION : MTP_FORMAT_UNDEFINED;
	if( format_id && (format_id != object_format) )
		return 0;

	if( handle == MTP_ALL_OBJECT_HANDLES )
		return 1;

	if( !handle )
		return entry->parent == 0;

	if( entry->handle == handle )
		return 1;

	return (depth == 1) && (entry->parent == handle);
}

static int mtp_objectproplist_stream_write(mtp_objectproplist_stream * stream, const void * data, int size)
{
	const unsigned char * source;
	int copy_size;
	int ret;

	if( !stream || !stream->ctx || !stream->ctx->wrbuffer || !data || (size < 0) )
		return -1;

	source = data;
	while(size)
	{
		copy_size = stream->transfer_limit - stream->offset;
		if( copy_size <= 0 )
			return -1;
		if( copy_size > size )
			copy_size = size;

		memcpy(stream->ctx->wrbuffer + stream->offset, source, copy_size);
		stream->offset += copy_size;
		stream->written += copy_size;
		source += copy_size;
		size -= copy_size;

		if( stream->offset == stream->transfer_limit )
		{
			ret = write_usb(stream->ctx->usb_ctx, EP_DESCRIPTOR_IN, stream->ctx->wrbuffer, stream->offset);
			if( ret == -2 )
				return -2;
			if( ret != stream->offset )
				return -1;
			stream->offset = 0;
		}
	}

	return 0;
}

static int mtp_objectproplist_stream_finish(mtp_objectproplist_stream * stream, int total_size)
{
	int ret;

	if( !stream || (stream->written != total_size) )
		return -1;

	if( stream->offset )
	{
		ret = write_usb(stream->ctx->usb_ctx, EP_DESCRIPTOR_IN, stream->ctx->wrbuffer, stream->offset);
		if( ret == -2 )
			return -2;
		if( ret != stream->offset )
			return -1;
	}

	ret = check_and_send_USB_ZLP(stream->ctx, total_size);
	return ret < 0 ? ret : 0;
}

static uint32_t mtp_sync_objectproplist_scope(mtp_ctx * ctx, uint32_t handle, uint32_t depth)
{
	fs_entry * entry;
	uint32_t response_code;
	int i;


	if( (handle == MTP_ALL_OBJECT_HANDLES) || !handle )
	{
		for(i = 0; i < MAX_STORAGE_NB; i++)
		{
			if( !ctx->storages[i].root_path )
				continue;

			response_code = mtp_sync_folder(ctx, ctx->storages[i].storage_id, 0);
			if( response_code != MTP_RESPONSE_OK )
				return response_code;
		}

		return MTP_RESPONSE_OK;
	}

	entry = get_entry_by_handle(ctx->fs_db, handle);
	if( !entry )
		return MTP_RESPONSE_INVALID_OBJECT_HANDLE;

	if( (depth == 1) && (entry->flags & ENTRY_IS_DIR) )
		return mtp_sync_folder(ctx, entry->storage_id, entry->handle);

	return MTP_RESPONSE_OK;
}
uint32_t mtp_op_GetObjectPropList(mtp_ctx * ctx,MTP_PACKET_HEADER * mtp_packet_hdr, int * size,uint32_t * ret_params, int * ret_params_size)
{
	fs_entry * entry;
	uint32_t response_code;
	uint32_t handle;
	uint32_t format_id;
	uint32_t prop_code;
	uint32_t prop_group_code;
	uint32_t depth;
	uint64_t dataset_size;
	uint64_t numberofelements;
	int transfer_limit;
	int entry_size;
	int entry_elements;
	int total_size;
	int stream_ret;
	int sz;
	unsigned char * entry_buffer;
	mtp_objectproplist_stream stream;

	if(!ctx->fs_db)
		return MTP_RESPONSE_SESSION_NOT_OPEN;

	if( pthread_mutex_lock( &ctx->inotify_mutex ) )
		return MTP_RESPONSE_GENERAL_ERROR;

	handle = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER), 4);
	format_id = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 4, 4);
	prop_code = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 8, 4);
	prop_group_code = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 12, 4);
	depth = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 16, 4);
	entry_buffer = NULL;
	response_code = MTP_RESPONSE_GENERAL_ERROR;

	PRINT_DEBUG("MTP_OPERATION_GET_OBJECT_PROP_LIST :(Handle: 0x%.8X FormatCode: 0x%.8X ObjPropCode: 0x%.8X ObjPropGroupCode: 0x%.8X Depth: %d)", handle, format_id, prop_code, prop_group_code, depth);

	if( prop_code == 0 )
	{
		response_code = prop_group_code ? MTP_RESPONSE_SPECIFICATION_BY_GROUP_UNSUPPORTED : MTP_RESPONSE_PARAMETER_NOT_SUPPORTED;
		goto out;
	}

	if( (prop_code != MTP_ALL_OBJECT_HANDLES) && !objectproplist_property_supported(prop_code) )
	{
		response_code = MTP_RESPONSE_OBJECT_PROP_NOT_SUPPORTED;
		goto out;
	}

	if( format_id && (format_id != MTP_FORMAT_UNDEFINED) && (format_id != MTP_FORMAT_ASSOCIATION) )
	{
		response_code = MTP_RESPONSE_SPECIFICATION_BY_FORMAT_UNSUPPORTED;
		goto out;
	}

	if( (depth == MTP_ALL_OBJECT_HANDLES) && (!handle || (handle == MTP_ALL_OBJECT_HANDLES)) )
	{
		handle = MTP_ALL_OBJECT_HANDLES;
		depth = 0;
	}
	if( depth > 1 )
	{
		response_code = MTP_RESPONSE_SPECIFICATION_BY_DEPTH_UNSUPPORTED;
		goto out;
	}

	response_code = mtp_sync_objectproplist_scope(ctx, handle, depth);
	if( response_code != MTP_RESPONSE_OK )
		goto out;

	dataset_size = sizeof(uint32_t);
	numberofelements = 0;
	for(entry = ctx->fs_db->entry_list; entry; entry = entry->next)
	{
		if( !mtp_objectproplist_entry_matches(entry, handle, format_id, depth) )
			continue;

		entry_size = build_objectproplist_entry(ctx, NULL, 0, entry, prop_code, &entry_elements);
		if( entry_size < 0 || (uint64_t)entry_size > (uint64_t)INT_MAX - dataset_size || (uint64_t)entry_elements > UINT32_MAX - numberofelements )
			goto out;

		dataset_size += entry_size;
		numberofelements += entry_elements;
	}

	if( dataset_size > (uint64_t)INT_MAX - sizeof(MTP_PACKET_HEADER) )
		goto out;
	total_size = (int)(sizeof(MTP_PACKET_HEADER) + dataset_size);

	transfer_limit = mtp_get_usb_bulk_transfer_limit(ctx);
	if( transfer_limit <= 0 )
		goto out;
	if( transfer_limit > ctx->usb_wr_buffer_max_size )
		transfer_limit = ctx->usb_wr_buffer_max_size;
	transfer_limit -= transfer_limit % (int)ctx->max_packet_size;
	if( transfer_limit <= (int)(sizeof(MTP_PACKET_HEADER) + sizeof(uint32_t)) )
		goto out;

	entry_buffer = malloc(MTP_OBJECTPROPLIST_ENTRY_BUFFER_SIZE);
	if( !entry_buffer )
		goto out;

	sz = build_response(ctx, mtp_packet_hdr->tx_id, MTP_CONTAINER_TYPE_DATA, mtp_packet_hdr->code, ctx->wrbuffer, ctx->usb_wr_buffer_max_size, 0, 0);
	if( sz < 0 || poke32(ctx->wrbuffer, 0, ctx->usb_wr_buffer_max_size, (uint32_t)total_size) < 0 || poke32(ctx->wrbuffer, sz, ctx->usb_wr_buffer_max_size, (uint32_t)numberofelements) < 0 )
		goto out;

	memset(&stream, 0, sizeof(stream));
	stream.ctx = ctx;
	stream.transfer_limit = transfer_limit;
	stream.offset = sz + (int)sizeof(uint32_t);
	stream.written = stream.offset;

	for(entry = ctx->fs_db->entry_list; entry; entry = entry->next)
	{
		if( !mtp_objectproplist_entry_matches(entry, handle, format_id, depth) )
			continue;

		entry_size = build_objectproplist_entry(ctx, entry_buffer, MTP_OBJECTPROPLIST_ENTRY_BUFFER_SIZE, entry, prop_code, &entry_elements);
		if( entry_size < 0 )
			goto out;

		stream_ret = mtp_objectproplist_stream_write(&stream, entry_buffer, entry_size);
		if( stream_ret )
		{
			response_code = stream_ret == -2 ? MTP_RESPONSE_NO_RESPONSE : MTP_RESPONSE_GENERAL_ERROR;
			goto out;
		}
	}

	stream_ret = mtp_objectproplist_stream_finish(&stream, total_size);
	if( stream_ret )
	{
		response_code = stream_ret == -2 ? MTP_RESPONSE_NO_RESPONSE : MTP_RESPONSE_GENERAL_ERROR;
		goto out;
	}

	*size = total_size;
	response_code = MTP_RESPONSE_OK;

out:
	if( entry_buffer )
		free(entry_buffer);
	if( pthread_mutex_unlock( &ctx->inotify_mutex ) )
	{
		if( response_code == MTP_RESPONSE_OK )
			response_code = MTP_RESPONSE_GENERAL_ERROR;
	}

	return response_code;
}
