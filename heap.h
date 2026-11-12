#ifndef TINYOS_HEAP_H
#define TINYOS_HEAP_H

struct kernel_heap_stats {
    unsigned int capacity;
    unsigned int used;
    unsigned int free;
    unsigned int largest_free;
    unsigned int allocations;
};

/* Initialize the bounded kernel heap. Safe to call once during boot. */
int heap_initialize(void);

/* Return a 16-byte-aligned block, or 0 when size is invalid or the heap is full. */
void *kmalloc(unsigned int size);

/* Free a pointer returned by kmalloc. Returns 1 on success, 0 if invalid. */
int kfree(void *pointer);

/* Return 1 and fill stats when the heap is healthy; otherwise return 0. */
int heap_get_stats(struct kernel_heap_stats *stats);

#endif

