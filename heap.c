#include "heap.h"

#define KERNEL_HEAP_BYTES (64U * 1024U)
#define HEAP_ALIGNMENT 16U
#define HEAP_HEADER_BYTES 16U
#define HEAP_MAGIC 0x48454150U
#define HEAP_BLOCK_FREE 0U
#define HEAP_BLOCK_USED 1U
#define HEAP_DEBUG_MAX_ALLOCATIONS (KERNEL_HEAP_BYTES / (HEAP_HEADER_BYTES + HEAP_ALIGNMENT))

struct heap_block {
    unsigned int total_size;
    unsigned int previous_size;
    unsigned int magic;
    unsigned int state;
};

static struct heap_block *block_at(unsigned int offset);

static unsigned char heap_area[KERNEL_HEAP_BYTES]
    __attribute__((aligned(HEAP_ALIGNMENT)));
static int heap_ready = 0;
static int heap_corrupt = 0;
static unsigned int heap_allocation_count = 0U;
#ifdef HEAP_DEBUG
struct heap_debug_record {
    unsigned int address;
    unsigned int active;
};
static struct heap_debug_record heap_debug_records[HEAP_DEBUG_MAX_ALLOCATIONS];
static unsigned int heap_invalid_free_count = 0U;
static unsigned int heap_double_free_count = 0U;

enum {
    HEAP_DEBUG_FREE_INVALID = 0,
    HEAP_DEBUG_FREE_ACTIVE = 1,
    HEAP_DEBUG_FREE_ALREADY_FREED = 2
};

static int heap_debug_register(void *pointer)
{
    unsigned int address = (unsigned int)pointer;
    unsigned int reusable = HEAP_DEBUG_MAX_ALLOCATIONS;
    for (unsigned int i = 0U; i < HEAP_DEBUG_MAX_ALLOCATIONS; ++i) {
        if (heap_debug_records[i].address == address) {
            if (heap_debug_records[i].active) {
                return 0;
            }
            reusable = i;
            break;
        }
        if (reusable == HEAP_DEBUG_MAX_ALLOCATIONS &&
            heap_debug_records[i].address == 0U) {
            reusable = i;
        }
    }
    if (reusable == HEAP_DEBUG_MAX_ALLOCATIONS) {
        for (unsigned int i = 0U; i < HEAP_DEBUG_MAX_ALLOCATIONS; ++i) {
            if (!heap_debug_records[i].active) {
                reusable = i;
                break;
            }
        }
    }
    if (reusable == HEAP_DEBUG_MAX_ALLOCATIONS) {
        return 0;
    }
    heap_debug_records[reusable].address = address;
    heap_debug_records[reusable].active = 1U;
    return 1;
}

static int heap_debug_release(void *pointer)
{
    unsigned int address = (unsigned int)pointer;
    for (unsigned int i = 0U; i < HEAP_DEBUG_MAX_ALLOCATIONS; ++i) {
        if (heap_debug_records[i].address == address) {
            if (heap_debug_records[i].active) {
                heap_debug_records[i].active = 0U;
                return HEAP_DEBUG_FREE_ACTIVE;
            }
            ++heap_double_free_count;
            return HEAP_DEBUG_FREE_ALREADY_FREED;
        }
    }
    ++heap_invalid_free_count;
    return HEAP_DEBUG_FREE_INVALID;
}

static void heap_debug_record_failed_free(void *pointer)
{
    if (pointer == 0) {
        ++heap_invalid_free_count;
        return;
    }
    (void)heap_debug_release(pointer);
}

static int heap_debug_validate(unsigned int used_blocks)
{
    unsigned int active = 0U;
    for (unsigned int i = 0U; i < HEAP_DEBUG_MAX_ALLOCATIONS; ++i) {
        if (!heap_debug_records[i].active) {
            continue;
        }
        ++active;
        unsigned int address = heap_debug_records[i].address;
        unsigned int start = (unsigned int)&heap_area[0];
        if (address < start + HEAP_HEADER_BYTES ||
            address >= start + KERNEL_HEAP_BYTES) {
            return 0;
        }
        unsigned int target = address - start - HEAP_HEADER_BYTES;
        unsigned int offset = 0U;
        int found = 0;
        while (offset < KERNEL_HEAP_BYTES) {
            struct heap_block *block = block_at(offset);
            if (offset == target && block->state == HEAP_BLOCK_USED) {
                found = 1;
                break;
            }
            offset += block->total_size;
        }
        if (!found) {
            return 0;
        }
    }
    return active == used_blocks && active == heap_allocation_count;
}
#endif

static unsigned int align_size(unsigned int size)
{
    if (size > 0xFFFFFFFFU - (HEAP_ALIGNMENT - 1U)) {
        return 0U;
    }
    return (size + HEAP_ALIGNMENT - 1U) & ~(HEAP_ALIGNMENT - 1U);
}

static struct heap_block *block_at(unsigned int offset)
{
    return (struct heap_block *)(void *)(heap_area + offset);
}

/* Validate the physical block chain before using any block metadata. */
static int heap_validate(void)
{
    if (!heap_ready || heap_corrupt) {
        return 0;
    }

    unsigned int offset = 0U;
    unsigned int previous_size = 0U;
    unsigned int used_blocks = 0U;
    while (offset < KERNEL_HEAP_BYTES) {
        struct heap_block *block = block_at(offset);
        if (block->magic != HEAP_MAGIC ||
            block->total_size < HEAP_HEADER_BYTES + HEAP_ALIGNMENT ||
            (block->total_size & (HEAP_ALIGNMENT - 1U)) != 0U ||
            block->previous_size != previous_size ||
            block->total_size > KERNEL_HEAP_BYTES - offset ||
            (block->state != HEAP_BLOCK_FREE && block->state != HEAP_BLOCK_USED)) {
            heap_corrupt = 1;
            return 0;
        }
        if (block->state == HEAP_BLOCK_USED) {
            ++used_blocks;
        }
        previous_size = block->total_size;
        offset += block->total_size;
    }

    if (offset != KERNEL_HEAP_BYTES) {
        heap_corrupt = 1;
        return 0;
    }
#ifdef HEAP_DEBUG
    if (!heap_debug_validate(used_blocks)) {
        heap_corrupt = 1;
        return 0;
    }
#else
    if (used_blocks != heap_allocation_count) {
        heap_corrupt = 1;
        return 0;
    }
#endif
    return 1;
}

int heap_initialize(void)
{
    struct heap_block *first = block_at(0U);
    first->total_size = KERNEL_HEAP_BYTES;
    first->previous_size = 0U;
    first->magic = HEAP_MAGIC;
    first->state = HEAP_BLOCK_FREE;
    heap_allocation_count = 0U;
    heap_corrupt = 0;
#ifdef HEAP_DEBUG
    for (unsigned int i = 0U; i < HEAP_DEBUG_MAX_ALLOCATIONS; ++i) {
        heap_debug_records[i].address = 0U;
        heap_debug_records[i].active = 0U;
    }
    heap_invalid_free_count = 0U;
    heap_double_free_count = 0U;
#endif
    heap_ready = 1;
    return 1;
}

void *kmalloc(unsigned int size)
{
    if (size == 0U || !heap_validate()) {
        return 0;
    }

    unsigned int payload_size = align_size(size);
    if (payload_size == 0U ||
        payload_size > KERNEL_HEAP_BYTES - HEAP_HEADER_BYTES) {
        return 0;
    }

    unsigned int offset = 0U;
    while (offset < KERNEL_HEAP_BYTES) {
        struct heap_block *block = block_at(offset);
        unsigned int available = block->total_size - HEAP_HEADER_BYTES;
        if (block->state == HEAP_BLOCK_FREE && available >= payload_size) {
            void *payload = (void *)(heap_area + offset + HEAP_HEADER_BYTES);
#ifdef HEAP_DEBUG
            if (!heap_debug_register(payload)) {
                return 0;
            }
#endif
            unsigned int requested_total = HEAP_HEADER_BYTES + payload_size;
            unsigned int remainder = block->total_size - requested_total;
            if (remainder >= HEAP_HEADER_BYTES + HEAP_ALIGNMENT) {
                unsigned int old_total = block->total_size;
                block->total_size = requested_total;
                struct heap_block *next = block_at(offset + requested_total);
                next->total_size = old_total - requested_total;
                next->previous_size = requested_total;
                next->magic = HEAP_MAGIC;
                next->state = HEAP_BLOCK_FREE;
                unsigned int next_offset = offset + old_total;
                if (next_offset < KERNEL_HEAP_BYTES) {
                    block_at(next_offset)->previous_size = next->total_size;
                }
            }
            block->state = HEAP_BLOCK_USED;
            ++heap_allocation_count;
            return payload;
        }
        offset += block->total_size;
    }
    return 0;
}

int kfree(void *pointer)
{
    if (pointer == 0) {
#ifdef HEAP_DEBUG
        heap_debug_record_failed_free(pointer);
#endif
        return 0;
    }
    if (!heap_validate()) {
        return 0;
    }

    unsigned int address = (unsigned int)pointer;
    unsigned int start = (unsigned int)&heap_area[0];
    if (address < start + HEAP_HEADER_BYTES ||
        address >= start + KERNEL_HEAP_BYTES ||
        ((address - start) & (HEAP_ALIGNMENT - 1U)) != 0U) {
#ifdef HEAP_DEBUG
        heap_debug_record_failed_free(pointer);
#endif
        return 0;
    }

    unsigned int target_offset = address - start - HEAP_HEADER_BYTES;
    unsigned int offset = 0U;
    while (offset < KERNEL_HEAP_BYTES) {
        struct heap_block *block = block_at(offset);
        if (offset == target_offset) {
            if (block->state != HEAP_BLOCK_USED) {
#ifdef HEAP_DEBUG
                heap_debug_record_failed_free(pointer);
#endif
                return 0;
            }
#ifdef HEAP_DEBUG
            if (heap_debug_release(pointer) != HEAP_DEBUG_FREE_ACTIVE) {
                return 0;
            }
#endif
            block->state = HEAP_BLOCK_FREE;
            if (heap_allocation_count > 0U) {
                --heap_allocation_count;
            }

            unsigned int next_offset = offset + block->total_size;
            if (next_offset < KERNEL_HEAP_BYTES) {
                struct heap_block *next = block_at(next_offset);
                if (next->state == HEAP_BLOCK_FREE) {
                    block->total_size += next->total_size;
                    next_offset = offset + block->total_size;
                    if (next_offset < KERNEL_HEAP_BYTES) {
                        block_at(next_offset)->previous_size = block->total_size;
                    }
                }
            }

            if (block->previous_size > 0U) {
                unsigned int previous_offset = offset - block->previous_size;
                struct heap_block *previous = block_at(previous_offset);
                if (previous->state == HEAP_BLOCK_FREE) {
                    previous->total_size += block->total_size;
                    next_offset = previous_offset + previous->total_size;
                    if (next_offset < KERNEL_HEAP_BYTES) {
                        block_at(next_offset)->previous_size = previous->total_size;
                    }
                }
            }
            return 1;
        }
        offset += block->total_size;
    }
#ifdef HEAP_DEBUG
    heap_debug_record_failed_free(pointer);
#endif
    return 0;
}

int heap_get_stats(struct kernel_heap_stats *stats)
{
    if (stats == 0 || !heap_validate()) {
        return 0;
    }

    stats->capacity = 0U;
    stats->used = 0U;
    stats->free = 0U;
    stats->largest_free = 0U;
    stats->allocations = heap_allocation_count;
#ifdef HEAP_DEBUG
    stats->invalid_frees = heap_invalid_free_count;
    stats->double_frees = heap_double_free_count;
    stats->debug_enabled = 1;
#else
    stats->invalid_frees = 0U;
    stats->double_frees = 0U;
    stats->debug_enabled = 0;
#endif

    unsigned int offset = 0U;
    while (offset < KERNEL_HEAP_BYTES) {
        struct heap_block *block = block_at(offset);
        unsigned int payload = block->total_size - HEAP_HEADER_BYTES;
        stats->capacity += payload;
        if (block->state == HEAP_BLOCK_FREE) {
            stats->free += payload;
            if (payload > stats->largest_free) {
                stats->largest_free = payload;
            }
        } else {
            stats->used += payload;
        }
        offset += block->total_size;
    }
    return 1;
}

