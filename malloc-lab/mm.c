/**
 * @file mm.c
 * @brief A 64-bit segregated explicit free-list allocator with mini blocks
 *
 * 15-213: Introduction to Computer Systems
 *
 * Allocated blocks store only a header and payload. Free blocks are tracked
 * in segregated free lists whose head pointers live in the heap prologue area.
 * Regular free blocks store a header, `next`/`prev` pointers in the payload,
 * and usually a footer. Mini free blocks are 16-byte free blocks that store
 * only a header and one `next` pointer, so they do not have room for a footer
 * or a back pointer. Very large free blocks at the physical end of the heap
 * may also omit their footer; the next header or epilogue records this with a
 * `prev_footerless` bit.
 *
 * Header layout:
 * - upper bits: block size, always a multiple of 16
 * - bit 0: current block allocated
 * - bit 1: previous physical block allocated
 * - bit 2: previous physical block is a mini free block
 * - bit 3: previous physical block is a footerless tail free block
 *
 * The allocator uses exact-size bins for small sizes and range bins for larger
 * sizes. Mini free blocks use a singly linked list in bucket 0; all other free
 * blocks use doubly linked lists, and the largest bucket is kept sorted by
 * size. Allocation removes a suitable free block, optionally splits it, and
 * updates the successor's predecessor metadata. Freeing writes the block back
 * as free, coalesces with adjacent free neighbors, and reinserts the merged
 * block into the appropriate free list.
 *
 *************************************************************************
 *
 * ADVICE FOR STUDENTS.
 * - Step 0: Please read the writeup!
 * - Step 1: Write your heap checker.
 * - Step 2: Write contracts / debugging assert statements.
 * - Good luck, and have fun!
 *
 *************************************************************************
 *
 * @author Melody Yin melodyyi@andrew.cmu.edu
 */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memlib.h"
#include "mm.h"

/* Do not change the following! */

#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#define memset mem_memset
#define memcpy mem_memcpy
#endif /* def DRIVER */

/* You can change anything from here onward */

/*
 *****************************************************************************
 * If DEBUG is defined (such as when running mdriver-dbg), these macros      *
 * are enabled. You can use them to print debugging output and to check      *
 * contracts only in debug mode.                                             *
 *                                                                           *
 * Only debugging macros with names beginning "dbg_" are allowed.            *
 * You may not define any other macros having arguments.                     *
 *****************************************************************************
 */

#ifdef DEBUG
/* When DEBUG is defined, these form aliases to useful functions */
#define dbg_requires(expr) assert(expr)
#define dbg_assert(expr) assert(expr)
#define dbg_ensures(expr) assert(expr)
#define dbg_printf(...) ((void)printf(__VA_ARGS__))
#else
/* When DEBUG is not defined, these should emit no code whatsoever,
 * not even from evaluation of argument expressions.  However,
 * argument expressions should still be syntax-checked and should
 * count as uses of any variables involved.  This used to use a
 * straightforward hack involving sizeof(), but that can sometimes
 * provoke warnings about misuse of sizeof().  I _hope_ that this
 * newer, less straightforward hack will be more robust.
 * Hat tip to Stack Overflow poster chqrlie (see
 * https://stackoverflow.com/questions/72647780).
 */
#define dbg_discard_expr_(...) ((void)((0) && printf(__VA_ARGS__)))
#define dbg_requires(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_assert(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_ensures(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_printf(...) dbg_discard_expr_(__VA_ARGS__)
#endif

/* Basic constants */

typedef uint64_t word_t;

/** @brief Word and header size (bytes) */
static const size_t wsize = sizeof(word_t);
/** @brief Double word size (bytes) */
static const size_t dsize = 2 * sizeof(word_t);

static const size_t min_alloc_block_size = dsize;
static const size_t min_free_block_size = 2 * dsize;

/**
 * @brief Default heap-growth request in bytes.
 *
 * The allocator asks `mem_sbrk` for at least this much space when it needs to
 * grow the heap for ordinary allocations. The value must remain aligned to
 * `dsize`.
 */
static const size_t chunksize = (1 << 11);

/**
 * @brief Bit masks used in packed block headers.
 *
 * `alloc_mask` marks the current block allocated. The remaining masks encode
 * metadata about the previous physical block so the allocator can coalesce
 * without always reading a footer.
 */
static const word_t alloc_mask = 0x1;
static const word_t prev_alloc_mask = 0x2;
static const word_t prev_mini_mask = 0x4;
static const word_t prev_footerless_mask = 0x8;
/**
 * @brief Mask that extracts the size field from a packed header word.
 *
 * The low 4 bits are reserved for status flags because all block sizes are
 * multiples of 16.
 */
static const word_t size_mask = ~(word_t)0xF;

static const size_t footerless_threshold = (1UL << 20);
static const size_t exact_bin_max_size = 512;

/** @brief Represents the header and payload of one block in the heap */
typedef struct block {
    /** @brief Header contains size + allocation flag */
    word_t header;

    union {
        struct {
            struct block *next;
            struct block *prev;
        } links;

        /**
         * @brief A pointer to the block payload.
         *
         * WARNING: A zero-length array must be the last element in a struct, so
         * there should not be any struct fields after it. For this lab, we will
         * allow you to include a zero-length array in a union, as long as the
         * union is the last field in its containing struct. However, this is
         * compiler-specific behavior and should be avoided in general.
         *
         * WARNING: DO NOT cast this pointer to/from other types! Instead, you
         * should use a union to alias this zero-length array with another
         * struct, in order to store additional types of data in the payload
         * memory.
         */
        char payload[0];
    } body;

} block_t;

/* Global variables */

/** @brief Pointer to first block in the heap */
static block_t *heap_start = NULL;
static block_t *heap_last = NULL;

#define EXACT_BIN_COUNT 32
#define RANGE_BIN_COUNT 6
#define SEG_SIZE (EXACT_BIN_COUNT + RANGE_BIN_COUNT + 1)
#define HEAP_META_PAD_WORDS (SEG_SIZE & 1)
#define PROLOGUE_WORD_INDEX (SEG_SIZE + HEAP_META_PAD_WORDS)
#define EPILOGUE_WORD_INDEX (PROLOGUE_WORD_INDEX + 1)
#define HEAP_INIT_WORDS (EPILOGUE_WORD_INDEX + 1)

static const size_t range_bin_thresholds[RANGE_BIN_COUNT] = {768,  1024, 1536,
                                                             2048, 4096, 16384};

static block_t *find_next(block_t *block);
static void split_block(block_t *block, size_t asize);
static void free_regular(void *bp);

/*
 *****************************************************************************
 * The functions below are short wrapper functions to perform                *
 * bit manipulation, pointer arithmetic, and other helper operations.        *
 *                                                                           *
 * We've given you the function header comments for the functions below      *
 * to help you understand how this baseline code works.                      *
 *                                                                           *
 * Note that these function header comments are short since the functions    *
 * they are describing are short as well; you will need to provide           *
 * adequate details for the functions that you write yourself!               *
 *****************************************************************************
 */

/*
 * ---------------------------------------------------------------------------
 *                        BEGIN SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Returns the maximum of two integers.
 * @param[in] x
 * @param[in] y
 * @return `x` if `x > y`, and `y` otherwise.
 */
static size_t max(size_t x, size_t y) {
    return (x > y) ? x : y;
}

/**
 * @brief Rounds `size` up to next multiple of n
 * @param[in] size
 * @param[in] n
 * @return The size after rounding up
 */
static size_t round_up(size_t size, size_t n) {
    return n * ((size + (n - 1)) / n);
}

static bool checked_add(size_t a, size_t b, size_t *result) {
    dbg_requires(result != NULL);

    if (a > SIZE_MAX - b) {
        return false;
    }

    *result = a + b;
    return true;
}

static bool checked_round_up(size_t size, size_t n, size_t *result) {
    dbg_requires(result != NULL);
    dbg_requires(n != 0);

    size_t remainder = size % n;
    if (remainder == 0) {
        *result = size;
        return true;
    }

    return checked_add(size, n - remainder, result);
}

static bool request_to_block_size(size_t size, size_t *asize) {
    dbg_requires(asize != NULL);

    size_t needed_size;
    if (!checked_add(size, wsize, &needed_size)) {
        return false;
    }

    if (!checked_round_up(needed_size, dsize, &needed_size)) {
        return false;
    }

    *asize = max(needed_size, min_alloc_block_size);
    return true;
}

static word_t *seg_head_word(int index) {
    dbg_requires(heap_start != NULL);
    dbg_requires(index >= 0 && index < SEG_SIZE);

    return ((word_t *)heap_start) - (SEG_SIZE + 1 + HEAP_META_PAD_WORDS) +
           index;
}

static block_t *get_seg_head(int index) {
    return (block_t *)(uintptr_t)(*seg_head_word(index));
}

static void set_seg_head(int index, block_t *block) {
    *seg_head_word(index) = (word_t)(uintptr_t)block;
}

static bool is_exact_bin(int index) {
    return index < EXACT_BIN_COUNT;
}

static bool is_mini_size(size_t size) {
    return size == min_alloc_block_size;
}

static bool is_footerless_size(size_t size) {
    return size >= footerless_threshold;
}

static word_t pack(size_t size, bool prev_footerless, bool prev_mini,
                   bool prev_alloc, bool alloc) {
    word_t word = size;
    if (prev_footerless) {
        word |= prev_footerless_mask;
    }
    if (prev_mini) {
        word |= prev_mini_mask;
    }
    if (prev_alloc) {
        word |= prev_alloc_mask;
    }
    if (alloc) {
        word |= alloc_mask;
    }
    return word;
}

/**
 * @brief Extracts the size represented in a packed word.
 *
 * This function simply clears the lowest 4 bits of the word, as the heap
 * is 16-byte aligned.
 *
 * @param[in] word
 * @return The size of the block represented by the word
 */
static size_t extract_size(word_t word) {
    return word & size_mask;
}

/**
 * @brief Returns the allocation status of a given header value.
 *
 * This is based on the lowest bit of the header value.
 *
 * @param[in] word
 * @return The allocation status correpsonding to the word
 */
static bool extract_alloc(word_t word) {
    return (bool)(word & alloc_mask);
}

static bool extract_prev_alloc(word_t word) {
    return (bool)(word & prev_alloc_mask);
}

static bool extract_prev_footerless(word_t word) {
    return (bool)(word & prev_footerless_mask);
}

static bool extract_prev_mini(word_t word) {
    return (bool)(word & prev_mini_mask);
}

/**
 * @brief Extracts the size of a block from its header.
 * @param[in] block
 * @return The size of the block
 */
static size_t get_size(block_t *block) {
    return extract_size(block->header);
}

/**
 * @brief Returns the allocation status of a block, based on its header.
 * @param[in] block
 * @return The allocation status of the block
 */
static bool get_alloc(block_t *block) {
    return extract_alloc(block->header);
}

static bool get_prev_alloc(block_t *block) {
    return extract_prev_alloc(block->header);
}

static bool get_prev_footerless(block_t *block) {
    return extract_prev_footerless(block->header);
}

static bool get_prev_mini(block_t *block) {
    return extract_prev_mini(block->header);
}

static word_t get_epilogue_word(void) {
    return *(word_t *)((char *)mem_heap_hi() + 1 - (ptrdiff_t)wsize);
}

/**
 * @brief Given a payload pointer, returns a pointer to the corresponding
 *        block.
 * @param[in] bp A pointer to a block's payload
 * @return The corresponding block
 */
static block_t *payload_to_header(void *bp) {
    return (block_t *)((char *)bp - offsetof(block_t, body.payload));
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        payload.
 * @param[in] block
 * @return A pointer to the block's payload
 * @pre The block must be a valid block, not a boundary tag.
 */
static void *header_to_payload(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0);
    return (void *)(block->body.payload);
}

static size_t get_payload_size(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_alloc(block));
    return get_size(block) - wsize;
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        footer.
 * @param[in] block
 * @return A pointer to the block's footer
 * @pre The block must be a valid block, not a boundary tag.
 */
static word_t *header_to_footer(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) >= min_free_block_size);
    dbg_requires(!get_alloc(block));
    return (word_t *)(block->body.payload + get_size(block) - dsize);
}

/**
 * @brief Given a block footer, returns a pointer to the corresponding
 *        header.
 * @param[in] footer A pointer to the block's footer
 * @return A pointer to the start of the block
 * @pre The footer must be the footer of a valid block, not a boundary tag.
 */
static block_t *footer_to_header(word_t *footer) {
    size_t size = extract_size(*footer);
    dbg_assert(size != 0);
    return (block_t *)((char *)footer + wsize - size);
}

/**
 * @brief Finds the next consecutive block on the heap.
 *
 * This function accesses the next block in the "implicit list" of the heap
 * by adding the size of the block.
 *
 * @param[in] block A block in the heap
 * @return The next consecutive block on the heap
 * @pre The block is not the epilogue
 */
static block_t *find_next(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0);
    return (block_t *)((char *)block + get_size(block));
}

static word_t *find_prev_footer(block_t *block) {
    dbg_requires(block != NULL);

    if (get_prev_alloc(block) || get_prev_mini(block) ||
        get_prev_footerless(block)) {
        return NULL;
    }

    // Compute previous footer position as one word before the header
    return &(block->header) - 1;
}

static block_t *find_prev(block_t *block) {
    dbg_requires(block != NULL);

    if (get_prev_alloc(block)) {
        return NULL;
    }

    if (get_prev_mini(block)) {
        return (block_t *)((char *)block - min_alloc_block_size);
    }

    word_t *footerp = find_prev_footer(block);
    if (footerp == NULL) {
        return NULL;
    }

    // Return NULL if called on first block in the heap
    if (extract_size(*footerp) == 0) {
        return NULL;
    }

    return footer_to_header(footerp);
}

/**
 * @brief Writes a block starting at the given address.
 *
 * This function writes the packed header for any block. It also writes a
 * matching footer for regular free blocks, except when the block is the large
 * footerless tail block at the end of the heap.
 *
 * @param[out] block The location to begin writing the block header
 * @param[in] size The size of the new block
 * @param[in] prev_footerless Whether the previous physical block omits a
 *                            footer.
 * @param[in] prev_mini Whether the previous physical block is a mini free
 *                      block.
 * @param[in] prev_alloc Whether the previous physical block is allocated.
 * @param[in] alloc The allocation status of the new block.
 * @pre `block` must be non-NULL and `size` must be positive.
 * @post The header matches the supplied metadata and any required footer is
 *       synchronized with the header.
 */
static void write_block(block_t *block, size_t size, bool prev_footerless,
                        bool prev_mini, bool prev_alloc, bool alloc) {
    dbg_requires(block != NULL);
    dbg_requires(size > 0);

    block->header = pack(size, prev_footerless, prev_mini, prev_alloc, alloc);

    // Mini free blocks and footerless tail blocks intentionally omit footers.
    bool footerless = (!alloc && !is_mini_size(size) && block == heap_last &&
                       is_footerless_size(size));
    if (!alloc && !is_mini_size(size) && !footerless) {
        word_t *footerp = header_to_footer(block);
        *footerp = block->header;
    }
}

/**
 * @brief Writes an epilogue header at the given address.
 *
 * The epilogue header has size 0, and is marked as allocated.
 *
 * @param[out] block The location to write the epilogue header
 */
static void write_epilogue(block_t *block, bool prev_footerless, bool prev_mini,
                           bool prev_alloc) {
    dbg_requires(block != NULL);
    block->header = pack(0, prev_footerless, prev_mini, prev_alloc, true);
}

static void set_prev_info(block_t *block, bool prev_footerless, bool prev_mini,
                          bool prev_alloc) {
    dbg_requires(block != NULL);

    block->header &= ~(prev_footerless_mask | prev_mini_mask | prev_alloc_mask);
    if (prev_footerless) {
        block->header |= prev_footerless_mask;
    }
    if (prev_mini) {
        block->header |= prev_mini_mask;
    }
    if (prev_alloc) {
        block->header |= prev_alloc_mask;
    }

    bool footerless =
        (!get_alloc(block) && block == heap_last &&
         !is_mini_size(get_size(block)) && is_footerless_size(get_size(block)));
    if (!get_alloc(block) && get_size(block) >= min_free_block_size &&
        !footerless) {
        word_t *footerp = header_to_footer(block);
        *footerp = block->header;
    }
}

static void update_next_prev_info(block_t *block) {
    dbg_requires(block != NULL);

    block_t *next = find_next(block);
    bool curr_alloc = get_alloc(block);
    bool curr_is_mini_free = (!curr_alloc && is_mini_size(get_size(block)));
    bool curr_is_footerless =
        (!curr_alloc && block == heap_last && !curr_is_mini_free &&
         is_footerless_size(get_size(block)));

    set_prev_info(next, curr_is_footerless, curr_is_mini_free, curr_alloc);
}

static int find_seg_index(size_t size) {
    dbg_requires(size >= min_alloc_block_size);

    if (size <= exact_bin_max_size) {
        return (int)(size / dsize) - 1;
    }

    for (int i = 0; i < RANGE_BIN_COUNT; i++) {
        if (size <= range_bin_thresholds[i]) {
            return EXACT_BIN_COUNT + i;
        }
    }

    return SEG_SIZE - 1;
}

static bool use_sorted_insert(int index) {
    return index == SEG_SIZE - 1;
}

static int search_limit_for_index(int index) {
    if (is_exact_bin(index)) {
        return 1;
    }
    if (index <= EXACT_BIN_COUNT + 1) {
        return 32;
    }
    if (index <= EXACT_BIN_COUNT + 3) {
        return 48;
    }
    if (index <= EXACT_BIN_COUNT + 5) {
        return 64;
    }
    return 80;
}

static bool close_enough_fit(size_t block_size, size_t asize) {
    return (block_size - asize) <= 16;
}

static void add_block(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    size_t size = get_size(block);
    int index = find_seg_index(size);

    // Mini free blocks use a singly linked list in bucket 0.
    if (is_mini_size(size)) {
        dbg_assert(index == 0);
        block->body.links.next = get_seg_head(0);
        set_seg_head(0, block);
        return;
    }

    block_t *head = get_seg_head(index);
    block->body.links.prev = NULL;
    block->body.links.next = NULL;

    if (head == NULL) {
        set_seg_head(index, block);
        return;
    }

    // Most buckets use LIFO insertion; the largest bucket stays size-sorted.
    if (!use_sorted_insert(index)) {
        block->body.links.next = head;
        head->body.links.prev = block;
        set_seg_head(index, block);
        return;
    }

    block_t *curr = head;
    block_t *prev = NULL;

    while (curr != NULL && get_size(curr) < size) {
        prev = curr;
        curr = curr->body.links.next;
    }

    block->body.links.prev = prev;
    block->body.links.next = curr;

    if (prev != NULL) {
        prev->body.links.next = block;
    } else {
        set_seg_head(index, block);
    }

    if (curr != NULL) {
        curr->body.links.prev = block;
    }
}

static void remove_block(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    size_t size = get_size(block);
    int index = find_seg_index(size);

    // Mini free blocks are removed by walking the singly linked mini list.
    if (is_mini_size(size)) {
        block_t *curr = get_seg_head(0);
        block_t *prev = NULL;

        while (curr != NULL && curr != block) {
            prev = curr;
            curr = curr->body.links.next;
        }

        dbg_assert(curr == block);

        if (prev == NULL) {
            set_seg_head(0, curr->body.links.next);
        } else {
            prev->body.links.next = curr->body.links.next;
        }

        curr->body.links.next = NULL;
        return;
    }

    if (block->body.links.prev != NULL) {
        block->body.links.prev->body.links.next = block->body.links.next;
    } else {
        set_seg_head(index, block->body.links.next);
    }

    if (block->body.links.next != NULL) {
        block->body.links.next->body.links.prev = block->body.links.prev;
    }

    block->body.links.prev = NULL;
    block->body.links.next = NULL;
}

static block_t *allocate_from_block(block_t *block, size_t asize) {
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    remove_block(block);

    size_t block_size = get_size(block);
    bool prev_footerless = get_prev_footerless(block);
    bool prev_alloc = get_prev_alloc(block);
    bool prev_mini = get_prev_mini(block);

    write_block(block, block_size, prev_footerless, prev_mini, prev_alloc,
                true);
    split_block(block, asize);
    return block;
}

/*
 * ---------------------------------------------------------------------------
 *                        END SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/******** The remaining content below are helper and debug routines ********/

/**
 * @brief Coalesces a free block with adjacent free neighbors when possible.
 *
 * The function consults the predecessor metadata in the current header and the
 * allocation bit of the next physical block to decide which of the four
 * coalescing cases applies. Any neighboring free blocks are removed from their
 * free lists before the merged block is written back and reinserted.
 *
 * @param[in] block Pointer to a block that has already been marked free.
 * @return Pointer to the coalesced free block.
 * @pre `block` must be a valid free block in the heap.
 * @post The returned block is in the free list and the next block's `prev_*`
 *       metadata matches the returned block.
 */
static block_t *coalesce_block(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    bool prev_alloc = get_prev_alloc(block);
    bool prev_footerless = get_prev_footerless(block);
    bool prev_mini = get_prev_mini(block);

    block_t *next_block = find_next(block);
    bool next_alloc = get_alloc(next_block);

    size_t size = get_size(block);

    if (prev_alloc && next_alloc) {
    } else if (!prev_alloc && next_alloc) {
        block_t *prev_block = find_prev(block);
        dbg_assert(prev_block != NULL);

        remove_block(prev_block);
        size += get_size(prev_block);

        bool prev_prev_alloc = get_prev_alloc(prev_block);
        bool prev_prev_footerless = get_prev_footerless(prev_block);
        bool prev_prev_mini = get_prev_mini(prev_block);

        if (block == heap_last) {
            heap_last = prev_block;
        }
        block = prev_block;
        write_block(block, size, prev_prev_footerless, prev_prev_mini,
                    prev_prev_alloc, false);
    } else if (prev_alloc && !next_alloc) {
        remove_block(next_block);
        size += get_size(next_block);
        if (next_block == heap_last) {
            heap_last = block;
        }
        write_block(block, size, prev_footerless, prev_mini, prev_alloc, false);
    } else {
        block_t *prev_block = find_prev(block);
        dbg_assert(prev_block != NULL);

        remove_block(prev_block);
        remove_block(next_block);

        size += get_size(prev_block) + get_size(next_block);

        bool prev_prev_alloc = get_prev_alloc(prev_block);
        bool prev_prev_footerless = get_prev_footerless(prev_block);
        bool prev_prev_mini = get_prev_mini(prev_block);

        block = prev_block;
        if (next_block == heap_last) {
            heap_last = block;
        }
        write_block(block, size, prev_prev_footerless, prev_prev_mini,
                    prev_prev_alloc, false);
    }

    update_next_prev_info(block);
    add_block(block);
    return block;
}

/**
 * @brief Extends the heap by a free block of at least `size` bytes.
 *
 * The requested size is rounded up to maintain 16-byte alignment. If the old
 * heap ended in a free block, the new space is merged into that tail block;
 * otherwise a new free block is created at the old epilogue position.
 *
 * @param[in] size Minimum number of bytes to request from `mem_sbrk`.
 * @return Pointer to the resulting free block, or `NULL` on failure.
 * @post On success, the heap ends with a valid epilogue and the returned block
 *       is present in the appropriate free list.
 */
static block_t *extend_heap(size_t size) {
    void *bp;
    word_t old_epilogue = get_epilogue_word();
    bool prev_footerless = extract_prev_footerless(old_epilogue);
    bool prev_mini = extract_prev_mini(old_epilogue);
    bool prev_alloc = extract_prev_alloc(old_epilogue);

    // Allocate an even number of words to maintain alignment
    if (!checked_round_up(size, dsize, &size)) {
        return NULL;
    }

    if (size > (size_t)INTPTR_MAX) {
        return NULL;
    }

    if (!prev_alloc) {
        dbg_assert(heap_last != NULL);
        dbg_assert(!get_alloc(heap_last));

        if ((bp = mem_sbrk((intptr_t)size)) == (void *)-1) {
            return NULL;
        }

        (void)bp;
        remove_block(heap_last);
        size += get_size(heap_last);

        bool prev_prev_alloc = get_prev_alloc(heap_last);
        bool prev_prev_footerless = get_prev_footerless(heap_last);
        bool prev_prev_mini = get_prev_mini(heap_last);

        write_block(heap_last, size, prev_prev_footerless, prev_prev_mini,
                    prev_prev_alloc, false);
        write_epilogue(find_next(heap_last),
                       is_footerless_size(get_size(heap_last)), false, false);
        add_block(heap_last);
        return heap_last;
    }

    dbg_assert(size >= min_alloc_block_size);
    if ((bp = mem_sbrk((intptr_t)size)) == (void *)-1) {
        return NULL;
    }

    // Initialize free block header/footer
    block_t *block = payload_to_header(bp);
    heap_last = block;
    write_block(block, size, prev_footerless, prev_mini, prev_alloc, false);

    // Create new epilogue header
    write_epilogue(find_next(block), is_footerless_size(size), false, false);
    add_block(block);
    return block;
}

/**
 * @brief Extends the heap specifically to satisfy a pending allocation.
 *
 * When the previous tail block is free, this defers to `extend_heap` and then
 * allocates from the resulting free block. Otherwise it constructs the new
 * allocated block directly in the freshly extended space and leaves any
 * remainder as the new tail free block.
 *
 * @param[in] extendsize Number of bytes to request from the system.
 * @param[in] asize Aligned block size needed for the allocation.
 * @return Pointer to an allocated block of size at least `asize`, or `NULL` on
 *         failure.
 */
static block_t *extend_heap_for_malloc(size_t extendsize, size_t asize) {
    dbg_requires(asize >= min_alloc_block_size);
    word_t old_epilogue = get_epilogue_word();
    bool prev_footerless = extract_prev_footerless(old_epilogue);
    bool prev_mini = extract_prev_mini(old_epilogue);
    bool prev_alloc = extract_prev_alloc(old_epilogue);

    if (!checked_round_up(extendsize, dsize, &extendsize)) {
        return NULL;
    }

    if (extendsize > (size_t)INTPTR_MAX) {
        return NULL;
    }

    if (!prev_alloc) {
        block_t *block = extend_heap(extendsize);
        if (block == NULL) {
            return NULL;
        }
        return allocate_from_block(block, asize);
    }

    dbg_assert(extendsize >= min_alloc_block_size);
    void *bp = mem_sbrk((intptr_t)extendsize);
    if (bp == (void *)-1) {
        return NULL;
    }

    block_t *block = payload_to_header(bp);
    size_t block_size = extendsize;
    size_t remainder_size = block_size - asize;

    // When the old tail is allocated, build the allocated block directly.
    if (remainder_size >= min_alloc_block_size) {
        write_block(block, asize, prev_footerless, prev_mini, prev_alloc, true);

        block_t *remainder = find_next(block);
        heap_last = remainder;
        write_block(remainder, remainder_size, false, false, true, false);
        write_epilogue(find_next(remainder), is_footerless_size(remainder_size),
                       false, false);
        add_block(remainder);
    } else {
        heap_last = block;
        write_block(block, block_size, prev_footerless, prev_mini, prev_alloc,
                    true);
        write_epilogue(find_next(block), false, false, true);
    }

    return block;
}

/**
 * @brief Splits an allocated block when the remainder can form a valid block.
 *
 * The front portion keeps size `asize` and remains allocated. Any remainder of
 * at least the minimum block size becomes a new free block that is inserted
 * into the free lists; otherwise the original block is left intact.
 *
 * @param[in] block Pointer to an allocated block chosen for placement.
 * @param[in] asize Aligned size to keep allocated at the front of `block`.
 * @pre `block` must be allocated and `asize` must not exceed its size.
 * @post The successor's predecessor metadata is updated to match the final
 *       physical layout.
 */
static void split_block(block_t *block, size_t asize) {
    dbg_requires(block != NULL);
    dbg_requires(get_alloc(block));

    size_t block_size = get_size(block);
    bool prev_footerless = get_prev_footerless(block);
    bool prev_alloc = get_prev_alloc(block);
    bool prev_mini = get_prev_mini(block);

    if ((block_size - asize) >= min_alloc_block_size) {
        size_t remainder_size = block_size - asize;
        bool was_last = (block == heap_last);

        write_block(block, asize, prev_footerless, prev_mini, prev_alloc, true);

        // Split off the remainder and return it to the appropriate free list.
        block_t *remainder = find_next(block);
        if (was_last) {
            heap_last = remainder;
        }
        write_block(remainder, remainder_size, false, false, true, false);

        update_next_prev_info(remainder);
        add_block(remainder);
    } else {
        update_next_prev_info(block);
    }

    dbg_ensures(get_alloc(block));
}

/**
 * @brief Searches the segregated free lists for a block that fits `asize`.
 *
 * Small exact-size bins are checked first. Larger bins perform a bounded
 * search for a good fit, while very large requests may search more
 * exhaustively to avoid poor placements.
 *
 * @param[in] asize Aligned size of the requested block.
 * @return A pointer to a suitable free block, or `NULL` if no fit exists.
 */
static block_t *find_fit(size_t asize) {
    int start = find_seg_index(asize);
    bool exhaustive_huge_search = asize >= footerless_threshold;

    if (start < EXACT_BIN_COUNT) {
        for (int i = start; i < EXACT_BIN_COUNT; i++) {
            block_t *head = get_seg_head(i);
            if (head != NULL) {
                return head;
            }
        }
        start = EXACT_BIN_COUNT;
    }

    for (int i = start; i < SEG_SIZE; i++) {
        block_t *best = NULL;
        size_t best_diff = (size_t)-1;
        int examined = 0;
        int limit = search_limit_for_index(i);

        for (block_t *block = get_seg_head(i);
             block != NULL && (exhaustive_huge_search || examined < limit);
             block = block->body.links.next) {
            examined++;

            dbg_assert(!get_alloc(block));

            size_t bsize = get_size(block);
            if (bsize < asize) {
                continue;
            }

            size_t diff = bsize - asize;
            if (diff < best_diff) {
                best_diff = diff;
                best = block;
            }

            if (diff == 0) {
                return block;
            }

            if (exhaustive_huge_search) {
                if (use_sorted_insert(i)) {
                    return block;
                }
                continue;
            }

            if (close_enough_fit(bsize, asize)) {
                return block;
            }

            if (use_sorted_insert(i) && best != NULL && diff > best_diff + 64) {
                break;
            }
        }

        if (best != NULL) {
            return best;
        }
    }

    return NULL;
}

static bool is_heap_address(const void *ptr) {
    const char *addr = (const char *)ptr;
    const char *lo = (const char *)mem_heap_lo();
    const char *hi = (const char *)mem_heap_hi();

    return addr >= lo && addr <= hi;
}

static bool is_aligned_block_size(size_t size) {
    return size != 0 && (size % dsize) == 0;
}

static bool is_valid_block_size(size_t size, bool alloc) {
    if (!is_aligned_block_size(size) || size < min_alloc_block_size) {
        return false;
    }

    if (alloc) {
        return true;
    }

    return is_mini_size(size) || size >= min_free_block_size;
}

static bool is_valid_free_list_link(block_t *block) {
    if (block == NULL) {
        return true;
    }

    if (!is_heap_address(block)) {
        return false;
    }

    return get_size(block) != 0 && !get_alloc(block) &&
           is_valid_block_size(get_size(block), false);
}

/**
 * @brief Checks the consistency of the heap and segregated free lists.
 *
 * The checker validates prologue/epilogue structure, block alignment, legal
 * block sizes, predecessor metadata bits, header/footer agreement for free
 * blocks that should have footers, absence of uncoalesced adjacent free
 * blocks, free-list link validity, bucket placement, and agreement between
 * heap and free-list free-block counts.
 *
 * @param[in] line Source line that requested the heap check, or `0` when
 *                 invoked by the driver.
 * @return `true` if all checked invariants hold, `false` otherwise.
 * @post The function is silent on success and prints diagnostics only on
 *       failure.
 */
bool mm_checkheap(int line) {
    void *lo = mem_heap_lo();
    void *hi = mem_heap_hi();

    size_t free_count_heap = 0;
    size_t free_count_list = 0;
    block_t *last_block = NULL;

    if (heap_start == NULL) {
        return true;
    }

    if (heap_last != NULL && !is_heap_address(heap_last)) {
        printf("Heap check failed: heap_last out of bounds at line %d\n", line);
        return false;
    }

    word_t *prologue = ((word_t *)heap_start) - 1;
    if (extract_size(*prologue) != 0 || !extract_alloc(*prologue)) {
        printf("Heap check failed: bad prologue at line %d\n", line);
        return false;
    }

    bool prev_alloc = true;
    bool prev_footerless = false;
    bool prev_mini = false;
    block_t *block;

    for (block = heap_start; block != NULL && get_size(block) > 0;
         block = find_next(block)) {
        size_t block_size = get_size(block);
        bool alloc = get_alloc(block);

        if ((void *)block < lo || (void *)block > hi) {
            printf("Heap check failed: block %p out of bounds at line %d\n",
                   (void *)block, line);
            return false;
        }

        if (!is_valid_block_size(block_size, alloc)) {
            printf("Heap check failed: invalid block size %zu at block %p line "
                   "%d\n",
                   block_size, (void *)block, line);
            return false;
        }

        if ((size_t)header_to_payload(block) % dsize != 0) {
            printf(
                "Heap check failed: payload not aligned at block %p line %d\n",
                (void *)block, line);
            return false;
        }

        if (get_prev_alloc(block) != prev_alloc) {
            printf(
                "Heap check failed: prev_alloc mismatch at block %p line %d\n",
                (void *)block, line);
            return false;
        }

        if (get_prev_footerless(block) != prev_footerless) {
            printf("Heap check failed: prev_footerless mismatch at block %p "
                   "line %d\n",
                   (void *)block, line);
            return false;
        }

        if (get_prev_mini(block) != prev_mini) {
            printf(
                "Heap check failed: prev_mini mismatch at block %p line %d\n",
                (void *)block, line);
            return false;
        }

        if (!alloc) {
            if (is_mini_size(block_size)) {
            } else if (block == heap_last && is_footerless_size(block_size)) {
            } else {
                // Regular free blocks should still have matching
                // headers/footers.
                word_t *footerp = header_to_footer(block);
                if ((void *)footerp < lo || (void *)footerp > hi) {
                    printf(
                        "Heap check failed: footer out of bounds at line %d\n",
                        line);
                    return false;
                }

                if (block->header != *footerp) {
                    printf("Heap check failed: header/footer mismatch at line "
                           "%d\n",
                           line);
                    return false;
                }
            }

            block_t *next = find_next(block);
            if (get_size(next) != 0 && !get_alloc(next)) {
                printf("Heap check failed: adjacent free blocks not coalesced "
                       "at line %d\n",
                       line);
                return false;
            }

            free_count_heap++;
        }

        last_block = block;
        prev_alloc = alloc;
        prev_footerless =
            (!prev_alloc && block == heap_last && !is_mini_size(block_size) &&
             is_footerless_size(block_size));
        prev_mini = (!prev_alloc && is_mini_size(block_size));
    }

    if (block == NULL) {
        printf("Heap check failed: traversal ended unexpectedly at line %d\n",
               line);
        return false;
    }

    if (get_size(block) != 0 || !get_alloc(block)) {
        printf("Heap check failed: bad epilogue at line %d\n", line);
        return false;
    }

    if (get_prev_alloc(block) != prev_alloc ||
        get_prev_footerless(block) != prev_footerless ||
        get_prev_mini(block) != prev_mini) {
        printf("Heap check failed: bad epilogue prev bits at line %d\n", line);
        return false;
    }

    if (last_block != heap_last) {
        printf("Heap check failed: heap_last mismatch at line %d\n", line);
        return false;
    }

    for (int i = 0; i < SEG_SIZE; i++) {
        block_t *head = get_seg_head(i);
        if (!is_valid_free_list_link(head)) {
            printf("Heap check failed: invalid free-list head at bucket %d "
                   "line %d\n",
                   i, line);
            return false;
        }

        for (block_t *curr = head; curr != NULL; curr = curr->body.links.next) {
            if ((void *)curr < lo || (void *)curr > hi) {
                printf("Heap check failed: free-list block out of bounds at "
                       "line %d\n",
                       line);
                return false;
            }

            if (get_alloc(curr)) {
                printf("Heap check failed: allocated block in free list at "
                       "line %d\n",
                       line);
                return false;
            }

            if (find_seg_index(get_size(curr)) != i) {
                printf("Heap check failed: wrong free-list bucket at line %d\n",
                       line);
                return false;
            }

            if (!is_valid_free_list_link(curr->body.links.next)) {
                printf("Heap check failed: invalid next free-list link at line "
                       "%d\n",
                       line);
                return false;
            }

            if (i == 0) {
                if (!is_mini_size(get_size(curr))) {
                    printf("Heap check failed: bucket 0 contains non-mini "
                           "block at line %d\n",
                           line);
                    return false;
                }

                if (curr->body.links.next != NULL &&
                    !is_mini_size(get_size(curr->body.links.next))) {
                    printf("Heap check failed: mini list links to non-mini "
                           "block at line %d\n",
                           line);
                    return false;
                }
            } else {
                if (!is_valid_free_list_link(curr->body.links.prev)) {
                    printf("Heap check failed: invalid prev free-list link at "
                           "line %d\n",
                           line);
                    return false;
                }

                if (curr->body.links.next != NULL &&
                    curr->body.links.next->body.links.prev != curr) {
                    printf("Heap check failed: broken next/prev links at line "
                           "%d\n",
                           line);
                    return false;
                }

                if (curr->body.links.prev != NULL &&
                    curr->body.links.prev->body.links.next != curr) {
                    printf("Heap check failed: broken prev/next links at line "
                           "%d\n",
                           line);
                    return false;
                }
            }

            free_count_list++;
        }
    }

    if (free_count_heap != free_count_list) {
        printf("Heap check failed: free count mismatch heap=%zu list=%zu at "
               "line %d\n",
               free_count_heap, free_count_list, line);
        return false;
    }

    return true;
}

/**
 * @brief Handles the main allocation path for a precomputed aligned size.
 *
 * The function first looks for a fitting free block in the segregated lists.
 * If none exists, it extends the heap. The chosen free block is then marked
 * allocated and split if profitable.
 *
 * @param[in] asize Aligned block size requested by the caller.
 * @return Payload pointer for the allocated block, or `NULL` on failure.
 */
static void *malloc_regular_asize(size_t asize) {
    // First try to satisfy the request from the segregated free lists.
    block_t *block = find_fit(asize);

    if (block == NULL) {
        size_t extendsize = max(asize, chunksize);
        block = extend_heap_for_malloc(extendsize, asize);
        if (block == NULL) {
            return NULL;
        }
    } else {
        dbg_assert(!get_alloc(block));
        block = allocate_from_block(block, asize);
    }

    return header_to_payload(block);
}

/**
 * @brief Frees a non-NULL payload pointer using the allocator's normal path.
 *
 * @param[in] bp Payload pointer previously returned by the allocator.
 * @pre `bp` must point to the payload of a currently allocated block.
 * @post The block is marked free, coalesced if possible, and reinserted into a
 *       free list.
 */
static void free_regular(void *bp) {
    dbg_requires(bp != NULL);

    block_t *block = payload_to_header(bp);
    dbg_assert(get_alloc(block));

    size_t size = get_size(block);
    bool prev_footerless = get_prev_footerless(block);
    bool prev_alloc = get_prev_alloc(block);
    bool prev_mini = get_prev_mini(block);

    // Mark the block free, then merge with adjacent free neighbors if possible.
    write_block(block, size, prev_footerless, prev_mini, prev_alloc, false);
    coalesce_block(block);
}

/**
 * @brief Initializes allocator state for a fresh heap.
 *
 * The function reserves heap space for segregated-list metadata plus the
 * prologue and epilogue words, zeros the metadata area, installs the boundary
 * tags, and extends the heap with the initial free block.
 *
 * @return `true` on success, `false` if the heap cannot be initialized.
 * @post On success, `heap_start` points at the first physical block header and
 *       the free lists contain one initial free block.
 */
bool mm_init(void) {
    // Create the initial empty heap
    word_t *start = (word_t *)(mem_sbrk(HEAP_INIT_WORDS * wsize));
    if (start == (void *)-1) {
        return false;
    }

    for (int i = 0; i < PROLOGUE_WORD_INDEX; i++) {
        start[i] = 0;
    }

    start[PROLOGUE_WORD_INDEX] = pack(0, false, false, true, true);
    start[EPILOGUE_WORD_INDEX] = pack(0, false, false, true, true);

    // Heap starts with first "block header", currently the epilogue
    heap_start = (block_t *)&(start[EPILOGUE_WORD_INDEX]);
    heap_last = NULL;

    // Extend the empty heap with a free block of chunksize bytes
    if (extend_heap(chunksize) == NULL) {
        heap_start = NULL;
        heap_last = NULL;
        return false;
    }

    return true;
}

/**
 * @brief Allocates a block whose payload can hold at least `size` bytes.
 *
 * Requests are rounded up to include allocator overhead and to preserve
 * 16-byte alignment. A fitting free block is reused when possible; otherwise
 * the heap is extended.
 *
 * @param[in] size Number of payload bytes requested by the caller.
 * @return Pointer to the payload of a newly allocated block, or `NULL` if the
 *         request cannot be satisfied.
 * @post Non-NULL results are 16-byte aligned and do not overlap other
 *       allocated blocks.
 */
void *malloc(size_t size) {
    dbg_requires(mm_checkheap(__LINE__));

    // Initialize heap if it isn't initialized
    if (heap_start == NULL) {
        if (!mm_init()) {
            return NULL;
        }
    }

    // Ignore spurious request
    if (size == 0) {
        dbg_ensures(mm_checkheap(__LINE__));
        return NULL;
    }

    // Adjust block size to include overhead and to meet alignment requirements
    size_t asize;
    if (!request_to_block_size(size, &asize)) {
        return NULL;
    }

    // Search the free list for a fit
    void *bp = malloc_regular_asize(asize);
    if (bp == NULL) {
        return NULL;
    }

    dbg_ensures(mm_checkheap(__LINE__));
    return bp;
}

/**
 * @brief Frees a previously allocated payload pointer.
 *
 * A `NULL` pointer is ignored. Non-NULL pointers are forwarded to the regular
 * free path, which marks the block free and coalesces it with free neighbors.
 *
 * @param[in] bp Payload pointer returned by `malloc`, `calloc`, or `realloc`,
 *               or `NULL`.
 */
void free(void *bp) {
    dbg_requires(mm_checkheap(__LINE__));

    if (bp == NULL) {
        return;
    }

    // Try to coalesce the block with its neighbors
    free_regular(bp);

    dbg_ensures(mm_checkheap(__LINE__));
}

/**
 * @brief Resizes an existing allocation.
 *
 * The implementation handles the required special cases
 * `realloc(NULL, size)` and `realloc(ptr, 0)`. For nontrivial reallocations it
 * first tries to reuse the current block by shrinking in place or by growing
 * into the following free block; if that fails, it allocates a new block,
 * copies the preserved payload prefix, and frees the old block.
 *
 * @param[in] ptr Existing payload pointer, or `NULL`.
 * @param[in] size Requested new payload size in bytes.
 * @return Pointer to the resized payload, or `NULL` on failure or when
 *         `size == 0`.
 */
void *realloc(void *ptr, size_t size) {
    // If ptr is NULL, then equivalent to malloc
    if (ptr == NULL) {
        return malloc(size);
    }

    // If size == 0, then free block and return NULL
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    block_t *block = payload_to_header(ptr);
    size_t old_size = get_size(block);

    size_t asize;
    if (!request_to_block_size(size, &asize)) {
        return NULL;
    }

    if (old_size >= asize) {
        size_t remainder = old_size - asize;
        if (remainder >= min_alloc_block_size) {
            bool prev_footerless = get_prev_footerless(block);
            bool prev_alloc = get_prev_alloc(block);
            bool prev_mini = get_prev_mini(block);
            bool was_last = (block == heap_last);

            write_block(block, asize, prev_footerless, prev_mini, prev_alloc,
                        true);

            block_t *split = find_next(block);
            if (was_last) {
                heap_last = split;
            }
            write_block(split, remainder, false, false, true, false);

            coalesce_block(split);
        } else {
            if (block == heap_last) {
                heap_last = block;
            }
            update_next_prev_info(block);
        }
        return ptr;
    }

    block_t *next = find_next(block);
    if (get_size(next) != 0 && !get_alloc(next)) {
        size_t combined = old_size + get_size(next);
        if (combined >= asize) {
            remove_block(next);

            bool prev_footerless = get_prev_footerless(block);
            bool prev_alloc = get_prev_alloc(block);
            bool prev_mini = get_prev_mini(block);
            bool next_was_last = (next == heap_last);

            write_block(block, combined, prev_footerless, prev_mini, prev_alloc,
                        true);

            size_t remainder = combined - asize;
            if (remainder >= min_alloc_block_size) {
                write_block(block, asize, prev_footerless, prev_mini,
                            prev_alloc, true);

                block_t *split = find_next(block);
                if (next_was_last) {
                    heap_last = split;
                }
                write_block(split, remainder, false, false, true, false);

                coalesce_block(split);
            } else {
                if (next_was_last) {
                    heap_last = block;
                }
                update_next_prev_info(block);
            }

            return ptr;
        }
    }

    // Otherwise, proceed with reallocation
    void *newptr = malloc(size);

    // If malloc fails, the original block is left untouched
    if (newptr == NULL) {
        return NULL;
    }

    // Copy the old data
    size_t copy_size = get_payload_size(block);
    if (size < copy_size) {
        copy_size = size;
    }

    memcpy(newptr, ptr, copy_size);

    // Free the old block
    free(ptr);
    return newptr;
}

/**
 * @brief Allocates zero-initialized storage for an array.
 *
 * The total byte count is computed as `elements * size` with overflow
 * detection. On success, the allocated payload is cleared to zero.
 *
 * @param[in] elements Number of array elements requested.
 * @param[in] size Size in bytes of each element.
 * @return Pointer to zero-initialized payload storage, or `NULL` on overflow
 *         or allocation failure.
 */
void *calloc(size_t elements, size_t size) {
    if (elements == 0) {
        return NULL;
    }

    size_t asize = elements * size;
    if (asize / elements != size) {
        // Multiplication overflowed
        return NULL;
    }

    void *bp = malloc(asize);
    if (bp == NULL) {
        return NULL;
    }

    // Initialize all bits to 0
    memset(bp, 0, asize);
    return bp;
}

/*
 *****************************************************************************
 * Do not delete the following super-secret(tm) lines!                       *
 *                                                                           *
 * 53 6f 20 79 6f 75 27 72 65 20 74 72 79 69 6e 67 20 74 6f 20               *
 *                                                                           *
 * 66 69 67 75 72 65 20 6f 75 74 20 77 68 61 74 20 74 68 65 20               *
 * 68 65 78 61 64 65 63 69 6d 61 6c 20 64 69 67 69 74 73 20 64               *
 * 6f 2e 2e 2e 20 68 61 68 61 68 61 21 20 41 53 43 49 49 20 69               *
 *                                                                           *
 * 73 6e 27 74 20 74 68 65 20 72 69 67 68 74 20 65 6e 63 6f 64               *
 * 69 6e 67 21 20 4e 69 63 65 20 74 72 79 2c 20 74 68 6f 75 67               *
 * 68 21 20 2d 44 72 2e 20 45 76 69 6c 0a c5 7c fc 80 6e 57 0a               *
 *                                                                           *
 *****************************************************************************
 */
