#include "heap.h"

#define KERNEL_HEAP_BYTES (64U * 1024U)
#define HEAP_ALIGNMENT 16U
#define HEAP_HEADER_BYTES 16U
#define HEAP_MAGIC 0x48454150U
#define HEAP_BLOCK_FREE 0U
#define HEAP_BLOCK_USED 1U

struct heap_block {
    unsigned int total_size;
    unsigned int previous_size;
    unsigned int magic;
    unsigned int state;
};

static unsigned char heap_area[KERNEL_HEAP_BYTES]
    __attribute__((aligned(HEAP_ALIGNMENT)));
static int heap_ready = 0;
static int heap_corrupt = 0;
static unsigned int heap_allocation_count = 0U;

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
        previous_size = block->total_size;
        offset += block->total_size;
    }

    if (offset != KERNEL_HEAP_BYTES) {
        heap_corrupt = 1;
        return 0;
    }
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
            return (void *)(heap_area + offset + HEAP_HEADER_BYTES);
        }
        offset += block->total_size;
    }
    return 0;
}

int kfree(void *pointer)
{
    if (pointer == 0 || !heap_validate()) {
        return 0;
    }

    unsigned int address = (unsigned int)pointer;
    unsigned int start = (unsigned int)&heap_area[0];
    if (address < start + HEAP_HEADER_BYTES ||
        address >= start + KERNEL_HEAP_BYTES ||
        ((address - start) & (HEAP_ALIGNMENT - 1U)) != 0U) {
        return 0;
    }

    unsigned int target_offset = address - start - HEAP_HEADER_BYTES;
    unsigned int offset = 0U;
    while (offset < KERNEL_HEAP_BYTES) {
        struct heap_block *block = block_at(offset);
        if (offset == target_offset) {
            if (block->state != HEAP_BLOCK_USED) {
                return 0;
            }
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

