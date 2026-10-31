# RT-Smart JFFS2

This is the eCos-derived JFFS2 core with RT-Thread DFS/MTD adapters. Its scan
and node-management sources retain July 2005 revision identifiers. It is not
a complete port of a current Linux release.

The mount path was compared against Linux mainline commit
[`d89e1d186ec9`](https://github.com/torvalds/linux/tree/d89e1d186ec9271bf78e62771779e9fc03bf0fc3/fs/jffs2)
on 2026-10-30. The following changes are incorporated:

| Change | Upstream reference | RT-Smart adaptation |
| --- | --- | --- |
| Allocate node references in batches | [9bfeb691e75b](https://github.com/torvalds/linux/commit/9bfeb691e75b21fdaa80ffae719083200b190381) | A mutex-protected cache grows by 128 records. Existing `next_phys` links and individual free calls remain compatible with the eCos core. |
| Reduce the initial empty-block probe to 256 bytes | [41bdc602eca8](https://github.com/torvalds/linux/commit/41bdc602eca8738d6f3c71235744f72d888fe6b4) | Retain a separate 1 KiB refill buffer so a directory node with a 255-byte filename fits. |
| Check every inode header CRC before accepting the node | [53043002ef6c](https://github.com/torvalds/linux/commit/53043002ef6cc0369fd5c5fa0a257f290ba6a3a6) | Reject a corrupt later version even when its inode already has a cache entry. |
| Accept a valid inode-only image with erased blocks | [a68005a36dc3](https://github.com/torvalds/linux/commit/a68005a36dc3a351957821d8820ffbfacb21dd36) | Include `unchecked_size` when deciding whether valid filesystem nodes exist. |

The reference cache reuses freed records during writes/GC. Chunks remain at
their high-water allocation until the last filesystem unmounts, then all
chunks and the cache mutex are released. Mount/unmount use the DFS adapter's
filesystem mutex to protect the shared cache/compressor lifecycle. Cache
creation and growth failures propagate as allocation errors.
Allocated records are not zeroed; callers initialize each field before use.
Once a reference is linked into the physical node list, that list owns its
lifetime. GC write failures retain partial-write references in that list and
free unused retry references exactly once.

The on-flash format is unchanged. Existing compressed images and writable
mounts remain supported. This does not add Linux's compact reference layout,
XATTR support, or eraseblock summaries. Summary support would require both
summary scanning/writing in the filesystem and generation of summary nodes
in factory images; running `sumtool` alone does not accelerate this port.

There are no automated tests. Board mount, read/write, remount and
power-loss checks are needed for each change.
