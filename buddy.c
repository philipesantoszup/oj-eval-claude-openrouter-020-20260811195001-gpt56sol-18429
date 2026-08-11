#include "buddy.h"

#include <stdint.h>
#include <stdlib.h>

#define MAX_RANK 16
#define PAGE_SIZE 4096U
#define FREE 1
#define ALLOCATED 2

struct page_info {
    int start;
    int heap_pos;
    unsigned char rank;
    unsigned char state;
};

struct free_heap {
    int *blocks;
    int count;
    int capacity;
};

static unsigned char *pool;
static int page_count;
static struct page_info *pages;
static struct free_heap free_heaps[MAX_RANK + 1];

static int block_pages(int rank) {
    return 1 << (rank - 1);
}

static void reset_state(void) {
    int rank;

    free(pages);
    pages = NULL;
    for (rank = 1; rank <= MAX_RANK; ++rank) {
        free(free_heaps[rank].blocks);
        free_heaps[rank].blocks = NULL;
        free_heaps[rank].count = 0;
        free_heaps[rank].capacity = 0;
    }
    pool = NULL;
    page_count = 0;
}

static void mark_block(int start, int rank, int state) {
    int i;
    int count = block_pages(rank);

    for (i = start; i < start + count; ++i) {
        pages[i].start = start;
        pages[i].rank = (unsigned char)rank;
        pages[i].state = (unsigned char)state;
        pages[i].heap_pos = -1;
    }
}

static void heap_swap(int rank, int a, int b) {
    int temporary = free_heaps[rank].blocks[a];

    free_heaps[rank].blocks[a] = free_heaps[rank].blocks[b];
    free_heaps[rank].blocks[b] = temporary;
    pages[free_heaps[rank].blocks[a]].heap_pos = a;
    pages[free_heaps[rank].blocks[b]].heap_pos = b;
}

static void heap_add(int start, int rank) {
    struct free_heap *heap = &free_heaps[rank];
    int position = heap->count++;

    heap->blocks[position] = start;
    pages[start].heap_pos = position;
    while (position > 0) {
        int parent = (position - 1) / 2;
        if (heap->blocks[parent] <= heap->blocks[position])
            break;
        heap_swap(rank, parent, position);
        position = parent;
    }
}

static int heap_remove_at(int rank, int position) {
    struct free_heap *heap = &free_heaps[rank];
    int removed = heap->blocks[position];
    int last = heap->blocks[--heap->count];

    pages[removed].heap_pos = -1;
    if (position == heap->count)
        return removed;

    heap->blocks[position] = last;
    pages[last].heap_pos = position;
    if (position > 0 && heap->blocks[position] < heap->blocks[(position - 1) / 2]) {
        while (position > 0) {
            int parent = (position - 1) / 2;
            if (heap->blocks[parent] <= heap->blocks[position])
                break;
            heap_swap(rank, parent, position);
            position = parent;
        }
    } else {
        for (;;) {
            int left = position * 2 + 1;
            int right = left + 1;
            int smallest = position;

            if (left < heap->count && heap->blocks[left] < heap->blocks[smallest])
                smallest = left;
            if (right < heap->count && heap->blocks[right] < heap->blocks[smallest])
                smallest = right;
            if (smallest == position)
                break;
            heap_swap(rank, position, smallest);
            position = smallest;
        }
    }
    return removed;
}

static int heap_pop(int rank) {
    return heap_remove_at(rank, 0);
}

static int pointer_to_page(void *address) {
    uintptr_t value;
    uintptr_t base;
    uintptr_t offset;
    uintptr_t pool_size;

    if (address == NULL || pool == NULL)
        return -1;
    value = (uintptr_t)address;
    base = (uintptr_t)pool;
    pool_size = (uintptr_t)page_count * PAGE_SIZE;
    if (value < base)
        return -1;
    offset = value - base;
    if (offset >= pool_size || offset % PAGE_SIZE != 0)
        return -1;
    return (int)(offset / PAGE_SIZE);
}

int init_page(void *p, int pgcount) {
    int rank;
    int index;

    if (p == NULL || pgcount <= 0)
        return -EINVAL;
    reset_state();

    pages = calloc((size_t)pgcount, sizeof(*pages));
    if (pages == NULL)
        return -ENOSPC;

    for (rank = 1; rank <= MAX_RANK; ++rank) {
        int capacity = (pgcount - 1) / block_pages(rank) + 1;
        free_heaps[rank].blocks = malloc((size_t)capacity * sizeof(int));
        if (free_heaps[rank].blocks == NULL) {
            reset_state();
            return -ENOSPC;
        }
        free_heaps[rank].capacity = capacity;
    }

    pool = p;
    page_count = pgcount;
    index = 0;
    while (index < page_count) {
        int remaining = page_count - index;
        int chosen_rank = 1;

        for (rank = 2; rank <= MAX_RANK; ++rank) {
            int size = block_pages(rank);
            if (size > remaining || index % size != 0)
                break;
            chosen_rank = rank;
        }
        mark_block(index, chosen_rank, FREE);
        heap_add(index, chosen_rank);
        index += block_pages(chosen_rank);
    }
    return OK;
}

void *alloc_pages(int rank) {
    int available_rank;
    int start;

    if (rank < 1 || rank > MAX_RANK)
        return ERR_PTR(-EINVAL);
    for (available_rank = rank; available_rank <= MAX_RANK; ++available_rank) {
        if (free_heaps[available_rank].count != 0)
            break;
    }
    if (available_rank > MAX_RANK)
        return ERR_PTR(-ENOSPC);

    start = heap_pop(available_rank);
    while (available_rank > rank) {
        int buddy;

        --available_rank;
        buddy = start + block_pages(available_rank);
        mark_block(start, available_rank, FREE);
        mark_block(buddy, available_rank, FREE);
        heap_add(buddy, available_rank);
    }
    mark_block(start, rank, ALLOCATED);
    return pool + (size_t)start * PAGE_SIZE;
}

int return_pages(void *p) {
    int start = pointer_to_page(p);
    int rank;

    if (start < 0 || pages[start].start != start || pages[start].state != ALLOCATED)
        return -EINVAL;

    rank = pages[start].rank;
    mark_block(start, rank, FREE);
    while (rank < MAX_RANK) {
        int buddy = start ^ block_pages(rank);
        int merged_start;

        if (buddy < 0 || buddy > page_count - block_pages(rank))
            break;
        if (pages[buddy].start != buddy || pages[buddy].state != FREE ||
            pages[buddy].rank != rank || pages[buddy].heap_pos < 0)
            break;

        heap_remove_at(rank, pages[buddy].heap_pos);
        merged_start = buddy < start ? buddy : start;
        ++rank;
        start = merged_start;
        mark_block(start, rank, FREE);
    }
    heap_add(start, rank);
    return OK;
}

int query_ranks(void *p) {
    int index = pointer_to_page(p);

    if (index < 0)
        return -EINVAL;
    return pages[index].rank;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK)
        return -EINVAL;
    return free_heaps[rank].count;
}
