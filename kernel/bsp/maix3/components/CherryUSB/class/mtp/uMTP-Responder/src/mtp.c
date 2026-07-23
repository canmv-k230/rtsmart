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
 * @file   mtp.c
 * @brief  Main MTP protocol functions.
 * @author Jean-François DEL NERO <Jean-Francois.DELNERO@viveris.fr>
 */

#include "buildconf.h"

#include <inttypes.h>
#include <pthread.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

#include "mtp.h"
#include "mtp_helpers.h"
#include "mtp_constant.h"
#include "mtp_constant_strings.h"
#include "mtp_datasets.h"
#include "mtp_properties.h"
#include "mtp_operations.h"

#include "usb_gadget_fct.h"
#include "mtp_support_def.h"
#include "fs_handles_db.h"

#include "inotify.h"
#include "msgqueue.h"

#include "logs_out.h"
#include "usbstring.h"
#include "dfs_fs.h"

mtp_ctx * mtp_init_responder()
{
	mtp_ctx * ctx;
	int inotify_mutex_initialized;

	PRINT_DEBUG("init_mtp_responder");

	ctx = malloc(sizeof(mtp_ctx));
	inotify_mutex_initialized = 0;
	if(ctx)
	{
		memset(ctx,0,sizeof(mtp_ctx));

		ctx->usb_wr_buffer_max_size = CONFIG_MAX_TX_USB_BUFFER_SIZE;
		ctx->wrbuffer = NULL;

		ctx->usb_rd_buffer_max_size = CONFIG_MAX_RX_USB_BUFFER_SIZE;
		ctx->rdbuffer = NULL;
		ctx->rdbuffer2 = NULL;

		ctx->read_file_buffer_size = CONFIG_READ_FILE_BUFFER_SIZE;
		ctx->read_file_buffer = NULL;
		ctx->fs_db_cache_buckets = CONFIG_FS_DB_CACHE_BUCKETS;
		ctx->fs_db_pool_size = MTP_FS_DB_POOL_SIZE;
		ctx->fs_dir_read_buffer_size = CONFIG_MTP_DIR_READ_BUFFER_SIZE;
		ctx->fs_db_scan_cache = CONFIG_FS_DB_SCAN_CACHE;
		ctx->fs_db_change_generation = 1;
		ctx->fs_db_session_generation = 0;
		ctx->fs_db_session_counter = 0;

		ctx->temp_array = malloc( MAX_STORAGE_NB * sizeof(uint32_t) );
		if(!ctx->temp_array)
			goto init_error;

		ctx->uid = getuid();
		ctx->euid = geteuid();
		ctx->gid = getgid();
		ctx->egid = getegid();

		ctx->default_uid = -1;
		ctx->default_gid = -1;

		ctx->SendObjInfoHandle = 0xFFFFFFFF;
		ctx->SetObjectPropValue_Handle = 0xFFFFFFFF;
		ctx->cancel_status_pending = 0;
		ctx->reset_req = 0;
		ctx->transaction_active = 0;
		ctx->active_transaction_id = 0;
		ctx->pending_data_operation = 0;
		ctx->pending_data_transaction_id = 0;

		if( rt_mutex_init (&ctx->inotify_mutex, "mtp_mtx", 0) )
			goto init_error;
		inotify_mutex_initialized = 1;

		if( inotify_handler_init( ctx ) )
			goto init_error;
		if( msgqueue_handler_init( ctx ) )
		{
			inotify_handler_deinit( ctx );
			goto init_error;
		}

		PRINT_DEBUG("init_mtp_responder : Ok !");

		return ctx;
	}

init_error:
	if(ctx)
	{
		if(ctx->wrbuffer)
			free(ctx->wrbuffer);

		if(ctx->rdbuffer)
			free(ctx->rdbuffer);

		if(ctx->rdbuffer2)
			free(ctx->rdbuffer2);

		if(ctx->temp_array)
			free(ctx->temp_array);

		if(inotify_mutex_initialized)
			rt_mutex_detach(&ctx->inotify_mutex);

		free(ctx);
	}

	PRINT_ERROR("init_mtp_responder : Failed !");

	return NULL;
}

uint32_t mtp_fs_db_session_begin(mtp_ctx * ctx)
{
	uint32_t generation;

	if( !ctx )
		return 0;

	do
	{
		generation = __atomic_add_fetch(&ctx->fs_db_session_counter, 1, __ATOMIC_RELAXED);
	}while( !generation );

	__atomic_store_n(&ctx->fs_db_session_generation, generation, __ATOMIC_RELEASE);
	return generation;
}

void mtp_fs_db_session_end(mtp_ctx * ctx)
{
	if( ctx )
		__atomic_store_n(&ctx->fs_db_session_generation, 0, __ATOMIC_RELEASE);
}

uint32_t mtp_fs_db_session_get(mtp_ctx * ctx)
{
	if( !ctx )
		return 0;

	return __atomic_load_n(&ctx->fs_db_session_generation, __ATOMIC_ACQUIRE);
}

void mtp_deinit_responder(mtp_ctx * ctx)
{
	if( ctx )
	{
		mtp_fs_db_session_end(ctx);
		inotify_handler_deinit( ctx );
		msgqueue_handler_deinit( ctx );

		if( ctx->fs_db && !pthread_mutex_lock(&ctx->inotify_mutex) )
		{
			fs_invalidate_scan_cache(ctx->fs_db);
			deinit_fs_db(ctx->fs_db);
			ctx->fs_db = NULL;
			pthread_mutex_unlock(&ctx->inotify_mutex);
		}

		if(ctx->wrbuffer)
			free(ctx->wrbuffer);

		if(ctx->rdbuffer)
			free(ctx->rdbuffer);

		if(ctx->rdbuffer2)
			free(ctx->rdbuffer2);

		if(ctx->temp_array)
			free(ctx->temp_array);

		if(ctx->read_file_buffer)
			free(ctx->read_file_buffer);

		rt_mutex_detach(&ctx->inotify_mutex);

		free(ctx);
	}
}

int build_response(mtp_ctx * ctx, uint32_t tx_id, uint16_t type, uint16_t status, void * buffer, int maxsize, void * datain,int size)
{
	MTP_PACKET_HEADER tmp_hdr;
	int ofs;

	ofs = 0;

	tmp_hdr.length = sizeof(tmp_hdr) + size;
	tmp_hdr.operation = type;
	tmp_hdr.code = status;
	tmp_hdr.tx_id = tx_id;

	ofs = poke_array(buffer, ofs, maxsize, sizeof(MTP_PACKET_HEADER), 1, (unsigned char*)&tmp_hdr,0);

	if(size)
		ofs = poke_array(buffer, ofs, maxsize, size, 1, (unsigned char*)datain,0);

	return ofs;
}

int is_storage_enough(mtp_ctx * ctx, uint32_t storage_id, uint32_t objectsize)
{
	struct statfs fsbuf;
	char *storage_path = NULL;

	storage_path = mtp_get_storage_root(ctx, storage_id);
	if( !storage_path )
		return 0;

	if (dfs_statfs(storage_path, &fsbuf) == 0)
	{
		uint64_t freespace;
		freespace = (uint64_t)fsbuf.f_bsize * (uint64_t)fsbuf.f_bfree;
		if (objectsize > freespace) {
			PRINT_ERROR("%s is full, Add %ld , remain %ld", storage_path, objectsize, freespace);
			return 0;
		}
	}

	return 1;
}

static int mtp_filename_is_valid(const char * filename)
{
	if( !filename[0] || !strcmp(filename, ".") || !strcmp(filename, "..") )
		return 0;

	if( strchr(filename, '/') || strchr(filename, '\\') )
		return 0;

	return 1;
}

int parse_incomming_dataset(mtp_ctx * ctx,void * datain,int size,uint32_t * newhandle, uint32_t parent_handle, uint32_t storage_id)
{
	MTP_PACKET_HEADER * tmp_hdr;
	unsigned char * dataset_ptr;
	uint32_t objectformat;
	uint32_t objectsize;
	uint32_t storage_flags;
	uint8_t string_len;
	char tmp_str[FS_HANDLE_MAX_FILENAME_SIZE + 1];
	uint16_t unicode_str[FS_HANDLE_MAX_FILENAME_SIZE + 1];
	char * parent_folder;
	char * tmp_path;
	fs_entry * parent_entry;
	fs_entry * entry;
	fs_entry * existing_entry;
	filefoundinfo fileinfo;
	struct stat target_stat;
	int dataset_size;
	int filename_size;
	int file;
	int ret;
	int i;

	if( !ctx || !ctx->fs_db || !datain || !newhandle || (size < (int)sizeof(MTP_PACKET_HEADER)) )
		return MTP_RESPONSE_INVALID_DATASET;

	tmp_hdr = (MTP_PACKET_HEADER *)datain;
	if( (tmp_hdr->length != (uint32_t)size) || (tmp_hdr->operation != MTP_CONTAINER_TYPE_DATA) || (tmp_hdr->code != MTP_OPERATION_SEND_OBJECT_INFO) )
		return MTP_RESPONSE_INVALID_DATASET;

	dataset_size = size - (int)sizeof(MTP_PACKET_HEADER);
	if( dataset_size < 0x35 )
		return MTP_RESPONSE_INVALID_DATASET;

	dataset_ptr = (unsigned char *)datain + sizeof(MTP_PACKET_HEADER);
	string_len = dataset_ptr[0x34];
	filename_size = 0x35 + ((int)string_len * (int)sizeof(uint16_t));
	if( !string_len || (string_len > FS_HANDLE_MAX_FILENAME_SIZE) || (filename_size > dataset_size) )
		return MTP_RESPONSE_INVALID_DATASET;

	memset(unicode_str, 0, sizeof(unicode_str));
	for(i = 0; i < string_len; i++)
		unicode_str[i] = peek(dataset_ptr, 0x35 + (i * 2), 2);
	if( unicode_str[string_len - 1] != 0 )
		return MTP_RESPONSE_INVALID_DATASET;
	for(i = 0; i < string_len - 1; i++)
	{
		if( unicode_str[i] == 0 )
			return MTP_RESPONSE_INVALID_DATASET;
	}
	if( unicode2charstring(tmp_str, unicode_str, sizeof(tmp_str)) )
		return MTP_RESPONSE_INVALID_DATASET;
	tmp_str[sizeof(tmp_str) - 1] = 0;
	if( !mtp_filename_is_valid(tmp_str) )
		return MTP_RESPONSE_INVALID_DATASET;

	if( parent_handle == 0xFFFFFFFFU )
		parent_handle = 0;

	storage_flags = mtp_get_storage_flags(ctx, storage_id);
	if( storage_flags == 0xFFFFFFFFU )
		return MTP_RESPONSE_INVALID_STORAGE_ID;
	if( storage_flags & UMTP_STORAGE_READONLY )
		return MTP_RESPONSE_STORE_READ_ONLY;

	parent_entry = get_entry_by_handle_and_storageid(ctx->fs_db, parent_handle, storage_id);
	if( !parent_entry || !(parent_entry->flags & ENTRY_IS_DIR) )
		return MTP_RESPONSE_INVALID_PARENT_OBJECT;

	objectformat = peek(dataset_ptr, 0x04, 2);
	objectsize = peek(dataset_ptr, 0x08, 4);
	if( objectsize > (UINT32_MAX - (uint32_t)sizeof(MTP_PACKET_HEADER)) )
		return MTP_RESPONSE_OBJECT_TOO_LARGE;
	if( !is_storage_enough(ctx, storage_id, objectsize) )
		return MTP_RESPONSE_STORAGE_FULL;

	parent_folder = build_full_path(ctx->fs_db, mtp_get_storage_root(ctx, storage_id), parent_entry);
	if( !parent_folder )
		return MTP_RESPONSE_GENERAL_ERROR;

	tmp_path = malloc(strlen(parent_folder) + strlen(tmp_str) + 2);
	if( !tmp_path )
	{
		free(parent_folder);
		return MTP_RESPONSE_GENERAL_ERROR;
	}
	snprintf(tmp_path, strlen(parent_folder) + strlen(tmp_str) + 2, "%s/%s", parent_folder, tmp_str);

	memset(&fileinfo, 0, sizeof(fileinfo));
	strncpy(fileinfo.filename, tmp_str, FS_HANDLE_MAX_FILENAME_SIZE);
	fileinfo.filename[FS_HANDLE_MAX_FILENAME_SIZE] = 0;
	fileinfo.size = objectsize;
	entry = NULL;
	existing_entry = search_entry(ctx->fs_db, &fileinfo, parent_handle, storage_id);
	if( existing_entry || !stat(tmp_path, &target_stat) )
	{
		free(tmp_path);
		free(parent_folder);
		return MTP_RESPONSE_ACCESS_DENIED;
	}

	if( objectformat == MTP_FORMAT_ASSOCIATION )
	{
		ret = -1;
		if( !set_storage_giduid(ctx, storage_id) )
			ret = mkdir(tmp_path, 0700);
		restore_giduid(ctx);
		if( ret )
		{
			free(tmp_path);
			free(parent_folder);
			return MTP_RESPONSE_ACCESS_DENIED;
		}

		if( ctx->usb_cfg.val_umask >= 0 )
			chmod(tmp_path, 0777 & (~ctx->usb_cfg.val_umask));

		fileinfo.isdirectory = 1;
		fileinfo.size = 0;
		entry = add_entry(ctx->fs_db, &fileinfo, parent_handle, storage_id);
	}
	else
	{
		file = -1;
		if( !set_storage_giduid(ctx, storage_id) )
			file = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_LARGEFILE, S_IRUSR | S_IWUSR);
		restore_giduid(ctx);
		if( file == -1 )
		{
			free(tmp_path);
			free(parent_folder);
			return MTP_RESPONSE_ACCESS_DENIED;
		}
		close(file);

		fileinfo.isdirectory = 0;
		entry = add_entry(ctx->fs_db, &fileinfo, parent_handle, storage_id);
	}

	if( !entry )
	{
		if( !set_storage_giduid(ctx, storage_id) )
		{
			if( objectformat == MTP_FORMAT_ASSOCIATION )
				rmdir(tmp_path);
			else
				unlink(tmp_path);
		}
		restore_giduid(ctx);
		free(tmp_path);
		free(parent_folder);
		return MTP_RESPONSE_GENERAL_ERROR;
	}

	free(tmp_path);
	free(parent_folder);

	*newhandle = entry->handle;
	if( objectformat != MTP_FORMAT_ASSOCIATION )
	{
		ctx->SendObjInfoHandle = entry->handle;
		ctx->SendObjInfoSize = objectsize;
		ctx->SendObjInfoOffset = 0;
	}
	fs_invalidate_scan_cache(ctx->fs_db);

	PRINT_DEBUG("MTP_OPERATION_SEND_OBJECT_INFO : format 0x%x size %u parent 0x%.8x name %s", objectformat, objectsize, parent_handle, tmp_str);
	return MTP_RESPONSE_OK;
}

int check_and_send_USB_ZLP(mtp_ctx * ctx , int size)
{
	int ret;

	if( !ctx || !ctx->wrbuffer || !ctx->max_packet_size || (size < 0) )
		return -1;

	// USB ZLP needed ?
	if( (size >= ctx->max_packet_size) && !(size % ctx->max_packet_size) )
	{
		PRINT_DEBUG("%d bytes transfert ended - ZLP packet needed", size);

		// Yes - Send zero lenght packet.
		ret = write_usb(ctx->usb_ctx,EP_DESCRIPTOR_IN,ctx->wrbuffer,0);
		if( ret < 0 )
			return ret;

		return 1;
	}

	return 0;
}

int mtp_get_usb_bulk_transfer_limit(mtp_ctx * ctx)
{
	uint64_t limit;

	if( !ctx || !ctx->max_packet_size )
		return -1;

	limit = (uint64_t)ctx->max_packet_size * MTP_USB_BULK_MAX_PACKET_COUNT;
	if( limit > MTP_USB_BULK_MAX_TRANSFER_SIZE )
		limit = MTP_USB_BULK_MAX_TRANSFER_SIZE;
	limit -= limit % ctx->max_packet_size;

	return (int)limit;
}

int mtp_send_data_response(mtp_ctx * ctx, int size)
{
	int transfer_limit;
	int remaining_size;
	int transfer_size;
	int offset;
	int ret;

	if( !ctx || !ctx->wrbuffer || (size < (int)sizeof(MTP_PACKET_HEADER)) || (size > ctx->usb_wr_buffer_max_size) )
		return -1;

	transfer_limit = mtp_get_usb_bulk_transfer_limit(ctx);
	if( transfer_limit <= 0 )
		return -1;

	offset = 0;
	remaining_size = size;
	while( remaining_size )
	{
		transfer_size = remaining_size;
		if( transfer_size > transfer_limit )
			transfer_size = transfer_limit;

		ret = write_usb(ctx->usb_ctx, EP_DESCRIPTOR_IN, ctx->wrbuffer + offset, transfer_size);
		if( ret == -2 )
			return -2;
		if( ret != transfer_size )
			return -1;

		offset += transfer_size;
		remaining_size -= transfer_size;
	}

	ret = check_and_send_USB_ZLP(ctx, size);
	return ret < 0 ? ret : 0;
}

int check_handle_access( mtp_ctx * ctx, fs_entry * entry, uint32_t handle, int wraccess, uint32_t * response)
{
	uint32_t storage_flags;

	if( !entry )
	{
		entry = get_entry_by_handle(ctx->fs_db, handle);
	}

	if(entry)
	{
		storage_flags = mtp_get_storage_flags(ctx, entry->storage_id);
		if( storage_flags == 0xFFFFFFFF )
		{
			PRINT_DEBUG("check_handle_access : Storage 0x%.8x is Invalid !",entry->storage_id);

			if( response )
				*response = MTP_RESPONSE_INVALID_STORAGE_ID;

			return 1;
		}

		if( (storage_flags & UMTP_STORAGE_LOCKED) )
		{
			PRINT_DEBUG("check_handle_access : Storage 0x%.8x is locked !",entry->storage_id);

			if( response )
				*response = MTP_RESPONSE_STORE_NOT_AVAILABLE;

			return 1;
		}

		if( (storage_flags & UMTP_STORAGE_READONLY) && wraccess )
		{
			PRINT_DEBUG("check_handle_access : Storage 0x%.8x is Read only !",entry->storage_id);

			if( response )
				*response = MTP_RESPONSE_STORE_READ_ONLY;

			return 1;
		}

		if( wraccess && entry->edit_session_id && (entry->edit_session_id != ctx->session_id) )
		{
			PRINT_DEBUG("check_handle_access : Handle 0x%.8x is edited by session 0x%.8x", entry->handle, entry->edit_session_id);

			if( response )
				*response = MTP_RESPONSE_DEVICE_BUSY;

			return 1;
		}
	}
	else
	{
		PRINT_DEBUG("check_handle_access : Handle 0x%.8x is invalid !",handle);

		if( response )
			*response = MTP_RESPONSE_INVALID_OBJECT_HANDLE;

		return 1;
	}

	return 0;
}

int begin_edit_object(mtp_ctx * ctx, uint32_t handle, uint32_t * response)
{
	fs_entry * entry;

	entry = get_entry_by_handle(ctx->fs_db, handle);
	if( check_handle_access(ctx, entry, handle, 1, response) )
		return 1;

	if( entry->edit_session_id == ctx->session_id )
		return 0;

	if( entry->edit_session_id )
	{
		if( response )
			*response = MTP_RESPONSE_DEVICE_BUSY;

		return 1;
	}

	entry->edit_session_id = ctx->session_id;

	return 0;
}

int end_edit_object(mtp_ctx * ctx, uint32_t handle, uint32_t * response)
{
	fs_entry * entry;

	entry = get_entry_by_handle(ctx->fs_db, handle);
	if( !entry )
	{
		if( response )
			*response = MTP_RESPONSE_INVALID_OBJECT_HANDLE;

		return 1;
	}

	if( entry->edit_session_id && (entry->edit_session_id != ctx->session_id) )
	{
		if( response )
			*response = MTP_RESPONSE_ACCESS_DENIED;

		return 1;
	}

	entry->edit_session_id = 0;

	return 0;
}

void clear_edit_locks(fs_handles_db * db)
{
	fs_entry * entry_list;

	if(!db)
		return;

	entry_list = db->entry_list;
	while( entry_list )
	{
		entry_list->edit_session_id = 0;
		entry_list = entry_list->next;
	}
}

static int mtp_min_command_parameter_size(uint16_t code)
{
	switch(code)
	{
		case MTP_OPERATION_OPEN_SESSION:
		case MTP_OPERATION_GET_STORAGE_INFO:
		case MTP_OPERATION_GET_DEVICE_PROP_DESC:
		case MTP_OPERATION_GET_DEVICE_PROP_VALUE:
		case MTP_OPERATION_GET_OBJECT:
		case MTP_OPERATION_GET_OBJECT_INFO:
		case MTP_OPERATION_GET_OBJECT_PROPS_SUPPORTED:
		case MTP_OPERATION_GET_OBJECT_REFERENCES:
		case MTP_OPERATION_BEGIN_EDIT_OBJECT:
		case MTP_OPERATION_END_EDIT_OBJECT:
		case MTP_OPERATION_DELETE_OBJECT:
			return 4;

		case MTP_OPERATION_GET_OBJECT_PROP_DESC:
		case MTP_OPERATION_GET_OBJECT_PROP_VALUE:
		case MTP_OPERATION_SET_OBJECT_PROP_VALUE:
		case MTP_OPERATION_SEND_OBJECT_INFO:
			return 8;

		case MTP_OPERATION_GET_OBJECT_HANDLES:
		case MTP_OPERATION_GET_PARTIAL_OBJECT:
		case MTP_OPERATION_TRUNCATE_OBJECT:
			return 12;

		case MTP_OPERATION_GET_PARTIAL_OBJECT_64:
		case MTP_OPERATION_SEND_PARTIAL_OBJECT:
			return 16;

		case MTP_OPERATION_GET_OBJECT_PROP_LIST:
			return 20;

		default:
			return 0;
	}
}

static int mtp_packet_is_valid(mtp_ctx * ctx, MTP_PACKET_HEADER * mtp_packet_hdr, int rawsize)
{
	int parameter_size;

	if( !ctx || !mtp_packet_hdr || (rawsize < (int)sizeof(MTP_PACKET_HEADER)) )
		return 0;

	if( mtp_packet_hdr->length < sizeof(MTP_PACKET_HEADER) || mtp_packet_hdr->length < (uint32_t)rawsize )
		return 0;

	switch(mtp_packet_hdr->operation)
	{
		case MTP_CONTAINER_TYPE_COMMAND:
			if( mtp_packet_hdr->length != (uint32_t)rawsize )
				return 0;

			parameter_size = rawsize - (int)sizeof(MTP_PACKET_HEADER);
			if( (parameter_size > (int)(sizeof(uint32_t) * 5)) || (parameter_size & ((int)sizeof(uint32_t) - 1)) )
				return 0;

			return parameter_size >= mtp_min_command_parameter_size(mtp_packet_hdr->code);

		case MTP_CONTAINER_TYPE_DATA:
			switch(mtp_packet_hdr->code)
			{
			case MTP_OPERATION_SEND_OBJECT:
			case MTP_OPERATION_SEND_PARTIAL_OBJECT:
					if( (ctx->pending_data_operation != mtp_packet_hdr->code) || (ctx->pending_data_transaction_id != mtp_packet_hdr->tx_id) || (ctx->SendObjInfoHandle == 0xFFFFFFFFU) )
						return 0;
					return mtp_packet_hdr->length == (sizeof(MTP_PACKET_HEADER) + ctx->SendObjInfoSize);

			case MTP_OPERATION_SET_OBJECT_PROP_VALUE:
					return (ctx->SetObjectPropValue_Handle != 0xFFFFFFFFU) && (ctx->pending_data_operation == mtp_packet_hdr->code) && (ctx->pending_data_transaction_id == mtp_packet_hdr->tx_id) && (mtp_packet_hdr->length == (uint32_t)rawsize) && (rawsize > (int)sizeof(MTP_PACKET_HEADER));

				default:
					return 0;
			}

		default:
			return 0;
	}
}

static int mtp_send_status_response(mtp_ctx * ctx, uint32_t tx_id, uint32_t response_code, uint32_t * params, int params_size)
{
	int response_size;
	int transfer_size;

	response_size = build_response(ctx, tx_id, MTP_CONTAINER_TYPE_RESPONSE, response_code, ctx->wrbuffer, ctx->usb_wr_buffer_max_size, params, params_size);
	if( response_size < 0 )
		return -1;

	PRINT_DEBUG("Status response (%d Bytes):",response_size);
	PRINT_DEBUG_BUF(ctx->wrbuffer, response_size);
	transfer_size = write_usb(ctx->usb_ctx, EP_DESCRIPTOR_IN, ctx->wrbuffer, response_size);
	if( transfer_size == -2 )
		return -2;

	return transfer_size == response_size ? 0 : -1;
}

static void mtp_clear_pending_data_state(mtp_ctx * ctx)
{
	ctx->SendObjInfoHandle = 0xFFFFFFFFU;
	ctx->SendObjInfoSize = 0;
	ctx->SendObjInfoOffset = 0;
	ctx->SetObjectPropValue_Handle = 0xFFFFFFFFU;
	ctx->SetObjectPropValue_PropCode = 0;
	ctx->pending_data_operation = 0;
	ctx->pending_data_transaction_id = 0;
}

int process_in_packet(mtp_ctx * ctx,MTP_PACKET_HEADER * mtp_packet_hdr, int rawsize)
{
	uint32_t params[5];
	int params_size;
	uint32_t response_code;
	int size;

	if( !mtp_packet_is_valid(ctx, mtp_packet_hdr, rawsize) )
		return -1;

	memset(params, 0, sizeof(params));

	params_size = 0; // No response parameter by default

	size = rawsize;

	switch( mtp_packet_hdr->code )
	{
		case MTP_OPERATION_OPEN_SESSION:
			response_code = mtp_op_OpenSession(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_CLOSE_SESSION:
			response_code = mtp_op_CloseSession(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_DEVICE_INFO:
			response_code = mtp_op_GetDeviceInfos(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_STORAGE_IDS:
			response_code = mtp_op_GetStorageIDs(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_STORAGE_INFO:
			response_code = mtp_op_GetStorageInfo(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_DEVICE_PROP_DESC:
			response_code = mtp_op_GetDevicePropDesc(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_DEVICE_PROP_VALUE:
			response_code = mtp_op_GetDevicePropValue(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_OBJECT_HANDLES:
			response_code = mtp_op_GetObjectHandles(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_OBJECT_INFO:
			response_code = mtp_op_GetObjectInfo(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_PARTIAL_OBJECT_64:
		case MTP_OPERATION_GET_PARTIAL_OBJECT:
			response_code = mtp_op_GetPartialObject(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_OBJECT:
			response_code = mtp_op_GetObject(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_SEND_OBJECT_INFO:
			response_code = mtp_op_SendObjectInfo(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_SEND_PARTIAL_OBJECT:
		case MTP_OPERATION_SEND_OBJECT:
			response_code = mtp_op_SendObject(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_DELETE_OBJECT:
			response_code = mtp_op_DeleteObject(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_OBJECT_PROPS_SUPPORTED:
			response_code = mtp_op_GetObjectPropsSupported(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_OBJECT_PROP_DESC:
			response_code = mtp_op_GetObjectPropDesc(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_OBJECT_PROP_VALUE:
			response_code = mtp_op_GetObjectPropValue(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_SET_OBJECT_PROP_VALUE:
			response_code = mtp_op_SetObjectPropValue(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_OBJECT_PROP_LIST:
			response_code = mtp_op_GetObjectPropList(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_GET_OBJECT_REFERENCES:
			response_code = mtp_op_GetObjectReferences(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_BEGIN_EDIT_OBJECT:
			response_code = mtp_op_BeginEditObject(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_END_EDIT_OBJECT:
			response_code = mtp_op_EndEditObject(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		case MTP_OPERATION_TRUNCATE_OBJECT :
			response_code = mtp_op_TruncateObject(ctx,mtp_packet_hdr,&size,(uint32_t*)&params,&params_size);
		break;

		default:

			PRINT_WARN("MTP code unsupported ! : 0x%.4X (%s)", mtp_packet_hdr->code,mtp_get_operation_string(mtp_packet_hdr->code));

			response_code = MTP_RESPONSE_OPERATION_NOT_SUPPORTED;

		break;
	}

	// Send the status response
	if(response_code != MTP_RESPONSE_NO_RESPONSE)
	{
		size = mtp_send_status_response(ctx, mtp_packet_hdr->tx_id, response_code, params, params_size);
		if( size < 0 )
			return size == -2 ? 0 : -1;
	}
	else
	{
		PRINT_DEBUG("No Status response sent");
	}

	return 0;
}

int mtp_incoming_packet(mtp_ctx * ctx)
{
	int size;
	MTP_PACKET_HEADER * mtp_packet_hdr;

	if(!ctx)
		return 0;

	size = read_usb(ctx->usb_ctx, ctx->rdbuffer, ctx->usb_rd_buffer_max_size);

	if(size>=0)
	{
		PRINT_DEBUG("--------------------------------------------------");
		PRINT_DEBUG("Incoming_packet : %p - rawsize : %d",ctx->rdbuffer,size);

		if(!size)
			return 0; // ZLP

		if( size < (int)sizeof(MTP_PACKET_HEADER) )
		{
			PRINT_WARN("incoming_packet : short MTP header (%d bytes)", size);
			return 0;
		}

		mtp_packet_hdr = (MTP_PACKET_HEADER *)ctx->rdbuffer;
		PRINT_DEBUG("MTP Packet size : %d bytes", mtp_packet_hdr->length);
		PRINT_DEBUG("MTP Operation   : 0x%.4X (%s)", mtp_packet_hdr->operation, mtp_get_type_string(mtp_packet_hdr->operation) );
		PRINT_DEBUG("MTP code        : 0x%.4X (%s)", mtp_packet_hdr->code,mtp_get_operation_string(mtp_packet_hdr->code));
		PRINT_DEBUG("MTP Tx ID       : 0x%.8X", mtp_packet_hdr->tx_id);

		if( !mtp_packet_is_valid(ctx, mtp_packet_hdr, size) )
		{
			PRINT_WARN("incoming_packet : invalid MTP container (length %u, raw %d)", mtp_packet_hdr->length, size);
			if( (mtp_packet_hdr->operation == MTP_CONTAINER_TYPE_DATA) && ctx->pending_data_operation )
			{
				mtp_clear_pending_data_state(ctx);
				ctx->transaction_active = 0;
				ctx->active_transaction_id = 0;
			}
			size = mtp_send_status_response(ctx, mtp_packet_hdr->tx_id, MTP_RESPONSE_INVALID_PARAMETER, NULL, 0);
			if( size < 0 )
				return size == -2 ? 0 : -1;
			return 0;
		}

		PRINT_DEBUG("Header : ");
		PRINT_DEBUG_BUF(ctx->rdbuffer, sizeof(MTP_PACKET_HEADER));


		if( mtp_packet_hdr->operation == MTP_CONTAINER_TYPE_COMMAND )
		{
			ctx->active_transaction_id = mtp_packet_hdr->tx_id;
			ctx->transaction_active = 1;
		}

		size = process_in_packet(ctx,mtp_packet_hdr,size);
		if( !ctx->pending_data_operation || (ctx->pending_data_transaction_id != ctx->active_transaction_id) )
		{
			ctx->transaction_active = 0;
			ctx->active_transaction_id = 0;
		}
		return size;
	}
	else
	{
		if( size == -2 )
		{
			mtp_clear_pending_data_state(ctx);
			ctx->transaction_active = 0;
			ctx->active_transaction_id = 0;
			ctx->cancel_req = 0;
			if( ctx->reset_req )
			{
				ctx->reset_req = 0;
				return -2;
			}
			return 0;
		}

		PRINT_DEBUG("incoming_packet : Read Error (%d)!",size);
	}

	return -1;
}

void mtp_set_usb_handle(mtp_ctx * ctx, void * handle, uint32_t max_packet_size)
{
	ctx->usb_ctx = handle;
	ctx->max_packet_size = max_packet_size;
}

uint32_t mtp_add_storage(mtp_ctx * ctx, char * path, char * description, int uid, int gid, uint32_t flags)
{
	int i;
	int root_path_len;
	int description_len;

	PRINT_DEBUG("mtp_add_storage : %s", path );

	if (mtp_get_storage_id_by_name(ctx, description) != 0xFFFFFFFF)
		return 0x00000000;

	i = 0;
	while(i < MAX_STORAGE_NB)
	{
		if( !ctx->storages[i].root_path )
		{
			root_path_len = strlen(path);
			description_len = strlen(description);

			ctx->storages[i].root_path = malloc(root_path_len + 1);
			ctx->storages[i].description = malloc(description_len + 1);

			if(ctx->storages[i].root_path && ctx->storages[i].description)
			{
				ctx->storages[i].uid = uid;
				ctx->storages[i].gid = gid;

				strncpy( ctx->storages[i].root_path, path, root_path_len + 1);
				ctx->storages[i].root_path[root_path_len] = 0;

				strncpy( ctx->storages[i].description, description, description_len + 1);
				ctx->storages[i].description[description_len] = 0;

				ctx->storages[i].flags = flags;

				ctx->storages[i].storage_id = 0xFFFF0000 + (i + 1);
				PRINT_DEBUG("mtp_add_storage : Storage %.8X mapped to %s (%s) (Flags: 0x%.8X)",
					    ctx->storages[i].storage_id,
					    ctx->storages[i].root_path,
					    ctx->storages[i].description,
					    ctx->storages[i].flags);

				return ctx->storages[i].storage_id;
			}
			else
			{
				if(ctx->storages[i].root_path)
					free(ctx->storages[i].root_path);

				if(ctx->storages[i].description)
					free(ctx->storages[i].description);

				ctx->storages[i].root_path =  NULL;
				ctx->storages[i].description =  NULL;
				ctx->storages[i].flags =  0x00000000;
				ctx->storages[i].storage_id = 0x00000000;
				ctx->storages[i].uid = -1;
				ctx->storages[i].gid = -1;

				return ctx->storages[i].storage_id;
			}
		}
		i++;
	}

	return 0x00000000;
}

int mtp_remove_storage(mtp_ctx * ctx, char * name)
{
	int index = mtp_get_storage_index_by_name(ctx, name);

	if (index < 0)
		return index;

	free(ctx->storages[index].root_path);
	free(ctx->storages[index].description);

	ctx->storages[index].root_path = NULL;
	ctx->storages[index].description = NULL;
	ctx->storages[index].flags = 0x00000000;
	ctx->storages[index].storage_id = 0x00000000;
	ctx->storages[index].uid = -1;
	ctx->storages[index].gid = -1;

	return 0;
}

uint32_t mtp_get_storage_id_by_name(mtp_ctx * ctx, char * name)
{
	int i;

	PRINT_DEBUG("mtp_get_storage_id_by_name : %s", name );

	i = 0;
	while(i < MAX_STORAGE_NB)
	{
		if( ctx->storages[i].root_path )
		{
			if( !strcmp(ctx->storages[i].description, name ) )
			{
				PRINT_DEBUG("mtp_get_storage_id_by_name : %s -> %.8X",
					    ctx->storages[i].root_path,
						ctx->storages[i].storage_id);

				return ctx->storages[i].storage_id;
			}
		}
		i++;
	}

	return 0xFFFFFFFF;
}

int mtp_get_storage_index_by_name(mtp_ctx * ctx, char * name)
{
	int i;

	PRINT_DEBUG("mtp_get_storage_index_by_name : %s", name );

	i = 0;
	while(i < MAX_STORAGE_NB)
	{
		if( ctx->storages[i].root_path )
		{
			if( !strcmp(ctx->storages[i].description, name ) )
			{
				PRINT_DEBUG("mtp_get_storage_index_by_name : %s -> %.8X",
					    ctx->storages[i].root_path,
						i);

				return i;
			}
		}
		i++;
	}

	return -1;
}

int mtp_get_storage_index_by_id(mtp_ctx * ctx, uint32_t storage_id)
{
	int i;

	PRINT_DEBUG("mtp_get_storage_index_by_id : 0x%X", storage_id );

	i = 0;
	while(i < MAX_STORAGE_NB)
	{
		if( ctx->storages[i].root_path )
		{
			if( ctx->storages[i].storage_id == storage_id )
			{
				PRINT_DEBUG("mtp_get_storage_index_by_id : %.8X -> %d",
					    storage_id,
					    i );
				return i;
			}
		}
		i++;
	}

	return -1;
}

char * mtp_get_storage_root(mtp_ctx * ctx, uint32_t storage_id)
{
	int i;

	PRINT_DEBUG("mtp_get_storage_root : %.8X", storage_id );

	i = 0;
	while(i < MAX_STORAGE_NB)
	{
		if( ctx->storages[i].root_path )
		{
			if( ctx->storages[i].storage_id == storage_id )
			{
				PRINT_DEBUG("mtp_get_storage_root : %.8X -> %s",
					    storage_id,
					    ctx->storages[i].root_path );
				return ctx->storages[i].root_path;
			}
		}
		i++;
	}

	return NULL;
}

char * mtp_get_storage_description(mtp_ctx * ctx, uint32_t storage_id)
{
	int i;

	PRINT_DEBUG("mtp_get_storage_description : %.8X", storage_id );

	i = 0;
	while(i < MAX_STORAGE_NB)
	{
		if( ctx->storages[i].root_path )
		{
			if( ctx->storages[i].storage_id == storage_id )
			{
				PRINT_DEBUG("mtp_get_storage_description : %.8X -> %s",
					    storage_id,
					    ctx->storages[i].description );
				return ctx->storages[i].description;
			}
		}
		i++;
	}

	return NULL;
}

uint32_t mtp_get_storage_flags(mtp_ctx * ctx, uint32_t storage_id)
{
	int i;

	PRINT_DEBUG("mtp_get_storage_flags : %.8X", storage_id );

	i = 0;
	while(i < MAX_STORAGE_NB)
	{
		if( ctx->storages[i].root_path )
		{
			if( ctx->storages[i].storage_id == storage_id )
			{
				PRINT_DEBUG("mtp_get_storage_flags : %.8X -> 0x%.8X",
					    storage_id,
					    ctx->storages[i].flags );
				return ctx->storages[i].flags;
			}
		}
		i++;
	}

	return 0xFFFFFFFF;
}

int mtp_push_event(mtp_ctx * ctx, uint32_t event, int nbparams, uint32_t * parameters )
{
	unsigned char event_buffer[64];
	int size;
	int ret;

	size = build_event_dataset( ctx, event_buffer, sizeof(event_buffer), event , ctx->session_id, 0x00000000, nbparams, parameters);
	if(size < 0)
		return -1;

	PRINT_DEBUG("mtp_push_event : Event packet buffer - %d Bytes :",size);
	PRINT_DEBUG_BUF(event_buffer, size);

	ret = write_usb(ctx->usb_ctx,EP_DESCRIPTOR_INT_IN,event_buffer,size);

	PRINT_DEBUG("write_usb return: %d", ret );

	return ret;
}
