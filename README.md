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
- CPU exception handlers with a readable panic screen; page faults report the address, access type, privilege level, and fault reason
- 32-bit paging: identity-maps the first 32 MiB, with a reusable page-table pool for temporary virtual mappings beyond that range
- Shell commands `paging` and `vminfo` report paging state, dynamic mappings, and page-table pool use
- Shell command `vmtest` maps a physical page at a separate virtual address, verifies reads and writes through both aliases, then unmaps and frees it
- Shell command `pfdemo` safely demonstrates how page-fault error codes are decoded without causing a fault
- A bounded 64 KiB kernel heap with 16-byte-aligned `kmalloc`/`kfree`, block splitting/coalescing, and invalid-free checks
- Shell commands `heapinfo`, `heapcheck`, and `heapdemo` to inspect and exercise the kernel heap; debug builds track live allocations and count rejected invalid/double frees
- Task groundwork: task states, IDs, saved CPU contexts, per-task 4 KiB stacks, cooperative yields, timed blocking/wakeup, and a round-robin scheduler demo
- Shell commands `taskinfo`, `taskdemo`, `sleepdemo`, and `preemptdemo`; the preemption demo lets the PIT timer switch between workers that do not yield
- Stage 3 privilege groundwork: kernel/user GDT descriptors, a 32-bit TSS, and a controlled `ring3demo` transition that returns through the TSS kernel stack
- Stage 3 system-call groundwork: a user-callable `int 0x80` gate with a controlled `syscalldemo` for `write`, `yield`, and `exit`
- Stage 3 memory protection: temporary user code and stack pages have explicit user permissions, user code is read-only, and kernel identity mappings remain supervisor-only
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

A QEMU window should open with the `tinyos>` prompt. Try `help`, `ring3demo`, `syscalldemo`, `usermemdemo`, `about`, `taskinfo`, `taskdemo`, `sleepdemo`, `preemptdemo`, `meminfo`, `mmap`, `pages`, `paging`, `vminfo`, `vmtest`, `heapinfo`, `heapcheck`, `heapdemo`, `alloc`, `echo hello`, `uptime`, `diskinfo`, `disktest`, `df`, `fscheck <file>`, `fsck`, and `clear`. `taskinfo` shows the current task, blocked-task count, cooperative switches, and timer preemptions. `taskdemo` starts two workers that alternate by explicitly yielding. `sleepdemo` blocks two workers for different numbers of PIT ticks; IRQ0 records elapsed ticks, and task context checks deadlines and marks due workers ready while the shell task idles between scheduling turns. `preemptdemo` starts two workers that never yield; the PIT timer switches among them and the shell task, then returns to the prompt. Timer-driven switching is limited to this demonstration so the rest of the kernel is not preempted while its shared services are still single-task-only. `heapinfo` reports the heap's allocated/free payload and largest free block. `heapcheck` verifies that heap metadata and the active-allocation count agree. `heapdemo` checks alignment, data integrity, splitting/coalescing, and rejection of invalid and repeated frees. The normal build rejects bad frees; a debug build additionally tracks live allocations and records each invalid or repeated free. Run `make debug-run` in Ubuntu to build and start that configuration. The heap is a bounded 64 KiB arena; allocation returns a 16-byte-aligned pointer or null on failure. `alloc` reserves one 4 KiB physical page and prints its hexadecimal address. Use `free` to return the most recently allocated page, or `free <address>` to return a specific page allocated during this boot. The page allocator tracks up to 256 active pages, all within the first 32 MiB that paging maps. `uptime` shows how many seconds have elapsed since the kernel started, driven by the PIT timer on IRQ0. `paging` reports the identity-mapped limit and dynamic mapping support. `vminfo` reports mapped pages and the bounded page-table pool. `vmtest` creates a temporary mapping outside the identity-mapped region, checks data through both virtual addresses, then removes the mapping and frees the page. `usermemdemo` maps a copied, read-only user code page and a separate writable user stack while leaving kernel identity mappings supervisor-only. It then attempts a ring-3 write to VGA memory; the expected page fault is contained and reported, and the temporary mappings are removed. `df` reports total, used, and free filesystem data clusters. `fscheck <file>` prints that file's cluster chain and checks its directory entry, allocation map, data sectors, and allocation ownership for corruption. `fsck` checks every directory entry, file chain, and saved data sector, and reports any unreferenced allocated clusters.

To verify virtual memory, run `paging`, `vminfo`, `vmtest`, then `vminfo` again. `vmtest` should pass; the final report should return to zero dynamic pages and zero dynamic page tables. The test maps one allocated physical page at virtual address 0x40000000, checks writes in both directions through the identity and dynamic addresses, then unmaps and releases it.

To check page-fault decoding without halting TinyOS, run `pfdemo`. It prints two simulated reports: a supervisor read from an unmapped address, and a user instruction fetch rejected by a protection rule. A real page fault uses the same report function and includes the CPU-reported fault address from CR2. The demo does not intentionally trigger a fault.

The keyboard uses a US QWERTY scan-code map. Either Shift key changes letters to uppercase and number-row/punctuation keys to their shifted symbols. Caps Lock toggles letter case; holding Shift while Caps Lock is on types lowercase letters. Caps Lock's keyboard LED is not controlled yet. At the TinyOS prompt, check it with `echo Hello`, `echo !@#`, then toggle Caps Lock and type `echo HELLO`; toggle it off afterward.

File commands: `ls` lists entries in the current directory (`/` marks directories), `mkdir <name>` creates a directory, `cd <name>` enters one, `cd ..` goes up, `cd /` returns to the root, and `pwd` prints the current path. `rmdir <name>` removes an empty directory. `touch <name>` creates an empty file, `write <name> <text>` creates or replaces a file (spaces are allowed in the text), `cat <name>` prints it, and `rm <name>` deletes it. `rename <old> <new>` changes a file or directory's name in the current directory; it refuses a name that already exists there and preserves the file's data clusters. `cp <source> <destination>` copies a file to a new name in the current directory, allocating independent data clusters and refusing to overwrite an existing entry. `df` shows free space. Directories and files share a limit of 16 total entries. Names can contain up to 15 characters; file contents can contain up to 32768 characters (64 disk sectors). The shell accepts long command lines so `write` can store content larger than one sector. The current directory starts at `/` after each restart. Metadata and contents are saved on the QEMU virtual disk.

Stage 3, Milestone 1 test: run `ring3demo`. It should report `Ring 3 entry and TSS kernel-stack return passed.` and return to the shell. Then run `uptime` and `help` to confirm shell operation. The user code runs from a temporary user page; this remains a controlled demonstration, not general user-program support. Stage 3, Milestone 1 is complete and verified in QEMU.

Stage 3, Milestone 2 test: run `syscalldemo`. The ring-3 stub should print `Ring 3 says hello through write syscall.` using syscall 1, invoke syscall 3 (`yield`), then invoke syscall 2 (`exit`) and return through the TSS kernel stack. The shell should print `System-call demo passed: write, yield, and exit returned safely.` Then run `ring3demo`, `uptime`, and `help` to confirm the earlier privilege transition and shell remain responsive. This is a controlled demonstration: `write` accepts only its fixed message buffer, and `yield` uses the existing scheduler (which has no other ready task during this command). Stage 3, Milestone 2 is complete and verified in QEMU.

Stage 3, Milestone 3 test: run `usermemdemo`. It should report user code as `user/read-only`, the user stack as `user/read-write`, and the kernel VGA page as `supervisor-only`. The deliberate ring-3 write to VGA memory should trigger a contained page fault and print `Protection demo passed: user write to the kernel VGA page was blocked.` Then run `syscalldemo`, `ring3demo`, `uptime`, and `help` to confirm the earlier commands still work. The code and stack mappings are temporary and are removed when each demo returns. General ELF loading and validation of arbitrary syscall pointers are later work. Stage 3, Milestone 3 is complete and verified in VirtualBox.

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

The ISO is written to `build/tinyos.iso`. To build into a separate folder for a milestone variant, pass `BUILD_DIR`; for example, the Stage 3 syscall milestone can be built and run with:

```sh
make clean BUILD_DIR=build-syscall
make iso BUILD_DIR=build-syscall
make run BUILD_DIR=build-syscall
```

This creates `build-syscall/tinyos.iso`. Attach that file to the VM's optical drive and boot from Optical before Hard Disk. For the user-page protection milestone, use a separate `BUILD_DIR=build-usermem` in the same commands; this creates `build-usermem/tinyos.iso` and preserves the prior ISO. Generated ISO files are not committed to GitHub.

## Roadmap

For Stage 2, Milestone 1, run `taskinfo` at the TinyOS prompt. It should report one running bootstrap task (ID 0), the saved inspection snapshot, and four task slots. For Stage 2, Milestone 2, run `taskdemo`: the output should alternate `Task A: step 1`, `Task B: step 1`, through step 3, then return to the shell. For Stage 2, Milestone 3, run `preemptdemo`: the output should show timer task A and B slices even though neither worker calls `yield`, then return to the shell. Run `taskinfo` afterward; the task count should return to one and the timer preemption count should have increased. For Stage 2, Milestone 4, run `sleepdemo`: A and B should print work messages at different intervals as PIT ticks pass; task context should make due workers ready, each worker should return and be marked exited, and the shell should reclaim its task slot only after switching off its stack. Run `taskinfo`; it should show one registered shell task and zero blocked tasks. Repeat `sleepdemo`, `taskdemo`, and `preemptdemo` to confirm the cleaned slots can be reused. IRQ handlers only collect input/time, update small state, acknowledge the PIC, and perform the already-bounded preemption switch; deadline scans, console output, disk operations, and task-slot cleanup stay outside the handlers. Timer-driven task switching is currently enabled only while the bounded preemption demonstration runs.

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
20. (Stage 2, Milestone 1 — Complete) Add task records, states, saved context layout, bootstrap task, and reserved per-task stacks.
21. (Stage 2, Milestone 2 — Complete; verified in QEMU) Add cooperative context switching and a two-worker `taskdemo`.
20. Track heap allocations and diagnose invalid or repeated frees in debug builds. (Complete; verified in QEMU)
21. Map and unmap pages in a separate virtual-address range using a reusable page-table pool. (Complete; verified in QEMU)
22. Decode page-fault error codes and report access, privilege, and fault details. (Complete; verified in QEMU)
23. (Stage 2, Milestone 1) Define task states, saved-context layout, task IDs, and kernel-stack slots. (Complete; verified in QEMU)
24. (Stage 2, Milestone 3 — Complete; verified in QEMU) Save and restore task interrupt frames so PIT IRQ0 can preempt workers in `preemptdemo` without explicit yields.
25. (Stage 2, Milestone 4 — Complete; verified in QEMU) Add sleep, exit and cleanup; keep interrupt handlers short and safe. `sleepdemo` exercises PIT-based blocking and wakeups, workers exit when their entry functions return, and the shell reclaims exited task metadata after switching off the worker stack. IRQ0 records that a tick passed; task context checks sleep deadlines.
26. (Stage 3, Milestone 1 — Complete; verified in QEMU) Add kernel/user privilege descriptors, a TSS kernel-stack pointer, and a controlled ring-3 entry/return demo.
27. (Stage 3, Milestone 2 — Complete; verified in QEMU) Add a user-callable system-call entry and the initial `write`, `yield`, and `exit` services.
28. (Stage 3, Milestone 3 — Complete; verified in VirtualBox) Map dedicated user code/stack pages with appropriate permissions and contain a user-mode write attempt against a supervisor-only kernel page.

## License

TinyOS is released under the [MIT License](LICENSE).
