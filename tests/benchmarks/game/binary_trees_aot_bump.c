/*
 * Binary Trees Benchmark - xray AOT with bump allocator (fair GC simulation)
 *
 * Uses a bump allocator that grows monotonically (never resets).
 * This simulates the allocation cost of a GC language without the
 * collection cost — a realistic lower bound for what xray AOT could
 * achieve with a simple copying/compacting GC.
 *
 * KEY: Tree struct is 16 bytes (2 pointers), same as Go/Dart.
 * Allocation is a single pointer bump (same as Dart's young-gen).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct Tree {
    struct Tree *left;
    struct Tree *right;
} Tree;

/* ========== Bump Allocator (never frees) ========== */

#define BUMP_BLOCK_SIZE (16 * 1024 * 1024) /* 16MB blocks */

static char *bump_cursor;
static char *bump_end;

typedef struct BumpBlock {
    struct BumpBlock *next;
    char data[];
} BumpBlock;

static BumpBlock *bump_blocks;

static void bump_init(void) {
    BumpBlock *b = (BumpBlock *) malloc(sizeof(BumpBlock) + BUMP_BLOCK_SIZE);
    b->next = NULL;
    bump_cursor = b->data;
    bump_end = b->data + BUMP_BLOCK_SIZE;
    bump_blocks = b;
}

static inline void *bump_alloc(size_t size) {
    size = (size + 7) & ~(size_t) 7;
    if (__builtin_expect(bump_cursor + size <= bump_end, 1)) {
        void *p = bump_cursor;
        bump_cursor += size;
        return p;
    }
    /* Slow path: new block */
    size_t bsize = BUMP_BLOCK_SIZE;
    if (size > bsize)
        bsize = size;
    BumpBlock *b = (BumpBlock *) malloc(sizeof(BumpBlock) + bsize);
    b->next = bump_blocks;
    bump_blocks = b;
    bump_cursor = b->data;
    bump_end = b->data + bsize;
    void *p = bump_cursor;
    bump_cursor += size;
    return p;
}

static void bump_destroy(void) {
    BumpBlock *b = bump_blocks;
    while (b) {
        BumpBlock *next = b->next;
        free(b);
        b = next;
    }
}

/* ========== Binary Trees ========== */

static Tree *makeTree(int depth) {
    Tree *t = (Tree *) bump_alloc(sizeof(Tree));
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

    int stretchDepth = maxDepth + 1;
    Tree *stretchTree = makeTree(stretchDepth);
    printf("stretch tree of depth %d\t check: %d\n", stretchDepth, checksum(stretchTree));

    Tree *longLivedTree = makeTree(maxDepth);

    for (int depth = minDepth; depth <= maxDepth; depth += 2) {
        int iterations = 1;
        for (int j = 0; j < maxDepth - depth + minDepth; j++) {
            iterations *= 2;
        }
        int check = 0;
        for (int i = 1; i <= iterations; i++) {
            Tree *tree = makeTree(depth);
            check += checksum(tree);
        }
        printf("%d\t trees of depth %d\t check: %d\n", iterations, depth, check);
    }

    printf("long lived tree of depth %d\t check: %d\n", maxDepth, checksum(longLivedTree));

    bump_destroy();
}

int main(int argc, char **argv) {
    int maxDepth = 16;
    if (argc > 1) {
        maxDepth = atoi(argv[1]);
    }
    bump_init();
    run(maxDepth);
    return 0;
}
