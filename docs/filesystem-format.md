# TinyOS persistent filesystem format

This document describes the current on-disk filesystem format used by TinyOS. The implementation is in `kernel.c`; the ATA/IDE sector interface and disk geometry are in `disk.c` and `disk.h`.

## Disk geometry and ownership

The configured QEMU disk is a 16 MiB raw image with 32,768 sectors of 512 bytes each. TinyOS reserves LBAs 0 through 4095 for its filesystem metadata and data. LBA 4096 is reserved for the `disktest` command and is not part of the filesystem. Sectors after LBA 4096 are left unused by the current filesystem.

| LBA range | Size | Purpose in format version 4 |
| --- | ---: | --- |
| 0 | 1 sector | Superblock: `TINYFS01` magic and version byte |
| 1 | 1 sector | Directory table: 16 records, 32 bytes each |
| 2-129 | 128 sectors | Legacy fixed-slot file data used by versions 1-3; unused by version 4 |
| 130-145 | 16 sectors | Version 4 allocation table (FAT), 4,096 little-endian 16-bit entries |
| 146-4095 | 3,950 sectors | Version 4 file-data clusters, one sector per cluster |
| 4096 | 1 sector | Reserved for the disk read/write test; never allocated to files |
| 4097-32767 | 28,671 sectors | Unused by the current TinyOS filesystem |

The first-format path only formats a blank filesystem region. If the superblock is not blank and has no recognized magic, or if an owned sector contains unexpected nonzero data, TinyOS reports a format error instead of automatically overwriting the disk.

## Superblock and versioning

The superblock is the 512-byte sector at LBA 0. Bytes 0-7 contain the ASCII magic `TINYFS01`; byte 8 contains the one-byte format version. A newly formatted disk uses version 4. Bytes 9-11 are checked as zero when loading; freshly formatted superblocks zero the rest of the sector as well.

TinyOS recognizes versions 1, 2, 3, and 4. Versions 1-3 use fixed data slots and are migrated in place to version 4 during initialization. Before the migration uses the version 4 FAT and data area, it verifies that LBAs 130-4095 are blank. It writes migrated data, the FAT, and the directory before updating the superblock version to 4. Unknown versions fail with a format error; corrupt metadata fails with a corruption error.

| Version | Maximum file text length | Entry behavior |
| --- | ---: | --- |
| 1 | 256 bytes | Files only; entries load in the root directory |
| 2 | 256 bytes | File and directory type and parent fields are available |
| 3 | 4,096 bytes | File and directory entries with fixed-slot data |
| 4 | 32,768 bytes | File and directory entries with dynamically allocated clusters |

The current kernel constant is `FILESYSTEM_VERSION` in `kernel.c`. Increase the version when an incompatible on-disk layout or interpretation changes, and add an explicit reader/migration path when old disks should remain usable. Do not silently reinterpret an existing version.

At startup, invalid metadata or file chains keep the file store unavailable. Version 4 orphaned allocations are preserved instead of being silently discarded; `fsck` can inspect the disk, and `fsck repair` previews cleanup before the user confirms it with `fsck repair confirm`. Before a normal file command runs, TinyOS audits the directory, FAT, chains, and file data and refuses the operation if the on-disk state differs from the checked in-memory state. A disk I/O error during a metadata transaction disables file commands for the rest of that boot when the on-disk result may be uncertain.

## Directory table

LBA 1 contains 16 records, each 32 bytes. Integer fields are little-endian. A record has this layout:

| Byte offset | Width | Meaning |
| ---: | ---: | --- |
| 0-15 | 16 bytes | NUL-terminated name, maximum 15 characters |
| 16-17 | 2 bytes | File-content length in bytes |
| 18 | 1 byte | Used flag: 0 unused, 1 used |
| 19 | 1 byte | Entry type: 0 file, 1 directory |
| 20 | 1 byte | Parent entry index; `0xFF` means the root directory |
| 21-22 | 2 bytes | First data-cluster index; `0xFFFF` for no clusters |
| 23-24 | 2 bytes | Number of data clusters |
| 25-31 | 7 bytes | Reserved; written as zero |

Versions 1-3 do not use all version 4 fields. The loader checks names, entry types, parent references, lengths, and version-4 cluster metadata before marking the store ready. The filesystem holds at most 16 files and directories combined. File contents are NUL-terminated text, so embedded zero bytes are not supported.

## Allocation table and data clusters

The FAT occupies 16 sectors (8,192 bytes) at LBAs 130-145. Entries are unsigned 16-bit little-endian values. The table has 4,096 entries, while the data region contains 3,950 valid clusters numbered 0 through 3,949. Entries beyond that valid range must remain free.

| FAT value | Meaning |
| ---: | --- |
| `0xFFFF` | Free cluster |
| `0xFFFE` | End of a file's cluster chain |
| 0-3949 | Index of the next cluster in the chain |

Cluster `n` maps to disk LBA `146 + n`. Each cluster is one 512-byte sector. A nonempty file needs `ceil(length / 512)` clusters; unused bytes at the end of its final sector are zero-filled. Empty files and directories have no clusters. TinyOS limits each file to 32,768 text bytes (64 sectors), even though the disk has more free clusters.

With 16 directory entries and a 64-cluster per-file limit, normal files can currently use at most 1,024 data clusters combined. The allocator rejects a request that cannot fit in the available free clusters without committing the new directory entry. `fsbounds` exercises the no-free-cluster path against a temporary in-memory FAT image, plus invalid filesystem cluster requests and out-of-range disk reads and writes; it does not modify the disk.

## Legacy fixed-slot data

Versions 1-3 store a file's first 512-byte chunk at LBA `2 + entry_index`. Remaining chunks use LBAs `18 + entry_index * 7 + (chunk_index - 1)`, for up to eight sectors per file. Version 4 does not read or write these fixed slots after migration; it follows each file's cluster chain instead.

## VM inspection

At the TinyOS prompt, `fsinfo` prints the active format version and the principal sector ranges. `fscheck <file>` checks a file's metadata, cluster chain, and contents. `fsck` audits the directory, allocation table, chains, and file data, including when startup has disabled normal file commands. Keep the existing VDI attached when rebooting to verify persistence; use a disposable disk image when experimenting with format migration or corruption recovery.
