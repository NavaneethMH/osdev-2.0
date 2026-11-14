#!/usr/bin/env python3
"""Create one recoverable orphan in the disposable TinyOS test disk image."""

from pathlib import Path
import struct
import sys


SECTOR_SIZE = 512
EXPECTED_IMAGE_SIZE = 16 * 1024 * 1024
DISK_IMAGE_NAME = "tinyos-recovery.img"
SUPERBLOCK_LBA = 0
DIRECTORY_LBA = 1
FAT_LBA = 130
FAT_SECTORS = 16
DATA_CLUSTER_COUNT = 3950
DATA_START_LBA = 146
FILE_ENTRY_COUNT = 16
FILE_ENTRY_SIZE = 32
CLUSTER_FREE = 0xFFFF
CLUSTER_END = 0xFFFE


def read_at(image, lba, sectors):
    image.seek(lba * SECTOR_SIZE)
    data = image.read(sectors * SECTOR_SIZE)
    expected = sectors * SECTOR_SIZE
    if len(data) != expected:
        raise ValueError(f"short read at LBA {lba}: expected {expected} bytes")
    return data


def verify_clean_filesystem(image, superblock, directory, fat):
    if superblock[:8] != b"TINYFS01" or superblock[8] != 4:
        raise ValueError("image is not a TinyOS version 4 filesystem")
    if superblock[9:12] != b"\0\0\0":
        raise ValueError("superblock reserved bytes are not zero")

    fat_entries = struct.unpack("<4096H", fat)
    if any(value != CLUSTER_FREE for value in fat_entries[DATA_CLUSTER_COUNT:]):
        raise ValueError("reserved allocation-map entries are not free")

    referenced = set()
    for slot in range(FILE_ENTRY_COUNT):
        start = slot * FILE_ENTRY_SIZE
        entry = directory[start : start + FILE_ENTRY_SIZE]
        used = entry[18]
        if used == 0:
            if any(entry):
                raise ValueError(f"unused directory entry {slot} is not blank")
            continue
        if used != 1:
            raise ValueError(f"directory entry {slot} has an invalid used flag")

        name = entry[:16]
        if b"\0" not in name or any(name[name.index(b"\0") + 1 :]):
            raise ValueError(f"directory entry {slot} has an invalid name field")
        length = struct.unpack_from("<H", entry, 16)[0]
        entry_type = entry[19]
        first_cluster, cluster_count = struct.unpack_from("<HH", entry, 21)

        if entry_type == 1:
            if length != 0 or first_cluster != CLUSTER_FREE or cluster_count != 0:
                raise ValueError(f"directory entry {slot} has invalid directory data")
            continue
        if entry_type != 0:
            raise ValueError(f"directory entry {slot} has an invalid type")

        expected_count = (length + SECTOR_SIZE - 1) // SECTOR_SIZE
        if cluster_count != expected_count:
            raise ValueError(f"file entry {slot} has an invalid cluster count")
        if expected_count == 0:
            if first_cluster != CLUSTER_FREE:
                raise ValueError(f"empty file entry {slot} has a first cluster")
            continue
        if first_cluster >= DATA_CLUSTER_COUNT:
            raise ValueError(f"file entry {slot} starts outside the data area")

        cluster = first_cluster
        remaining = length
        for index in range(cluster_count):
            if cluster >= DATA_CLUSTER_COUNT or cluster in referenced:
                raise ValueError("file chains overlap or leave the data area")
            referenced.add(cluster)
            sector_data = read_at(image, DATA_START_LBA + cluster, 1)
            content_bytes = min(remaining, SECTOR_SIZE)
            if any(byte == 0 for byte in sector_data[:content_bytes]):
                raise ValueError(f"file entry {slot} contains an unexpected NUL byte")
            if any(sector_data[content_bytes:]):
                raise ValueError(f"file entry {slot} has nonzero sector padding")
            remaining -= content_bytes
            next_cluster = fat_entries[cluster]
            if index + 1 == cluster_count:
                if next_cluster != CLUSTER_END:
                    raise ValueError(f"file entry {slot} does not end its chain")
            else:
                if next_cluster in (CLUSTER_FREE, CLUSTER_END) or next_cluster >= DATA_CLUSTER_COUNT:
                    raise ValueError(f"file entry {slot} has a broken cluster chain")
                cluster = next_cluster

    allocated = {
        cluster
        for cluster, value in enumerate(fat_entries[:DATA_CLUSTER_COUNT])
        if value != CLUSTER_FREE
    }
    if allocated != referenced:
        raise ValueError("image already has an orphan or damaged allocation chain")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"Usage: {Path(sys.argv[0]).name} disk/{DISK_IMAGE_NAME}")

    image_path = Path(sys.argv[1])
    if image_path.name != DISK_IMAGE_NAME:
        raise SystemExit(f"Refusing to edit any image not named {DISK_IMAGE_NAME}")
    if not image_path.is_file():
        raise SystemExit(f"Disposable image not found: {image_path}")
    if image_path.stat().st_size != EXPECTED_IMAGE_SIZE:
        raise SystemExit("Expected a 16 MiB TinyOS disposable disk image")

    with image_path.open("r+b") as image:
        superblock = read_at(image, SUPERBLOCK_LBA, 1)
        directory = read_at(image, DIRECTORY_LBA, 1)
        fat = read_at(image, FAT_LBA, FAT_SECTORS)
        verify_clean_filesystem(image, superblock, directory, fat)

        entries = struct.unpack("<4096H", fat)
        orphan = next(
            (index for index, value in enumerate(entries[:DATA_CLUSTER_COUNT])
             if value == CLUSTER_FREE),
            None,
        )
        if orphan is None:
            raise SystemExit("No free clusters are available for the recovery demo")

        image.seek(FAT_LBA * SECTOR_SIZE + orphan * 2)
        image.write(struct.pack("<H", CLUSTER_END))
        image.flush()
        import os

        os.fsync(image.fileno())

    print(f"Marked cluster {orphan} as one synthetic orphan in {image_path}.")
    print("Boot this disposable image and use fsck before confirming fsck repair.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(f"Recovery test setup refused: {error}") from error
