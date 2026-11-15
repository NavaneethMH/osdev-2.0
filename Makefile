BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/isodir
KERNEL := $(BUILD_DIR)/kernel.elf
ISO := $(BUILD_DIR)/tinyos.iso
DISK_IMAGE := disk/tinyos.img
RECOVERY_DISK_IMAGE := disk/tinyos-recovery.img
DISK_SIZE_MB := 16

CC := gcc
AS := nasm
LD := ld
CFLAGS := -m32 -std=c11 -ffreestanding -O2 -Wall -Wextra -Werror \
          -fno-stack-protector -fno-pic -fno-pie \
          -fno-asynchronous-unwind-tables -fno-unwind-tables

ifeq ($(DEBUG),1)
CFLAGS += -DHEAP_DEBUG
endif

.PHONY: all iso run serial-run debug debug-run recovery-run clean disk-image

all: iso

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: boot.asm | $(BUILD_DIR)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/interrupts.o: interrupts.asm | $(BUILD_DIR)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/kernel.o: kernel.c block.h disk.h heap.h gdt.h elf.h pci.h display.h keyboard.h timer.h pic.h serial.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/block.o: block.c block.h disk.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/display.o: display.c display.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/keyboard.o: keyboard.c keyboard.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/timer.o: timer.c timer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pic.o: pic.c pic.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/serial.o: serial.c serial.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/elf.o: elf.c elf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pci.o: pci.c pci.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hello_app.o: apps/hello.asm | $(BUILD_DIR)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/hello.elf: $(BUILD_DIR)/hello_app.o apps/user.ld
	$(LD) -m elf_i386 -T apps/user.ld -nostdlib -o $@ $(BUILD_DIR)/hello_app.o

$(BUILD_DIR)/filedemo_app.o: apps/filedemo.asm | $(BUILD_DIR)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/filedemo.elf: $(BUILD_DIR)/filedemo_app.o apps/user.ld
	$(LD) -m elf_i386 -T apps/user.ld -nostdlib -o $@ $(BUILD_DIR)/filedemo_app.o

$(BUILD_DIR)/disk.o: disk.c disk.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/heap.o: heap.c heap.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt.o: gdt.c gdt.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(BUILD_DIR)/boot.o $(BUILD_DIR)/interrupts.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/block.o $(BUILD_DIR)/disk.o $(BUILD_DIR)/display.o $(BUILD_DIR)/keyboard.o $(BUILD_DIR)/timer.o $(BUILD_DIR)/pic.o $(BUILD_DIR)/serial.o $(BUILD_DIR)/heap.o $(BUILD_DIR)/gdt.o $(BUILD_DIR)/elf.o $(BUILD_DIR)/pci.o linker.ld
	$(LD) -m elf_i386 -T linker.ld -nostdlib -o $@ $(BUILD_DIR)/boot.o $(BUILD_DIR)/interrupts.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/block.o $(BUILD_DIR)/disk.o $(BUILD_DIR)/display.o $(BUILD_DIR)/keyboard.o $(BUILD_DIR)/timer.o $(BUILD_DIR)/pic.o $(BUILD_DIR)/serial.o $(BUILD_DIR)/heap.o $(BUILD_DIR)/gdt.o $(BUILD_DIR)/elf.o $(BUILD_DIR)/pci.o
	grub-file --is-x86-multiboot $@

iso: $(KERNEL) $(BUILD_DIR)/hello.elf $(BUILD_DIR)/filedemo.elf grub.cfg
	set -eu; \
	staging=$$(mktemp -d /tmp/tinyos-isodir.XXXXXX); \
	image=$$(mktemp /tmp/tinyos-image.XXXXXX.iso); \
	trap 'rm -rf "$$staging"; rm -f "$$image"' EXIT; \
	mkdir -p "$$staging/boot/grub" "$$staging/boot/apps" "$(ISO_ROOT)/boot/grub" "$(ISO_ROOT)/boot/apps"; \
	cp "$(KERNEL)" "$$staging/boot/kernel.elf"; \
	cp "$(BUILD_DIR)/hello.elf" "$$staging/boot/apps/hello.elf"; \
	cp "$(BUILD_DIR)/filedemo.elf" "$$staging/boot/apps/filedemo.elf"; \
	cp grub.cfg "$$staging/boot/grub/grub.cfg"; \
	grub-mkrescue -o "$$image" "$$staging"; \
	xorriso -osirrox on -indev "$$image" -extract /boot/kernel.elf "$$staging/kernel.checked"; \
	cmp "$(KERNEL)" "$$staging/kernel.checked"; \
	xorriso -osirrox on -indev "$$image" -extract /boot/grub/grub.cfg "$$staging/grub.checked"; \
	cmp grub.cfg "$$staging/grub.checked"; \
	xorriso -osirrox on -indev "$$image" -extract /boot/apps/hello.elf "$$staging/hello.checked"; \
	cmp "$(BUILD_DIR)/hello.elf" "$$staging/hello.checked"; \
	xorriso -osirrox on -indev "$$image" -extract /boot/apps/filedemo.elf "$$staging/filedemo.checked"; \
	cmp "$(BUILD_DIR)/filedemo.elf" "$$staging/filedemo.checked"; \
	cp "$(KERNEL)" "$(ISO_ROOT)/boot/kernel.elf"; \
	cp "$(BUILD_DIR)/hello.elf" "$(ISO_ROOT)/boot/apps/hello.elf"; \
	cp "$(BUILD_DIR)/filedemo.elf" "$(ISO_ROOT)/boot/apps/filedemo.elf"; \
	cp grub.cfg "$(ISO_ROOT)/boot/grub/grub.cfg"; \
	cp "$$image" "$(ISO)"

disk-image:
	mkdir -p disk
	if [ ! -f "$(DISK_IMAGE)" ]; then truncate -s $(DISK_SIZE_MB)M "$(DISK_IMAGE)"; fi

run: iso disk-image
	qemu-system-i386 -boot order=d -cdrom $(ISO) -drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk -m 128M

serial-run: iso disk-image
	qemu-system-i386 -boot order=d -cdrom $(ISO) -drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk -serial stdio -m 128M

debug:
	$(MAKE) clean
	$(MAKE) DEBUG=1 iso

debug-run: debug disk-image
	qemu-system-i386 -boot order=d -cdrom $(ISO) -drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk -m 128M

recovery-run: iso
	mkdir -p disk
	if [ ! -f "$(RECOVERY_DISK_IMAGE)" ]; then truncate -s $(DISK_SIZE_MB)M "$(RECOVERY_DISK_IMAGE)"; fi
	qemu-system-i386 -boot order=d -cdrom $(ISO) -drive file=$(RECOVERY_DISK_IMAGE),format=raw,if=ide,index=0,media=disk -m 128M

clean:
	rm -rf $(BUILD_DIR)

