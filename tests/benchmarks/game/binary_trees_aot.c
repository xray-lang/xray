/*
 * Binary Trees Benchmark - xray AOT optimized version
 *
 * Simulates what an ideal xray AOT compiler would generate:
 * - Struct promotion: Json {left, right} → C struct with two pointers
 * - Arena allocator: bump-pointer allocation, bulk reset per iteration
 * - No GC overhead: arena freed in bulk after each iteration
 * - Direct recursion: no closure/vtable indirection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct Tree {
    struct Tree *left;
    struct Tree *right;
} Tree;

/* ========== Arena Allocator ========== */

#define ARENA_BLOCK_SIZE (4 * 1024 * 1024) /* 4MB blocks */

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    char *cursor;
    char *end;
    char data[];
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
    ArenaBlock *current;
} Arena;

static ArenaBlock *arena_block_new(size_t size) {
    ArenaBlock *b = (ArenaBlock *) malloc(sizeof(ArenaBlock) + size);
    b->next = NULL;
    b->cursor = b->data;
    b->end = b->data + size;
    return b;
}

static void arena_init(Arena *a) {
    a->head = arena_block_new(ARENA_BLOCK_SIZE);
    a->current = a->head;
}

static inline void *arena_alloc(Arena *a, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t) 7;
    ArenaBlock *b = a->current;
    if (__builtin_expect(b->cursor + size <= b->end, 1)) {
        void *p = b->cursor;
        b->cursor += size;
        return p;
    }
    /* Slow path: try next existing block or allocate new */
    if (b->next) {
        b = b->next;
    } else {
        size_t block_size = ARENA_BLOCK_SIZE;
        if (size > block_size)
            block_size = size;
        b->next = arena_block_new(block_size);
        b = b->next;
    }
    b->cursor = b->data;
    a->current = b;
    void *p = b->cursor;
    b->cursor += size;
    return p;
}

static void arena_reset(Arena *a) {
    /* Reset all blocks' cursors without freeing memory */
    for (ArenaBlock *b = a->head; b; b = b->next) {
        b->cursor = b->data;
    }
    a->current = a->head;
}

static void arena_destroy(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->current = NULL;
}

/* ========== Binary Trees ========== */

static Arena g_arena;

static Tree *makeTree(int depth) {
    Tree *t = (Tree *) arena_alloc(&g_arena, sizeof(Tree));
    if (depth > 0) {
        t->left = makeTree(depth - 1);
        t->right = makeTree(depth - 1);
    } else {
        t->left = NULL;
        t->right = NULL;
    }
    return t;
}

static int checksum(const Tree *node) {
    if (!node)
        return 0;
    if (!node->left)
        return 1;
    return checksum(node->left) + checksum(node->right) + 1;
}

static void run(int maxDepth) {
    int minDepth = 4;

    /* Stretch tree */
    int stretchDepth = maxDepth + 1;
    arena_init(&g_arena);
    Tree *stretchTree = makeTree(stretchDepth);
    printf("stretch tree of depth %d\t check: %d\n", stretchDepth, checksum(stretchTree));

    /* Long-lived tree (separate arena) */
    Arena long_arena;
    arena_init(&long_arena);
    Arena saved = g_arena;
    g_arena = long_arena;
    Tree *longLivedTree = makeTree(maxDepth);
    long_arena = g_arena;
    g_arena = saved;

    /* Free stretch tree arena */
    arena_reset(&g_arena);

    /* Iterations */
    for (int depth = minDepth; depth <= maxDepth; depth += 2) {
        int iterations = 1;
        for (int j = 0; j < maxDepth - depth + minDepth; j++) {
            iterations *= 2;
        }
        int check = 0;
        for (int i = 1; i <= iterations; i++) {
            Tree *tree = makeTree(depth);
            check += checksum(tree);
            arena_reset(&g_arena);
        }
        printf("%d\t trees of depth %d\t check: %d\n", iterations, depth, check);
    }

    printf("long lived tree of depth %d\t check: %d\n", maxDepth, checksum(longLivedTree));

    arena_destroy(&g_arena);
    arena_destroy(&long_arena);
}

int main(int argc, char **argv) {
    int maxDepth = 16;
    if (argc > 1) {
        maxDepth = atoi(argv[1]);
    }
    run(maxDepth);
    return 0;
}
