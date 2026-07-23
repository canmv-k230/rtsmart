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
 * @file   mtp_op_sendobject.c
 * @brief  send object operation.
 * @author Jean-François DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#include "buildconf.h"

#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "logs_out.h"

#include "mtp.h"
#include "mtp_helpers.h"
#include "mtp_constant.h"
#include "mtp_operations.h"

#include "usb_gadget_fct.h"

static void mtp_clear_send_object_state(mtp_ctx * ctx)
{
	ctx->SendObjInfoHandle = 0xFFFFFFFFU;
	ctx->SendObjInfoSize = 0;
	ctx->SendObjInfoOffset = 0;
	ctx->pending_data_operation = 0;
	ctx->pending_data_transaction_id = 0;
}

static int mtp_write_file_data(int file, const unsigned char * data, mtp_size size)
{
	ssize_t written;

	while(size > 0)
	{
		written = write(file, data, size);
		if( written < 0 && errno == EINTR )
			continue;
		if( written <= 0 )
			return -1;

		data += written;
		size -= written;
	}

	return 0;
}

uint32_t mtp_op_SendObject(mtp_ctx * ctx,MTP_PACKET_HEADER * mtp_packet_hdr, int * size,uint32_t * ret_params, int * ret_params_size)
{
	uint32_t response_code;
	fs_entry * entry;
	const unsigned char * data;
	char * full_path;
	int file;
	int received_size;
	int request_size;
	int transfer_failed;
	int transport_failed;
	int cancelled;
	int is_partial;
	int transfer_limit;
	filefoundinfo fileinfo;
	mtp_size expected_size;
	mtp_size remaining_size;
	mtp_size entry_size;

	if(!ctx->fs_db)
		return MTP_RESPONSE_SESSION_NOT_OPEN;

	if( pthread_mutex_lock( &ctx->inotify_mutex ) )
		return MTP_RESPONSE_GENERAL_ERROR;

	response_code = MTP_RESPONSE_GENERAL_ERROR;
	is_partial = mtp_packet_hdr->code == MTP_OPERATION_SEND_PARTIAL_OBJECT;

	if( mtp_packet_hdr->operation == MTP_CONTAINER_TYPE_COMMAND )
	{
		if( ctx->pending_data_operation )
		{
			response_code = MTP_RESPONSE_DEVICE_BUSY;
			goto out;
		}

		if( is_partial )
		{
			if( ctx->SendObjInfoHandle != 0xFFFFFFFFU )
			{
				response_code = MTP_RESPONSE_DEVICE_BUSY;
				goto out;
			}

			ctx->SendObjInfoHandle = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER), 4);
			ctx->SendObjInfoOffset = peek64(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 4, 8);
			ctx->SendObjInfoSize = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 12, 4);
			if( ctx->SendObjInfoSize > (UINT32_MAX - (uint32_t)sizeof(MTP_PACKET_HEADER)) )
			{
				response_code = MTP_RESPONSE_OBJECT_TOO_LARGE;
				mtp_clear_send_object_state(ctx);
				goto out;
			}
		}

		if( ctx->SendObjInfoHandle == 0xFFFFFFFFU )
		{
			response_code = MTP_RESPONSE_INVALID_OBJECT_HANDLE;
			mtp_clear_send_object_state(ctx);
			goto out;
		}

		if( check_handle_access(ctx, NULL, ctx->SendObjInfoHandle, 1, &response_code) )
		{
			mtp_clear_send_object_state(ctx);
			goto out;
		}

		if( is_partial )
		{
			entry = get_entry_by_handle(ctx->fs_db, ctx->SendObjInfoHandle);
			if( !entry || (entry->edit_session_id != ctx->session_id) )
			{
				response_code = MTP_RESPONSE_ACCESS_DENIED;
				mtp_clear_send_object_state(ctx);
				goto out;
			}
		}

		ctx->pending_data_operation = mtp_packet_hdr->code;
		ctx->pending_data_transaction_id = mtp_packet_hdr->tx_id;
		response_code = MTP_RESPONSE_NO_RESPONSE;
		goto out;
	}

	if( (mtp_packet_hdr->operation != MTP_CONTAINER_TYPE_DATA) || (ctx->pending_data_operation != mtp_packet_hdr->code) || (ctx->pending_data_transaction_id != mtp_packet_hdr->tx_id) || (ctx->SendObjInfoHandle == 0xFFFFFFFFU) )
	{
		response_code = MTP_RESPONSE_INVALID_PARAMETER;
		goto out;
	}

	expected_size = ctx->SendObjInfoSize;
	if( (mtp_packet_hdr->length != (uint32_t)(sizeof(MTP_PACKET_HEADER) + expected_size)) || (*size < (int)sizeof(MTP_PACKET_HEADER)) || (*size > (int)mtp_packet_hdr->length) )
	{
		response_code = MTP_RESPONSE_INVALID_PARAMETER;
		mtp_clear_send_object_state(ctx);
		goto out;
	}

	entry = get_entry_by_handle(ctx->fs_db, ctx->SendObjInfoHandle);
	if( !entry )
	{
		response_code = MTP_RESPONSE_INVALID_OBJECT_HANDLE;
		mtp_clear_send_object_state(ctx);
		goto out;
	}
	if( check_handle_access(ctx, entry, 0, 1, &response_code) )
	{
		mtp_clear_send_object_state(ctx);
		goto out;
	}
	if( is_partial && (entry->edit_session_id != ctx->session_id) )
	{
		response_code = MTP_RESPONSE_ACCESS_DENIED;
		mtp_clear_send_object_state(ctx);
		goto out;
	}

	full_path = build_full_path(ctx->fs_db, mtp_get_storage_root(ctx, entry->storage_id), entry);
	if( !full_path )
	{
		mtp_clear_send_object_state(ctx);
		goto out;
	}

	file = -1;
	if( !set_storage_giduid(ctx, entry->storage_id) )
	{
		if( is_partial )
			file = open(full_path, O_RDWR | O_LARGEFILE);
		else
			file = open(full_path, O_WRONLY | O_TRUNC | O_LARGEFILE);
	}
	restore_giduid(ctx);
	if( file == -1 )
	{
		free(full_path);
		mtp_clear_send_object_state(ctx);
		goto out;
	}

	transfer_failed = 0;
	transport_failed = 0;
	cancelled = ctx->cancel_req ? 1 : 0;
	transfer_limit = mtp_get_usb_bulk_transfer_limit(ctx);
	if( transfer_limit > ctx->usb_rd_buffer_max_size )
		transfer_limit = ctx->usb_rd_buffer_max_size;
	if( transfer_limit <= 0 )
		transfer_failed = 1;
	if( lseek64(file, ctx->SendObjInfoOffset, SEEK_SET) < 0 )
		transfer_failed = 1;

	received_size = *size - (int)sizeof(MTP_PACKET_HEADER);
	remaining_size = expected_size - received_size;
	if( (received_size < 0) || (remaining_size < 0) )
		transfer_failed = 1;

	ctx->transferring_file_data = 1;
	if( !transfer_failed && !cancelled && received_size > 0 )
	{
		data = (const unsigned char *)mtp_packet_hdr + sizeof(MTP_PACKET_HEADER);
		if( mtp_write_file_data(file, data, received_size) )
			transfer_failed = 1;
	}

	while( !transfer_failed && !transport_failed && !cancelled && (remaining_size > 0) )
	{
		request_size = remaining_size > transfer_limit ? transfer_limit : (int)remaining_size;
		if( request_size <= 0 )
		{
			transfer_failed = 1;
			break;
		}

		received_size = read_usb(ctx->usb_ctx, ctx->rdbuffer2, request_size);
		if( received_size == -2 )
		{
			cancelled = 1;
			break;
		}
		if( (received_size <= 0) || (received_size > request_size) )
		{
			transport_failed = 1;
			break;
		}
		if( mtp_write_file_data(file, ctx->rdbuffer2, received_size) )
		{
			transfer_failed = 1;
			break;
		}

		remaining_size -= received_size;
		if( received_size < request_size && remaining_size )
			transport_failed = 1;
		if( ctx->cancel_req )
			cancelled = 1;
	}

	entry_size = lseek64(file, 0, SEEK_END);
	if( entry_size >= 0 )
		entry->size = entry_size;
	else
		transfer_failed = 1;
	ctx->transferring_file_data = 0;
	close(file);
	fs_invalidate_scan_cache(ctx->fs_db);

	if( !is_partial && !transfer_failed && !transport_failed && !cancelled && (remaining_size == 0) && (ctx->usb_cfg.val_umask >= 0) )
		chmod(full_path, 0777 & (~ctx->usb_cfg.val_umask));

	memset(&fileinfo, 0, sizeof(fileinfo));
	if( !set_storage_giduid(ctx, entry->storage_id) )
	{
		if( fs_entry_stat(full_path, &fileinfo) )
		{
			entry->size = fileinfo.size;
			entry->date = fileinfo.date;
			entry->fat_date = fileinfo.fat_date;
			entry->fat_time = fileinfo.fat_time;
		}
	}
	restore_giduid(ctx);

	free(full_path);

	if( cancelled || ctx->cancel_req || transport_failed )
		response_code = MTP_RESPONSE_NO_RESPONSE;
	else if( transfer_failed || remaining_size )
		response_code = MTP_RESPONSE_INCOMPLETE_TRANSFER;
	else
		response_code = MTP_RESPONSE_OK;

	mtp_clear_send_object_state(ctx);

out:
	if( pthread_mutex_unlock(&ctx->inotify_mutex) )
		return MTP_RESPONSE_GENERAL_ERROR;

	return response_code;
}
