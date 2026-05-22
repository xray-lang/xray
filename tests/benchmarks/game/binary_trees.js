// Binary Trees Benchmark - Node.js version
// For performance comparison

function makeTree(depth) {
    if (depth > 0) {
        return { left: makeTree(depth - 1), right: makeTree(depth - 1) };
    }
    return { left: null, right: null };
}

function checksum(node) {
    if (node === null) return 0;
    if (node.left === null) return 1;
    return checksum(node.left) + checksum(node.right) + 1;
}

function run(maxDepth) {
    const minDepth = 4;

    // Stretch tree
    const stretchDepth = maxDepth + 1;
    const stretchTree = makeTree(stretchDepth);
    console.log(`stretch tree of depth ${stretchDepth}\t check: ${checksum(stretchTree)}`);

    // Long-lived tree
    const longLivedTree = makeTree(maxDepth);

    // Iterations
    for (let depth = minDepth; depth <= maxDepth; depth += 2) {
        let iterations = 1;
        for (let j = 0; j < maxDepth - depth + minDepth; j++) {
            iterations *= 2;
        }
        let check = 0;
        for (let i = 1; i <= iterations; i++) {
            const tree = makeTree(depth);
            check += checksum(tree);
        }
        console.log(`${iterations}\t trees of depth ${depth}\t check: ${check}`);
    }

    console.log(`long lived tree of depth ${maxDepth}\t check: ${checksum(longLivedTree)}`);
}

const maxDepth = process.argv[2] ? parseInt(process.argv[2]) : 16;
run(maxDepth);
