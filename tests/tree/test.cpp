// Regression tests for the lazy file tree (src/model/tree_builder.cpp).
//
// The tree is what keeps a 10 000 entry package responsive, and it has one
// subtle invariant that a bug slipped through before: children are contiguous,
// but a sibling does NOT directly follow its predecessor - that child's whole
// subtree does - so a collapsed node must be skipped by `subtree`, not by
// "first_child + children".
#include <cstdio>
#include <string>
#include <vector>

#include "model/tree_builder.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}

// Walks the flat tree exactly the way the UI does and returns the visible names
// (prefix "*" marks an expanded directory's children).
std::vector<std::string> render(const std::vector<apkinfo::TreeNode>& tree, const std::vector<char>& expanded,
                                size_t index, int depth, std::vector<std::string>& out) {
    const apkinfo::TreeNode& node = tree[index];
    out.push_back(std::string(static_cast<size_t>(depth) * 2, ' ') + node.name + (node.is_dir ? "/" : ""));
    const bool open = node.is_dir && index < expanded.size() && expanded[index];
    if (!open || node.children == 0) return out;
    size_t child = index + 1;
    for (uint32_t i = 0; i < node.children && child < tree.size(); ++i) {
        render(tree, expanded, child, depth + 1, out);
        child += tree[child].subtree ? tree[child].subtree : 1;
    }
    return out;
}

void test_structure() {
    apkinfo::TreeBuilder builder;
    builder.add_file("META-INF/services/a.Foo", 10);
    builder.add_file("META-INF/services/b.Bar", 20);
    builder.add_file("META-INF/MANIFEST.MF", 30);
    builder.add_file("assets/model.pb", 40);
    builder.add_file("classes.dex", 60);
    const std::vector<apkinfo::TreeNode> tree = builder.finish("pkg.apk");

    check(builder.file_count() == 5, "file count");
    check(tree.size() == 9, "node count (root + 3 dirs + 5 files), got " + std::to_string(tree.size()));
    check(tree[0].name == "pkg.apk" && tree[0].is_dir, "root is the package name");
    check(tree[0].children == 3, "root has 3 children (META-INF, assets, classes.dex)");
    check(tree[0].size == 160, "root accumulates every file size");
    check(tree[0].subtree == tree.size(), "root subtree covers the whole tree");

    // directories first, then files, both alphabetical
    check(tree[1].name == "META-INF", "first child is a directory");
    // depth first order: META-INF's subtree comes before its next sibling
    check(tree[2].name == "services" && tree[2].depth == 2, "a child's subtree follows it immediately");
    check(tree[6].name == "assets", "the next top-level directory comes after META-INF's subtree");

    // depth is recorded, and children always come after their parent
    for (size_t i = 1; i < tree.size(); ++i) {
        check(tree[i].depth >= 1, "non-root depth");
        if (tree[i].first_child) check(tree[i].first_child > i, "children follow their parent");
    }

    // every node belongs to exactly one subtree: walking by subtree sizes must
    // visit all nodes exactly once
    size_t visited = 0;
    size_t index = 0;
    while (index < tree.size()) {
        ++visited;
        index += tree[index].subtree ? tree[index].subtree : 1;
    }
    check(visited == 1, "top level walk visits only the root, got " + std::to_string(visited));

    size_t walked = 0;
    index = 0;
    while (index < tree.size()) {
        walked += tree[index].subtree;
        index += tree[index].subtree;
    }
    check(walked == tree.size(), "subtree sizes sum to the node count");
}

void test_directory_entries() {
    // Alpine packages list directories as entries ("usr/bin/"); the trailing slash
    // must not become an empty child node.
    apkinfo::TreeBuilder builder;
    builder.add_dir("usr/bin/");
    builder.add_dir("usr/lib/");
    builder.add_file("usr/bin/sh", 4);
    const std::vector<apkinfo::TreeNode> tree = builder.finish("pkg");

    for (const apkinfo::TreeNode& node : tree) {
        check(!node.name.empty(), "no empty node names");
    }
    check(builder.file_count() == 1, "one file");
    check(tree[0].children == 1 && tree[1].name == "usr", "usr is the single top-level directory");
    check(tree[1].children == 2, "usr has bin and lib");
}

void test_expansion() {
    apkinfo::TreeBuilder builder;
    builder.add_file("lib/arm64-v8a/liba.so", 100);
    builder.add_file("lib/x86_64/liba.so", 200);
    builder.add_file("res/values/strings.xml", 10);
    builder.add_file("AndroidManifest.xml", 20);
    const std::vector<apkinfo::TreeNode> tree = builder.finish("pkg");
    std::vector<char> expanded(tree.size(), 0);

    std::vector<std::string> rows;
    render(tree, expanded, 0, 0, rows);
    check(rows.size() == 1, "collapsed tree shows only the root, got " + std::to_string(rows.size()));

    expanded[0] = 1;
    rows.clear();
    render(tree, expanded, 0, 0, rows);
    check(rows.size() == 4, "root expanded: root + 3 children, got " + std::to_string(rows.size()));
    check(rows[1] == "  lib/" && rows[2] == "  res/" && rows[3] == "  AndroidManifest.xml",
          "top level order is directories then files");

    // expand lib/ only: its two children appear, res/ stays collapsed
    const size_t lib = 1;
    expanded[lib] = 1;
    rows.clear();
    render(tree, expanded, 0, 0, rows);
    check(rows.size() == 6, "lib expanded: 6 rows, got " + std::to_string(rows.size()));
    check(rows[2] == "    arm64-v8a/", "first lib child at depth 2");
    check(rows[4] == "  res/", "the sibling after lib is res, not one of lib's children");
}

void test_large_tree() {
    // 10 000 files in 100 directories: the shape of a real modern package.
    apkinfo::TreeBuilder builder;
    for (int dir = 0; dir < 100; ++dir) {
        for (int file = 0; file < 100; ++file) {
            builder.add_file("res/d" + std::to_string(dir) + "/f" + std::to_string(file) + ".png", 4);
        }
    }
    const std::vector<apkinfo::TreeNode> tree = builder.finish("big.apk");
    check(builder.file_count() == 10000, "10000 files");
    check(tree.size() == 10000 + 100 + 1 + 1, "nodes = files + dirs + res + root, got " + std::to_string(tree.size()));

    std::vector<char> expanded(tree.size(), 0);
    expanded[0] = 1;
    std::vector<std::string> rows;
    render(tree, expanded, 0, 0, rows);
    // root + res/ (+100 dirs hidden) => root, res
    check(rows.size() == 2, "one collapsed top-level directory below the root, got " + std::to_string(rows.size()));

    expanded[1] = 1;  // res/
    rows.clear();
    render(tree, expanded, 0, 0, rows);
    check(rows.size() == 2 + 100, "res expanded shows its 100 dirs, got " + std::to_string(rows.size()));
}

}  // namespace

int main() {
    test_structure();
    test_directory_entries();
    test_expansion();
    test_large_tree();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
