#include "model/tree_builder.h"

#include <algorithm>

namespace apkinfo {

uint32_t TreeBuilder::ensure_dir(std::string_view path) {
    // Directory entries in Alpine packages are written as "usr/bin/"; the slash is
    // not part of the path, and keeping it would create a bogus "" child node.
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    if (path.empty()) return 0;
    const std::string key(path);

    // Linear search is fine at package scale (a few thousand directories) and
    // keeps node order deterministic, which the tests rely on.
    for (const auto& entry : directories_) {
        if (entry.first == key) return entry.second;
    }

    const size_t slash = key.find_last_of('/');
    // Create the parent first so that a parent always has a lower index than any
    // of its descendants (the size accumulation below depends on that).
    const uint32_t parent = slash == std::string::npos ? 0 : ensure_dir(std::string_view(key).substr(0, slash));

    Temp node;
    node.name = slash == std::string::npos ? key : key.substr(slash + 1);
    node.path = key;
    node.is_dir = true;

    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(std::move(node));
    nodes_[parent].children.push_back(index);
    directories_.emplace_back(key, index);
    return index;
}

void TreeBuilder::add_file(std::string_view path, uint64_t size, uint8_t detail) {
    if (path.empty()) return;
    const std::string key(path);
    const size_t slash = key.find_last_of('/');
    const uint32_t parent = slash == std::string::npos ? 0 : ensure_dir(std::string_view(key).substr(0, slash));

    Temp node;
    node.name = slash == std::string::npos ? key : key.substr(slash + 1);
    node.path = key;
    node.is_dir = false;
    node.size = size;
    node.detail = detail;

    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(std::move(node));
    nodes_[parent].children.push_back(index);
    ++file_count_;
}

void TreeBuilder::add_dir(std::string_view path) {
    if (!path.empty()) ensure_dir(path);
}

std::vector<TreeNode> TreeBuilder::finish(std::string root_name) {
    nodes_[0].name = std::move(root_name);
    nodes_[0].is_dir = true;

    // Directories first, then plain lexical order - the usual file-manager order.
    for (Temp& node : nodes_) {
        std::sort(node.children.begin(), node.children.end(), [this](uint32_t a, uint32_t b) {
            const Temp& lhs = nodes_[a];
            const Temp& rhs = nodes_[b];
            if (lhs.is_dir != rhs.is_dir) return lhs.is_dir;
            return lhs.name < rhs.name;
        });
    }

    // Accumulate directory sizes bottom-up: every child index is greater than its
    // parent's, so one reverse pass is enough.
    for (size_t i = nodes_.size(); i-- > 0;) {
        if (!nodes_[i].is_dir) continue;
        for (uint32_t child : nodes_[i].children) nodes_[i].size += nodes_[child].size;
    }

    // Pre-order DFS with an explicit stack. Pushing children in reverse keeps
    // them sorted, and because a child's own subtree is pushed on top of its
    // siblings, every subtree ends up contiguous.
    //
    // A node's subtree size is only known once its last descendant has been
    // emitted, so a sentinel frame is pushed under the children and fixes the
    // size on the way back up.
    std::vector<TreeNode> flat;
    flat.reserve(nodes_.size());
    struct Frame {
        uint32_t index;
        uint8_t depth;
        bool closing = false;
    };
    std::vector<Frame> stack{{0, 0, false}};
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();

        if (frame.closing) {
            flat[frame.index].subtree = static_cast<uint32_t>(flat.size()) - frame.index;
            continue;
        }

        const Temp& source = nodes_[frame.index];
        TreeNode row;
        row.name = source.name;
        row.path = source.path;
        row.is_dir = source.is_dir;
        row.size = source.size;
        row.detail = source.detail;
        row.depth = frame.depth;
        row.children = static_cast<uint32_t>(source.children.size());
        // The children are emitted right after this node, so their start index is
        // simply the next slot.
        row.first_child = row.children ? static_cast<uint32_t>(flat.size()) + 1 : 0;
        row.subtree = 1;
        const uint32_t emitted = static_cast<uint32_t>(flat.size());
        flat.push_back(std::move(row));

        stack.push_back(Frame{emitted, 0, true});
        for (size_t i = source.children.size(); i-- > 0;) {
            stack.push_back(Frame{source.children[i], static_cast<uint8_t>(frame.depth + 1), false});
        }
    }
    return flat;
}

}  // namespace apkinfo
