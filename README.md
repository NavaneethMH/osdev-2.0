# TinyOS — Educational 32-bit x86 Operating System

TinyOS is a small operating system built from scratch as a systems-programming learning project. Written in C and x86 assembly, it boots as a freestanding 32-bit kernel through GRUB Multiboot and runs in QEMU. It includes interrupt-driven keyboard input, paging, a physical page allocator, and a persistent filesystem on a virtual disk. TinyOS is experimental and intended for learning, not everyday use.

## Current features

- GRUB Multiboot boot
- VGA text output with screen clearing and scrolling
- PS/2 Set 1 keyboard input via IRQ1, including left/right Shift, Caps Lock, and shifted US-layout symbols
- Hardware timer using the PIT (Programmable Interval Timer) and IRQ0
- Tiny shell commands for system information, memory, and files, including `mkdir`, `cd`, `pwd`, and `rmdir`
- Copies GRUB's memory map before using it
- A simple physical page allocator for usable memory below 4 GiB, starting after the kernel
- CPU exception handlers with a readable panic screen
- 32-bit paging: identity-maps the first 32 MiB with 4 KiB pages; the page allocator only returns pages within that mapped region
- Shell command `paging` reports whether paging is enabled
- A bounded 64 KiB kernel heap with 16-byte-aligned `kmalloc`/`kfree`, block splitting/coalescing, and invalid-free checks
- Shell commands `heapinfo` and `heapdemo` to inspect and exercise the kernel heap
- Persistent TinyOS file store on the QEMU disk: up to 16 files and directories combined, names up to 15 characters, file contents up to 32768 characters across dynamically allocated sectors
- FAT-style free-space map and linked disk clusters, allowing files to share disk space and reclaim it after deletion or rewrite
- Basic ATA/IDE primary-master PIO driver with bounded LBA-28 sector reads and writes
- QEMU data disk at `disk/tinyos.img` (created only if missing; ignored by Git and preserved by `make clean`)
- Shell commands `diskinfo` and `disktest` for checking the virtual disk and verifying sector read/write
- Shell command `fscheck <file>` to inspect a file's cluster chain and compare its on-disk metadata and contents
- Shell command `fsck` to audit all directory entries, allocation chains, and file data; confirmed recovery only frees unreferenced clusters
- Shell command `rename <old> <new>` to rename a file or directory in the current directory without rewriting file data
- Shell command `cp <source> <destination>` to create an independent persistent copy of a file

## Build tools

In Ubuntu, install the tools with:

```bash
sudo apt update
sudo apt install -y build-essential nasm xorriso qemu-system-x86 grub-pc-bin grub-common
```

## Build and run

From the project folder:

```bash
make run
```

A QEMU window should open with the `tinyos>` prompt. Try `help`, `about`, `meminfo`, `mmap`, `pages`, `paging`, `heapinfo`, `heapdemo`, `alloc`, `echo hello`, `uptime`, `diskinfo`, `disktest`, `df`, `fscheck <file>`, `fsck`, and `clear`. `heapinfo` reports the heap's allocated/free payload and largest free block. `heapdemo` performs aligned allocations, checks that data remains intact, frees blocks in a mixed order, and checks that coalesced space can be reused. The heap is a bounded 64 KiB arena; allocation returns a 16-byte-aligned pointer or null on failure, and `kfree` rejects invalid or repeated frees. `alloc` reserves one 4 KiB physical page and prints its hexadecimal address. Use `free` to return the most recently allocated page, or `free <address>` to return a specific page allocated during this boot. The page allocator tracks up to 256 active pages, all within the first 32 MiB that paging maps. `uptime` shows how many seconds have elapsed since the kernel started, driven by the PIT timer on IRQ0. `paging` reports whether paging is enabled and the mapped limit. `df` reports total, used, and free filesystem data clusters. `fscheck <file>` prints that file's cluster chain and checks its directory entry, allocation map, data sectors, and allocation ownership for corruption. `fsck` checks every directory entry, file chain, and saved data sector, and reports any unreferenced allocated clusters.

The keyboard uses a US QWERTY scan-code map. Either Shift key changes letters to uppercase and number-row/punctuation keys to their shifted symbols. Caps Lock toggles letter case; holding Shift while Caps Lock is on types lowercase letters. Caps Lock's keyboard LED is not controlled yet. At the TinyOS prompt, check it with `echo Hello`, `echo !@#`, then toggle Caps Lock and type `echo HELLO`; toggle it off afterward.

File commands: `ls` lists entries in the current directory (`/` marks directories), `mkdir <name>` creates a directory, `cd <name>` enters one, `cd ..` goes up, `cd /` returns to the root, and `pwd` prints the current path. `rmdir <name>` removes an empty directory. `touch <name>` creates an empty file, `write <name> <text>` creates or replaces a file (spaces are allowed in the text), `cat <name>` prints it, and `rm <name>` deletes it. `rename <old> <new>` changes a file or directory's name in the current directory; it refuses a name that already exists there and preserves the file's data clusters. `cp <source> <destination>` copies a file to a new name in the current directory, allocating independent data clusters and refusing to overwrite an existing entry. `df` shows free space. Directories and files share a limit of 16 total entries. Names can contain up to 15 characters; file contents can contain up to 32768 characters (64 disk sectors). The shell accepts long command lines so `write` can store content larger than one sector. The current directory starts at `/` after each restart. Metadata and contents are saved on the QEMU virtual disk.

To check renaming, run `write draft keep this text`, `df`, `rename draft final`, `cat final`, and `df` again; the content should remain and cluster usage should not change. `cat draft` should report that the old name is missing. Create a directory with `mkdir olddir`, rename it with `rename olddir newdir`, then run `cd newdir` and `pwd`; the path should show `/newdir`. Try `rename final newdir` to confirm an existing destination name is refused. Restart TinyOS and run `cat final` to confirm the new name persists.

To check copying, run `write original copy this text`, `cp original duplicate`, `cat original`, and `cat duplicate`; both should show the same contents. Run `write duplicate changed` and confirm `cat original` still shows the original text. Try `cp original duplicate` again to check that an existing destination is not overwritten. Restart TinyOS and run `cat duplicate` to confirm the copy persisted independently.

Example directory check in the TinyOS prompt: run `mkdir docs`, `cd docs`, `pwd`, `write note saved`, and `cat note`. Then run `cd ..` and `ls`; it should show `docs/`. After restarting TinyOS, run `cd docs` and `cat note` to confirm the directory and file persisted. `rmdir docs` should refuse while `note` exists; remove it with `cd docs`, `rm note`, `cd ..`, then `rmdir docs`.

To check multi-sector files, paste a text value longer than 512 characters after `write long `, then run `cat long`. Restart TinyOS and run `cat long` again; the contents should still match. The maximum stored content is 32768 characters. Rewriting a file allocates its new cluster chain before releasing the old chain, so a failed write leaves the previous file contents intact. Deleting a file releases its clusters for reuse. Empty files use no data clusters.

To check dynamic allocation, run `df`, create a file with `write alpha <text longer than 512 characters>`, then run `df` again; used clusters should increase. Run `rm alpha` and `df`; the clusters should be free again. Repeat with a different file name to confirm freed clusters are reusable. Restart TinyOS and check `df` and `cat` to confirm the saved files and allocation counts remain consistent.

To inspect a file interactively, run `fscheck <name>` from the directory containing it. The command prints its cluster chain, checks on-disk directory and allocation-map metadata against the loaded filesystem state, verifies the file's data sectors against the cached contents, and checks for broken links, loops, shared clusters, and orphaned allocations. It reports the first problem it finds; it does not repair disk data.

To audit the whole filesystem, run `fsck`. It checks the directory table, file sizes and cluster chains, ownership of every allocated cluster, and saved data against the in-memory copy. `fsck repair` performs the same checks and previews recovery only when the sole issue is unreferenced allocated clusters. Review the count; only then run `fsck repair confirm` to mark those clusters free. Repair refuses to run if it finds other errors and never changes file data sectors. Back up `disk/tinyos.img` before experimenting with disk recovery.

The ATA driver talks only to QEMU's emulated primary IDE master. `diskinfo` reports whether the virtual disk is ready and its sector count. `disktest` writes and verifies a pattern in reserved sector 4096 of the TinyOS disk image. The image is 16 MiB and is created automatically the first time `make run` is used. It is stored at `disk/tinyos.img`, outside `build/`, so `make clean` keeps it. The test command overwrites that reserved sector each time; do not use this image for other data.

Filesystem version 4 uses sector 0 for its signature and version, sector 1 for the 16-entry directory, and sectors 130–145 for a 16-sector FAT-style allocation map. The map has 3950 usable clusters, each one 512-byte disk sector, in LBAs 146–4095. A directory entry records each file's first cluster and cluster count; FAT links let a file's sectors be non-contiguous. Files can therefore share the data area instead of owning fixed per-slot sectors. The metadata also detects invalid chains and duplicate cluster ownership. Clusters orphaned by an interrupted create, rewrite, or delete are reclaimed during startup after all live files have been checked.

TinyOS upgrades version 1, 2, and 3 disks by reading their old fixed-sector files, copying them into the new cluster area, and then writing the new directory and format version. It verifies that the new allocation area is blank before migrating, and reports an error rather than overwriting unknown data. A new filesystem is initialized only if LBAs 0–4095 are blank. Sector 4096 remains reserved for `disktest`. To reset the virtual disk, shut down QEMU, remove `disk/tinyos.img`, and run `make run` to create a fresh blank image. Resetting deletes all saved files.

To build the ISO without starting QEMU:

```bash
make iso
```

The ISO is written to `build/tinyos.iso`.

## Roadmap

1. Boot and print a message. (Complete)
2. Read keyboard input and run a small command loop. (Complete)
3. Read and preserve GRUB's memory map. (Complete)
4. Allocate 4 KiB physical pages from usable ranges below 4 GiB. (Complete)
5. Handle CPU exceptions with an IDT and readable panic screen. (Complete)
6. Add a timer interrupt with PIT and IRQ0. (Complete)
7. Replace keyboard polling with IRQ1-driven input. (Complete)
8. Identity-map the first 32 MiB with 4 KiB pages. (Complete)
9. Add a simple in-memory file store. (Complete)
10. Add a persistent filesystem on the virtual disk. (Complete)
11. Add persistent directories and navigation commands. (Complete)
12. Store persistent files across multiple disk sectors. (Complete; later expanded to 32768 characters)
13. Handle Shift and Caps Lock in the IRQ1 keyboard input path. (Complete)
14. Add a FAT-style free-space map and dynamically allocated file clusters. (Complete)
15. Inspect a file's allocated cluster chain and detect disk corruption interactively with `fscheck`. (Complete)
16. (Complete) Add a read-only filesystem-wide integrity report and a confirmation-gated recovery workflow for orphaned clusters.
17. (Complete) Add a persistent `rename` command for files and directories that preserves existing data clusters.
18. (Complete) Add a persistent file-copy command that allocates an independent data chain.
19. Add a bounded, aligned kernel heap with `kmalloc`/`kfree`. (Complete; verified in QEMU)

## License

TinyOS is released under the [MIT License](LICENSE).

