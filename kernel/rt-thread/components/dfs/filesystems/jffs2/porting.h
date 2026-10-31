#ifndef _PORTING_H 
#define _PORTING_H

#include "jffs2_config.h"
struct jffs2_fs_info
{
	unsigned sector_size; /* a erasing block size*/
	unsigned nr_blocks; /* number of blocks in flash */
	unsigned free_size;
};

struct jffs2_dirent
{
#ifdef CYGPKG_FILEIO_DIRENT_DTYPE

	mode_t      d_type; // Only supported with FATFS, RAMFS, ROMFS,
	// and JFFS2.
	// d_type is not part of POSIX so
	// should be used with caution.
#endif
	char        d_name[JFFS2_NAME_MAX+1];
};

extern cyg_fileops jffs2_fileops;
extern cyg_fileops jffs2_dirops;
extern struct cyg_fstab_entry jffs2_fste;

time_t jffs2_get_timestamp(void);
void jffs2_get_info_from_sb(void * data, struct jffs2_fs_info * info);

#endif
