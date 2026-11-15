/* TinyOS: a small freestanding x86 kernel for learning in QEMU. */
#include "block.h"
#include "heap.h"
#include "gdt.h"
#include "elf.h"
#include "pci.h"
#include "display.h"
#include "keyboard.h"
#include "timer.h"
#include "pic.h"
#include "serial.h"
#include "mouse.h"

#define PAGE_SIZE 4096U
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002U
#define MULTIBOOT_INFO_MEMORY 0x00000001U
#define MULTIBOOT_INFO_MODULES 0x00000008U
#define MULTIBOOT_INFO_MEMORY_MAP 0x00000040U
#define MAX_MEMORY_REGIONS 32U
#define MAX_TRACKED_PAGES 256U
#define PAGING_MAPPED_LIMIT 0x02000000U /* First 32 MiB identity-mapped. */
#define VM_DYNAMIC_TABLES 8U
#define VM_TEST_VIRTUAL_ADDRESS 0x40000000U
#define USER_CODE_ADDRESS 0x40000000U
#define USER_STACK_ADDRESS (USER_CODE_ADDRESS + PAGE_SIZE)
#define USER_ADDRESS_SPACE_DIRECTORY (USER_CODE_ADDRESS >> 22)
#define SYSCALL_WRITE_MAX 128U
#define MAX_TASKS 4U
#define TASK_STACK_SIZE 4096U
#define TASK_NAME_MAX 15U
#define MAX_FILES 16U
#define FILENAME_MAX_LEN 15U
#define FILE_CONTENT_MAX 32768U
#define BOOT_APP_MAX_SIZE (FILE_CONTENT_MAX / 2U)
#define COMMAND_MAX (FILE_CONTENT_MAX + FILENAME_MAX_LEN + 9U)
#define FILESYSTEM_SUPER_LBA 0U
#define FILESYSTEM_DIRECTORY_LBA 1U
#define FILESYSTEM_DATA_LBA 2U
#define FILESYSTEM_EXTENSION_LBA (FILESYSTEM_DATA_LBA + MAX_FILES)
#define LEGACY_FILE_SECTORS_PER_SLOT 8U
#define LEGACY_FILE_EXTRA_SECTORS_PER_SLOT (LEGACY_FILE_SECTORS_PER_SLOT - 1U)
#define LEGACY_FILESYSTEM_END_LBA \
    (FILESYSTEM_EXTENSION_LBA + MAX_FILES * LEGACY_FILE_EXTRA_SECTORS_PER_SLOT)
#define FILESYSTEM_FAT_LBA LEGACY_FILESYSTEM_END_LBA
#define FILESYSTEM_FAT_SECTORS 16U
#define FILESYSTEM_FAT_ENTRIES (FILESYSTEM_FAT_SECTORS * DISK_SECTOR_SIZE / 2U)
#define FILESYSTEM_DATA_START_LBA (FILESYSTEM_FAT_LBA + FILESYSTEM_FAT_SECTORS)
#define FILESYSTEM_DATA_END_LBA (DISK_TEST_SECTOR - 1U)
#define FILESYSTEM_CLUSTER_COUNT \
    (FILESYSTEM_DATA_END_LBA - FILESYSTEM_DATA_START_LBA + 1U)
#define FILESYSTEM_CLUSTER_FREE 0xFFFFU
#define FILESYSTEM_CLUSTER_END 0xFFFEU
#define FILESYSTEM_MAX_FILE_SECTORS \
    ((FILE_CONTENT_MAX + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE)
#define FILESYSTEM_ENTRY_SIZE 32U
#define FILESYSTEM_VERSION 4U
#define FILE_PARENT_ROOT 0xFFU
#define ENTRY_TYPE_FILE 0U
#define ENTRY_TYPE_DIRECTORY 1U
#define FILESYSTEM_FORMAT_ERROR (-100)
#define FILESYSTEM_CORRUPT_ERROR (-101)
#define FILESYSTEM_FULL_ERROR (-102)

struct multiboot_info {
    unsigned int flags;
    unsigned int mem_lower;
    unsigned int mem_upper;
    unsigned int boot_device;
    unsigned int cmdline;
    unsigned int mods_count;
    unsigned int mods_addr;
    unsigned int symbols[4];
    unsigned int mmap_length;
    unsigned int mmap_addr;
};

struct multiboot_module {
    unsigned int start;
    unsigned int end;
    unsigned int string;
    unsigned int reserved;
};

struct multiboot_mmap_entry {
    unsigned int size;
    unsigned int base_low;
    unsigned int base_high;
    unsigned int length_low;
    unsigned int length_high;
    unsigned int type;
} __attribute__((packed));

struct memory_region {
    unsigned int base_low;
    unsigned int base_high;
    unsigned int length_low;
    unsigned int length_high;
    unsigned int type;
};

struct page_range {
    unsigned int start;
    unsigned int end;
};

extern unsigned char __kernel_end;

struct idt_entry {
    unsigned short offset_low;
    unsigned short selector;
    unsigned char zero;
    unsigned char flags;
    unsigned short offset_high;
} __attribute__((packed));

struct idt_pointer {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

#define DECLARE_ISR(vector) extern void isr##vector(void)
DECLARE_ISR(0);  DECLARE_ISR(1);  DECLARE_ISR(2);  DECLARE_ISR(3);
DECLARE_ISR(4);  DECLARE_ISR(5);  DECLARE_ISR(6);  DECLARE_ISR(7);
DECLARE_ISR(8);  DECLARE_ISR(9);  DECLARE_ISR(10); DECLARE_ISR(11);
DECLARE_ISR(12); DECLARE_ISR(13); DECLARE_ISR(14); DECLARE_ISR(15);
DECLARE_ISR(16); DECLARE_ISR(17); DECLARE_ISR(18); DECLARE_ISR(19);
DECLARE_ISR(20); DECLARE_ISR(21); DECLARE_ISR(22); DECLARE_ISR(23);
DECLARE_ISR(24); DECLARE_ISR(25); DECLARE_ISR(26); DECLARE_ISR(27);
DECLARE_ISR(28); DECLARE_ISR(29); DECLARE_ISR(30); DECLARE_ISR(31);
#undef DECLARE_ISR

/* IRQ handlers for hardware interrupts 32-47 */
#define DECLARE_IRQ(vector) extern void irq##vector(void)
DECLARE_IRQ(32);  DECLARE_IRQ(33);  DECLARE_IRQ(34);  DECLARE_IRQ(35);
DECLARE_IRQ(36);  DECLARE_IRQ(37);  DECLARE_IRQ(38);  DECLARE_IRQ(39);
DECLARE_IRQ(40);  DECLARE_IRQ(41);  DECLARE_IRQ(42);  DECLARE_IRQ(43);
DECLARE_IRQ(44);  DECLARE_IRQ(45);  DECLARE_IRQ(46);  DECLARE_IRQ(47);
#undef DECLARE_IRQ

extern void idt_load(const struct idt_pointer *pointer);
extern void enter_user_mode(unsigned int entry, unsigned int stack_top);
extern void ring3_demo_user(void);
extern void ring3_demo_return_trampoline(void);
extern void syscall128(void);
extern unsigned char user_blob_start[];
extern unsigned char user_blob_end[];
extern void ring3_syscall_demo_user(void);
extern void ring3_protection_demo_user(void);
extern unsigned char ring3_syscall_message[];
extern unsigned char ring3_syscall_message_end[];

static char command[COMMAND_MAX];
static unsigned int command_length = 0;
static unsigned int memory_lower_kib = 0;
static unsigned int memory_upper_kib = 0;
static int memory_info_available = 0;
static struct memory_region memory_regions[MAX_MEMORY_REGIONS];
static unsigned int memory_region_count = 0;
static int memory_map_available = 0;
static int memory_map_truncated = 0;
static struct page_range page_ranges[MAX_MEMORY_REGIONS];
static unsigned int page_range_count = 0;
static unsigned int current_range = 0;
static unsigned int next_page = 0;
static unsigned int active_page_count = 0;
static unsigned int active_pages[MAX_TRACKED_PAGES];
static unsigned int released_pages[MAX_TRACKED_PAGES];
static unsigned int released_page_count = 0;
static unsigned int kernel_end_address = 0;
static int page_allocator_available = 0;

/* Paging state.
   32 MiB = 8 page tables (each 4 MiB = 1024 * 4 KiB). */
#define PAGE_TABLES_FOR_LIMIT (PAGING_MAPPED_LIMIT / 0x400000U)
static unsigned int page_directory[1024] __attribute__((aligned(4096)));
static unsigned int page_tables[PAGE_TABLES_FOR_LIMIT][1024]
    __attribute__((aligned(4096)));
static unsigned int user_address_space_directory[1024]
    __attribute__((aligned(4096)));
static unsigned int user_address_space_table[1024]
    __attribute__((aligned(4096)));
static unsigned int dynamic_page_tables[VM_DYNAMIC_TABLES][1024]
    __attribute__((aligned(4096)));
static unsigned int dynamic_page_table_directory[VM_DYNAMIC_TABLES];
static int dynamic_page_table_used[VM_DYNAMIC_TABLES];
static unsigned int dynamic_mapped_pages = 0U;
static int paging_enabled = 0;

static volatile int ring3_demo_active = 0;
static volatile int ring3_demo_trap_seen = 0;
static volatile int syscall_demo_active = 0;
static volatile int syscall_demo_write_seen = 0;
static volatile int syscall_demo_yield_seen = 0;
static volatile int syscall_demo_exit_seen = 0;
static volatile int syscall_demo_bad_pointer_seen = 0;
static volatile int syscall_demo_large_buffer_seen = 0;
static volatile int syscall_demo_cross_page_seen = 0;
static volatile int syscall_demo_unknown_number_seen = 0;
static volatile unsigned int syscall_demo_exit_status = 0xFFFFFFFFU;
static volatile int user_protection_fault_seen = 0;
static volatile int user_demo_active = 0;
static volatile int embedded_app_active = 0;
static volatile int embedded_app_exit_seen = 0;
static volatile unsigned int embedded_app_exit_status = 0xFFFFFFFFU;
static unsigned int user_code_physical = 0U;
static unsigned int user_stack_physical = 0U;
static unsigned int address_space_code_physical = 0U;
static unsigned int address_space_stack_physical = 0U;
static unsigned int *active_user_address_space = 0;
static unsigned char boot_app_image[BOOT_APP_MAX_SIZE];
static unsigned int boot_app_image_size = 0U;
static unsigned char boot_filedemo_image[BOOT_APP_MAX_SIZE];
static unsigned int boot_filedemo_image_size = 0U;
static unsigned char app_image_buffer[BOOT_APP_MAX_SIZE];

#define MAX_APP_FILE_HANDLES 4U
#define APP_FILE_READ 1U
#define APP_FILE_WRITE 2U
#define APP_FILE_CREATE 4U
#define APP_FILE_TRUNCATE 8U

struct app_file_handle {
    unsigned int slot;
    unsigned int offset;
    unsigned int flags;
    int open;
};

static struct app_file_handle app_file_handles[MAX_APP_FILE_HANDLES];

/* Stage 2 task model. The bootstrap task continues on GRUB's stack while
   worker tasks use reserved stacks and saved cooperative/interrupt contexts. */
enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_EXITED
};

struct task_context {
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
    unsigned int eip;
    unsigned int eflags;
};

struct task_control_block {
    unsigned int id;
    volatile enum task_state state;
    char name[TASK_NAME_MAX + 1U];
    struct task_context context;
    /* Interrupt frames are used by the timer scheduler, separately from the
       cooperative context above. */
    unsigned int interrupt_esp;
    /* Nonzero only while a task is blocked until a PIT tick deadline. */
    volatile unsigned int wake_tick;
    void (*entry)(void);
    unsigned char kernel_stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
};

extern void task_switch(struct task_context *old_context,
                        const struct task_context *new_context);

static struct task_control_block tasks[MAX_TASKS];
static volatile unsigned int current_task_id = 0U;
static unsigned int task_count = 0U;
static unsigned int task_switch_count = 0U;
static volatile unsigned int timer_preemption_count = 0U;
static int task_model_ready = 0;
static int task_switching_enabled = 0;
static volatile int timer_preemption_enabled = 0;
/* IRQ0 sets this; deadline scans happen later in task context. */
static volatile int task_wakeup_pending = 0;

static unsigned int save_and_disable_interrupts(void);
static void restore_interrupts(unsigned int flags);
static void process_sleep_wakeups(void);

/* File metadata and contents are cached in RAM and backed by the QEMU disk. */
struct file_entry {
    char name[FILENAME_MAX_LEN + 1U]; /* NUL-terminated. */
    char content[FILE_CONTENT_MAX + 1U]; /* NUL-terminated. */
    int used;
    int is_directory;
    unsigned char parent;
    unsigned short first_cluster;
    unsigned short cluster_count;
};

/* Freestanding builds cannot rely on the compiler-provided libc memcpy. */
static void file_entry_copy(struct file_entry *destination,
                            const struct file_entry *source)
{
    unsigned int i;

    for (i = 0U; i < FILENAME_MAX_LEN + 1U; ++i) {
        destination->name[i] = source->name[i];
    }
    for (i = 0U; i < FILE_CONTENT_MAX + 1U; ++i) {
        destination->content[i] = source->content[i];
    }
    destination->used = source->used;
    destination->is_directory = source->is_directory;
    destination->parent = source->parent;
    destination->first_cluster = source->first_cluster;
    destination->cluster_count = source->cluster_count;
}

static struct file_entry files[MAX_FILES];
/* Shared transaction backup avoids large per-command stack allocations. */
static struct file_entry file_entry_backup;
static unsigned short filesystem_fat[FILESYSTEM_FAT_ENTRIES];
static unsigned short filesystem_fat_backup[FILESYSTEM_FAT_ENTRIES];
static unsigned char filesystem_fat_disk[FILESYSTEM_FAT_SECTORS * DISK_SECTOR_SIZE];
static unsigned char filesystem_scan_buffer[DISK_SECTOR_SIZE * 32U];
static char filesystem_write_buffer[FILE_CONTENT_MAX + 1U];
static unsigned char filesystem_cluster_seen[FILESYSTEM_CLUSTER_COUNT];
static int file_store_ready = 0;
static int file_store_error = 0;
static unsigned char current_directory = FILE_PARENT_ROOT;
static void report_file_store_unavailable(void);

static void clear_screen(void)
{
    display_clear();
}

static void put_char(char character)
{
    display_put_char(character);
    serial_write_char(character);
}

static void put_string(const char *text)
{
    for (unsigned int i = 0; text[i] != '\0'; ++i) {
        put_char(text[i]);
    }
}

static void put_unsigned(unsigned int value)
{
    char digits[10];
    unsigned int count = 0;

    if (value == 0U) {
        put_char('0');
        return;
    }

    while (value > 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    while (count > 0U) {
        put_char(digits[--count]);
    }
}

static void put_signed(int value)
{
    if (value < 0) {
        put_char('-');
        put_unsigned((unsigned int)(-value));
    } else {
        put_unsigned((unsigned int)value);
    }
}

static void put_hex32(unsigned int value)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        put_char(hex[(value >> (unsigned int)shift) & 0x0FU]);
    }
}

static void put_hex8(unsigned char value)
{
    static const char hex[] = "0123456789ABCDEF";
    put_char(hex[(value >> 4) & 0x0FU]);
    put_char(hex[value & 0x0FU]);
}

static void put_hex16(unsigned short value)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = 12; shift >= 0; shift -= 4) {
        put_char(hex[(value >> (unsigned int)shift) & 0x0FU]);
    }
}

static void show_pci_devices(void)
{
    static struct pci_device_info devices[PCI_DEVICE_LIST_CAPACITY];
    unsigned int count = pci_discover(devices, PCI_DEVICE_LIST_CAPACITY);
    unsigned int shown = count < PCI_DEVICE_LIST_CAPACITY
        ? count : PCI_DEVICE_LIST_CAPACITY;

    put_string("PCI functions found: ");
    put_unsigned(count);
    put_char('\n');
    if (count == 0U) {
        put_string("No PCI functions were discovered.\n");
        return;
    }

    for (unsigned int i = 0U; i < shown; ++i) {
        put_string("PCI ");
        put_hex8(devices[i].bus);
        put_char(':');
        put_hex8(devices[i].device);
        put_char('.');
        put_unsigned(devices[i].function);
        put_string(" vendor=0x");
        put_hex16(devices[i].vendor_id);
        put_string(" device=0x");
        put_hex16(devices[i].device_id);
        put_string(" class=0x");
        put_hex8(devices[i].class_code);
        put_string(" subclass=0x");
        put_hex8(devices[i].subclass);
        put_string(" prog-if=0x");
        put_hex8(devices[i].programming_interface);
        put_char('\n');
    }
    if (count > shown) {
        put_string("Output limited to the first ");
        put_unsigned(shown);
        put_string(" functions.\n");
    }
}

static void show_mouse_info(void)
{
    struct mouse_state state;
    mouse_get_state(&state);
    put_string("PS/2 mouse: ");
    put_string(state.available ? "ready\n" : "unavailable\n");
    put_string("Packets received: ");
    put_unsigned(state.packets);
    put_string("\nRelative X: ");
    put_signed(state.relative_x);
    put_string("\nRelative Y: ");
    put_signed(state.relative_y);
    put_string("\nButtons: left=");
    put_unsigned((state.buttons & 0x01U) != 0U);
    put_string(" right=");
    put_unsigned((state.buttons & 0x02U) != 0U);
    put_string(" middle=");
    put_unsigned((state.buttons & 0x04U) != 0U);
    put_string("\nButton presses: left=");
    put_unsigned(state.left_presses);
    put_string(" right=");
    put_unsigned(state.right_presses);
    put_string(" middle=");
    put_unsigned(state.middle_presses);
    put_char('\n');
}

static const char *task_state_name(enum task_state state)
{
    switch (state) {
    case TASK_READY: return "READY";
    case TASK_RUNNING: return "RUNNING";
    case TASK_BLOCKED: return "BLOCKED";
    case TASK_EXITED: return "EXITED";
    default: return "UNUSED";
    }
}

static void initialize_task_model(void)
{
    for (unsigned int i = 0U; i < MAX_TASKS; ++i) {
        tasks[i].id = i;
        tasks[i].state = TASK_UNUSED;
        tasks[i].name[0] = '\0';
        tasks[i].entry = 0;
        tasks[i].context.edi = 0U;
        tasks[i].context.esi = 0U;
        tasks[i].context.ebp = 0U;
        tasks[i].context.esp = 0U;
        tasks[i].context.ebx = 0U;
        tasks[i].context.edx = 0U;
        tasks[i].context.ecx = 0U;
        tasks[i].context.eax = 0U;
        tasks[i].context.eip = 0U;
        tasks[i].context.eflags = 0U;
        tasks[i].interrupt_esp = 0U;
        tasks[i].wake_tick = 0U;
    }

    tasks[0].state = TASK_RUNNING;
    tasks[0].name[0] = 'k';
    tasks[0].name[1] = 'e';
    tasks[0].name[2] = 'r';
    tasks[0].name[3] = 'n';
    tasks[0].name[4] = 'e';
    tasks[0].name[5] = 'l';
    tasks[0].name[6] = '\0';

    /* Bootstrap task context is captured when the scheduler is initialized. */
    __asm__ volatile (
        "movl %%esp, %0\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        "call 1f\n\t"
        "1: popl %2\n\t"
        : "=r" (tasks[0].context.esp),
          "=r" (tasks[0].context.eflags),
          "=r" (tasks[0].context.eip)
        :
        : "memory");

    task_count = 1U;
    current_task_id = 0U;
    task_switch_count = 0U;
    task_wakeup_pending = 0;
    task_model_ready = 1;
}

static void show_task_info(void)
{
    if (!task_model_ready) {
        put_string("Task model is not initialized.\n");
        return;
    }

    unsigned int blocked_count = 0U;
    for (unsigned int i = 0U; i < MAX_TASKS; ++i) {
        if (tasks[i].state == TASK_BLOCKED) {
            ++blocked_count;
        }
    }

    put_string("Task model: initialized\nRegistered tasks: ");
    put_unsigned(task_count);
    put_string(" / ");
    put_unsigned(MAX_TASKS);
    put_string("\nCurrent task: ");
    put_unsigned(current_task_id);
    put_string(" (");
    put_string(tasks[current_task_id].name);
    put_string(") ");
    put_string(task_state_name(tasks[current_task_id].state));
    put_string("\nBootstrap context snapshot: ESP=0x");
    put_hex32(tasks[0].context.esp);
    put_string(" EIP=0x");
    put_hex32(tasks[0].context.eip);
    put_string(" EFLAGS=0x");
    put_hex32(tasks[0].context.eflags);
    put_string("\nTask slots: ");
    put_unsigned(MAX_TASKS);
    put_string("; stack size per task: ");
    put_unsigned(TASK_STACK_SIZE);
    put_string(" bytes\nTask switching: ");
    put_string(task_switching_enabled ? "enabled\n" : "not enabled yet.\n");
    put_string("Blocked tasks: ");
    put_unsigned(blocked_count);
    put_char('\n');
    put_string("Cooperative switches completed: ");
    put_unsigned(task_switch_count);
    put_string("\nTimer preemptions completed: ");
    put_unsigned(timer_preemption_count);
    put_char('\n');
}

static void task_yield(void)
{
    process_sleep_wakeups();
    if (!task_switching_enabled || task_count < 2U) {
        return;
    }

    unsigned int previous_id = current_task_id;
    unsigned int next_id = previous_id;
    for (unsigned int offset = 1U; offset <= MAX_TASKS; ++offset) {
        unsigned int candidate = (previous_id + offset) % MAX_TASKS;
        if (tasks[candidate].state == TASK_READY) {
            next_id = candidate;
            break;
        }
    }

    if (next_id == previous_id) {
        if (tasks[previous_id].state == TASK_READY) {
            tasks[previous_id].state = TASK_RUNNING;
        }
        return;
    }

    if (tasks[previous_id].state == TASK_RUNNING) {
        tasks[previous_id].state = TASK_READY;
    }
    tasks[next_id].state = TASK_RUNNING;
    current_task_id = next_id;
    ++task_switch_count;
    task_switch(&tasks[previous_id].context, &tasks[next_id].context);
}

/* Block the current task until a timer deadline. Deadlines are compared
   modulo 32 bits; intervals must stay below half the counter range. */
static void task_sleep_ticks(unsigned int ticks)
{
    if (ticks == 0U || ticks >= 0x80000000U ||
        !task_switching_enabled || task_count < 2U) {
        task_yield();
        return;
    }

    unsigned int flags = save_and_disable_interrupts();
    unsigned int id = current_task_id;
    tasks[id].wake_tick = timer_ticks() + ticks;
    tasks[id].state = TASK_BLOCKED;
    restore_interrupts(flags);
    task_yield();
}

static void task_exit_current(void)
{
    unsigned int flags = save_and_disable_interrupts();
    tasks[current_task_id].state = TASK_EXITED;
    restore_interrupts(flags);
    if (timer_preemption_enabled) {
        /* The IRQ scheduler will return to another task on the next tick. */
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }
    task_yield();
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static void task_bootstrap(void)
{
    unsigned int id = current_task_id;
    if (id < MAX_TASKS && tasks[id].entry != 0) {
        tasks[id].entry();
    }
    task_exit_current();
}

static void initialize_task_stack(unsigned int id, void (*entry)(void),
                                   const char *name)
{
    struct task_control_block *task = &tasks[id];
    unsigned int *stack = (unsigned int *)
        ((unsigned int)&task->kernel_stack[TASK_STACK_SIZE]);

    /* Build the stack frame expected by task_switch's four register pops
       and ret. The extra return address safely catches an unexpected return. */
    *--stack = (unsigned int)task_exit_current;
    *--stack = (unsigned int)task_bootstrap;
    *--stack = 0U; /* EBP */
    *--stack = 0U; /* EBX */
    *--stack = 0U; /* ESI */
    *--stack = 0U; /* EDI */
    task->context.esp = (unsigned int)stack;
    task->context.eip = (unsigned int)task_bootstrap;
    task->context.eflags = 0x202U;
    task->entry = entry;
    for (unsigned int i = 0U; i <= TASK_NAME_MAX; ++i) {
        task->name[i] = name[i];
        if (name[i] == '\0') {
            break;
        }
    }
    task->state = TASK_READY;
}

static void initialize_preempt_stack(unsigned int id, void (*entry)(void),
                                     const char *name)
{
    struct task_control_block *task = &tasks[id];
    unsigned short code_selector;
    unsigned int *stack = (unsigned int *)
        ((unsigned int)&task->kernel_stack[TASK_STACK_SIZE]);

    /* GRUB supplies the active GDT selectors; use the same code selector
       installed in our IDT instead of assuming it is always 0x08. */
    __asm__ volatile ("mov %%cs, %0" : "=r" (code_selector));

    /* irq_common restores this exact pusha + vector/error + iret layout. */
    *--stack = (unsigned int)task_exit_current; /* C return address after iret */
    *--stack = 0x202U;                         /* EFLAGS: interrupts enabled */
    *--stack = (unsigned int)code_selector;    /* Current kernel code segment */
    *--stack = (unsigned int)task_bootstrap;    /* EIP */
    *--stack = 0U;                             /* Error code */
    *--stack = 32U;                            /* IRQ vector */
    *--stack = 0U;                             /* EAX */
    *--stack = 0U;                             /* ECX */
    *--stack = 0U;                             /* EDX */
    *--stack = 0U;                             /* EBX */
    *--stack = 0U;                             /* Original ESP (ignored by popa) */
    *--stack = 0U;                             /* EBP */
    *--stack = 0U;                             /* ESI */
    *--stack = 0U;                             /* EDI */
    task->interrupt_esp = (unsigned int)stack;
    task->entry = entry;
    for (unsigned int i = 0U; i <= TASK_NAME_MAX; ++i) {
        task->name[i] = name[i];
        if (name[i] == '\0') {
            break;
        }
    }
    task->state = TASK_READY;
}

static unsigned int save_and_disable_interrupts(void)
{
    unsigned int flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
    return flags;
}

static void restore_interrupts(unsigned int flags)
{
    __asm__ volatile ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

/* IRQ0 only records that time advanced. This short, bounded scan and the
   task-state changes happen later in ordinary task context. */
static void process_sleep_wakeups(void)
{
    unsigned int flags;
    unsigned int now;

    if (!task_wakeup_pending) {
        return;
    }

    flags = save_and_disable_interrupts();
    if (!task_wakeup_pending) {
        restore_interrupts(flags);
        return;
    }
    task_wakeup_pending = 0;
    now = timer_ticks();
    for (unsigned int i = 0U; i < MAX_TASKS; ++i) {
        if (tasks[i].state == TASK_BLOCKED &&
            (unsigned int)(now - tasks[i].wake_tick) < 0x80000000U) {
            tasks[i].wake_tick = 0U;
            tasks[i].state = TASK_READY;
        }
    }
    restore_interrupts(flags);
}

/* Reclaim only an exited worker after execution has switched away from its
   stack. Keep the stack storage reserved, but clear all reusable metadata. */
static int task_reap(unsigned int id)
{
    unsigned int flags;
    unsigned int *context_words;

    if (id == 0U || id >= MAX_TASKS || id == current_task_id ||
        tasks[id].state != TASK_EXITED) {
        return 0;
    }

    flags = save_and_disable_interrupts();
    if (tasks[id].state != TASK_EXITED) {
        restore_interrupts(flags);
        return 0;
    }

    tasks[id].state = TASK_UNUSED;
    tasks[id].wake_tick = 0U;
    tasks[id].interrupt_esp = 0U;
    tasks[id].entry = 0;
    context_words = (unsigned int *)&tasks[id].context;
    for (unsigned int i = 0U;
         i < sizeof(tasks[id].context) / sizeof(unsigned int); ++i) {
        context_words[i] = 0U;
    }
    for (unsigned int i = 0U; i <= TASK_NAME_MAX; ++i) {
        tasks[id].name[i] = '\0';
    }
    if (task_count > 1U) {
        --task_count;
    }
    restore_interrupts(flags);
    return 1;
}

static void task_preempt_worker_a(void)
{
    for (unsigned int step = 1U; step <= 3U; ++step) {
        unsigned int start = timer_ticks();
        while ((unsigned int)(timer_ticks() - start) < 4U) {
            __asm__ volatile ("pause");
        }
        unsigned int flags = save_and_disable_interrupts();
        put_string("Timer task A: slice ");
        put_unsigned(step);
        put_char('\n');
        restore_interrupts(flags);
    }
}

static void task_preempt_worker_b(void)
{
    for (unsigned int step = 1U; step <= 3U; ++step) {
        unsigned int start = timer_ticks();
        while ((unsigned int)(timer_ticks() - start) < 4U) {
            __asm__ volatile ("pause");
        }
        unsigned int flags = save_and_disable_interrupts();
        put_string("Timer task B: slice ");
        put_unsigned(step);
        put_char('\n');
        restore_interrupts(flags);
    }
}

static void run_preempt_demo(void)
{
    if (!task_model_ready || task_count != 1U ||
        tasks[1].state != TASK_UNUSED || tasks[2].state != TASK_UNUSED ||
        timer_preemption_enabled) {
        put_string("Preemption demo unavailable: tasks are already active.\n");
        return;
    }

    initialize_preempt_stack(1U, task_preempt_worker_a, "timer-a");
    initialize_preempt_stack(2U, task_preempt_worker_b, "timer-b");
    task_count = 3U;
    task_switching_enabled = 1;
    put_string("Timer preemption demo: two workers run without yielding.\n");
    timer_preemption_enabled = 1;
    while (tasks[1].state != TASK_EXITED || tasks[2].state != TASK_EXITED) {
        __asm__ volatile ("pause");
    }

    timer_preemption_enabled = 0;
    (void)task_reap(1U);
    (void)task_reap(2U);
    put_string("Preemption demo complete; timer switched between both workers.\n");
}

static void task_worker_a(void)
{
    for (unsigned int i = 1U; i <= 3U; ++i) {
        put_string("Task A: step ");
        put_unsigned(i);
        put_char('\n');
        task_yield();
    }
}

static void task_worker_b(void)
{
    for (unsigned int i = 1U; i <= 3U; ++i) {
        put_string("Task B: step ");
        put_unsigned(i);
        put_char('\n');
        task_yield();
    }
}

static void task_sleep_worker_a(void)
{
    for (unsigned int i = 1U; i <= 3U; ++i) {
        put_string("Sleep task A: working ");
        put_unsigned(i);
        put_char('\n');
        task_sleep_ticks(5U);
    }
}

static void task_sleep_worker_b(void)
{
    for (unsigned int i = 1U; i <= 3U; ++i) {
        put_string("Sleep task B: working ");
        put_unsigned(i);
        put_char('\n');
        task_sleep_ticks(8U);
    }
}

static void run_sleep_demo(void)
{
    if (!task_model_ready || task_count != 1U ||
        tasks[1].state != TASK_UNUSED || tasks[2].state != TASK_UNUSED) {
        put_string("Sleep demo unavailable: tasks are already active.\n");
        return;
    }

    initialize_task_stack(1U, task_sleep_worker_a, "sleep-a");
    initialize_task_stack(2U, task_sleep_worker_b, "sleep-b");
    task_count = 3U;
    task_switching_enabled = 1;
    put_string("Timed blocking demo: workers sleep for PIT ticks and wake up.\n");

    while (tasks[1].state != TASK_EXITED || tasks[2].state != TASK_EXITED) {
        task_yield();
        /* Let the PIT advance while no worker is runnable. */
        __asm__ volatile ("hlt");
    }

    (void)task_reap(1U);
    (void)task_reap(2U);
    put_string("Sleep demo complete; both workers woke and exited.\n");
}

static void run_task_demo(void)
{
    if (!task_model_ready || task_count != 1U ||
        tasks[1].state != TASK_UNUSED || tasks[2].state != TASK_UNUSED) {
        put_string("Task demo unavailable: tasks are already active.\n");
        return;
    }

    initialize_task_stack(1U, task_worker_a, "worker-a");
    initialize_task_stack(2U, task_worker_b, "worker-b");
    task_count = 3U;
    task_switching_enabled = 1;

    put_string("Cooperative task demo: A and B yield three times each.\n");
    while (tasks[1].state != TASK_EXITED || tasks[2].state != TASK_EXITED) {
        task_yield();
    }

    (void)task_reap(1U);
    (void)task_reap(2U);
    put_string("Task demo complete; returned to the shell task.\n");
}

static void put_hex64(unsigned int high, unsigned int low)
{
    put_hex32(high);
    put_hex32(low);
}

static const char *memory_type_name(unsigned int type)
{
    switch (type) {
        case 1U: return "available";
        case 2U: return "reserved";
        case 3U: return "ACPI reclaimable";
        case 4U: return "ACPI NVS";
        case 5U: return "bad RAM";
        default: return "unknown";
    }
}

static void capture_memory_map(unsigned int address, unsigned int length)
{
    const unsigned char *bytes = (const unsigned char *)address;
    unsigned int offset = 0;

    memory_region_count = 0;
    memory_map_truncated = 0;

    while (offset < length) {
        unsigned int remaining = length - offset;
        if (remaining < sizeof(unsigned int)) {
            return;
        }

        const struct multiboot_mmap_entry *entry =
            (const struct multiboot_mmap_entry *)(bytes + offset);
        if (entry->size < 20U || entry->size > remaining - sizeof(unsigned int)) {
            return;
        }

        if (memory_region_count < MAX_MEMORY_REGIONS) {
            struct memory_region *region = &memory_regions[memory_region_count++];
            region->base_low = entry->base_low;
            region->base_high = entry->base_high;
            region->length_low = entry->length_low;
            region->length_high = entry->length_high;
            region->type = entry->type;
        } else {
            memory_map_truncated = 1;
        }

        offset += entry->size + sizeof(unsigned int);
    }

    memory_map_available = memory_region_count > 0U;
}

static void initialize_page_allocator(void)
{
    kernel_end_address = (unsigned int)&__kernel_end;
    page_range_count = 0;
    active_page_count = 0;
    released_page_count = 0;
    page_allocator_available = 0;

    if (!memory_map_available || kernel_end_address > 0xFFFFF000U) {
        return;
    }

    for (unsigned int i = 0; i < memory_region_count; ++i) {
        const struct memory_region *region = &memory_regions[i];
        if (region->type != 1U || region->base_high != 0U ||
            region->length_high != 0U ||
            region->base_low > 0xFFFFFFFFU - region->length_low) {
            continue;
        }

        unsigned int region_end = region->base_low + region->length_low;
        unsigned int start = region->base_low;
        if (start < kernel_end_address) {
            start = kernel_end_address;
        }
        if (start < 0x00100000U) {
            start = 0x00100000U;
        }
        if (start > 0xFFFFF000U) {
            continue;
        }

        start = (start + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
        unsigned int end = region_end & ~(PAGE_SIZE - 1U);
        /* Never hand out pages above the identity-mapped paging limit. */
        if (end > PAGING_MAPPED_LIMIT) {
            end = PAGING_MAPPED_LIMIT;
        }
        if (start >= end || page_range_count >= MAX_MEMORY_REGIONS) {
            continue;
        }

        page_ranges[page_range_count].start = start;
        page_ranges[page_range_count].end = end;
        ++page_range_count;
    }

    if (page_range_count > 0U) {
        current_range = 0;
        next_page = page_ranges[0].start;
        page_allocator_available = 1;
    }
}

static unsigned int total_pages(void)
{
    unsigned int total = 0;
    for (unsigned int i = 0; i < page_range_count; ++i) {
        total += (page_ranges[i].end - page_ranges[i].start) / PAGE_SIZE;
    }
    return total;
}

static unsigned int free_pages(void)
{
    unsigned int total = total_pages();
    return total >= active_page_count ? total - active_page_count : 0U;
}

/* Identity-map the first PAGING_MAPPED_LIMIT bytes using 4 KiB pages.
   Must be called after initialize_page_allocator() so that the kernel
   and stack are already placed. */
static void initialize_paging(void)
{
    /* Zero the page directory. */
    for (unsigned int i = 0; i < 1024U; ++i) {
        page_directory[i] = 0;
    }
    for (unsigned int i = 0U; i < VM_DYNAMIC_TABLES; ++i) {
        dynamic_page_table_directory[i] = 0U;
        dynamic_page_table_used[i] = 0;
    }
    dynamic_mapped_pages = 0U;

    /* Fill each of the 8 page tables and wire them into the directory. */
    for (unsigned int table = 0; table < PAGE_TABLES_FOR_LIMIT; ++table) {
        for (unsigned int i = 0; i < 1024U; ++i) {
            unsigned int address = table * 0x400000U + i * PAGE_SIZE;
            if (address >= PAGING_MAPPED_LIMIT) {
                page_tables[table][i] = 0;
            } else {
                /* Present | Read/Write; supervisor-only (US bit is clear). */
                page_tables[table][i] = address | 0x03U;
            }
        }

        unsigned int table_address = (unsigned int)&page_tables[table][0];
        /* Present | Read/Write; supervisor-only (US bit is clear). */
        page_directory[table] = table_address | 0x03U;
    }

    /* Load page directory base and enable paging. */
    __asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)page_directory));
    unsigned int cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000U; /* Set PG bit. */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    paging_enabled = 1;
}

static void invalidate_page(unsigned int virtual_address)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

/* Find or create one of the bounded page tables used for non-identity maps. */
static int dynamic_table_for(unsigned int directory_index, int create,
                             unsigned int **table)
{
    if ((page_directory[directory_index] & 0x01U) != 0U) {
        unsigned int physical = page_directory[directory_index] & 0xFFFFF000U;
        for (unsigned int i = 0U; i < VM_DYNAMIC_TABLES; ++i) {
            if (dynamic_page_table_used[i] &&
                dynamic_page_table_directory[i] == directory_index &&
                (unsigned int)&dynamic_page_tables[i][0] == physical) {
                *table = dynamic_page_tables[i];
                return (int)i;
            }
        }
        return -1;
    }
    if (!create) {
        return -1;
    }

    for (unsigned int i = 0U; i < VM_DYNAMIC_TABLES; ++i) {
        if (dynamic_page_table_used[i]) {
            continue;
        }
        for (unsigned int entry = 0U; entry < 1024U; ++entry) {
            dynamic_page_tables[i][entry] = 0U;
        }
        dynamic_page_table_directory[i] = directory_index;
        dynamic_page_table_used[i] = 1;
        page_directory[directory_index] =
            (unsigned int)&dynamic_page_tables[i][0] | 0x03U;
        *table = dynamic_page_tables[i];
        return (int)i;
    }
    return -1;
}

static int vm_map_page_flags(unsigned int virtual_address,
                             unsigned int physical_address,
                             unsigned int flags)
{
    if (!paging_enabled || virtual_address < PAGING_MAPPED_LIMIT ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0U ||
        (physical_address & (PAGE_SIZE - 1U)) != 0U ||
        physical_address >= PAGING_MAPPED_LIMIT) {
        return 0;
    }

    unsigned int directory_index = virtual_address >> 22;
    unsigned int table_index = (virtual_address >> 12) & 0x3FFU;
    unsigned int *table = 0;
    if (dynamic_table_for(directory_index, 1, &table) < 0 ||
        (table[table_index] & 0x01U) != 0U) {
        return 0;
    }

    table[table_index] = physical_address | flags;
    if ((flags & 0x04U) != 0U) {
        page_directory[directory_index] |= 0x04U;
    }
    ++dynamic_mapped_pages;
    invalidate_page(virtual_address);
    return 1;
}

static int vm_map_page(unsigned int virtual_address, unsigned int physical_address)
{
    return vm_map_page_flags(virtual_address, physical_address, 0x03U);
}

static int vm_page_is_mapped(unsigned int virtual_address)
{
    unsigned int directory_index = virtual_address >> 22;
    unsigned int table_index = (virtual_address >> 12) & 0x3FFU;
    unsigned int *table = 0;
    if (directory_index >= 1024U ||
        dynamic_table_for(directory_index, 0, &table) < 0) {
        return 0;
    }
    return (table[table_index] & 0x01U) != 0U;
}

static int vm_unmap_page(unsigned int virtual_address)
{
    if (!paging_enabled || virtual_address < PAGING_MAPPED_LIMIT ||
        (virtual_address & (PAGE_SIZE - 1U)) != 0U) {
        return 0;
    }

    unsigned int directory_index = virtual_address >> 22;
    unsigned int table_index = (virtual_address >> 12) & 0x3FFU;
    unsigned int *table = 0;
    int slot = dynamic_table_for(directory_index, 0, &table);
    if (slot < 0 || (table[table_index] & 0x01U) == 0U) {
        return 0;
    }

    table[table_index] = 0U;
    if (dynamic_mapped_pages > 0U) {
        --dynamic_mapped_pages;
    }
    invalidate_page(virtual_address);

    int empty = 1;
    for (unsigned int i = 0U; i < 1024U; ++i) {
        if ((table[i] & 0x01U) != 0U) {
            empty = 0;
            break;
        }
    }
    if (empty) {
        page_directory[directory_index] = 0U;
        dynamic_page_table_used[(unsigned int)slot] = 0;
        dynamic_page_table_directory[(unsigned int)slot] = 0U;
    }
    return 1;
}

static unsigned int vm_page_flags(unsigned int virtual_address)
{
    unsigned int directory_index = virtual_address >> 22;
    unsigned int table_index = (virtual_address >> 12) & 0x3FFU;
    unsigned int *table = 0;
    if (directory_index >= 1024U ||
        dynamic_table_for(directory_index, 0, &table) < 0) {
        return 0U;
    }
    return table[table_index] & 0xFFFU;
}

/* Validate every user page a syscall will read before dereferencing it.
   User pointers must be present in both paging levels and stay below the
   small, fixed output limit. */
static int syscall_user_buffer_readable(unsigned int address,
                                        unsigned int length)
{
    if (length == 0U || length > SYSCALL_WRITE_MAX ||
        address > 0xFFFFFFFFU - (length - 1U)) {
        return 0;
    }

    unsigned int last = address + length - 1U;
    unsigned int page = address & ~(PAGE_SIZE - 1U);
    unsigned int last_page = last & ~(PAGE_SIZE - 1U);
    unsigned int *directory = active_user_address_space != 0
        ? active_user_address_space : page_directory;
    for (;;) {
        unsigned int directory_index = page >> 22;
        unsigned int page_index = (page >> 12) & 0x3FFU;
        unsigned int directory_entry = directory[directory_index];
        unsigned int directory_flags = directory_entry & 0xFFFU;
        unsigned int page_flags = 0U;
        if ((directory_entry & 0x01U) != 0U) {
            unsigned int *table =
                (unsigned int *)(directory_entry & 0xFFFFF000U);
            page_flags = table[page_index] & 0xFFFU;
        }
        if ((directory_flags & 0x05U) != 0x05U ||
            (page_flags & 0x05U) != 0x05U) {
            return 0;
        }
        if (page == last_page) {
            return 1;
        }
        page += PAGE_SIZE;
    }
}

static int syscall_user_buffer_writable(unsigned int address,
                                        unsigned int length)
{
    if (length == 0U || length > SYSCALL_WRITE_MAX ||
        address > 0xFFFFFFFFU - (length - 1U)) {
        return 0;
    }

    unsigned int last = address + length - 1U;
    unsigned int page = address & ~(PAGE_SIZE - 1U);
    unsigned int last_page = last & ~(PAGE_SIZE - 1U);
    unsigned int *directory = active_user_address_space != 0
        ? active_user_address_space : page_directory;
    for (;;) {
        unsigned int directory_entry = directory[page >> 22];
        unsigned int page_flags = 0U;
        if ((directory_entry & 0x01U) != 0U) {
            unsigned int *table =
                (unsigned int *)(directory_entry & 0xFFFFF000U);
            page_flags = table[(page >> 12) & 0x3FFU] & 0xFFFU;
        }
        if ((directory_entry & 0x07U) != 0x07U ||
            (page_flags & 0x07U) != 0x07U) {
            return 0;
        }
        if (page == last_page) {
            return 1;
        }
        page += PAGE_SIZE;
    }
}

static unsigned int allocate_page(void)
{
    if (!page_allocator_available || active_page_count >= MAX_TRACKED_PAGES) {
        return 0U;
    }

    unsigned int result = 0U;
    if (released_page_count > 0U) {
        result = released_pages[--released_page_count];
    } else {
        while (current_range < page_range_count) {
            const struct page_range *range = &page_ranges[current_range];
            if (next_page <= range->end && range->end - next_page >= PAGE_SIZE) {
                result = next_page;
                next_page += PAGE_SIZE;
                break;
            }

            ++current_range;
            if (current_range < page_range_count) {
                next_page = page_ranges[current_range].start;
            }
        }
    }

    if (result != 0U) {
        active_pages[active_page_count++] = result;
    }
    return result;
}

static int parse_hex_address(const char *text, unsigned int *address)
{
    unsigned int value = 0U;
    unsigned int digits = 0U;

    if (text[0] == '0' && text[1] == 'x') {
        text += 2;
    }

    while (*text != '\0') {
        unsigned int digit;
        if (*text >= '0' && *text <= '9') {
            digit = (unsigned int)(*text - '0');
        } else if (*text >= 'a' && *text <= 'f') {
            digit = (unsigned int)(*text - 'a') + 10U;
        } else if (*text >= 'A' && *text <= 'F') {
            digit = (unsigned int)(*text - 'A') + 10U;
        } else {
            return 0;
        }

        if (digits >= 8U || value > (0xFFFFFFFFU - digit) / 16U) {
            return 0;
        }
        value = value * 16U + digit;
        ++digits;
        ++text;
    }

    if (digits == 0U || (value & (PAGE_SIZE - 1U)) != 0U) {
        return 0;
    }

    *address = value;
    return 1;
}

static int release_page(unsigned int address)
{
    if (released_page_count >= MAX_TRACKED_PAGES) {
        return 0;
    }

    for (unsigned int i = 0; i < active_page_count; ++i) {
        if (active_pages[i] == address) {
            active_pages[i] = active_pages[active_page_count - 1U];
            --active_page_count;
            released_pages[released_page_count++] = address;
            return 1;
        }
    }

    return 0;
}

static void clear_physical_page(unsigned int address)
{
    volatile unsigned char *page = (volatile unsigned char *)address;
    for (unsigned int i = 0U; i < PAGE_SIZE; ++i) {
        page[i] = 0U;
    }
}

/* Map a copied, read-only user code page and a separate writable user stack.
   The kernel's low identity map remains supervisor-only. */
static int prepare_user_demo_memory(void)
{
    unsigned int blob_start = (unsigned int)user_blob_start;
    unsigned int blob_end = (unsigned int)user_blob_end;
    unsigned int blob_size = blob_end - blob_start;
    if (blob_end < blob_start || blob_size == 0U || blob_size > PAGE_SIZE) {
        return 0;
    }

    user_code_physical = allocate_page();
    if (user_code_physical == 0U) {
        return 0;
    }
    user_stack_physical = allocate_page();
    if (user_stack_physical == 0U) {
        (void)release_page(user_code_physical);
        user_code_physical = 0U;
        return 0;
    }

    clear_physical_page(user_code_physical);
    clear_physical_page(user_stack_physical);
    unsigned char *destination = (unsigned char *)user_code_physical;
    const unsigned char *source = user_blob_start;
    for (unsigned int i = 0U; i < blob_size; ++i) {
        destination[i] = source[i];
    }

    if (!vm_map_page_flags(USER_CODE_ADDRESS, user_code_physical, 0x05U) ||
        !vm_map_page_flags(USER_STACK_ADDRESS, user_stack_physical, 0x07U)) {
        (void)vm_unmap_page(USER_STACK_ADDRESS);
        (void)vm_unmap_page(USER_CODE_ADDRESS);
        (void)release_page(user_stack_physical);
        (void)release_page(user_code_physical);
        user_code_physical = 0U;
        user_stack_physical = 0U;
        return 0;
    }

    return 1;
}

static void cleanup_user_demo_memory(void)
{
    (void)vm_unmap_page(USER_CODE_ADDRESS);
    (void)vm_unmap_page(USER_STACK_ADDRESS);
    if (user_stack_physical != 0U) {
        (void)release_page(user_stack_physical);
        user_stack_physical = 0U;
    }
    if (user_code_physical != 0U) {
        (void)release_page(user_code_physical);
        user_code_physical = 0U;
    }
}

static void load_page_directory(unsigned int *directory)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"((unsigned int)directory) : "memory");
}

static unsigned int address_space_directory_flags(unsigned int *directory,
                                                  unsigned int address)
{
    return directory[address >> 22] & 0xFFFU;
}

static unsigned int address_space_page_flags(unsigned int *directory,
                                             unsigned int address)
{
    unsigned int directory_index = address >> 22;
    unsigned int page_index = (address >> 12) & 0x3FFU;
    unsigned int entry = directory[directory_index];
    if ((entry & 0x01U) == 0U) {
        return 0U;
    }
    unsigned int *table = (unsigned int *)(entry & 0xFFFFF000U);
    return table[page_index] & 0xFFFU;
}

/* Build a small process address space with a supervisor-only kernel identity
   map plus one user code page and one user stack page. */
static int prepare_user_address_space(void)
{
    unsigned int blob_start = (unsigned int)user_blob_start;
    unsigned int blob_end = (unsigned int)user_blob_end;
    unsigned int blob_size = blob_end - blob_start;

    if (!paging_enabled || !page_allocator_available ||
        USER_CODE_ADDRESS < PAGING_MAPPED_LIMIT ||
        USER_ADDRESS_SPACE_DIRECTORY < PAGE_TABLES_FOR_LIMIT ||
        USER_ADDRESS_SPACE_DIRECTORY >= 1024U ||
        (USER_CODE_ADDRESS & (PAGE_SIZE - 1U)) != 0U ||
        (USER_STACK_ADDRESS & (PAGE_SIZE - 1U)) != 0U ||
        USER_CODE_ADDRESS >> 22 != USER_STACK_ADDRESS >> 22 ||
        (page_directory[USER_ADDRESS_SPACE_DIRECTORY] & 0x01U) != 0U ||
        blob_end < blob_start || blob_size == 0U || blob_size > PAGE_SIZE) {
        return 0;
    }

    address_space_code_physical = allocate_page();
    if (address_space_code_physical == 0U) {
        return 0;
    }
    address_space_stack_physical = allocate_page();
    if (address_space_stack_physical == 0U) {
        (void)release_page(address_space_code_physical);
        address_space_code_physical = 0U;
        return 0;
    }

    for (unsigned int i = 0U; i < 1024U; ++i) {
        user_address_space_directory[i] = 0U;
        user_address_space_table[i] = 0U;
    }
    for (unsigned int i = 0U; i < PAGE_TABLES_FOR_LIMIT; ++i) {
        /* Keep every shared kernel mapping supervisor-only in the process. */
        user_address_space_directory[i] = page_directory[i] & ~0x04U;
    }

    clear_physical_page(address_space_code_physical);
    clear_physical_page(address_space_stack_physical);
    unsigned char *destination = (unsigned char *)address_space_code_physical;
    const unsigned char *source = user_blob_start;
    for (unsigned int i = 0U; i < blob_size; ++i) {
        destination[i] = source[i];
    }

    /* Minimal 32-bit process entry stack: argc=0, argv[0]=NULL, envp[0]=NULL. */
    unsigned int *stack = (unsigned int *)address_space_stack_physical;
    unsigned int initial_words = PAGE_SIZE / sizeof(unsigned int) - 3U;
    stack[initial_words] = 0U;
    stack[initial_words + 1U] = 0U;
    stack[initial_words + 2U] = 0U;

    unsigned int code_index = (USER_CODE_ADDRESS >> 12) & 0x3FFU;
    unsigned int stack_index = (USER_STACK_ADDRESS >> 12) & 0x3FFU;
    user_address_space_table[code_index] = address_space_code_physical | 0x05U;
    user_address_space_table[stack_index] = address_space_stack_physical | 0x07U;
    user_address_space_directory[USER_ADDRESS_SPACE_DIRECTORY] =
        (unsigned int)&user_address_space_table[0] | 0x07U;
    return 1;
}

static void cleanup_user_address_space(void)
{
    for (unsigned int i = 0U; i < 1024U; ++i) {
        user_address_space_directory[i] = 0U;
        user_address_space_table[i] = 0U;
    }
    if (address_space_stack_physical != 0U) {
        (void)release_page(address_space_stack_physical);
        address_space_stack_physical = 0U;
    }
    if (address_space_code_physical != 0U) {
        (void)release_page(address_space_code_physical);
        address_space_code_physical = 0U;
    }
}

static int user_address_space_permissions_valid(void)
{
    unsigned int code_directory = address_space_directory_flags(
        user_address_space_directory, USER_CODE_ADDRESS);
    unsigned int stack_directory = address_space_directory_flags(
        user_address_space_directory, USER_STACK_ADDRESS);
    unsigned int code_page = address_space_page_flags(
        user_address_space_directory, USER_CODE_ADDRESS);
    unsigned int stack_page = address_space_page_flags(
        user_address_space_directory, USER_STACK_ADDRESS);
    unsigned int vga_directory = address_space_directory_flags(
        user_address_space_directory, 0x000B8000U);
    unsigned int vga_page = address_space_page_flags(
        user_address_space_directory, 0x000B8000U);
    unsigned int *stack = (unsigned int *)address_space_stack_physical;
    unsigned int initial_words = PAGE_SIZE / sizeof(unsigned int) - 3U;

    return (code_directory & 0x05U) == 0x05U &&
           (stack_directory & 0x07U) == 0x07U &&
           (code_page & 0x05U) == 0x05U && (code_page & 0x02U) == 0U &&
           (stack_page & 0x07U) == 0x07U &&
           (vga_directory & 0x04U) == 0U && (vga_page & 0x05U) == 0x01U &&
           (page_directory[USER_ADDRESS_SPACE_DIRECTORY] & 0x01U) == 0U &&
           stack[initial_words] == 0U && stack[initial_words + 1U] == 0U &&
           stack[initial_words + 2U] == 0U;
}

static unsigned int kernel_page_flags(unsigned int virtual_address)
{
    unsigned int directory_index = virtual_address >> 22;
    unsigned int table_index = (virtual_address >> 12) & 0x3FFU;
    if (directory_index >= PAGE_TABLES_FOR_LIMIT ||
        (page_directory[directory_index] & 0x01U) == 0U) {
        return 0U;
    }
    return page_tables[directory_index][table_index] & 0xFFFU;
}

static void show_vm_info(void)
{
    unsigned int used_tables = 0U;
    for (unsigned int i = 0U; i < VM_DYNAMIC_TABLES; ++i) {
        if (dynamic_page_table_used[i]) {
            ++used_tables;
        }
    }
    put_string("Virtual memory: ");
    put_string(paging_enabled ? "enabled\n" : "disabled\n");
    put_string("Identity map: first ");
    put_unsigned(PAGING_MAPPED_LIMIT / 0x100000U);
    put_string(" MiB\nDynamic pages mapped: ");
    put_unsigned(dynamic_mapped_pages);
    put_string("\nDynamic page tables in use: ");
    put_unsigned(used_tables);
    put_string(" / ");
    put_unsigned(VM_DYNAMIC_TABLES);
    put_char('\n');
}

static void run_vm_test(void)
{
    const unsigned int first_pattern = 0x13579BDFU;
    const unsigned int second_pattern = 0x2468ACE0U;
    unsigned int physical = allocate_page();
    if (physical == 0U) {
        put_string("VM test failed: no physical page available.\n");
        return;
    }

    if (!vm_map_page(VM_TEST_VIRTUAL_ADDRESS, physical)) {
        (void)release_page(physical);
        put_string("VM test failed: could not create the virtual mapping.\n");
        return;
    }

    volatile unsigned int *identity = (volatile unsigned int *)physical;
    volatile unsigned int *alias =
        (volatile unsigned int *)VM_TEST_VIRTUAL_ADDRESS;
    identity[0] = first_pattern;
    int alias_reads_identity = vm_page_is_mapped(VM_TEST_VIRTUAL_ADDRESS) &&
                              alias[0] == first_pattern;
    alias[1] = second_pattern;
    int identity_reads_alias = identity[1] == second_pattern;
    int unmap_ok = vm_unmap_page(VM_TEST_VIRTUAL_ADDRESS);
    int mapping_removed = !vm_page_is_mapped(VM_TEST_VIRTUAL_ADDRESS);
    int page_released = unmap_ok && release_page(physical);

    if (alias_reads_identity && identity_reads_alias && unmap_ok &&
        mapping_removed && page_released) {
        put_string("VM test passed: mapped alias reads/writes physical page; unmap and cleanup succeeded.\n");
    } else {
        put_string("VM test failed: alias read/write, unmap, or cleanup check failed.\n");
    }
}

static int command_is(const char *expected)
{
    unsigned int i = 0;
    while (command[i] != '\0' && expected[i] != '\0') {
        if (command[i] != expected[i]) {
            return 0;
        }
        ++i;
    }
    return command[i] == '\0' && expected[i] == '\0';
}

static int command_starts_with(const char *prefix)
{
    unsigned int i = 0;
    while (prefix[i] != '\0') {
        if (command[i] != prefix[i]) {
            return 0;
        }
        ++i;
    }
    return 1;
}

/* --- In-memory file store helpers --- */

/* Find a file or directory with this name in the specified directory. */
static int entry_find(const char *name, unsigned char parent)
{
    for (unsigned int i = 0; i < MAX_FILES; ++i) {
        if (files[i].used && files[i].parent == parent) {
            const char *a = files[i].name;
            const char *b = name;
            int same = 1;
            while (*a != '\0' || *b != '\0') {
                if (*a != *b) {
                    same = 0;
                    break;
                }
                if (*a != '\0') {
                    ++a;
                }
                if (*b != '\0') {
                    ++b;
                }
            }
            if (same) {
                return (int)i;
            }
        }
    }
    return -1;
}

/* Returns the index of the first unused slot, or -1 if the store is full. */
static int file_free_slot(void)
{
    for (unsigned int i = 0; i < MAX_FILES; ++i) {
        if (!files[i].used) {
            return (int)i;
        }
    }
    return -1;
}

/* Returns a pointer into command[] past the command keyword, skipping
   any spaces, or NULL if the rest of the line is empty. */
static const char *command_args_after(const char *prefix)
{
    const char *p = command;
    while (*p != '\0' && *prefix != '\0') {
        if (*p != *prefix) {
            return 0;
        }
        ++p;
        ++prefix;
    }
    while (*p == ' ') {
        ++p;
    }
    if (*p == '\0') {
        return 0;
    }
    return p;
}

/* Returns the length of a NUL-terminated string. */
static unsigned int string_length(const char *s)
{
    unsigned int len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    return len;
}

static int filesystem_bounded_content_length(const char *content,
                                              unsigned int *length)
{
    if (content == 0 || length == 0) {
        return 0;
    }
    for (unsigned int i = 0; i <= FILE_CONTENT_MAX; ++i) {
        if (content[i] == '\0') {
            *length = i;
            return 1;
        }
    }
    *length = 0U;
    return 0;
}

static int strings_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* Returns 1 if the name is valid (non-empty, no spaces, fits in 15 chars). */
static int file_name_valid(const char *name)
{
    unsigned int len = 0;
    if (name[0] == '\0') {
        return 0;
    }
    if ((name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
        return 0;
    }
    while (name[len] != '\0') {
        if (name[len] == ' ' || name[len] == '\n' || name[len] == '\b' ||
            name[len] == '/' || name[len] == '\\') {
            return 0;
        }
        ++len;
        if (len > FILENAME_MAX_LEN) {
            return 0;
        }
    }
    return 1;
}

/* Copies a NUL-terminated string. Returns 0 if it does not fit in max. */
static int copy_string(char *dest, unsigned int max, const char *src)
{
    unsigned int i = 0;
    while (src[i] != '\0') {
        if (i >= max) {
            return 0; /* Would overflow. */
        }
        dest[i] = src[i];
        ++i;
    }
    dest[i] = '\0';
    return 1;
}

static const unsigned char filesystem_magic[8] = {
    'T', 'I', 'N', 'Y', 'F', 'S', '0', '1'
};

static int sector_is_zero(const unsigned char *sector)
{
    for (unsigned int i = 0; i < DISK_SECTOR_SIZE; ++i) {
        if (sector[i] != 0U) {
            return 0;
        }
    }
    return 1;
}

/* Legacy versions store up to eight sectors at a fixed location per slot. */
static unsigned int legacy_filesystem_data_lba(unsigned int slot,
                                               unsigned int file_sector)
{
    if (file_sector == 0U) {
        return FILESYSTEM_DATA_LBA + slot;
    }
    return FILESYSTEM_EXTENSION_LBA +
           slot * LEGACY_FILE_EXTRA_SECTORS_PER_SLOT + (file_sector - 1U);
}

static unsigned int filesystem_cluster_lba(unsigned short cluster)
{
    return FILESYSTEM_DATA_START_LBA + (unsigned int)cluster;
}

static int filesystem_load_fat(void)
{
    int result = block_read_sectors(FILESYSTEM_FAT_LBA,
                                   FILESYSTEM_FAT_SECTORS,
                                   filesystem_fat_disk);
    if (result < 0) {
        return result;
    }
    for (unsigned int i = 0; i < FILESYSTEM_FAT_ENTRIES; ++i) {
        filesystem_fat[i] = (unsigned short)filesystem_fat_disk[i * 2U] |
            ((unsigned short)filesystem_fat_disk[i * 2U + 1U] << 8);
        if (i < FILESYSTEM_CLUSTER_COUNT) {
            unsigned short value = filesystem_fat[i];
            if (value != FILESYSTEM_CLUSTER_FREE &&
                value != FILESYSTEM_CLUSTER_END &&
                value >= FILESYSTEM_CLUSTER_COUNT) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
        } else if (filesystem_fat[i] != FILESYSTEM_CLUSTER_FREE) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
    }
    return 0;
}

static int filesystem_write_fat(void)
{
    for (unsigned int i = 0; i < FILESYSTEM_FAT_ENTRIES; ++i) {
        if ((i < FILESYSTEM_CLUSTER_COUNT &&
             filesystem_fat[i] != FILESYSTEM_CLUSTER_FREE &&
             filesystem_fat[i] != FILESYSTEM_CLUSTER_END &&
             filesystem_fat[i] >= FILESYSTEM_CLUSTER_COUNT) ||
            (i >= FILESYSTEM_CLUSTER_COUNT &&
             filesystem_fat[i] != FILESYSTEM_CLUSTER_FREE)) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        filesystem_fat_disk[i * 2U] =
            (unsigned char)(filesystem_fat[i] & 0xFFU);
        filesystem_fat_disk[i * 2U + 1U] =
            (unsigned char)((filesystem_fat[i] >> 8) & 0xFFU);
    }
    int result = block_write_sectors(FILESYSTEM_FAT_LBA,
                                    FILESYSTEM_FAT_SECTORS,
                                    filesystem_fat_disk);
    return result < 0 ? result : 0;
}

static int filesystem_allocate_chain(unsigned int count,
                                     unsigned short *first_cluster)
{
    unsigned short allocated[FILESYSTEM_MAX_FILE_SECTORS];
    if (first_cluster == 0 || count > FILESYSTEM_MAX_FILE_SECTORS) {
        return FILESYSTEM_CORRUPT_ERROR;
    }
    *first_cluster = FILESYSTEM_CLUSTER_FREE;
    if (count == 0U) {
        return 0;
    }

    unsigned int allocated_count = 0U;
    for (unsigned int cluster = 0; cluster < FILESYSTEM_CLUSTER_COUNT &&
         allocated_count < count; ++cluster) {
        if (filesystem_fat[cluster] == FILESYSTEM_CLUSTER_FREE) {
            allocated[allocated_count++] = (unsigned short)cluster;
        }
    }
    if (allocated_count != count) {
        return FILESYSTEM_FULL_ERROR;
    }
    for (unsigned int i = 0; i < count; ++i) {
        filesystem_fat[allocated[i]] = i + 1U < count
            ? allocated[i + 1U] : FILESYSTEM_CLUSTER_END;
    }
    *first_cluster = allocated[0];
    return 0;
}

static int filesystem_release_chain(unsigned short first_cluster,
                                    unsigned int count)
{
    unsigned short chain[FILESYSTEM_MAX_FILE_SECTORS];
    if (count > FILESYSTEM_MAX_FILE_SECTORS) {
        return FILESYSTEM_CORRUPT_ERROR;
    }
    if (count == 0U) {
        return first_cluster == FILESYSTEM_CLUSTER_FREE
            ? 0 : FILESYSTEM_CORRUPT_ERROR;
    }
    if (first_cluster >= FILESYSTEM_CLUSTER_COUNT) {
        return FILESYSTEM_CORRUPT_ERROR;
    }

    unsigned short cluster = first_cluster;
    for (unsigned int i = 0; i < count; ++i) {
        if (cluster >= FILESYSTEM_CLUSTER_COUNT ||
            filesystem_fat[cluster] == FILESYSTEM_CLUSTER_FREE) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        for (unsigned int prior = 0U; prior < i; ++prior) {
            if (chain[prior] == cluster) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
        }
        chain[i] = cluster;
        unsigned short next = filesystem_fat[cluster];
        if (i + 1U == count) {
            if (next != FILESYSTEM_CLUSTER_END) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
        } else {
            if (next == FILESYSTEM_CLUSTER_END ||
                next == FILESYSTEM_CLUSTER_FREE ||
                next >= FILESYSTEM_CLUSTER_COUNT) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
            cluster = next;
        }
    }
    for (unsigned int i = 0; i < count; ++i) {
        filesystem_fat[chain[i]] = FILESYSTEM_CLUSTER_FREE;
    }
    return 0;
}

static int filesystem_write_data_chain(unsigned short first_cluster,
                                       unsigned int sector_count,
                                       const char *content)
{
    unsigned int length;
    unsigned int expected_count;
    unsigned short chain[FILESYSTEM_MAX_FILE_SECTORS];
    if (!filesystem_bounded_content_length(content, &length) ||
        length > FILE_CONTENT_MAX) {
        return FILESYSTEM_CORRUPT_ERROR;
    }
    expected_count = length == 0U ? 0U :
        (length + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE;
    if (sector_count != expected_count ||
        sector_count > FILESYSTEM_MAX_FILE_SECTORS ||
        (sector_count == 0U && first_cluster != FILESYSTEM_CLUSTER_FREE) ||
        (sector_count != 0U && first_cluster >= FILESYSTEM_CLUSTER_COUNT)) {
        return FILESYSTEM_CORRUPT_ERROR;
    }

    /* Validate the full chain before writing any data sector. */
    unsigned short cluster = first_cluster;
    for (unsigned int i = 0U; i < sector_count; ++i) {
        if (cluster >= FILESYSTEM_CLUSTER_COUNT ||
            filesystem_fat[cluster] == FILESYSTEM_CLUSTER_FREE) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        for (unsigned int prior = 0U; prior < i; ++prior) {
            if (chain[prior] == cluster) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
        }
        chain[i] = cluster;
        unsigned short next = filesystem_fat[cluster];
        if (i + 1U == sector_count) {
            if (next != FILESYSTEM_CLUSTER_END) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
        } else {
            if (next == FILESYSTEM_CLUSTER_END ||
                next == FILESYSTEM_CLUSTER_FREE ||
                next >= FILESYSTEM_CLUSTER_COUNT) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
            cluster = next;
        }
    }

    unsigned int offset = 0U;
    for (unsigned int file_sector = 0; file_sector < sector_count; ++file_sector) {
        cluster = chain[file_sector];
        for (unsigned int i = 0; i < DISK_SECTOR_SIZE; ++i) {
            filesystem_fat_disk[i] = 0U;
        }
        unsigned int bytes_here = length - offset;
        if (bytes_here > DISK_SECTOR_SIZE) {
            bytes_here = DISK_SECTOR_SIZE;
        }
        for (unsigned int i = 0; i < bytes_here; ++i) {
            filesystem_fat_disk[i] = (unsigned char)content[offset + i];
        }
        int result = block_write_sectors(filesystem_cluster_lba(cluster), 1U,
                                        filesystem_fat_disk);
        if (result < 0) {
            return result;
        }
        offset += bytes_here;
    }
    return 0;
}

static int filesystem_read_data_chain(unsigned short first_cluster,
                                      unsigned int sector_count,
                                      unsigned int length,
                                      char *content)
{
    unsigned int expected_count;
    unsigned short chain[FILESYSTEM_MAX_FILE_SECTORS];
    if (content == 0 || length > FILE_CONTENT_MAX) {
        return FILESYSTEM_CORRUPT_ERROR;
    }
    expected_count = length == 0U ? 0U :
        (length + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE;
    if (sector_count != expected_count ||
        sector_count > FILESYSTEM_MAX_FILE_SECTORS ||
        (sector_count == 0U && first_cluster != FILESYSTEM_CLUSTER_FREE) ||
        (sector_count != 0U && first_cluster >= FILESYSTEM_CLUSTER_COUNT)) {
        return FILESYSTEM_CORRUPT_ERROR;
    }

    /* Validate the entire chain before reading any sectors into the file. */
    unsigned short cluster = first_cluster;
    for (unsigned int i = 0U; i < sector_count; ++i) {
        if (cluster >= FILESYSTEM_CLUSTER_COUNT ||
            filesystem_fat[cluster] == FILESYSTEM_CLUSTER_FREE) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        for (unsigned int prior = 0U; prior < i; ++prior) {
            if (chain[prior] == cluster) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
        }
        chain[i] = cluster;
        unsigned short next = filesystem_fat[cluster];
        if (i + 1U == sector_count) {
            if (next != FILESYSTEM_CLUSTER_END) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
        } else {
            if (next == FILESYSTEM_CLUSTER_END ||
                next == FILESYSTEM_CLUSTER_FREE ||
                next >= FILESYSTEM_CLUSTER_COUNT) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
            cluster = next;
        }
    }

    unsigned int offset = 0U;
    for (unsigned int file_sector = 0; file_sector < sector_count; ++file_sector) {
        cluster = chain[file_sector];
        if (filesystem_cluster_seen[cluster]) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        filesystem_cluster_seen[cluster] = 1U;
        int result = block_read_sectors(filesystem_cluster_lba(cluster), 1U,
                                       filesystem_fat_disk);
        if (result < 0) {
            return result;
        }
        unsigned int bytes_here = length - offset;
        if (bytes_here > DISK_SECTOR_SIZE) {
            bytes_here = DISK_SECTOR_SIZE;
        }
        for (unsigned int i = 0; i < bytes_here; ++i) {
            if (filesystem_fat_disk[i] == 0U) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
            content[offset + i] = (char)filesystem_fat_disk[i];
        }
        for (unsigned int i = bytes_here; i < DISK_SECTOR_SIZE; ++i) {
            if (filesystem_fat_disk[i] != 0U) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
        }
        offset += bytes_here;
    }
    content[length] = '\0';
    return 0;
}

static void filesystem_make_superblock(unsigned char *sector)
{
    for (unsigned int i = 0; i < DISK_SECTOR_SIZE; ++i) {
        sector[i] = 0U;
    }
    for (unsigned int i = 0; i < sizeof(filesystem_magic); ++i) {
        sector[i] = filesystem_magic[i];
    }
    sector[8] = FILESYSTEM_VERSION; /* Little-endian version number. */
}

static int filesystem_has_magic(const unsigned char *sector)
{
    for (unsigned int i = 0; i < sizeof(filesystem_magic); ++i) {
        if (sector[i] != filesystem_magic[i]) {
            return 0;
        }
    }
    return 1;
}

static int filesystem_write_directory(void)
{
    unsigned char sector[DISK_SECTOR_SIZE];
    for (unsigned int i = 0; i < DISK_SECTOR_SIZE; ++i) {
        sector[i] = 0U;
    }

    for (unsigned int slot = 0; slot < MAX_FILES; ++slot) {
        if (!files[slot].used) {
            continue;
        }
        if (files[slot].used != 1 ||
            (files[slot].is_directory != 0 &&
             files[slot].is_directory != 1)) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        if (!file_name_valid(files[slot].name)) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        if (files[slot].parent != FILE_PARENT_ROOT &&
            (files[slot].parent >= MAX_FILES ||
             !files[files[slot].parent].used ||
             !files[files[slot].parent].is_directory ||
             files[slot].parent == slot)) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        unsigned int ancestor = files[slot].parent;
        for (unsigned int depth = 0U; ancestor != FILE_PARENT_ROOT; ++depth) {
            if (depth >= MAX_FILES || ancestor >= MAX_FILES ||
                !files[ancestor].used || !files[ancestor].is_directory ||
                ancestor == slot) {
                return FILESYSTEM_CORRUPT_ERROR;
            }
            ancestor = files[ancestor].parent;
        }
        unsigned int content_length = 0U;
        if (!files[slot].is_directory &&
            !filesystem_bounded_content_length(files[slot].content,
                                               &content_length)) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        if (content_length > FILE_CONTENT_MAX) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        unsigned int required_clusters = files[slot].is_directory ||
            content_length == 0U ? 0U :
            (content_length + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE;
        if (files[slot].cluster_count != required_clusters ||
            (required_clusters == 0U &&
             files[slot].first_cluster != FILESYSTEM_CLUSTER_FREE) ||
            (required_clusters != 0U &&
             files[slot].first_cluster >= FILESYSTEM_CLUSTER_COUNT)) {
            return FILESYSTEM_CORRUPT_ERROR;
        }
        for (unsigned int other = 0; other < slot; ++other) {
            if (files[other].used && files[other].parent == files[slot].parent &&
                string_length(files[other].name) == string_length(files[slot].name)) {
                unsigned int i = 0;
                while (files[slot].name[i] != '\0' &&
                       files[slot].name[i] == files[other].name[i]) {
                    ++i;
                }
                if (files[slot].name[i] == '\0' && files[other].name[i] == '\0') {
                    return FILESYSTEM_CORRUPT_ERROR;
                }
            }
        }

        unsigned int offset = slot * FILESYSTEM_ENTRY_SIZE;
        for (unsigned int i = 0; i <= FILENAME_MAX_LEN; ++i) {
            sector[offset + i] = (unsigned char)files[slot].name[i];
        }
        sector[offset + 16U] = (unsigned char)(content_length & 0xFFU);
        sector[offset + 17U] = (unsigned char)((content_length >> 8) & 0xFFU);
        sector[offset + 18U] = 1U;
        sector[offset + 19U] = files[slot].is_directory
            ? ENTRY_TYPE_DIRECTORY : ENTRY_TYPE_FILE;
        sector[offset + 20U] = files[slot].parent;
        sector[offset + 21U] = (unsigned char)(files[slot].first_cluster & 0xFFU);
        sector[offset + 22U] = (unsigned char)(files[slot].first_cluster >> 8);
        sector[offset + 23U] = (unsigned char)(files[slot].cluster_count & 0xFFU);
        sector[offset + 24U] = (unsigned char)(files[slot].cluster_count >> 8);
    }

    int result = block_write_sectors(FILESYSTEM_DIRECTORY_LBA, 1U, sector);
    return result < 0 ? result : 1;
}

static int filesystem_save_change(unsigned int slot,
                                  const struct file_entry *previous)
{
    if (slot >= MAX_FILES || previous == 0 || previous == &files[slot]) {
        return FILESYSTEM_CORRUPT_ERROR;
    }
    int new_is_file = files[slot].used && !files[slot].is_directory;
    int old_is_file = previous->used && !previous->is_directory;
    int result;

    if (new_is_file) {
        unsigned int length;
        if (!filesystem_bounded_content_length(files[slot].content, &length)) {
            file_entry_copy(&files[slot], previous);
            return FILESYSTEM_CORRUPT_ERROR;
        }
        unsigned int count = length == 0U ? 0U :
            (length + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE;
        unsigned short first;
        result = filesystem_allocate_chain(count, &first);
        if (result < 0) {
            file_entry_copy(&files[slot], previous);
            return result;
        }
        files[slot].first_cluster = first;
        files[slot].cluster_count = (unsigned short)count;
        if (count != 0U) {
            result = filesystem_write_data_chain(first, count, files[slot].content);
            if (result < 0) {
                (void)filesystem_release_chain(first, count);
                file_entry_copy(&files[slot], previous);
                return result;
            }
            result = filesystem_write_fat();
            if (result < 0) {
                (void)filesystem_release_chain(first, count);
                file_entry_copy(&files[slot], previous);
                file_store_ready = 0;
                file_store_error = result;
                return result;
            }
        }
    }

    result = filesystem_write_directory();
    if (result < 0) {
        if (new_is_file && files[slot].cluster_count != 0U) {
            (void)filesystem_release_chain(files[slot].first_cluster,
                                           files[slot].cluster_count);
            (void)filesystem_write_fat();
        }
        file_entry_copy(&files[slot], previous);
        (void)filesystem_write_directory();
        file_store_ready = 0;
        file_store_error = result;
        return result;
    }

    if (old_is_file && previous->cluster_count != 0U) {
        result = filesystem_release_chain(previous->first_cluster,
                                          previous->cluster_count);
        if (result < 0) {
            file_store_ready = 0;
            file_store_error = result;
            return result;
        }
        result = filesystem_write_fat();
        if (result < 0) {
            file_store_ready = 0;
            file_store_error = result;
            return result;
        }
    }
    return 0;
}

/* Persist a name-only update without allocating new data clusters or touching
   the file's existing data chain. */
static int filesystem_save_metadata_change(unsigned int slot,
                                           const struct file_entry *previous)
{
    if (slot >= MAX_FILES || previous == 0 || previous == &files[slot]) {
        return FILESYSTEM_CORRUPT_ERROR;
    }
    int result = filesystem_write_directory();
    if (result < 0) {
        file_entry_copy(&files[slot], previous);
        (void)filesystem_write_directory();
        file_store_ready = 0;
        file_store_error = result;
        return result;
    }
    return 0;
}

/* Verify a whole LBA range before formatting or claiming previously unused
   sectors. The bounded chunk keeps the kernel stack small. */
static int filesystem_check_zero_range(unsigned int first_lba,
                                       unsigned int last_lba)
{
    unsigned int lba = first_lba;
    while (lba <= last_lba) {
        unsigned int count = last_lba - lba + 1U;
        if (count > 32U) {
            count = 32U;
        }
        int result = block_read_sectors(lba, count, filesystem_scan_buffer);
        if (result < 0) {
            return result;
        }
        for (unsigned int sector = 0; sector < count; ++sector) {
            if (!sector_is_zero(filesystem_scan_buffer +
                                sector * DISK_SECTOR_SIZE)) {
                return FILESYSTEM_FORMAT_ERROR;
            }
        }
        lba += count;
    }
    return 0;
}

static void filesystem_reset_fat(void)
{
    for (unsigned int i = 0; i < FILESYSTEM_FAT_ENTRIES; ++i) {
        filesystem_fat[i] = FILESYSTEM_CLUSTER_FREE;
    }
}

static int initialize_file_store(void)
{
    unsigned char superblock[DISK_SECTOR_SIZE];
    unsigned char directory[DISK_SECTOR_SIZE];
    unsigned char data_sector[DISK_SECTOR_SIZE];
    int result = block_read_sectors(FILESYSTEM_SUPER_LBA, 1U, superblock);
    if (result < 0) {
        file_store_error = result;
        return result;
    }

    if (!filesystem_has_magic(superblock)) {
        if (!sector_is_zero(superblock)) {
            file_store_error = FILESYSTEM_FORMAT_ERROR;
            return file_store_error;
        }

        /* The format owns LBAs 0..4095. Do not format if any are nonblank;
           LBA 4096 remains reserved for disktest and is deliberately skipped. */
        result = filesystem_check_zero_range(0U, DISK_TEST_SECTOR - 1U);
        if (result < 0) {
            file_store_error = result;
            return result;
        }

        for (unsigned int i = 0; i < DISK_SECTOR_SIZE; ++i) {
            directory[i] = 0U;
        }
        filesystem_reset_fat();
        result = filesystem_write_fat();
        if (result < 0) {
            file_store_error = result;
            return result;
        }
        result = block_write_sectors(FILESYSTEM_DIRECTORY_LBA, 1U, directory);
        if (result < 0) {
            file_store_error = result;
            return result;
        }
        filesystem_make_superblock(superblock);
        result = block_write_sectors(FILESYSTEM_SUPER_LBA, 1U, superblock);
        if (result < 0) {
            file_store_error = result;
            return result;
        }
        file_store_ready = 1;
        file_store_error = 0;
        return 0;
    }

    unsigned int version = superblock[8];
    if ((version != 1U && version != 2U && version != 3U &&
         version != FILESYSTEM_VERSION) ||
        superblock[9] != 0U || superblock[10] != 0U || superblock[11] != 0U) {
        file_store_error = FILESYSTEM_FORMAT_ERROR;
        return file_store_error;
    }

    if (version == FILESYSTEM_VERSION) {
        result = filesystem_load_fat();
        if (result < 0) {
            file_store_error = result;
            return result;
        }
    }

    result = block_read_sectors(FILESYSTEM_DIRECTORY_LBA, 1U, directory);
    if (result < 0) {
        file_store_error = result;
        return result;
    }
    for (unsigned int i = 0; i < MAX_FILES; ++i) {
        files[i].used = 0;
        files[i].is_directory = 0;
        files[i].parent = FILE_PARENT_ROOT;
        files[i].name[0] = '\0';
        files[i].content[0] = '\0';
        files[i].first_cluster = FILESYSTEM_CLUSTER_FREE;
        files[i].cluster_count = 0U;
    }

    for (unsigned int slot = 0; slot < MAX_FILES; ++slot) {
        unsigned int offset = slot * FILESYSTEM_ENTRY_SIZE;
        unsigned char used = directory[offset + 18U];
        if (used == 0U) {
            if (version == FILESYSTEM_VERSION) {
                for (unsigned int byte = 0U; byte < FILESYSTEM_ENTRY_SIZE; ++byte) {
                    if (directory[offset + byte] != 0U) {
                        file_store_error = FILESYSTEM_CORRUPT_ERROR;
                        return file_store_error;
                    }
                }
            }
            continue;
        }
        if (used != 1U) {
            file_store_error = FILESYSTEM_CORRUPT_ERROR;
            return file_store_error;
        }

        char name[FILENAME_MAX_LEN + 1U];
        int terminated = 0;
        for (unsigned int i = 0; i <= FILENAME_MAX_LEN; ++i) {
            name[i] = (char)directory[offset + i];
            if (name[i] == '\0') {
                terminated = 1;
                break;
            }
        }
        unsigned char entry_type = version == 1U
            ? ENTRY_TYPE_FILE : directory[offset + 19U];
        unsigned char parent = version == 1U
            ? FILE_PARENT_ROOT : directory[offset + 20U];
        if (!terminated || !file_name_valid(name) ||
            (entry_type != ENTRY_TYPE_FILE && entry_type != ENTRY_TYPE_DIRECTORY) ||
            (parent != FILE_PARENT_ROOT && parent >= MAX_FILES)) {
            file_store_error = FILESYSTEM_CORRUPT_ERROR;
            return file_store_error;
        }

        unsigned int length = (unsigned int)directory[offset + 16U] |
                              ((unsigned int)directory[offset + 17U] << 8);
        unsigned int version_content_limit = version <= 2U ? 256U :
            (version == 3U ? 4096U : FILE_CONTENT_MAX);
        if (length > version_content_limit ||
            (entry_type == ENTRY_TYPE_DIRECTORY && length != 0U)) {
            file_store_error = FILESYSTEM_CORRUPT_ERROR;
            return file_store_error;
        }

        copy_string(files[slot].name, FILENAME_MAX_LEN, name);
        files[slot].used = 1;
        files[slot].is_directory = entry_type == ENTRY_TYPE_DIRECTORY;
        files[slot].parent = parent;
        files[slot].content[0] = '\0';
        if (version == FILESYSTEM_VERSION) {
            for (unsigned int i = 0U; i <= FILENAME_MAX_LEN; ++i) {
                if (name[i] == '\0') {
                    for (unsigned int padding = i + 1U;
                         padding <= FILENAME_MAX_LEN; ++padding) {
                        if (directory[offset + padding] != 0U) {
                            file_store_error = FILESYSTEM_CORRUPT_ERROR;
                            return file_store_error;
                        }
                    }
                    break;
                }
            }
            for (unsigned int byte = 25U; byte < FILESYSTEM_ENTRY_SIZE; ++byte) {
                if (directory[offset + byte] != 0U) {
                    file_store_error = FILESYSTEM_CORRUPT_ERROR;
                    return file_store_error;
                }
            }
            files[slot].first_cluster =
                (unsigned short)directory[offset + 21U] |
                ((unsigned short)directory[offset + 22U] << 8);
            files[slot].cluster_count =
                (unsigned short)directory[offset + 23U] |
                ((unsigned short)directory[offset + 24U] << 8);
            unsigned int expected_count = files[slot].is_directory || length == 0U
                ? 0U : (length + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE;
            if (files[slot].cluster_count != expected_count ||
                (expected_count == 0U &&
                 files[slot].first_cluster != FILESYSTEM_CLUSTER_FREE) ||
                (expected_count != 0U &&
                 files[slot].first_cluster >= FILESYSTEM_CLUSTER_COUNT)) {
                file_store_error = FILESYSTEM_CORRUPT_ERROR;
                return file_store_error;
            }
        }
    }

    for (unsigned int slot = 0; slot < MAX_FILES; ++slot) {
        if (!files[slot].used) {
            continue;
        }
        if (files[slot].parent != FILE_PARENT_ROOT) {
            unsigned int parent = files[slot].parent;
            if (!files[parent].used || !files[parent].is_directory || parent == slot) {
                file_store_error = FILESYSTEM_CORRUPT_ERROR;
                return file_store_error;
            }
        }
        if (entry_find(files[slot].name, files[slot].parent) != (int)slot) {
            file_store_error = FILESYSTEM_CORRUPT_ERROR;
            return file_store_error;
        }

        unsigned int ancestor = files[slot].parent;
        for (unsigned int depth = 0; ancestor != FILE_PARENT_ROOT; ++depth) {
            if (depth >= MAX_FILES || ancestor == slot) {
                file_store_error = FILESYSTEM_CORRUPT_ERROR;
                return file_store_error;
            }
            ancestor = files[ancestor].parent;
        }
    }

    for (unsigned int i = 0; i < FILESYSTEM_CLUSTER_COUNT; ++i) {
        filesystem_cluster_seen[i] = 0U;
    }
    for (unsigned int slot = 0; slot < MAX_FILES; ++slot) {
        if (!files[slot].used || files[slot].is_directory) {
            continue;
        }
        unsigned int offset = slot * FILESYSTEM_ENTRY_SIZE;
        unsigned int length = (unsigned int)directory[offset + 16U] |
                              ((unsigned int)directory[offset + 17U] << 8);
        if (version == FILESYSTEM_VERSION) {
            result = filesystem_read_data_chain(files[slot].first_cluster,
                                                files[slot].cluster_count,
                                                length, files[slot].content);
            if (result < 0) {
                file_store_error = result;
                return result;
            }
        } else {
            unsigned int remaining = length;
            unsigned int content_offset = 0U;
            unsigned int file_sector = 0U;
            while (remaining > 0U) {
                result = block_read_sectors(
                    legacy_filesystem_data_lba(slot, file_sector), 1U,
                    data_sector);
                if (result < 0) {
                    file_store_error = result;
                    return result;
                }
                unsigned int bytes_here = remaining;
                if (bytes_here > DISK_SECTOR_SIZE) {
                    bytes_here = DISK_SECTOR_SIZE;
                }
                for (unsigned int i = 0; i < bytes_here; ++i) {
                    if (data_sector[i] == 0U) {
                        file_store_error = FILESYSTEM_CORRUPT_ERROR;
                        return file_store_error;
                    }
                    files[slot].content[content_offset + i] = (char)data_sector[i];
                }
                remaining -= bytes_here;
                content_offset += bytes_here;
                ++file_sector;
            }
            files[slot].content[length] = '\0';
        }
    }

    current_directory = FILE_PARENT_ROOT;
    if (version < FILESYSTEM_VERSION) {
        /* The new data area was unused by v1-v3. Verify it before migration,
           preserving unknown disk contents rather than overwriting them. */
        result = filesystem_check_zero_range(FILESYSTEM_FAT_LBA,
                                             DISK_TEST_SECTOR - 1U);
        if (result < 0) {
            file_store_error = result;
            return result;
        }
        filesystem_reset_fat();
        for (unsigned int slot = 0; slot < MAX_FILES; ++slot) {
            if (!files[slot].used || files[slot].is_directory) {
                continue;
            }
            unsigned int length;
            if (!filesystem_bounded_content_length(files[slot].content,
                                                   &length)) {
                file_store_error = FILESYSTEM_CORRUPT_ERROR;
                return file_store_error;
            }
            unsigned int count = length == 0U ? 0U :
                (length + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE;
            unsigned short first;
            result = filesystem_allocate_chain(count, &first);
            if (result < 0) {
                file_store_error = result;
                return result;
            }
            files[slot].first_cluster = first;
            files[slot].cluster_count = (unsigned short)count;
            if (count != 0U) {
                result = filesystem_write_data_chain(first, count,
                                                     files[slot].content);
                if (result < 0) {
                    file_store_error = result;
                    return result;
                }
            }
        }
        result = filesystem_write_fat();
        if (result < 0) {
            file_store_error = result;
            return result;
        }
        result = filesystem_write_directory();
        if (result < 0) {
            file_store_error = result;
            return result;
        }
        superblock[8] = FILESYSTEM_VERSION;
        result = block_write_sectors(FILESYSTEM_SUPER_LBA, 1U, superblock);
        if (result < 0) {
            file_store_error = result;
            return result;
        }
    } else {
        /* Preserve unreferenced data until the user reviews it with fsck. */
        int found_orphan = 0;
        for (unsigned int i = 0; i < FILESYSTEM_CLUSTER_COUNT; ++i) {
            if (filesystem_fat[i] != FILESYSTEM_CLUSTER_FREE &&
                !filesystem_cluster_seen[i]) {
                found_orphan = 1;
            }
        }
        if (found_orphan) {
            file_store_error = FILESYSTEM_CORRUPT_ERROR;
            return file_store_error;
        }
    }

    file_store_ready = 1;
    file_store_error = 0;
    return 0;
}

static const char *file_store_error_message(int error)
{
    if (error == FILESYSTEM_FORMAT_ERROR) {
        return "disk area is not a TinyOS filesystem; no data was changed";
    }
    if (error == FILESYSTEM_CORRUPT_ERROR) {
        return "TinyOS filesystem metadata is invalid";
    }
    if (error == FILESYSTEM_FULL_ERROR) {
        return "disk is full; no free data clusters are available";
    }
    return block_error_string(error);
}

static void show_filesystem_space(void)
{
    unsigned int free_clusters = 0U;
    for (unsigned int i = 0; i < FILESYSTEM_CLUSTER_COUNT; ++i) {
        if (filesystem_fat[i] == FILESYSTEM_CLUSTER_FREE) {
            ++free_clusters;
        }
    }
    put_string("Data clusters: ");
    put_unsigned(FILESYSTEM_CLUSTER_COUNT);
    put_string(" (512 bytes each)\nUsed clusters: ");
    put_unsigned(FILESYSTEM_CLUSTER_COUNT - free_clusters);
    put_string("\nFree clusters: ");
    put_unsigned(free_clusters);
    put_string("\nFree bytes: ");
    put_unsigned(free_clusters * DISK_SECTOR_SIZE);
    put_char('\n');
}

static void inspect_file_chain(unsigned int slot)
{
    const struct file_entry *file;
    unsigned int length;
    if (slot >= MAX_FILES || !files[slot].used ||
        !filesystem_bounded_content_length(files[slot].content, &length)) {
        put_string("Filesystem check failed: invalid file entry or unterminated content.\n");
        return;
    }
    file = &files[slot];
    unsigned int expected_count = length == 0U ? 0U :
        (length + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE;
    int result = block_read_sectors(FILESYSTEM_DIRECTORY_LBA, 1U,
                                   filesystem_scan_buffer);
    if (result < 0) {
        put_string("Filesystem check could not read directory: ");
        put_string(block_error_string(result));
        put_char('\n');
        return;
    }

    unsigned int offset = slot * FILESYSTEM_ENTRY_SIZE;
    unsigned char expected_type = file->is_directory
        ? ENTRY_TYPE_DIRECTORY : ENTRY_TYPE_FILE;
    if (filesystem_scan_buffer[offset + 18U] != 1U ||
        filesystem_scan_buffer[offset + 19U] != expected_type ||
        filesystem_scan_buffer[offset + 20U] != file->parent ||
        (unsigned int)filesystem_scan_buffer[offset + 16U] +
            ((unsigned int)filesystem_scan_buffer[offset + 17U] << 8) != length ||
        ((unsigned short)filesystem_scan_buffer[offset + 21U] |
         ((unsigned short)filesystem_scan_buffer[offset + 22U] << 8)) !=
            file->first_cluster ||
        ((unsigned short)filesystem_scan_buffer[offset + 23U] |
         ((unsigned short)filesystem_scan_buffer[offset + 24U] << 8)) !=
            file->cluster_count) {
        put_string("Filesystem check failed: directory metadata differs from disk.\n");
        return;
    }
    for (unsigned int i = 0; i <= FILENAME_MAX_LEN; ++i) {
        if (filesystem_scan_buffer[offset + i] !=
            (unsigned char)file->name[i]) {
            put_string("Filesystem check failed: filename differs from disk.\n");
            return;
        }
    }

    result = block_read_sectors(FILESYSTEM_FAT_LBA, FILESYSTEM_FAT_SECTORS,
                               filesystem_fat_disk);
    if (result < 0) {
        put_string("Filesystem check could not read allocation map: ");
        put_string(block_error_string(result));
        put_char('\n');
        return;
    }
    for (unsigned int i = 0; i < FILESYSTEM_FAT_ENTRIES; ++i) {
        unsigned short disk_value =
            (unsigned short)filesystem_fat_disk[i * 2U] |
            ((unsigned short)filesystem_fat_disk[i * 2U + 1U] << 8);
        if (disk_value != filesystem_fat[i]) {
            put_string("Filesystem check failed: allocation map differs at entry ");
            put_unsigned(i);
            put_char('.');
            put_char('\n');
            return;
        }
        if ((i < FILESYSTEM_CLUSTER_COUNT &&
             disk_value != FILESYSTEM_CLUSTER_FREE &&
             disk_value != FILESYSTEM_CLUSTER_END &&
             disk_value >= FILESYSTEM_CLUSTER_COUNT) ||
            (i >= FILESYSTEM_CLUSTER_COUNT &&
             disk_value != FILESYSTEM_CLUSTER_FREE)) {
            put_string("Filesystem check failed: invalid allocation-map entry ");
            put_unsigned(i);
            put_char('.');
            put_char('\n');
            return;
        }
    }

    if (file->is_directory) {
        put_string("Directories do not have data clusters.\n");
        return;
    }
    if (file->cluster_count != expected_count ||
        (expected_count == 0U &&
         file->first_cluster != FILESYSTEM_CLUSTER_FREE) ||
        (expected_count != 0U &&
         file->first_cluster >= FILESYSTEM_CLUSTER_COUNT)) {
        put_string("Filesystem check failed: file size and cluster metadata disagree.\n");
        return;
    }

    for (unsigned int i = 0; i < FILESYSTEM_CLUSTER_COUNT; ++i) {
        filesystem_cluster_seen[i] = 0U;
    }
    put_string("Cluster chain for ");
    put_string(file->name);
    put_string(": ");
    if (expected_count == 0U) {
        put_string("(empty)");
    }

    unsigned short cluster = file->first_cluster;
    unsigned int content_offset = 0U;
    for (unsigned int i = 0; i < expected_count; ++i) {
        if (cluster >= FILESYSTEM_CLUSTER_COUNT ||
            filesystem_cluster_seen[cluster] ||
            filesystem_fat[cluster] == FILESYSTEM_CLUSTER_FREE) {
            put_string("\nFilesystem check failed: invalid or repeated cluster.\n");
            return;
        }
        filesystem_cluster_seen[cluster] = 1U;
        if (i != 0U) {
            put_string(" -> ");
        }
        put_unsigned(cluster);

        unsigned short next = filesystem_fat[cluster];
        if (i + 1U == expected_count) {
            if (next != FILESYSTEM_CLUSTER_END) {
                put_string("\nFilesystem check failed: chain does not end at the expected cluster.\n");
                return;
            }
        } else {
            if (next == FILESYSTEM_CLUSTER_END ||
                next == FILESYSTEM_CLUSTER_FREE ||
                next >= FILESYSTEM_CLUSTER_COUNT) {
                put_string("\nFilesystem check failed: chain link is invalid or too short.\n");
                return;
            }
        }

        result = block_read_sectors(filesystem_cluster_lba(cluster), 1U,
                                   filesystem_fat_disk);
        if (result < 0) {
            put_string("\nFilesystem check could not read file data: ");
            put_string(block_error_string(result));
            put_char('\n');
            return;
        }
        unsigned int bytes_here = length - content_offset;
        if (bytes_here > DISK_SECTOR_SIZE) {
            bytes_here = DISK_SECTOR_SIZE;
        }
        for (unsigned int byte = 0; byte < bytes_here; ++byte) {
            if (filesystem_fat_disk[byte] !=
                (unsigned char)file->content[content_offset + byte]) {
                put_string("\nFilesystem check failed: data mismatch at byte ");
                put_unsigned(content_offset + byte);
                put_string(".\n");
                return;
            }
        }
        for (unsigned int byte = bytes_here; byte < DISK_SECTOR_SIZE; ++byte) {
            if (filesystem_fat_disk[byte] != 0U) {
                put_string("\nFilesystem check failed: nonzero data after end of file.\n");
                return;
            }
        }
        content_offset += bytes_here;
        cluster = next;
    }

    /* Check other live entries too, so a shared cluster or orphan is caught. */
    for (unsigned int other = 0; other < MAX_FILES; ++other) {
        if (other == slot || !files[other].used || files[other].is_directory) {
            continue;
        }
        unsigned int other_length;
        if (!filesystem_bounded_content_length(files[other].content,
                                               &other_length)) {
            put_string("\nFilesystem check failed: another file has unterminated content.\n");
            return;
        }
        unsigned int other_count = other_length == 0U ? 0U :
            (other_length + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE;
        if (files[other].cluster_count != other_count ||
            (other_count == 0U &&
             files[other].first_cluster != FILESYSTEM_CLUSTER_FREE) ||
            (other_count != 0U &&
             files[other].first_cluster >= FILESYSTEM_CLUSTER_COUNT)) {
            put_string("\nFilesystem check failed: another file has invalid cluster metadata.\n");
            return;
        }
        cluster = files[other].first_cluster;
        for (unsigned int i = 0; i < other_count; ++i) {
            if (cluster >= FILESYSTEM_CLUSTER_COUNT ||
                filesystem_cluster_seen[cluster] ||
                filesystem_fat[cluster] == FILESYSTEM_CLUSTER_FREE) {
                put_string("\nFilesystem check failed: files share a cluster or contain a loop.\n");
                return;
            }
            filesystem_cluster_seen[cluster] = 1U;
            unsigned short next = filesystem_fat[cluster];
            if (i + 1U == other_count) {
                if (next != FILESYSTEM_CLUSTER_END) {
                    put_string("\nFilesystem check failed: another file has an invalid chain end.\n");
                    return;
                }
            } else {
                if (next == FILESYSTEM_CLUSTER_END ||
                    next == FILESYSTEM_CLUSTER_FREE ||
                    next >= FILESYSTEM_CLUSTER_COUNT) {
                    put_string("\nFilesystem check failed: another file has a broken chain.\n");
                    return;
                }
            }
            cluster = next;
        }
    }
    for (unsigned int i = 0; i < FILESYSTEM_CLUSTER_COUNT; ++i) {
        if (filesystem_fat[i] != FILESYSTEM_CLUSTER_FREE &&
            !filesystem_cluster_seen[i]) {
            put_string("\nFilesystem check failed: orphaned allocated cluster ");
            put_unsigned(i);
            put_string(".\n");
            return;
        }
    }
    put_string("\nFilesystem check passed: directory, chains, and file data match.\n");
}

struct filesystem_audit_summary {
    unsigned int entries;
    unsigned int files;
    unsigned int directories;
    unsigned int clusters_checked;
    unsigned int errors;
    unsigned int orphans;
    int scan_complete;
};

static void filesystem_audit_error(struct filesystem_audit_summary *summary,
                                   const char *message)
{
    ++summary->errors;
    put_string("ERROR: ");
    put_string(message);
    put_char('\n');
}

static unsigned char filesystem_expected_entry_byte(unsigned int slot,
                                                     unsigned int length,
                                                     unsigned int byte)
{
    const struct file_entry *file = &files[slot];
    if (!file->used) {
        return 0U;
    }
    if (byte <= FILENAME_MAX_LEN) {
        return (unsigned char)file->name[byte];
    }
    switch (byte) {
        case 16U: return (unsigned char)(length & 0xFFU);
        case 17U: return (unsigned char)((length >> 8) & 0xFFU);
        case 18U: return 1U;
        case 19U: return file->is_directory
            ? ENTRY_TYPE_DIRECTORY : ENTRY_TYPE_FILE;
        case 20U: return file->parent;
        case 21U: return (unsigned char)(file->first_cluster & 0xFFU);
        case 22U: return (unsigned char)(file->first_cluster >> 8);
        case 23U: return (unsigned char)(file->cluster_count & 0xFFU);
        case 24U: return (unsigned char)(file->cluster_count >> 8);
        default: return 0U;
    }
}

static void filesystem_run_audit(struct filesystem_audit_summary *summary)
{
    unsigned int lengths[MAX_FILES];
    unsigned char content_valid[MAX_FILES];

    summary->entries = 0U;
    summary->files = 0U;
    summary->directories = 0U;
    summary->clusters_checked = 0U;
    summary->errors = 0U;
    summary->orphans = 0U;
    summary->scan_complete = 0;

    int result = block_read_sectors(FILESYSTEM_DIRECTORY_LBA, 1U,
                                   filesystem_scan_buffer);
    if (result < 0) {
        filesystem_audit_error(summary, "could not read the directory sector.");
        put_string(block_error_string(result));
        put_char('\n');
        return;
    }

    for (unsigned int slot = 0; slot < MAX_FILES; ++slot) {
        lengths[slot] = 0U;
        content_valid[slot] = 1U;
        if (files[slot].used) {
            ++summary->entries;
            if (files[slot].used != 1 ||
                (files[slot].is_directory != 0 &&
                 files[slot].is_directory != 1)) {
                filesystem_audit_error(summary,
                                       "an entry has an invalid in-memory type flag.");
            }
            if (files[slot].is_directory) {
                ++summary->directories;
            } else {
                ++summary->files;
            }
            if (!filesystem_bounded_content_length(files[slot].content,
                                                   &lengths[slot])) {
                content_valid[slot] = 0U;
                filesystem_audit_error(summary,
                                       "a file content buffer is not terminated.");
            }

            int name_terminated = 0;
            for (unsigned int i = 0; i <= FILENAME_MAX_LEN; ++i) {
                if (files[slot].name[i] == '\0') {
                    name_terminated = 1;
                    break;
                }
            }
            if (!name_terminated || !file_name_valid(files[slot].name)) {
                filesystem_audit_error(summary,
                                       "a directory entry has an invalid name.");
            }
            if (files[slot].is_directory && lengths[slot] != 0U) {
                filesystem_audit_error(summary,
                                       "a directory entry contains file data.");
            }
            if (files[slot].parent != FILE_PARENT_ROOT &&
                (files[slot].parent >= MAX_FILES ||
                 !files[files[slot].parent].used ||
                 !files[files[slot].parent].is_directory ||
                 files[slot].parent == slot)) {
                filesystem_audit_error(summary,
                                       "a directory entry has an invalid parent.");
            }
            unsigned int ancestor = files[slot].parent;
            for (unsigned int depth = 0;
                 ancestor != FILE_PARENT_ROOT && ancestor < MAX_FILES;
                 ++depth) {
                if (depth >= MAX_FILES || ancestor == slot) {
                    filesystem_audit_error(summary,
                                           "the directory tree contains a cycle.");
                    break;
                }
                ancestor = files[ancestor].parent;
            }
            for (unsigned int other = 0; other < slot; ++other) {
                if (!files[other].used ||
                    files[other].parent != files[slot].parent) {
                    continue;
                }
                unsigned int i = 0U;
                while (i <= FILENAME_MAX_LEN &&
                       files[other].name[i] == files[slot].name[i] &&
                       files[slot].name[i] != '\0') {
                    ++i;
                }
                if (i <= FILENAME_MAX_LEN &&
                    files[other].name[i] == '\0' &&
                    files[slot].name[i] == '\0') {
                    filesystem_audit_error(summary,
                                           "duplicate names exist in one directory.");
                    break;
                }
            }
        }

        unsigned int offset = slot * FILESYSTEM_ENTRY_SIZE;
        for (unsigned int byte = 0; byte < FILESYSTEM_ENTRY_SIZE; ++byte) {
            unsigned char expected = filesystem_expected_entry_byte(
                slot, lengths[slot], byte);
            if (filesystem_scan_buffer[offset + byte] != expected) {
                filesystem_audit_error(summary,
                                       "a directory entry differs from the disk.");
                break;
            }
        }
    }

    result = block_read_sectors(FILESYSTEM_FAT_LBA, FILESYSTEM_FAT_SECTORS,
                               filesystem_fat_disk);
    if (result < 0) {
        filesystem_audit_error(summary,
                               "could not read the allocation map.");
        put_string(block_error_string(result));
        put_char('\n');
        return;
    }

    int allocation_map_matches = 1;
    for (unsigned int i = 0; i < FILESYSTEM_FAT_ENTRIES; ++i) {
        unsigned short disk_value =
            (unsigned short)filesystem_fat_disk[i * 2U] |
            ((unsigned short)filesystem_fat_disk[i * 2U + 1U] << 8);
        if (disk_value != filesystem_fat[i]) {
            if (allocation_map_matches) {
                filesystem_audit_error(summary,
                                       "the on-disk allocation map differs from memory.");
                put_string("First differing allocation entry: ");
                put_unsigned(i);
                put_char('\n');
            }
            allocation_map_matches = 0;
        }
        if ((i < FILESYSTEM_CLUSTER_COUNT &&
             disk_value != FILESYSTEM_CLUSTER_FREE &&
             disk_value != FILESYSTEM_CLUSTER_END &&
             disk_value >= FILESYSTEM_CLUSTER_COUNT) ||
            (i >= FILESYSTEM_CLUSTER_COUNT &&
             disk_value != FILESYSTEM_CLUSTER_FREE)) {
            filesystem_audit_error(summary,
                                   "the allocation map contains an invalid value.");
            allocation_map_matches = 0;
        }
    }
    if (!allocation_map_matches) {
        return;
    }

    for (unsigned int i = 0; i < FILESYSTEM_CLUSTER_COUNT; ++i) {
        filesystem_cluster_seen[i] = 0U;
    }
    for (unsigned int slot = 0; slot < MAX_FILES; ++slot) {
        if (!files[slot].used) {
            continue;
        }
        if (files[slot].is_directory) {
            if (files[slot].cluster_count != 0U ||
                files[slot].first_cluster != FILESYSTEM_CLUSTER_FREE) {
                filesystem_audit_error(summary,
                                       "a directory has allocated data clusters.");
            }
            continue;
        }
        if (!content_valid[slot]) {
            continue;
        }

        unsigned int expected_count = lengths[slot] == 0U ? 0U :
            (lengths[slot] + DISK_SECTOR_SIZE - 1U) / DISK_SECTOR_SIZE;
        if (files[slot].cluster_count != expected_count ||
            (expected_count == 0U &&
             files[slot].first_cluster != FILESYSTEM_CLUSTER_FREE) ||
            (expected_count != 0U &&
             files[slot].first_cluster >= FILESYSTEM_CLUSTER_COUNT)) {
            filesystem_audit_error(summary,
                                   "file size and cluster metadata disagree.");
            continue;
        }

        unsigned short cluster = files[slot].first_cluster;
        unsigned int content_offset = 0U;
        for (unsigned int i = 0; i < expected_count; ++i) {
            if (cluster >= FILESYSTEM_CLUSTER_COUNT ||
                filesystem_cluster_seen[cluster] ||
                filesystem_fat[cluster] == FILESYSTEM_CLUSTER_FREE) {
                filesystem_audit_error(summary,
                                       "a file chain contains a loop or shared cluster.");
                break;
            }
            filesystem_cluster_seen[cluster] = 1U;
            ++summary->clusters_checked;

            unsigned short next = filesystem_fat[cluster];
            if (i + 1U == expected_count) {
                if (next != FILESYSTEM_CLUSTER_END) {
                    filesystem_audit_error(summary,
                                           "a file chain does not end at its expected length.");
                    break;
                }
            } else if (next == FILESYSTEM_CLUSTER_END ||
                       next == FILESYSTEM_CLUSTER_FREE ||
                       next >= FILESYSTEM_CLUSTER_COUNT) {
                filesystem_audit_error(summary,
                                       "a file chain has a broken link.");
                break;
            }

            result = block_read_sectors(filesystem_cluster_lba(cluster), 1U,
                                       filesystem_fat_disk);
            if (result < 0) {
                filesystem_audit_error(summary,
                                       "could not read a file data sector.");
                put_string(block_error_string(result));
                put_char('\n');
                content_offset = lengths[slot];
                break;
            }
            unsigned int bytes_here = lengths[slot] - content_offset;
            if (bytes_here > DISK_SECTOR_SIZE) {
                bytes_here = DISK_SECTOR_SIZE;
            }
            int data_matches = 1;
            for (unsigned int byte = 0; byte < bytes_here; ++byte) {
                if (filesystem_fat_disk[byte] !=
                    (unsigned char)files[slot].content[content_offset + byte]) {
                    data_matches = 0;
                    break;
                }
            }
            for (unsigned int byte = bytes_here;
                 data_matches && byte < DISK_SECTOR_SIZE; ++byte) {
                if (filesystem_fat_disk[byte] != 0U) {
                    data_matches = 0;
                }
            }
            if (!data_matches) {
                filesystem_audit_error(summary,
                                       "a file data sector differs from its contents.");
            }
            content_offset += bytes_here;
            cluster = next;
        }
    }

    for (unsigned int i = 0; i < FILESYSTEM_CLUSTER_COUNT; ++i) {
        if (filesystem_fat[i] != FILESYSTEM_CLUSTER_FREE &&
            !filesystem_cluster_seen[i]) {
            ++summary->orphans;
        }
    }
    summary->scan_complete = 1;
}

static void show_filesystem_audit_summary(
    const struct filesystem_audit_summary *summary)
{
    put_string("Entries checked: ");
    put_unsigned(summary->entries);
    put_string(" (files: ");
    put_unsigned(summary->files);
    put_string(", directories: ");
    put_unsigned(summary->directories);
    put_string(")\nClusters checked: ");
    put_unsigned(summary->clusters_checked);
    put_string("\nErrors: ");
    put_unsigned(summary->errors);
    put_string("\nUnreferenced allocated clusters: ");
    put_unsigned(summary->orphans);
    put_char('\n');
    if (summary->scan_complete && summary->errors == 0U &&
        summary->orphans == 0U) {
        put_string("Filesystem is clean.\n");
    } else if (summary->scan_complete && summary->errors == 0U) {
        put_string("Only unreferenced clusters were found. Run fsck repair to preview safe cleanup.\n");
    } else {
        put_string("Filesystem has errors; automatic repair is disabled.\n");
    }
}

static void run_filesystem_check(void)
{
    struct filesystem_audit_summary summary;
    put_string("Read-only filesystem check:\n");
    filesystem_run_audit(&summary);
    show_filesystem_audit_summary(&summary);
}

static void run_filesystem_repair_preview(int confirm)
{
    struct filesystem_audit_summary summary;
    put_string("Filesystem repair safety check:\n");
    filesystem_run_audit(&summary);
    show_filesystem_audit_summary(&summary);
    if (!summary.scan_complete || summary.errors != 0U) {
        put_string("Repair refused: resolve filesystem errors before cleanup.\n");
        return;
    }
    if (summary.orphans == 0U) {
        put_string("No orphaned clusters need cleanup.\n");
        return;
    }
    if (!confirm) {
        put_string("Preview: only unreferenced clusters would be marked free.\n");
        put_string("To apply this repair, enter: fsck repair confirm\n");
        return;
    }

    unsigned int reclaimed = 0U;
    for (unsigned int i = 0; i < FILESYSTEM_CLUSTER_COUNT; ++i) {
        if (filesystem_fat[i] != FILESYSTEM_CLUSTER_FREE &&
            !filesystem_cluster_seen[i]) {
            filesystem_fat[i] = FILESYSTEM_CLUSTER_FREE;
            ++reclaimed;
        }
    }
    int result = filesystem_write_fat();
    if (result < 0) {
        int reload_result = filesystem_load_fat();
        put_string("Repair write failed: ");
        put_string(block_error_string(result));
        put_char('\n');
        if (reload_result < 0) {
            file_store_ready = 0;
            file_store_error = reload_result;
            put_string("Could not reload the allocation map; file commands are disabled until reboot.\n");
        } else {
            put_string("Allocation map reloaded. Run fsck again to inspect the result.\n");
        }
        return;
    }
    file_store_ready = 1;
    file_store_error = 0;
    put_string("Repair complete: reclaimed ");
    put_unsigned(reclaimed);
    put_string(" unreferenced clusters; file contents were not changed.\n");
}

static int filesystem_command_needs_audit(void)
{
    return command_is("ls") || command_is("pwd") || command_is("df") ||
           command_is("fsbounds") ||
           command_is("install hello") || command_is("run") ||
           command_starts_with("run ") || command_starts_with("mkdir ") ||
           command_starts_with("cd ") || command_starts_with("rmdir ") ||
           command_starts_with("touch ") || command_starts_with("write ") ||
           command_starts_with("cat ") || command_starts_with("rm ") ||
           command_is("rename") || command_starts_with("rename ") ||
           command_is("cp") || command_starts_with("cp ");
}

static int filesystem_preflight_audit(void)
{
    struct filesystem_audit_summary summary;
    filesystem_run_audit(&summary);
    if (!summary.scan_complete || summary.errors != 0U ||
        summary.orphans != 0U) {
        show_filesystem_audit_summary(&summary);
        put_string("Operation refused: filesystem is not clean; inspect with fsck.\n");
        return 0;
    }
    return 1;
}

static void run_filesystem_bounds_demo(void)
{
    if (!file_store_ready) {
        report_file_store_unavailable();
        return;
    }

    for (unsigned int i = 0U; i < FILESYSTEM_FAT_ENTRIES; ++i) {
        filesystem_fat_backup[i] = filesystem_fat[i];
    }
    for (unsigned int i = 0U; i < FILESYSTEM_CLUSTER_COUNT; ++i) {
        filesystem_fat[i] = FILESYSTEM_CLUSTER_END;
    }

    unsigned short first = FILESYSTEM_CLUSTER_FREE;
    int full_result = filesystem_allocate_chain(1U, &first);
    int too_many_result = filesystem_allocate_chain(
        FILESYSTEM_MAX_FILE_SECTORS + 1U, &first);
    int null_result = filesystem_allocate_chain(1U, 0);
    int bad_write_result = filesystem_write_data_chain(
        (unsigned short)FILESYSTEM_CLUSTER_COUNT, 1U, "x");
    char read_buffer[2];
    int bad_read_result = filesystem_read_data_chain(
        (unsigned short)FILESYSTEM_CLUSTER_COUNT, 1U, 1U, read_buffer);
    unsigned char io_probe[DISK_SECTOR_SIZE];
    for (unsigned int i = 0U; i < DISK_SECTOR_SIZE; ++i) {
        io_probe[i] = 0U;
    }
    unsigned int disk_sectors = block_sector_count();
    int bad_lba_read = block_read_sectors(disk_sectors, 1U, io_probe);
    int bad_lba_write = block_write_sectors(disk_sectors, 1U, io_probe);
    int zero_count_read = block_read_sectors(0U, 0U, io_probe);

    for (unsigned int i = 0U; i < FILESYSTEM_FAT_ENTRIES; ++i) {
        filesystem_fat[i] = filesystem_fat_backup[i];
    }

    if (full_result == FILESYSTEM_FULL_ERROR &&
        too_many_result == FILESYSTEM_CORRUPT_ERROR &&
        null_result == FILESYSTEM_CORRUPT_ERROR &&
        bad_write_result == FILESYSTEM_CORRUPT_ERROR &&
        bad_read_result == FILESYSTEM_CORRUPT_ERROR &&
        bad_lba_read < 0 && bad_lba_write < 0 && zero_count_read < 0) {
        put_string("Filesystem bounds demo passed: full allocation and invalid filesystem/disk read-write bounds were rejected; disk data was untouched.\n");
    } else {
        put_string("Filesystem bounds demo failed: full=");
        put_hex32((unsigned int)full_result);
        put_string(" oversized=");
        put_hex32((unsigned int)too_many_result);
        put_string(" null=");
        put_hex32((unsigned int)null_result);
        put_string(" bad-write=");
        put_hex32((unsigned int)bad_write_result);
        put_string(" bad-read=");
        put_hex32((unsigned int)bad_read_result);
        put_string(" bad-LBA-read=");
        put_hex32((unsigned int)bad_lba_read);
        put_string(" bad-LBA-write=");
        put_hex32((unsigned int)bad_lba_write);
        put_string(" zero-count=");
        put_hex32((unsigned int)zero_count_read);
        put_char('\n');
    }
}

/* --- End file store helpers --- */

static struct idt_entry idt[256];

/* These helpers are defined below initialize_idt(). */
static void set_irq_gates(unsigned short selector);

/* Save the interrupted task's IRQ frame and select the next ready task.
   This bounded state switch does not scan deadlines or perform cleanup. */
static unsigned int schedule_from_timer(unsigned int saved_stack_pointer)
{
    unsigned int previous_id = current_task_id;
    unsigned int next_id = previous_id;

    tasks[previous_id].interrupt_esp = saved_stack_pointer;
    if (tasks[previous_id].state == TASK_RUNNING) {
        tasks[previous_id].state = TASK_READY;
    }

    for (unsigned int offset = 1U; offset <= MAX_TASKS; ++offset) {
        unsigned int candidate = (previous_id + offset) % MAX_TASKS;
        if (tasks[candidate].state == TASK_READY) {
            next_id = candidate;
            break;
        }
    }

    if (next_id == previous_id) {
        if (tasks[previous_id].state == TASK_READY) {
            tasks[previous_id].state = TASK_RUNNING;
        }
        return saved_stack_pointer;
    }

    tasks[next_id].state = TASK_RUNNING;
    current_task_id = next_id;
    ++timer_preemption_count;
    return tasks[next_id].interrupt_esp;
}

static void set_idt_gate(unsigned int vector, unsigned int handler,
                         unsigned short selector)
{
    idt[vector].offset_low = (unsigned short)(handler & 0xFFFFU);
    idt[vector].selector = selector;
    idt[vector].zero = 0;
    idt[vector].flags = (vector == 3U || vector == 128U) ? 0xEEU : 0x8EU;
    idt[vector].offset_high = (unsigned short)((handler >> 16) & 0xFFFFU);
}

static void initialize_idt(void)
{
    static void (*const stubs[32])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    unsigned short code_selector;
    __asm__ volatile ("mov %%cs, %0" : "=r"(code_selector));

    for (unsigned int i = 0; i < 256U; ++i) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].zero = 0;
        idt[i].flags = 0;
        idt[i].offset_high = 0;
    }

    for (unsigned int i = 0; i < 32U; ++i) {
        set_idt_gate(i, (unsigned int)stubs[i], code_selector);
    }
    set_idt_gate(128U, (unsigned int)syscall128, code_selector);

    /* Set up IRQ gates for hardware interrupts 32-47 */
    set_irq_gates(code_selector);

    pic_initialize();
    timer_initialize();
}

/* Set up IRQ gates for hardware interrupts 32-47 */
static void set_irq_gates(unsigned short selector)
{
    extern void irq32(void), irq33(void), irq34(void), irq35(void);
    extern void irq36(void), irq37(void), irq38(void), irq39(void);
    extern void irq40(void), irq41(void), irq42(void), irq43(void);
    extern void irq44(void), irq45(void), irq46(void), irq47(void);

    static void (*const irq_stubs[16])(void) = {
        irq32, irq33, irq34, irq35, irq36, irq37, irq38, irq39,
        irq40, irq41, irq42, irq43, irq44, irq45, irq46, irq47
    };

    for (unsigned int i = 0; i < 16U; ++i) {
        unsigned int vector = 32U + i;
        idt[vector].offset_low = (unsigned short)((unsigned int)irq_stubs[i] & 0xFFFFU);
        idt[vector].selector = selector;
        idt[vector].zero = 0;
        idt[vector].flags = 0x8E; /* Present, 32-bit interrupt gate, Ring 0 */
        idt[vector].offset_high = (unsigned short)(((unsigned int)irq_stubs[i] >> 16) & 0xFFFFU);
    }

    struct idt_pointer pointer;
    pointer.limit = (unsigned short)(sizeof(idt) - 1U);
    pointer.base = (unsigned int)idt;
    idt_load(&pointer);
}

unsigned int irq_handler(unsigned int irq_vector,
                         unsigned int saved_stack_pointer)
{
    unsigned int return_stack = saved_stack_pointer;
    switch (irq_vector) {
        case 32: /* IRQ0: PIT timer */
            timer_handle_interrupt();
            task_wakeup_pending = 1;
            pic_send_eoi(irq_vector);
            if (timer_preemption_enabled && task_model_ready && task_count > 1U) {
                return_stack = schedule_from_timer(saved_stack_pointer);
            }
            break;

        case 33: /* IRQ1: PS/2 keyboard */
            keyboard_handle_interrupt();
            pic_send_eoi(irq_vector);
            break;

        case 44: /* IRQ12: PS/2 mouse */
            mouse_handle_interrupt();
            pic_send_eoi(irq_vector);
            break;

        default:
            pic_send_eoi(irq_vector);
            break;
    }
    return return_stack;
}
static void report_page_fault(unsigned int error_code, unsigned int address)
{
    put_string("Page fault address: 0x");
    put_hex32(address);
    put_char('\n');

    if ((error_code & 0x01U) != 0U) {
        put_string("Reason: protection violation (page is present, access denied).\n");
    } else {
        put_string("Reason: non-present page (address is not mapped).\n");
    }
    put_string("Access: ");
    put_string((error_code & 0x02U) != 0U ? "write\n" : "read\n");
    put_string("Privilege: ");
    put_string((error_code & 0x04U) != 0U ? "user mode\n" : "supervisor mode\n");
    if ((error_code & 0x08U) != 0U) {
        put_string("Detail: reserved page-table bit was set.\n");
    }
    if ((error_code & 0x10U) != 0U) {
        put_string("Operation: instruction fetch.\n");
    } else {
        put_string("Operation: data access.\n");
    }
}

static void show_page_fault_demo(void)
{
    put_string("Simulated page-fault reports (no fault is triggered):\n");
    put_string("Example 1: read from an unmapped address\n");
    report_page_fault(0x00U, 0x08000000U);
    put_string("Example 2: user-mode instruction fetch blocked by protection\n");
    report_page_fault(0x15U, 0x00400000U);
}

struct exception_stack_frame {
    unsigned int vector;
    unsigned int error_code;
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
    unsigned int user_esp;
    unsigned int user_ss;
} __attribute__((packed));

void exception_handler(unsigned int vector, unsigned int error_code,
                       struct exception_stack_frame *frame)
{
    if (vector == 3U && ring3_demo_active != 0 && frame != 0 &&
        (frame->cs & 3U) == 3U) {
        frame->eip = (unsigned int)ring3_demo_return_trampoline;
        frame->cs = 0x08U;
        ring3_demo_trap_seen = 1;
        return;
    }
    if (vector == 14U && user_demo_active != 0 && frame != 0 &&
        (frame->cs & 3U) == 3U) {
        unsigned int cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        if (cr2 == 0x000B8000U && (error_code & 0x07U) == 0x07U) {
            frame->eip = (unsigned int)ring3_demo_return_trampoline;
            frame->cs = 0x08U;
            user_protection_fault_seen = 1;
            return;
        }
    }
    clear_screen();
    put_string("TinyOS stopped after a CPU exception.\n");
    put_string("Exception vector: 0x");
    put_hex32(vector);
    put_string("\nError code: 0x");
    put_hex32(error_code);
    put_string("\n");
    if (vector == 14U) {
        unsigned int cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        report_page_fault(error_code, cr2);
    }
    put_string("System halted.\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* int 0x80 saves the user register set before entering this C dispatcher. */
struct syscall_cpu_frame {
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int saved_esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
    unsigned int vector;
    unsigned int error_code;
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
    unsigned int user_esp;
    unsigned int user_ss;
} __attribute__((packed));

enum syscall_number {
    SYSCALL_WRITE = 1U,
    SYSCALL_EXIT = 2U,
    SYSCALL_YIELD = 3U,
    SYSCALL_FILE_OPEN = 4U,
    SYSCALL_FILE_READ = 5U,
    SYSCALL_FILE_WRITE = 6U,
    SYSCALL_FILE_CLOSE = 7U
};

static int syscall_copy_filename(unsigned int address,
                                 char name[FILENAME_MAX_LEN + 1U])
{
    if (address == 0U) {
        return 0;
    }
    for (unsigned int i = 0U; i <= FILENAME_MAX_LEN; ++i) {
        if (address > 0xFFFFFFFFU - i ||
            !syscall_user_buffer_readable(address + i, 1U)) {
            return 0;
        }
        char character = ((const char *)address)[i];
        name[i] = character;
        if (character == '\0') {
            return file_name_valid(name);
        }
    }
    return 0;
}

static int syscall_open_file(unsigned int name_address, unsigned int flags)
{
    char name[FILENAME_MAX_LEN + 1U];
    unsigned int handle_index = MAX_APP_FILE_HANDLES;
    for (unsigned int i = 0U; i < MAX_APP_FILE_HANDLES; ++i) {
        if (!app_file_handles[i].open) {
            handle_index = i;
            break;
        }
    }
    if (!file_store_ready ||
        handle_index >= MAX_APP_FILE_HANDLES ||
        !syscall_copy_filename(name_address, name) ||
        (flags & ~(APP_FILE_READ | APP_FILE_WRITE | APP_FILE_CREATE |
                   APP_FILE_TRUNCATE)) != 0U ||
        (flags & (APP_FILE_READ | APP_FILE_WRITE)) == 0U ||
        ((flags & APP_FILE_TRUNCATE) != 0U &&
         (flags & APP_FILE_WRITE) == 0U) ||
        ((flags & APP_FILE_CREATE) != 0U &&
         (flags & APP_FILE_WRITE) == 0U)) {
        return -1;
    }

    int slot = entry_find(name, current_directory);
    if (slot < 0 && (flags & APP_FILE_CREATE) != 0U) {
        slot = file_free_slot();
        if (slot < 0) {
            return -1;
        }
        file_entry_copy(&file_entry_backup, &files[slot]);
        files[slot].name[0] = '\0';
        files[slot].content[0] = '\0';
        if (!copy_string(files[slot].name, FILENAME_MAX_LEN, name)) {
            file_entry_copy(&files[slot], &file_entry_backup);
            return -1;
        }
        files[slot].used = 1;
        files[slot].is_directory = 0;
        files[slot].parent = current_directory;
        files[slot].first_cluster = FILESYSTEM_CLUSTER_FREE;
        files[slot].cluster_count = 0U;
        if (filesystem_save_change((unsigned int)slot, &file_entry_backup) < 0) {
            return -1;
        }
    }
    if (slot < 0 || files[slot].is_directory) {
        return -1;
    }

    if ((flags & APP_FILE_TRUNCATE) != 0U) {
        file_entry_copy(&file_entry_backup, &files[slot]);
        files[slot].content[0] = '\0';
        if (filesystem_save_change((unsigned int)slot,
                                   &file_entry_backup) < 0) {
            return -1;
        }
    }

    app_file_handles[handle_index].slot = (unsigned int)slot;
    app_file_handles[handle_index].offset = 0U;
    app_file_handles[handle_index].flags = flags;
    app_file_handles[handle_index].open = 1;
    return (int)(handle_index + 1U);
}

static int syscall_read_file(unsigned int descriptor, unsigned int address,
                             unsigned int length)
{
    if (descriptor == 0U || descriptor > MAX_APP_FILE_HANDLES ||
        length > SYSCALL_WRITE_MAX ||
        !app_file_handles[descriptor - 1U].open ||
        (app_file_handles[descriptor - 1U].flags & APP_FILE_READ) == 0U ||
        (length != 0U && !syscall_user_buffer_writable(address, length))) {
        return -1;
    }

    struct app_file_handle *handle = &app_file_handles[descriptor - 1U];
    unsigned int content_length;
    if (handle->slot >= MAX_FILES ||
        !filesystem_bounded_content_length(
            files[handle->slot].content, &content_length)) {
        return -1;
    }
    if (handle->offset >= content_length || length == 0U) {
        return 0;
    }
    unsigned int count = content_length - handle->offset;
    if (count > length) {
        count = length;
    }
    unsigned char *destination = (unsigned char *)address;
    const char *source = files[handle->slot].content + handle->offset;
    for (unsigned int i = 0U; i < count; ++i) {
        destination[i] = (unsigned char)source[i];
    }
    handle->offset += count;
    return (int)count;
}

static int syscall_write_file(unsigned int descriptor, unsigned int address,
                              unsigned int length)
{
    if (descriptor == 0U || descriptor > MAX_APP_FILE_HANDLES ||
        length > SYSCALL_WRITE_MAX ||
        !app_file_handles[descriptor - 1U].open ||
        (app_file_handles[descriptor - 1U].flags & APP_FILE_WRITE) == 0U) {
        return -1;
    }
    if (length == 0U) {
        return 0;
    }
    if (!syscall_user_buffer_readable(address, length)) {
        return -1;
    }

    struct app_file_handle *handle = &app_file_handles[descriptor - 1U];
    unsigned int old_length;
    if (handle->slot >= MAX_FILES ||
        !filesystem_bounded_content_length(
            files[handle->slot].content, &old_length) ||
        handle->offset > FILE_CONTENT_MAX ||
        length > FILE_CONTENT_MAX - handle->offset) {
        return -1;
    }
    const unsigned char *source = (const unsigned char *)address;
    for (unsigned int i = 0U; i < length; ++i) {
        if (source[i] == 0U) {
            return -1;
        }
    }

    file_entry_copy(&file_entry_backup, &files[handle->slot]);
    unsigned int end = handle->offset + length;
    unsigned int new_length = old_length > end ? old_length : end;
    while (old_length < handle->offset) {
        files[handle->slot].content[old_length++] = ' ';
    }
    for (unsigned int i = 0U; i < length; ++i) {
        files[handle->slot].content[handle->offset + i] = (char)source[i];
    }
    files[handle->slot].content[new_length] = '\0';
    if (filesystem_save_change(handle->slot, &file_entry_backup) < 0) {
        return -1;
    }
    handle->offset = end;
    return (int)length;
}

void syscall_handler(struct syscall_cpu_frame *frame)
{
    if (frame == 0 || frame->vector != 128U ||
        (frame->cs & 3U) != 3U ||
        (syscall_demo_active == 0 && embedded_app_active == 0)) {
        return;
    }

    switch (frame->eax) {
        case SYSCALL_WRITE:
            if (syscall_demo_active != 0) {
                if (frame->ebx == 0x000B8000U && frame->ecx == 4U) {
                    syscall_demo_bad_pointer_seen = 1;
                } else if (frame->ebx == USER_STACK_ADDRESS + PAGE_SIZE - 1U &&
                           frame->ecx == 2U) {
                    syscall_demo_cross_page_seen = 1;
                } else if (frame->ecx > SYSCALL_WRITE_MAX) {
                    syscall_demo_large_buffer_seen = 1;
                }
            }

            if (!syscall_user_buffer_readable(frame->ebx, frame->ecx)) {
                frame->eax = 0xFFFFFFFFU;
                break;
            }

            const unsigned char *message =
                (const unsigned char *)frame->ebx;
            for (unsigned int i = 0; i < frame->ecx; ++i) {
                put_char((char)message[i]);
            }
            if (syscall_demo_active != 0) {
                syscall_demo_write_seen = 1;
            }
            frame->eax = frame->ecx;
            break;

        case SYSCALL_YIELD:
            if (syscall_demo_active != 0) {
                ++syscall_demo_yield_seen;
            }
            task_yield();
            frame->eax = 0U;
            break;

        case SYSCALL_FILE_OPEN:
            frame->eax = (unsigned int)syscall_open_file(frame->ebx,
                                                         frame->ecx);
            break;

        case SYSCALL_FILE_READ:
            frame->eax = (unsigned int)syscall_read_file(frame->ebx,
                                                         frame->ecx,
                                                         frame->edx);
            break;

        case SYSCALL_FILE_WRITE:
            frame->eax = (unsigned int)syscall_write_file(frame->ebx,
                                                           frame->ecx,
                                                           frame->edx);
            break;

        case SYSCALL_FILE_CLOSE:
            if (frame->ebx != 0U && frame->ebx <= MAX_APP_FILE_HANDLES &&
                app_file_handles[frame->ebx - 1U].open) {
                app_file_handles[frame->ebx - 1U].open = 0;
                frame->eax = 0U;
            } else {
                frame->eax = 0xFFFFFFFFU;
            }
            break;

        case SYSCALL_EXIT:
            if (syscall_demo_active != 0) {
                syscall_demo_exit_seen = 1;
                syscall_demo_exit_status = frame->ebx;
            }
            if (embedded_app_active != 0) {
                embedded_app_exit_seen = 1;
                embedded_app_exit_status = frame->ebx;
            }
            frame->eip = (unsigned int)ring3_demo_return_trampoline;
            frame->cs = 0x08U;
            break;

        default:
            if (syscall_demo_active != 0) {
                syscall_demo_unknown_number_seen = 1;
            }
            frame->eax = 0xFFFFFFFFU;
            break;
    }
}

static void show_prompt(void)
{
    put_string("tinyos> ");
}

static void show_memory_info(void)
{
    if (!memory_info_available) {
        put_string("GRUB did not provide memory information.\n");
        return;
    }

    put_string("Lower memory reported by GRUB: ");
    put_unsigned(memory_lower_kib);
    put_string(" KiB\n");
    put_string("Upper memory reported by GRUB: ");
    put_unsigned(memory_upper_kib);
    put_string(" KiB (above 1 MiB)\n");
    put_string("Kernel end: 0x");
    put_hex32(kernel_end_address);
    put_char('\n');
}

static void show_memory_map(void)
{
    if (!memory_map_available) {
        put_string("GRUB did not provide a memory map.\n");
        return;
    }

    put_string("GRUB memory map (base, length, type):\n");
    for (unsigned int i = 0; i < memory_region_count; ++i) {
        const struct memory_region *region = &memory_regions[i];
        put_string("base=0x");
        put_hex64(region->base_high, region->base_low);
        put_string(" length=0x");
        put_hex64(region->length_high, region->length_low);
        put_string(" ");
        put_string(memory_type_name(region->type));
        put_char('\n');
    }

    if (memory_map_truncated) {
        put_string("Map output truncated.\n");
    }
}

static void show_pages(void)
{
    if (!page_allocator_available) {
        put_string("Page allocator is not available.\n");
        return;
    }

    put_string("Page size: ");
    put_unsigned(PAGE_SIZE);
    put_string(" bytes\nAllocated pages: ");
    put_unsigned(active_page_count);
    put_string("\nFree pages: ");
    put_unsigned(free_pages());
    put_string("\n");
}

static void show_paging(void)
{
    if (paging_enabled) {
        put_string("Paging: enabled\n");
        put_string("Identity-mapped first ");
        put_unsigned(PAGING_MAPPED_LIMIT / 0x100000U);
        put_string(" MiB with 4 KiB pages; dynamic page mappings are available.\n");
    } else {
        put_string("Paging: disabled\n");
    }
}

static void show_heap_info(void)
{
    struct kernel_heap_stats stats;
    if (!heap_get_stats(&stats)) {
        put_string("Kernel heap is unavailable or corrupted.\n");
        return;
    }

    put_string("Kernel heap: 64 KiB fixed arena\nAllocated payload: ");
    put_unsigned(stats.used);
    put_string(" bytes\nFree payload: ");
    put_unsigned(stats.free);
    put_string(" bytes\nLargest free block: ");
    put_unsigned(stats.largest_free);
    put_string(" bytes\nActive allocations: ");
    put_unsigned(stats.allocations);
    if (stats.debug_enabled) {
        put_string("\nHeap debug checks: enabled");
    } else {
        put_string("\nHeap debug checks: disabled (use debug build)");
    }
    put_char('\n');
}

static void show_heap_check(void)
{
    struct kernel_heap_stats stats;
    if (!heap_get_stats(&stats)) {
        put_string("Heap check failed: heap metadata is damaged or inconsistent.\n");
        return;
    }

    put_string("Heap check passed: ");
    put_unsigned(stats.allocations);
    put_string(" active allocation(s).\nDebug checks: ");
    put_string(stats.debug_enabled ? "enabled" : "disabled");
    put_string("\nInvalid frees rejected: ");
    put_unsigned(stats.invalid_frees);
    put_string("\nDouble frees rejected: ");
    put_unsigned(stats.double_frees);
    put_char('\n');
}

static void run_heap_demo(void)
{
    unsigned char *first = (unsigned char *)kmalloc(1U);
    unsigned char *middle = (unsigned char *)kmalloc(17U);
    unsigned char *last = (unsigned char *)kmalloc(40U);
    if (first == 0 || middle == 0 || last == 0) {
        if (first != 0) (void)kfree(first);
        if (middle != 0) (void)kfree(middle);
        if (last != 0) (void)kfree(last);
        put_string("Heap demo could not allocate memory.\n");
        return;
    }

    first[0] = 0xA5U;
    for (unsigned int i = 0U; i < 17U; ++i) {
        middle[i] = (unsigned char)(i ^ 0x5AU);
    }
    for (unsigned int i = 0U; i < 40U; ++i) {
        last[i] = (unsigned char)(i + 1U);
    }

    int aligned = (((unsigned int)first & 15U) == 0U) &&
                  (((unsigned int)middle & 15U) == 0U) &&
                  (((unsigned int)last & 15U) == 0U);
    int data_ok = first[0] == 0xA5U;
    for (unsigned int i = 0U; i < 17U; ++i) {
        data_ok = data_ok && middle[i] == (unsigned char)(i ^ 0x5AU);
    }
    for (unsigned int i = 0U; i < 40U; ++i) {
        data_ok = data_ok && last[i] == (unsigned char)(i + 1U);
    }

    int frees_ok = kfree(middle) && kfree(first) && kfree(last);
    unsigned char *coalesced = (unsigned char *)kmalloc(1024U);
    int coalesce_ok = coalesced != 0 && kfree(coalesced);
    int invalid_free_ok = 0;
    int double_free_ok = 0;
    if (coalesce_ok) {
        int stack_value = 0;
        invalid_free_ok = !kfree(&stack_value);
        double_free_ok = !kfree(coalesced);
    }
    if (aligned && data_ok && frees_ok && coalesce_ok &&
        invalid_free_ok && double_free_ok) {
        put_string("Heap demo passed: alignment, data, coalescing, and bad-free rejection work.\n");
    } else {
        put_string("Heap demo failed: ");
        if (!aligned) put_string("alignment ");
        if (!data_ok) put_string("data ");
        if (!frees_ok) put_string("free ");
        if (!coalesce_ok) put_string("coalescing/allocation ");
        if (!invalid_free_ok) put_string("invalid-free rejection ");
        if (!double_free_ok) put_string("double-free rejection ");
        put_char('\n');
    }
}

static void show_disk_info(void)
{
    int result = block_initialize();
    if (result != 0) {
        put_string("Disk unavailable: ");
        put_string(block_error_string(result));
        put_char('\n');
        return;
    }

    unsigned int sectors = block_sector_count();
    put_string("Primary IDE disk: ready\nSectors: ");
    put_unsigned(sectors);
    put_string("\nCapacity: ");
    put_unsigned(sectors / 2048U);
    put_string(" MiB\nTest sector: ");
    put_unsigned(DISK_TEST_SECTOR);
    put_char('\n');
}

static void report_file_store_unavailable(void);

static void show_filesystem_info(void)
{
    if (!file_store_ready) {
        report_file_store_unavailable();
        return;
    }

    put_string("TinyOS filesystem: ready\nFormat version: ");
    put_unsigned(FILESYSTEM_VERSION);
    put_string(" (legacy versions 1-3 migrate to this format)\nSector size: ");
    put_unsigned(DISK_SECTOR_SIZE);
    put_string(" bytes\nSuperblock: LBA ");
    put_unsigned(FILESYSTEM_SUPER_LBA);
    put_string("\nDirectory: LBA ");
    put_unsigned(FILESYSTEM_DIRECTORY_LBA);
    put_string(" (16 entries, 32 bytes each)\nAllocation table: LBA ");
    put_unsigned(FILESYSTEM_FAT_LBA);
    put_string("-");
    put_unsigned(FILESYSTEM_FAT_LBA + FILESYSTEM_FAT_SECTORS - 1U);
    put_string(" (4096 little-endian 16-bit entries)\nData clusters: ");
    put_unsigned(FILESYSTEM_CLUSTER_COUNT);
    put_string(" clusters, 512 bytes each; LBAs ");
    put_unsigned(FILESYSTEM_DATA_START_LBA);
    put_string("-");
    put_unsigned(FILESYSTEM_DATA_END_LBA);
    put_string("\nReserved disk-test sector: LBA ");
    put_unsigned(DISK_TEST_SECTOR);
    put_char('\n');
}

static void run_disk_test(void)
{
    unsigned char write_buffer[DISK_SECTOR_SIZE];
    unsigned char read_buffer[DISK_SECTOR_SIZE];
    unsigned int sectors = block_sector_count();

    if (sectors <= DISK_TEST_SECTOR) {
        put_string("Disk test unavailable: disk is missing or too small.\n");
        return;
    }

    for (unsigned int i = 0; i < DISK_SECTOR_SIZE; ++i) {
        write_buffer[i] = (unsigned char)(0xA5U ^ (i * 37U));
        read_buffer[i] = 0U;
    }

    int result = block_write_sectors(DISK_TEST_SECTOR, 1U, write_buffer);
    if (result < 0) {
        put_string("Disk write failed: ");
        put_string(block_error_string(result));
        put_char('\n');
        return;
    }

    result = block_read_sectors(DISK_TEST_SECTOR, 1U, read_buffer);
    if (result < 0) {
        put_string("Disk read failed: ");
        put_string(block_error_string(result));
        put_char('\n');
        return;
    }

    for (unsigned int i = 0; i < DISK_SECTOR_SIZE; ++i) {
        if (read_buffer[i] != write_buffer[i]) {
            put_string("Disk test failed: data did not match.\n");
            return;
        }
    }

    put_string("Disk test passed: wrote and verified sector ");
    put_unsigned(DISK_TEST_SECTOR);
    put_string(".\n");
}

static void report_file_store_unavailable(void)
{
    put_string("File store unavailable: ");
    put_string(file_store_error_message(file_store_error));
    put_char('\n');
}

static unsigned int entries_in_directory(unsigned char parent)
{
    unsigned int count = 0;
    for (unsigned int i = 0; i < MAX_FILES; ++i) {
        if (files[i].used && files[i].parent == parent) {
            ++count;
        }
    }
    return count;
}

static void show_current_path(void)
{
    unsigned char path[MAX_FILES];
    unsigned int count = 0;
    unsigned char dir = current_directory;
    while (dir != FILE_PARENT_ROOT && count < MAX_FILES) {
        path[count++] = dir;
        dir = files[dir].parent;
    }
    put_char('/');
    while (count > 0U) {
        unsigned char slot = path[--count];
        put_string(files[slot].name);
        put_char('/');
    }
    put_char('\n');
}

static void run_ring3_demo(void)
{
    if (paging_enabled == 0) {
        put_string("Paging must be enabled before ring3demo.\n");
        return;
    }
    if (!prepare_user_demo_memory()) {
        put_string("Could not allocate/map the temporary user pages.\n");
        return;
    }
    ring3_demo_trap_seen = 0;
    ring3_demo_active = 1;
    enter_user_mode(USER_CODE_ADDRESS +
                        ((unsigned int)ring3_demo_user -
                         (unsigned int)user_blob_start),
                    USER_STACK_ADDRESS + PAGE_SIZE - 16U);
    ring3_demo_active = 0;
    cleanup_user_demo_memory();
    __asm__ volatile ("sti");
    put_string(ring3_demo_trap_seen != 0
        ? "Ring 3 entry and TSS kernel-stack return passed.\n"
        : "Ring 3 demo returned without the expected breakpoint.\n");
}

static void run_address_space_demo(void)
{
    if (!prepare_user_address_space()) {
        put_string("Could not create the separate user address space.\n");
        return;
    }
    if (!user_address_space_permissions_valid()) {
        cleanup_user_address_space();
        put_string("Address-space permission or initial-stack check failed.\n");
        return;
    }

    put_string("User address space: separate page directory loaded for the demo.\n");
    put_string("User code: user/read-only; user stack: user/read-write.\n");
    put_string("Kernel VGA mapping: supervisor-only; kernel address space has no user mappings.\n");
    put_string("Initial stack: argc=0, argv=NULL, envp=NULL.\n");

    ring3_demo_trap_seen = 0;
    ring3_demo_active = 1;
    load_page_directory(user_address_space_directory);
    enter_user_mode(USER_CODE_ADDRESS +
                        ((unsigned int)ring3_demo_user -
                         (unsigned int)user_blob_start),
                    USER_STACK_ADDRESS + PAGE_SIZE - 3U * sizeof(unsigned int));
    load_page_directory(page_directory);
    ring3_demo_active = 0;
    cleanup_user_address_space();
    __asm__ volatile ("sti");
    put_string(ring3_demo_trap_seen != 0
        ? "Address-space demo passed: ring 3 returned safely to the kernel.\n"
        : "Address-space demo failed: user code did not return as expected.\n");
}

/* Load a small ELF from the persistent file store into the user window. */
static int prepare_user_elf_address_space(
    const unsigned char *image,
    const struct elf32_image_info *info)
{
    if (!paging_enabled || !page_allocator_available || image == 0 ||
        info == 0 || info->load_segment_count != 1U ||
        (page_directory[USER_ADDRESS_SPACE_DIRECTORY] & 0x01U) != 0U) {
        return 0;
    }

    const struct elf32_load_segment *segment = &info->segments[0];
    if (segment->virtual_address < USER_CODE_ADDRESS ||
        segment->virtual_address >= USER_STACK_ADDRESS ||
        segment->memory_size > USER_STACK_ADDRESS - segment->virtual_address ||
        segment->file_size > PAGE_SIZE ||
        (segment->flags & 0x01U) == 0U ||
        (segment->flags & 0x02U) != 0U ||
        info->entry < segment->virtual_address ||
        info->entry >= segment->virtual_address + segment->file_size) {
        return 0;
    }

    address_space_code_physical = allocate_page();
    if (address_space_code_physical == 0U) {
        return 0;
    }
    address_space_stack_physical = allocate_page();
    if (address_space_stack_physical == 0U) {
        (void)release_page(address_space_code_physical);
        address_space_code_physical = 0U;
        return 0;
    }

    for (unsigned int i = 0U; i < 1024U; ++i) {
        user_address_space_directory[i] = 0U;
        user_address_space_table[i] = 0U;
    }
    for (unsigned int i = 0U; i < PAGE_TABLES_FOR_LIMIT; ++i) {
        user_address_space_directory[i] = page_directory[i] & ~0x04U;
    }

    clear_physical_page(address_space_code_physical);
    clear_physical_page(address_space_stack_physical);
    unsigned char *destination = (unsigned char *)address_space_code_physical;
    unsigned int code_offset = segment->virtual_address - USER_CODE_ADDRESS;
    for (unsigned int i = 0U; i < segment->file_size; ++i) {
        destination[code_offset + i] = image[segment->offset + i];
    }

    unsigned int *stack = (unsigned int *)address_space_stack_physical;
    unsigned int initial_words = PAGE_SIZE / sizeof(unsigned int) - 3U;
    stack[initial_words] = 0U;
    stack[initial_words + 1U] = 0U;
    stack[initial_words + 2U] = 0U;

    unsigned int code_index = (USER_CODE_ADDRESS >> 12) & 0x3FFU;
    unsigned int stack_index = (USER_STACK_ADDRESS >> 12) & 0x3FFU;
    user_address_space_table[code_index] = address_space_code_physical | 0x05U;
    user_address_space_table[stack_index] = address_space_stack_physical | 0x07U;
    user_address_space_directory[USER_ADDRESS_SPACE_DIRECTORY] =
        (unsigned int)&user_address_space_table[0] | 0x07U;
    return 1;
}

static int app_hex_digit(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static void run_stored_application(const char *app_name)
{
    char file_name[FILENAME_MAX_LEN + 1U];
    if (!file_store_ready) {
        report_file_store_unavailable();
        return;
    }
    unsigned int name_length = string_length(app_name);
    if (name_length == 0U || name_length > FILENAME_MAX_LEN - 4U ||
        !file_name_valid(app_name)) {
        put_string("Invalid app name. Use run <name> (max 11 characters).\n");
        return;
    }
    for (unsigned int i = 0U; i < name_length; ++i) {
        file_name[i] = app_name[i];
    }
    file_name[name_length] = '.';
    file_name[name_length + 1U] = 'e';
    file_name[name_length + 2U] = 'l';
    file_name[name_length + 3U] = 'f';
    file_name[name_length + 4U] = '\0';

    int slot = entry_find(file_name, current_directory);
    if (slot < 0) {
        put_string("App file not found: ");
        put_string(file_name);
        put_string(". Use install hello to add the sample app.\n");
        return;
    }
    if (files[slot].is_directory) {
        put_string("App path is a directory: ");
        put_string(file_name);
        put_char('\n');
        return;
    }

    unsigned int encoded_size;
    if (!filesystem_bounded_content_length(files[slot].content,
                                           &encoded_size)) {
        put_string("Stored app has unterminated or oversized content.\n");
        return;
    }
    if (encoded_size == 0U || (encoded_size & 1U) != 0U ||
        encoded_size / 2U > sizeof(app_image_buffer)) {
        put_string("Stored app is empty or has an invalid hex encoding.\n");
        return;
    }
    unsigned int image_size = encoded_size / 2U;
    for (unsigned int i = 0U; i < image_size; ++i) {
        int high = app_hex_digit(files[slot].content[i * 2U]);
        int low = app_hex_digit(files[slot].content[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            put_string("Stored app contains a non-hexadecimal byte.\n");
            return;
        }
        app_image_buffer[i] = (unsigned char)((high << 4) | low);
    }

    struct elf32_image_info info;
    if (elf32_validate(app_image_buffer, image_size, &info) != ELF32_VALID) {
        put_string("Stored app failed ELF32 validation.\n");
        return;
    }
    if (!prepare_user_elf_address_space(app_image_buffer, &info)) {
        put_string("Could not load app into a user address space.\n");
        return;
    }

    embedded_app_exit_seen = 0;
    embedded_app_exit_status = 0xFFFFFFFFU;
    for (unsigned int i = 0U; i < MAX_APP_FILE_HANDLES; ++i) {
        app_file_handles[i].open = 0;
        app_file_handles[i].slot = 0U;
        app_file_handles[i].offset = 0U;
        app_file_handles[i].flags = 0U;
    }
    embedded_app_active = 1;
    active_user_address_space = user_address_space_directory;
    load_page_directory(user_address_space_directory);
    enter_user_mode(info.entry, USER_STACK_ADDRESS + PAGE_SIZE -
                    3U * sizeof(unsigned int));
    load_page_directory(page_directory);
    active_user_address_space = 0;
    embedded_app_active = 0;
    for (unsigned int i = 0U; i < MAX_APP_FILE_HANDLES; ++i) {
        app_file_handles[i].open = 0;
    }
    cleanup_user_address_space();
    __asm__ volatile ("sti");

    if (embedded_app_exit_seen != 0) {
        put_string("Application '");
        put_string(app_name);
        put_string("' exited with status ");
        put_unsigned(embedded_app_exit_status);
        put_string(".\n");
    } else {
        put_string("Application '");
        put_string(app_name);
        put_string("' returned without an exit status.\n");
    }
}

static void install_sample_application(const char *app_name)
{
    if (!file_store_ready) {
        report_file_store_unavailable();
        return;
    }
    const unsigned char *image;
    unsigned int image_size;
    if (strings_equal(app_name, "hello")) {
        image = boot_app_image;
        image_size = boot_app_image_size;
    } else if (strings_equal(app_name, "filedemo")) {
        image = boot_filedemo_image;
        image_size = boot_filedemo_image_size;
    } else {
        put_string("Usage: install hello|filedemo\n");
        return;
    }
    if (image_size == 0U) {
        put_string("Sample ELF is unavailable from the boot ISO. Rebuild and boot the latest ISO.\n");
        return;
    }

    struct elf32_image_info info;
    if (elf32_validate(image, image_size, &info) !=
        ELF32_VALID) {
        put_string("Sample app from the boot ISO failed ELF32 validation.\n");
        return;
    }
    if (image_size > FILE_CONTENT_MAX / 2U) {
        put_string("Sample app is too large for the TinyOS file store.\n");
        return;
    }

    static const char hex_digits[] = "0123456789ABCDEF";
    for (unsigned int i = 0U; i < image_size; ++i) {
        unsigned char byte = image[i];
        filesystem_write_buffer[i * 2U] = hex_digits[byte >> 4];
        filesystem_write_buffer[i * 2U + 1U] = hex_digits[byte & 0x0FU];
    }
    filesystem_write_buffer[image_size * 2U] = '\0';

    char file_name[FILENAME_MAX_LEN + 1U];
    unsigned int name_length = string_length(app_name);
    if (name_length == 0U || name_length + 4U > FILENAME_MAX_LEN) {
        put_string("Invalid app name.\n");
        return;
    }
    for (unsigned int i = 0U; i < name_length; ++i) {
        file_name[i] = app_name[i];
    }
    file_name[name_length] = '.';
    file_name[name_length + 1U] = 'e';
    file_name[name_length + 2U] = 'l';
    file_name[name_length + 3U] = 'f';
    file_name[name_length + 4U] = '\0';
    int slot = entry_find(file_name, current_directory);
    if (slot >= 0 && files[slot].is_directory) {
        put_string("Cannot install app: destination is a directory.\n");
        return;
    }
    if (slot < 0) {
        slot = file_free_slot();
    }
    if (slot < 0) {
        put_string("Entry store is full (max 16 total entries).\n");
        return;
    }

    file_entry_copy(&file_entry_backup, &files[slot]);
    copy_string(files[slot].name, FILENAME_MAX_LEN, file_name);
    copy_string(files[slot].content, FILE_CONTENT_MAX, filesystem_write_buffer);
    files[slot].used = 1;
    files[slot].is_directory = 0;
    files[slot].parent = current_directory;
    files[slot].first_cluster = FILESYSTEM_CLUSTER_FREE;
    files[slot].cluster_count = 0U;
    int result = filesystem_save_change((unsigned int)slot, &file_entry_backup);
    if (result < 0) {
        put_string("App install failed: ");
        put_string(file_store_error_message(result));
        put_char('\n');
        return;
    }
    put_string("Installed ");
    put_string(file_name);
    put_string(" in the persistent file store (hex-encoded ");
    put_unsigned(image_size);
    put_string("-byte ELF).\n");
}

static void run_syscall_demo(void)
{
    if (paging_enabled == 0) {
        put_string("Paging must be enabled before syscalldemo.\n");
        return;
    }
    if (!prepare_user_demo_memory()) {
        put_string("Could not allocate/map the temporary user pages.\n");
        return;
    }

    syscall_demo_write_seen = 0;
    syscall_demo_yield_seen = 0;
    syscall_demo_exit_seen = 0;
    syscall_demo_bad_pointer_seen = 0;
    syscall_demo_large_buffer_seen = 0;
    syscall_demo_cross_page_seen = 0;
    syscall_demo_unknown_number_seen = 0;
    syscall_demo_exit_status = 0xFFFFFFFFU;
    syscall_demo_active = 1;
    enter_user_mode(USER_CODE_ADDRESS +
                        ((unsigned int)ring3_syscall_demo_user -
                         (unsigned int)user_blob_start),
                    USER_STACK_ADDRESS + PAGE_SIZE - 16U);
    syscall_demo_active = 0;
    cleanup_user_demo_memory();
    __asm__ volatile ("sti");

    if (syscall_demo_write_seen != 0 && syscall_demo_yield_seen == 1U &&
        syscall_demo_exit_seen != 0 && syscall_demo_exit_status == 0U &&
        syscall_demo_bad_pointer_seen != 0 &&
        syscall_demo_large_buffer_seen != 0 &&
        syscall_demo_cross_page_seen != 0 &&
        syscall_demo_unknown_number_seen != 0) {
        put_string("System-call checks passed: write/yield/exit work; bad pointer, cross-page buffer, oversized buffer, and unknown number were rejected.\n");
    } else {
        put_string("System-call checks incomplete: write=");
        put_unsigned((unsigned int)syscall_demo_write_seen);
        put_string(" yield=");
        put_unsigned((unsigned int)syscall_demo_yield_seen);
        put_string(" exit=");
        put_unsigned((unsigned int)syscall_demo_exit_seen);
        put_string(" status=");
        put_unsigned((unsigned int)syscall_demo_exit_status);
        put_string(" badptr=");
        put_unsigned((unsigned int)syscall_demo_bad_pointer_seen);
        put_string(" large=");
        put_unsigned((unsigned int)syscall_demo_large_buffer_seen);
        put_string(" crosspage=");
        put_unsigned((unsigned int)syscall_demo_cross_page_seen);
        put_string(" badnum=");
        put_unsigned((unsigned int)syscall_demo_unknown_number_seen);
        put_char('\n');
    }
}

static void run_user_protection_demo(void)
{
    if (paging_enabled == 0) {
        put_string("Paging must be enabled before usermemdemo.\n");
        return;
    }
    if (!prepare_user_demo_memory()) {
        put_string("Could not allocate/map the temporary user pages.\n");
        return;
    }

    unsigned int code_flags = vm_page_flags(USER_CODE_ADDRESS);
    unsigned int stack_flags = vm_page_flags(USER_STACK_ADDRESS);
    unsigned int kernel_flags = kernel_page_flags(0x000B8000U);
    put_string("User code page: ");
    put_string((code_flags & 0x05U) == 0x05U &&
                       (code_flags & 0x02U) == 0U
                   ? "user/read-only\n" : "permission check failed\n");
    put_string("User stack page: ");
    put_string((stack_flags & 0x07U) == 0x07U
                   ? "user/read-write\n" : "permission check failed\n");
    put_string("Kernel VGA page: ");
    put_string((kernel_flags & 0x05U) == 0x01U
                   ? "supervisor-only\n" : "permission check failed\n");

    user_protection_fault_seen = 0;
    user_demo_active = 1;
    enter_user_mode(USER_CODE_ADDRESS +
                        ((unsigned int)ring3_protection_demo_user -
                         (unsigned int)user_blob_start),
                    USER_STACK_ADDRESS + PAGE_SIZE - 16U);
    user_demo_active = 0;
    cleanup_user_demo_memory();
    __asm__ volatile ("sti");
    put_string(user_protection_fault_seen != 0
        ? "Protection demo passed: user write to the kernel VGA page was blocked.\n"
        : "Protection demo failed: expected kernel-page write fault was not observed.\n");
}

static void run_elf_validation_demo(void)
{
    if (elf32_validation_demo()) {
        put_string("ELF32 validation passed: valid image accepted; malformed headers and loadable segments rejected.\n");
    } else {
        put_string("ELF32 validation demo failed.\n");
    }
}

static void run_command(void)
{
    if (file_store_ready && filesystem_command_needs_audit() &&
        !filesystem_preflight_audit()) {
        command_length = 0U;
        command[0] = '\0';
        show_prompt();
        return;
    }

    if (command_is("help")) {
        put_string("Commands: help, about, taskinfo, taskdemo, sleepdemo, preemptdemo, meminfo, mmap, pages, paging, vminfo, vmtest, pfdemo, heapinfo, heapcheck, heapdemo, alloc, free [address], uptime, pci, mouseinfo, diskinfo, fsinfo, disktest, df, fscheck <file>, fsck, fsbounds, clear, echo <text>, ring3demo, syscalldemo, usermemdemo, elfdemo, addrspacedemo, install hello|filedemo, run <app>\n");
        put_string("Apps: install hello or install filedemo saves a sample ELF; run <name> loads <name>.elf from the current directory.\n");
        put_string("App file syscalls: open(name, flags), read(handle, buffer, count), write(handle, buffer, count), close(handle). Flags: read=1, write=2, create=4, truncate=8. Each I/O call is limited to 128 bytes.\n");
        put_string("Files: ls, mkdir <name>, cd <name|..|/>, pwd, rmdir <name>, rename <old> <new>\n");
        put_string("       touch <name>, write <name> <text>, cat <name>, rm <name>, cp <source> <destination>, df\n");
        put_string("       fscheck <file> validates disk metadata, cluster links, and file data.\n");
        put_string("       fsck checks the full filesystem; fsck repair previews orphan cleanup; fsbounds checks full-disk and invalid-bound handling without disk writes.\n");
        put_string("Files persist on disk; 16 entries, 15-char names, up to 32768 chars per file.\n");
        put_string("Disk space is shared dynamically; remove or shrink files to free clusters.\n");
        put_string("Type a command and press Enter.\n");
    } else if (command_is("ring3demo")) {
        run_ring3_demo();
    } else if (command_is("syscalldemo")) {
        run_syscall_demo();
    } else if (command_is("usermemdemo")) {
        run_user_protection_demo();
    } else if (command_is("elfdemo")) {
        run_elf_validation_demo();
    } else if (command_is("addrspacedemo")) {
        run_address_space_demo();
    } else if (command_is("install hello")) {
        install_sample_application("hello");
    } else if (command_is("install filedemo")) {
        install_sample_application("filedemo");
    } else if (command_starts_with("install ")) {
        put_string("Usage: install hello|filedemo\n");
    } else if (command_is("run")) {
        put_string("Usage: run <app>\n");
    } else if (command_starts_with("run ")) {
        const char *app_name = command_args_after("run ");
        if (app_name != 0) {
            run_stored_application(app_name);
        } else {
            put_string("Usage: run <app>\n");
        }
    } else if (command_is("taskinfo")) {
        show_task_info();
    } else if (command_is("taskdemo")) {
        run_task_demo();
    } else if (command_is("sleepdemo")) {
        run_sleep_demo();
    } else if (command_is("preemptdemo")) {
        run_preempt_demo();
    } else if (command_is("uptime")) {
        put_string("Uptime: ");
        put_unsigned(timer_uptime_seconds());
        put_string(" seconds\n");
    } else if (command_is("about")) {
        put_string("TinyOS is a small educational operating system.\n");
    } else if (command_is("pci")) {
        show_pci_devices();
    } else if (command_is("mouseinfo")) {
        show_mouse_info();
    } else if (command_is("meminfo")) {
        show_memory_info();
    } else if (command_is("mmap")) {
        show_memory_map();
    } else if (command_is("pages")) {
        show_pages();
    } else if (command_is("paging")) {
        show_paging();
    } else if (command_is("vminfo")) {
        show_vm_info();
    } else if (command_is("vmtest")) {
        run_vm_test();
    } else if (command_is("pfdemo")) {
        show_page_fault_demo();
    } else if (command_is("heapinfo")) {
        show_heap_info();
    } else if (command_is("heapcheck")) {
        show_heap_check();
    } else if (command_is("heapdemo")) {
        run_heap_demo();
    } else if (command_is("diskinfo")) {
        show_disk_info();
    } else if (command_is("fsinfo")) {
        show_filesystem_info();
    } else if (command_is("disktest")) {
        run_disk_test();
    } else if (command_is("fsbounds")) {
        run_filesystem_bounds_demo();
    } else if (command_is("df")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            show_filesystem_space();
        }
    } else if (command_starts_with("fscheck ")) {
        const char *name = command_args_after("fscheck ");
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else if (name == 0) {
            put_string("Usage: fscheck <file>\n");
        } else if (!file_name_valid(name)) {
            put_string("Invalid name (max 15 chars, no spaces).\n");
        } else {
            int idx = entry_find(name, current_directory);
            if (idx < 0) {
                put_string("File not found: ");
                put_string(name);
                put_char('\n');
            } else {
                inspect_file_chain((unsigned int)idx);
            }
        }
    } else if (command_is("fsck repair confirm") ||
               command_is("fsck confirm")) {
        run_filesystem_repair_preview(1);
    } else if (command_is("fsck repair")) {
        run_filesystem_repair_preview(0);
    } else if (command_is("fsck")) {
        run_filesystem_check();
    } else if (command_starts_with("fsck ")) {
        put_string("Usage: fsck, fsck repair, fsck repair confirm, or fsck confirm\n");
    } else if (command_is("alloc")) {
        unsigned int page = allocate_page();
        if (page == 0U) {
            put_string("No page could be allocated.\n");
        } else {
            put_string("Allocated a 4 KiB page at 0x");
            put_hex32(page);
            put_char('\n');
        }
    } else if (command_is("free")) {
        if (active_page_count == 0U) {
            put_string("No allocated pages to free.\n");
        } else {
            unsigned int address = active_pages[active_page_count - 1U];
            if (release_page(address)) {
                put_string("Freed most recent page at 0x");
                put_hex32(address);
                put_char('\n');
            } else {
                put_string("Could not release the most recent page.\n");
            }
        }
    } else if (command_starts_with("free ")) {
        unsigned int address;
        if (parse_hex_address(command + 5, &address) && release_page(address)) {
            put_string("Freed page at 0x");
            put_hex32(address);
            put_char('\n');
        } else {
            put_string("Address must be an aligned hex page address currently allocated by alloc.\n");
        }
    } else if (command_is("clear")) {
        clear_screen();
        put_string("TinyOS shell. Type help for commands.\n");
    } else if (command_is("pwd")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            show_current_path();
        }
    } else if (command_is("ls")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            unsigned int count = entries_in_directory(current_directory);
            if (count == 0U) {
                put_string("No files or directories.\n");
            }
            for (unsigned int i = 0; i < MAX_FILES; ++i) {
                if (files[i].used && files[i].parent == current_directory) {
                    put_string(files[i].name);
                    if (files[i].is_directory) {
                        put_char('/');
                    }
                    put_char('\n');
                }
            }
        }
    } else if (command_is("rename") || command_starts_with("rename ")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            const char *args = command_args_after("rename ");
            char old_name[FILENAME_MAX_LEN + 1U];
            char new_name[FILENAME_MAX_LEN + 1U];
            unsigned int old_length = 0U;
            unsigned int new_length = 0U;

            if (args == 0) {
                put_string("Usage: rename <old-name> <new-name>\n");
            } else {
                while (args[old_length] != '\0' && args[old_length] != ' ') {
                    ++old_length;
                }
                const char *new_start = args + old_length;
                while (*new_start == ' ') {
                    ++new_start;
                }
                while (new_start[new_length] != '\0' &&
                       new_start[new_length] != ' ') {
                    ++new_length;
                }
                const char *extra = new_start + new_length;
                while (*extra == ' ') {
                    ++extra;
                }

                if (old_length == 0U || old_length > FILENAME_MAX_LEN ||
                    new_length == 0U || new_length > FILENAME_MAX_LEN ||
                    *extra != '\0') {
                    put_string("Usage: rename <old-name> <new-name>\n");
                } else {
                    for (unsigned int i = 0; i < old_length; ++i) {
                        old_name[i] = args[i];
                    }
                    old_name[old_length] = '\0';
                    for (unsigned int i = 0; i < new_length; ++i) {
                        new_name[i] = new_start[i];
                    }
                    new_name[new_length] = '\0';

                    if (!file_name_valid(old_name) ||
                        !file_name_valid(new_name)) {
                        put_string("Invalid name (max 15 chars; no spaces or slashes).\n");
                    } else {
                        int idx = entry_find(old_name, current_directory);
                        if (idx < 0) {
                            put_string("Entry not found: ");
                            put_string(old_name);
                            put_char('\n');
                        } else if (strings_equal(old_name, new_name)) {
                            put_string("Name unchanged: ");
                            put_string(old_name);
                            put_char('\n');
                        } else if (entry_find(new_name, current_directory) >= 0) {
                            put_string("Name already exists: ");
                            put_string(new_name);
                            put_char('\n');
                        } else {
                            file_entry_copy(&file_entry_backup, &files[idx]);
                            copy_string(files[idx].name, FILENAME_MAX_LEN,
                                        new_name);
                            int result = filesystem_save_metadata_change(
                                (unsigned int)idx, &file_entry_backup);
                            if (result < 0) {
                                put_string("Rename failed: ");
                                put_string(file_store_error_message(result));
                                put_char('\n');
                            } else {
                                put_string("Renamed ");
                                put_string(old_name);
                                put_string(" to ");
                                put_string(new_name);
                                put_char('\n');
                            }
                        }
                    }
                }
            }
        }
    } else if (command_is("cp") || command_starts_with("cp ")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            const char *args = command_args_after("cp ");
            char source_name[FILENAME_MAX_LEN + 1U];
            char destination_name[FILENAME_MAX_LEN + 1U];
            unsigned int source_length = 0U;
            unsigned int destination_length = 0U;

            if (args == 0) {
                put_string("Usage: cp <source> <destination>\n");
            } else {
                while (args[source_length] != '\0' && args[source_length] != ' ') {
                    ++source_length;
                }
                const char *destination_start = args + source_length;
                while (*destination_start == ' ') {
                    ++destination_start;
                }
                while (destination_start[destination_length] != '\0' &&
                       destination_start[destination_length] != ' ') {
                    ++destination_length;
                }
                const char *extra = destination_start + destination_length;
                while (*extra == ' ') {
                    ++extra;
                }

                if (source_length == 0U || source_length > FILENAME_MAX_LEN ||
                    destination_length == 0U ||
                    destination_length > FILENAME_MAX_LEN || *extra != '\0') {
                    put_string("Usage: cp <source> <destination>\n");
                } else {
                    for (unsigned int i = 0; i < source_length; ++i) {
                        source_name[i] = args[i];
                    }
                    source_name[source_length] = '\0';
                    for (unsigned int i = 0; i < destination_length; ++i) {
                        destination_name[i] = destination_start[i];
                    }
                    destination_name[destination_length] = '\0';

                    if (!file_name_valid(source_name) ||
                        !file_name_valid(destination_name)) {
                        put_string("Invalid name (max 15 chars; no spaces or slashes).\n");
                    } else {
                        int source_idx = entry_find(source_name, current_directory);
                        if (source_idx < 0) {
                            put_string("Source file not found: ");
                            put_string(source_name);
                            put_char('\n');
                        } else if (files[source_idx].is_directory) {
                            put_string("Cannot copy a directory: ");
                            put_string(source_name);
                            put_char('\n');
                        } else if (entry_find(destination_name,
                                              current_directory) >= 0) {
                            put_string("Name already exists: ");
                            put_string(destination_name);
                            put_char('\n');
                        } else {
                            int slot = file_free_slot();
                            if (slot < 0) {
                                put_string("Entry store is full (max 16 total entries).\n");
                            } else {
                                file_entry_copy(&file_entry_backup, &files[slot]);
                                copy_string(files[slot].name, FILENAME_MAX_LEN,
                                            destination_name);
                                copy_string(files[slot].content,
                                            FILE_CONTENT_MAX,
                                            files[source_idx].content);
                                files[slot].used = 1;
                                files[slot].is_directory = 0;
                                files[slot].parent = current_directory;
                                files[slot].first_cluster =
                                    FILESYSTEM_CLUSTER_FREE;
                                files[slot].cluster_count = 0U;
                                int result = filesystem_save_change(
                                    (unsigned int)slot, &file_entry_backup);
                                if (result < 0) {
                                    put_string("Copy failed: ");
                                    put_string(file_store_error_message(result));
                                    put_char('\n');
                                } else {
                                    put_string("Copied ");
                                    put_string(source_name);
                                    put_string(" to ");
                                    put_string(destination_name);
                                    put_string(" (");
                                    put_unsigned(string_length(
                                        files[slot].content));
                                    put_string(" bytes).\n");
                                }
                            }
                        }
                    }
                }
            }
        }
    } else if (command_starts_with("mkdir ")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            const char *name = command_args_after("mkdir ");
            if (name == 0) {
                put_string("Usage: mkdir <name>\n");
            } else if (!file_name_valid(name)) {
                put_string("Invalid name (max 15 chars; no spaces or slashes).\n");
            } else if (entry_find(name, current_directory) >= 0) {
                put_string("Name already exists: ");
                put_string(name);
                put_char('\n');
            } else {
                int slot = file_free_slot();
                if (slot < 0) {
                    put_string("Entry store is full (max 16 total entries).\n");
                } else {
                    file_entry_copy(&file_entry_backup, &files[slot]);
                    copy_string(files[slot].name, FILENAME_MAX_LEN, name);
                    files[slot].content[0] = '\0';
                    files[slot].used = 1;
                    files[slot].is_directory = 1;
                    files[slot].parent = current_directory;
                    files[slot].first_cluster = FILESYSTEM_CLUSTER_FREE;
                    files[slot].cluster_count = 0U;
                    int result = filesystem_save_change((unsigned int)slot,
                                                        &file_entry_backup);
                    if (result < 0) {
                        put_string("Create directory failed: ");
                        put_string(file_store_error_message(result));
                        put_char('\n');
                    } else {
                        put_string("Created directory: ");
                        put_string(files[slot].name);
                        put_char('\n');
                    }
                }
            }
        }
    } else if (command_is("cd /")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            current_directory = FILE_PARENT_ROOT;
        }
    } else if (command_is("cd ..")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else if (current_directory == FILE_PARENT_ROOT) {
            put_string("Already at root.\n");
        } else {
            current_directory = files[current_directory].parent;
        }
    } else if (command_starts_with("cd ")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            const char *name = command_args_after("cd ");
            if (name == 0) {
                put_string("Usage: cd <directory|..|/>\n");
            } else if (!file_name_valid(name)) {
                put_string("Invalid directory name.\n");
            } else {
                int idx = entry_find(name, current_directory);
                if (idx < 0) {
                    put_string("Directory not found: ");
                    put_string(name);
                    put_char('\n');
                } else if (!files[idx].is_directory) {
                    put_string("Not a directory: ");
                    put_string(name);
                    put_char('\n');
                } else {
                    current_directory = (unsigned char)idx;
                }
            }
        }
    } else if (command_starts_with("rmdir ")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            const char *name = command_args_after("rmdir ");
            if (name == 0) {
                put_string("Usage: rmdir <name>\n");
            } else if (!file_name_valid(name)) {
                put_string("Invalid name (max 15 chars; no spaces or slashes).\n");
            } else {
                int idx = entry_find(name, current_directory);
                if (idx < 0) {
                    put_string("Directory not found: ");
                    put_string(name);
                    put_char('\n');
                } else if (!files[idx].is_directory) {
                    put_string("Not a directory: ");
                    put_string(name);
                    put_char('\n');
                } else if (entries_in_directory((unsigned char)idx) != 0U) {
                    put_string("Directory is not empty: ");
                    put_string(name);
                    put_char('\n');
                } else {
                    file_entry_copy(&file_entry_backup, &files[idx]);
                    files[idx].used = 0;
                    files[idx].name[0] = '\0';
                    files[idx].content[0] = '\0';
                    files[idx].is_directory = 0;
                    files[idx].parent = FILE_PARENT_ROOT;
                    files[idx].first_cluster = FILESYSTEM_CLUSTER_FREE;
                    files[idx].cluster_count = 0U;
                    int result = filesystem_save_change((unsigned int)idx,
                                                        &file_entry_backup);
                    if (result < 0) {
                        put_string("Remove directory failed: ");
                        put_string(file_store_error_message(result));
                        put_char('\n');
                    } else {
                        put_string("Removed directory: ");
                        put_string(name);
                        put_char('\n');
                    }
                }
            }
        }
    } else if (command_starts_with("touch ")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            const char *name = command_args_after("touch ");
            if (name == 0) {
                put_string("Usage: touch <name>\n");
            } else if (!file_name_valid(name)) {
                put_string("Invalid name (max 15 chars, no spaces).\n");
            } else if (entry_find(name, current_directory) >= 0) {
                put_string("Name already exists: ");
                put_string(name);
                put_char('\n');
            } else {
                int slot = file_free_slot();
                if (slot < 0) {
                    put_string("Entry store is full (max 16 total entries).\n");
                } else {
                    file_entry_copy(&file_entry_backup, &files[slot]);
                    copy_string(files[slot].name, FILENAME_MAX_LEN, name);
                    files[slot].content[0] = '\0';
                    files[slot].used = 1;
                    files[slot].is_directory = 0;
                    files[slot].parent = current_directory;
                    files[slot].first_cluster = FILESYSTEM_CLUSTER_FREE;
                    files[slot].cluster_count = 0U;
                    int result = filesystem_save_change((unsigned int)slot,
                                                        &file_entry_backup);
                    if (result < 0) {
                        put_string("Create failed: ");
                        put_string(file_store_error_message(result));
                        put_char('\n');
                    } else {
                        put_string("Created: ");
                        put_string(files[slot].name);
                        put_char('\n');
                    }
                }
            }
        }
    } else if (command_starts_with("write ")) {
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else {
            const char *name = command_args_after("write ");
            if (name == 0) {
                put_string("Usage: write <name> <text>\n");
            } else {
                const char *text = name;
                while (*text != '\0' && *text != ' ') {
                    ++text;
                }
                char name_buf[FILENAME_MAX_LEN + 1U];
                unsigned int name_len = (unsigned int)(text - name);
                if (name_len == 0U || name_len > FILENAME_MAX_LEN) {
                    put_string("Invalid name (max 15 chars, no spaces).\n");
                } else {
                    for (unsigned int i = 0; i < name_len; ++i) {
                        name_buf[i] = name[i];
                    }
                    name_buf[name_len] = '\0';
                    if (!file_name_valid(name_buf)) {
                        put_string("Invalid name (max 15 chars, no spaces).\n");
                    } else {
                        while (*text == ' ') {
                            ++text;
                        }
                        if (*text == '\0') {
                            put_string("No text given.\n");
                        } else {
                            if (!copy_string(filesystem_write_buffer,
                                             FILE_CONTENT_MAX, text)) {
                                put_string("Content too long (max ");
                                put_unsigned(FILE_CONTENT_MAX);
                                put_string(" chars).\n");
                            } else {
                                int idx = entry_find(name_buf, current_directory);
                                if (idx >= 0 && files[idx].is_directory) {
                                    put_string("Name is a directory: ");
                                    put_string(name_buf);
                                    put_char('\n');
                                    idx = -2;
                                }
                                if (idx == -1) {
                                    idx = file_free_slot();
                                }
                                if (idx == -2) {
                                    /* Already reported above. */
                                } else if (idx < 0) {
                                    put_string("Entry store is full (max 16 total entries).\n");
                                } else {
                                    file_entry_copy(&file_entry_backup, &files[idx]);
                                    copy_string(files[idx].name, FILENAME_MAX_LEN, name_buf);
                                    copy_string(files[idx].content, FILE_CONTENT_MAX,
                                                filesystem_write_buffer);
                                    files[idx].used = 1;
                                    files[idx].is_directory = 0;
                                    files[idx].parent = current_directory;
                                    files[idx].first_cluster = FILESYSTEM_CLUSTER_FREE;
                                    files[idx].cluster_count = 0U;
                                    int result = filesystem_save_change(
                                        (unsigned int)idx, &file_entry_backup);
                                    if (result < 0) {
                                        put_string("Write failed: ");
                                        put_string(file_store_error_message(result));
                                        put_char('\n');
                                    } else {
                                        put_string("Wrote ");
                                        put_unsigned(string_length(filesystem_write_buffer));
                                        put_string(" bytes to ");
                                        put_string(files[idx].name);
                                        put_char('\n');
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } else if (command_starts_with("cat ")) {
        const char *name = command_args_after("cat ");
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else if (name == 0) {
            put_string("Usage: cat <name>\n");
        } else if (!file_name_valid(name)) {
            put_string("Invalid name (max 15 chars, no spaces).\n");
        } else {
            int idx = entry_find(name, current_directory);
            if (idx < 0) {
                put_string("File not found: ");
                put_string(name);
                put_char('\n');
            } else if (files[idx].is_directory) {
                put_string("Is a directory: ");
                put_string(name);
                put_char('\n');
            } else {
                put_string(files[idx].content);
                put_char('\n');
            }
        }
    } else if (command_starts_with("rm ")) {
        const char *name = command_args_after("rm ");
        if (!file_store_ready) {
            report_file_store_unavailable();
        } else if (name == 0) {
            put_string("Usage: rm <name>\n");
        } else if (!file_name_valid(name)) {
            put_string("Invalid name (max 15 chars, no spaces).\n");
        } else {
            int idx = entry_find(name, current_directory);
            if (idx < 0) {
                put_string("File not found: ");
                put_string(name);
                put_char('\n');
            } else {
                if (files[idx].is_directory) {
                    put_string("Use rmdir for directories: ");
                    put_string(name);
                    put_char('\n');
                } else {
                    file_entry_copy(&file_entry_backup, &files[idx]);
                    files[idx].used = 0;
                    files[idx].name[0] = '\0';
                    files[idx].content[0] = '\0';
                    files[idx].is_directory = 0;
                    files[idx].parent = FILE_PARENT_ROOT;
                    files[idx].first_cluster = FILESYSTEM_CLUSTER_FREE;
                    files[idx].cluster_count = 0U;
                    int result = filesystem_save_change((unsigned int)idx,
                                                        &file_entry_backup);
                    if (result < 0) {
                        put_string("Remove failed: ");
                        put_string(file_store_error_message(result));
                        put_char('\n');
                    } else {
                        put_string("Removed: ");
                        put_string(name);
                        put_char('\n');
                    }
                }
            }
        }
    } else if (command_starts_with("echo ")) {
        put_string(command + 5);
        put_char('\n');
    } else if (command[0] != '\0') {
        put_string("Unknown command: ");
        put_string(command);
        put_char('\n');
    }

    command_length = 0;
    command[0] = '\0';
    show_prompt();
}

/* Returns the next buffered keyboard character, or '\0' if the buffer is
   empty. Called by the shell loop (with interrupts enabled). */


void kernel_main(unsigned int boot_magic, unsigned int info_address)
{
    serial_initialize();

    if (boot_magic == MULTIBOOT_BOOTLOADER_MAGIC && info_address != 0U) {
        const struct multiboot_info *info =
            (const struct multiboot_info *)info_address;
        if ((info->flags & MULTIBOOT_INFO_MODULES) != 0U &&
            info->mods_count != 0U && info->mods_addr != 0U) {
            const struct multiboot_module *module =
                (const struct multiboot_module *)info->mods_addr;
            for (unsigned int index = 0U; index < info->mods_count; ++index) {
                unsigned int module_size = module[index].end -
                                           module[index].start;
                if (module[index].end < module[index].start ||
                    module_size > BOOT_APP_MAX_SIZE) {
                    continue;
                }
                const char *module_name =
                    (const char *)module[index].string;
                int is_filedemo = module_name != 0 &&
                    module_name[0] == 'f' && module_name[1] == 'i' &&
                    module_name[2] == 'l' && module_name[3] == 'e' &&
                    module_name[4] == 'd' && module_name[5] == 'e' &&
                    module_name[6] == 'm' && module_name[7] == 'o' &&
                    module_name[8] == '\0';
                unsigned char *destination = is_filedemo
                    ? boot_filedemo_image : boot_app_image;
                if (is_filedemo) {
                    boot_filedemo_image_size = module_size;
                } else if (boot_app_image_size == 0U) {
                    boot_app_image_size = module_size;
                } else {
                    continue;
                }
                const unsigned char *source =
                    (const unsigned char *)module[index].start;
                for (unsigned int i = 0U; i < module_size; ++i) {
                    destination[i] = source[i];
                }
            }
        }
        if ((info->flags & MULTIBOOT_INFO_MEMORY) != 0U) {
            memory_lower_kib = info->mem_lower;
            memory_upper_kib = info->mem_upper;
            memory_info_available = 1;
        }
        if ((info->flags & MULTIBOOT_INFO_MEMORY_MAP) != 0U &&
            info->mmap_addr != 0U && info->mmap_length != 0U) {
            capture_memory_map(info->mmap_addr, info->mmap_length);
        }
    }

    initialize_page_allocator();
    gdt_initialize();
    keyboard_initialize();
    /* Install the IDT first so a page fault during paging setup is
       reported cleanly instead of triple-faulting. */
    initialize_idt();
    int mouse_result = mouse_initialize();
    initialize_paging();
    initialize_task_model();
    (void)heap_initialize();
    int file_store_result = initialize_file_store();
    display_initialize();
    put_string("TinyOS shell. Type help for commands.\n");
    if (file_store_result == 0) {
        put_string("Persistent file store ready.\n");
    } else {
        put_string("Persistent file store unavailable: ");
        put_string(file_store_error_message(file_store_result));
        put_string(".\n");
    }
    put_string(mouse_result ? "PS/2 mouse ready on IRQ 12.\n"
                            : "PS/2 mouse unavailable.\n");
    show_prompt();

    /* Enable interrupts so timer, keyboard, and mouse drivers can run. */
    __asm__ volatile ("sti");

    for (;;) {
        /* Block until the keyboard IRQ1 handler has buffered a character. */
        while (!keyboard_has_input()) {
            __asm__ volatile ("hlt");
        }

        char character = keyboard_read_char();

        if (character == '\n') {
            put_char('\n');
            run_command();
            continue;
        }

        if (character == '\b') {
            if (command_length > 0U) {
                --command_length;
                command[command_length] = '\0';
                put_char('\b');
            }
            continue;
        }

        if (character != '\0' && command_length < COMMAND_MAX - 1U) {
            command[command_length++] = character;
            command[command_length] = '\0';
            put_char(character);
        }
    }
}

