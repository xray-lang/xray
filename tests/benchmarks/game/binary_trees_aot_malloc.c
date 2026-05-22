/*
 * Binary Trees Benchmark - xray AOT version (malloc-based, fair comparison)
 *
 * Simulates what xray AOT would generate WITHOUT escape analysis/arena:
 * - Struct promotion: Json {left, right} → C struct with two pointers
 * - malloc/free for each node (same cost model as Go/Dart GC)
 * - Direct recursion, no closure overhead
 *
 * This is a "fair" comparison — same allocation model as GC languages.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct Tree {
    struct Tree *left;
    struct Tree *right;
} Tree;

static Tree *makeTree(int depth) {
    Tree *t = (Tree *) malloc(sizeof(Tree));
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

static void freeTree(Tree *node) {
    if (!node)
        return;
    freeTree(node->left);
    freeTree(node->right);
    free(node);
}

static void run(int maxDepth) {
    int minDepth = 4;

    int stretchDepth = maxDepth + 1;
    Tree *stretchTree = makeTree(stretchDepth);
    printf("stretch tree of depth %d\t check: %d\n", stretchDepth, checksum(stretchTree));
    freeTree(stretchTree);

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
            freeTree(tree);
        }
        printf("%d\t trees of depth %d\t check: %d\n", iterations, depth, check);
    }

    printf("long lived tree of depth %d\t check: %d\n", maxDepth, checksum(longLivedTree));
    freeTree(longLivedTree);
}

int main(int argc, char **argv) {
    int maxDepth = 16;
    if (argc > 1) {
        maxDepth = atoi(argv[1]);
    }
    run(maxDepth);
    return 0;
}
