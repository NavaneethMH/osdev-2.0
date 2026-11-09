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
- Persistent TinyOS file store on the QEMU disk: up to 16 files and directories combined, names up to 15 characters, file contents up to 4096 characters across multiple sectors
- Basic ATA/IDE primary-master PIO driver with bounded LBA-28 sector reads and writes
- QEMU data disk at `disk/tinyos.img` (created only if missing; ignored by Git and preserved by `make clean`)
- Shell commands `diskinfo` and `disktest` for checking the virtual disk and verifying sector read/write

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

A QEMU window should open with the `tinyos>` prompt. Try `help`, `about`, `meminfo`, `mmap`, `pages`, `paging`, `alloc`, `echo hello`, `uptime`, `diskinfo`, `disktest`, and `clear`. `alloc` reserves one 4 KiB page and prints its hexadecimal address. Use `free` to return the most recently allocated page, or `free <address>` to return a specific page allocated during this boot. The allocator tracks up to 256 active pages, all within the first 32 MiB that paging maps. `uptime` shows how many seconds have elapsed since the kernel started, driven by the PIT timer on IRQ0. `paging` reports whether paging is enabled and the mapped limit.

The keyboard uses a US QWERTY scan-code map. Either Shift key changes letters to uppercase and number-row/punctuation keys to their shifted symbols. Caps Lock toggles letter case; holding Shift while Caps Lock is on types lowercase letters. Caps Lock's keyboard LED is not controlled yet. At the TinyOS prompt, check it with `echo Hello`, `echo !@#`, then toggle Caps Lock and type `echo HELLO`; toggle it off afterward.

File commands: `ls` lists entries in the current directory (`/` marks directories), `mkdir <name>` creates a directory, `cd <name>` enters one, `cd ..` goes up, `cd /` returns to the root, and `pwd` prints the current path. `rmdir <name>` removes an empty directory. `touch <name>` creates an empty file, `write <name> <text>` creates or replaces a file (spaces are allowed in the text), `cat <name>` prints it, and `rm <name>` deletes it. Directories and files share a limit of 16 total entries. Names can contain up to 15 characters; file contents can contain up to 4096 characters (8 disk sectors). The shell accepts long command lines so `write` can store content larger than one sector. The current directory starts at `/` after each restart. Metadata and contents are saved on the QEMU virtual disk.

Example directory check in the TinyOS prompt: run `mkdir docs`, `cd docs`, `pwd`, `write note saved`, and `cat note`. Then run `cd ..` and `ls`; it should show `docs/`. After restarting TinyOS, run `cd docs` and `cat note` to confirm the directory and file persisted. `rmdir docs` should refuse while `note` exists; remove it with `cd docs`, `rm note`, `cd ..`, then `rmdir docs`.

To check multi-sector files, paste a text value longer than 512 characters after `write long `, then run `cat long`. Restart TinyOS and run `cat long` again; the contents should still match. The maximum stored content is 4096 characters.

The ATA driver talks only to QEMU's emulated primary IDE master. `diskinfo` reports whether the virtual disk is ready and its sector count. `disktest` writes and verifies a pattern in reserved sector 4096 of the TinyOS disk image. The image is 16 MiB and is created automatically the first time `make run` is used. It is stored at `disk/tinyos.img`, outside `build/`, so `make clean` keeps it. The test command overwrites that reserved sector each time; do not use this image for other data.

The TinyOS filesystem uses sector 0 for a signature and version, sector 1 for the 16-entry directory, and sectors 2–17 for the first sector of each entry's file data. Additional file sectors occupy a separate fixed extension area at sectors 18–129, with seven extension sectors reserved for each file slot. Each directory entry stores a name, content length, entry type, and parent slot; the root directory uses a reserved parent value. Existing version 1 and 2 disks are upgraded automatically while preserving their files; their first data sectors stay at the same locations. Sector 4096 remains reserved for `disktest`. On first boot, TinyOS initializes the filesystem only if sectors 0–129 are all blank; an unknown signature or non-blank unformatted filesystem area is reported without formatting it. To reset the virtual disk, shut down QEMU, remove `disk/tinyos.img`, and run `make run` to create a fresh blank image. Resetting deletes all saved files.

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
12. Store persistent files across multiple disk sectors, up to 4096 characters. (Complete)
13. Handle Shift and Caps Lock in the IRQ1 keyboard input path. (Complete)
14. (Future) Add a free-space map and dynamically allocated file extents.

## License

TinyOS is released under the [MIT License](LICENSE).

