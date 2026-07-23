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
 * @file   mtp_op_sendobjectinfo.c
 * @brief  send object info operation
 * @author Jean-François DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#include "buildconf.h"

#include <pthread.h>
#include <inttypes.h>

#include "logs_out.h"

#include "mtp.h"
#include "mtp_helpers.h"
#include "mtp_constant.h"
#include "mtp_operations.h"

#include "usb_gadget_fct.h"

uint32_t mtp_op_SendObjectInfo(mtp_ctx * ctx,MTP_PACKET_HEADER * mtp_packet_hdr, int * size,uint32_t * ret_params, int * ret_params_size)
{
	uint32_t response_code;
	uint32_t storageid;
	uint32_t parent_handle;
	uint32_t new_handle;
	MTP_PACKET_HEADER * data_packet_hdr;
	int sz;

	if(!ctx->fs_db)
		return MTP_RESPONSE_SESSION_NOT_OPEN;
	if( (mtp_packet_hdr->operation != MTP_CONTAINER_TYPE_COMMAND) || (*size < (int)(sizeof(MTP_PACKET_HEADER) + (2 * sizeof(uint32_t))) ) )
		return MTP_RESPONSE_INVALID_PARAMETER;

	if( pthread_mutex_lock( &ctx->inotify_mutex ) )
		return MTP_RESPONSE_GENERAL_ERROR;

	if( ctx->pending_data_operation || (ctx->SendObjInfoHandle != 0xFFFFFFFFU) )
	{
		pthread_mutex_unlock( &ctx->inotify_mutex );
		return MTP_RESPONSE_DEVICE_BUSY;
	}

	storageid = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER), 4);         // Get param 1 - storage id
	parent_handle = peek(mtp_packet_hdr, sizeof(MTP_PACKET_HEADER) + 4, 4); // Get param 2 - parent handle

	PRINT_DEBUG("MTP_OPERATION_SEND_OBJECT_INFO : Rx dataset...");

	sz = read_usb(ctx->usb_ctx, ctx->rdbuffer2, ctx->usb_rd_buffer_max_size);
	if( sz < (int)sizeof(MTP_PACKET_HEADER) )
	{
		ctx->SendObjInfoHandle = 0xFFFFFFFFU;
		ctx->SendObjInfoSize = 0;
		ctx->SendObjInfoOffset = 0;
		ctx->pending_data_operation = 0;
		ctx->pending_data_transaction_id = 0;
		pthread_mutex_unlock( &ctx->inotify_mutex );
		return sz == -2 ? MTP_RESPONSE_NO_RESPONSE : MTP_RESPONSE_INCOMPLETE_TRANSFER;
	}
	PRINT_DEBUG_BUF(ctx->rdbuffer2, sz);

	new_handle = 0xFFFFFFFF;
	data_packet_hdr = (MTP_PACKET_HEADER *)ctx->rdbuffer2;

	if( data_packet_hdr->tx_id != mtp_packet_hdr->tx_id )
		response_code = MTP_RESPONSE_INVALID_DATASET;
	else
		response_code = parse_incomming_dataset(ctx,ctx->rdbuffer2,sz,&new_handle,parent_handle,storageid);
	if( response_code == MTP_RESPONSE_OK )
	{
		PRINT_DEBUG("MTP_OPERATION_SEND_OBJECT_INFO : Response - storageid: 0x%.8X, parent_handle: 0x%.8X, new_handle: 0x%.8X ",storageid,parent_handle,new_handle);
		ret_params[0] = storageid;
		ret_params[1] = parent_handle;
		ret_params[2] = new_handle;
		*ret_params_size = sizeof(uint32_t) * 3;
	}
	else
	{
		ctx->SendObjInfoHandle = 0xFFFFFFFFU;
		ctx->SendObjInfoSize = 0;
		ctx->SendObjInfoOffset = 0;
		ctx->pending_data_operation = 0;
		ctx->pending_data_transaction_id = 0;
	}

	pthread_mutex_unlock( &ctx->inotify_mutex );

	*size = sz;

	return response_code;
}
