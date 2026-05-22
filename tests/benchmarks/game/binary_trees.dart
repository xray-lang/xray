// Binary Trees Benchmark - Dart version
// For performance comparison

class Tree {
  Tree? left;
  Tree? right;
  Tree({this.left, this.right});
}

Tree makeTree(int depth) {
  if (depth > 0) {
    return Tree(left: makeTree(depth - 1), right: makeTree(depth - 1));
  }
  return Tree();
}

int checksum(Tree? node) {
  if (node == null) return 0;
  if (node.left == null) return 1;
  return checksum(node.left) + checksum(node.right) + 1;
}

void run(int maxDepth) {
  final minDepth = 4;

  // Stretch tree
  final stretchDepth = maxDepth + 1;
  final stretchTree = makeTree(stretchDepth);
  print('stretch tree of depth $stretchDepth\t check: ${checksum(stretchTree)}');

  // Long-lived tree
  final longLivedTree = makeTree(maxDepth);

  // Iterations
  for (var depth = minDepth; depth <= maxDepth; depth += 2) {
    var iterations = 1;
    for (var j = 0; j < maxDepth - depth + minDepth; j++) {
      iterations *= 2;
    }
    var check = 0;
    for (var i = 1; i <= iterations; i++) {
      final tree = makeTree(depth);
      check += checksum(tree);
    }
    print('$iterations\t trees of depth $depth\t check: $check');
  }

  print('long lived tree of depth $maxDepth\t check: ${checksum(longLivedTree)}');
}

void main(List<String> args) {
  final maxDepth = args.isNotEmpty ? (int.tryParse(args[0]) ?? 16) : 16;
  run(maxDepth);
}
