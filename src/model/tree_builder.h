// Builds the lazily expandable file tree shared by both package formats.
//
// A package can list tens of thousands of entries; rendering them all is what
// made the window sluggish. The tree stores names, sizes and structure only
// (children of a node are contiguous), so the UI can expand one directory
// without touching the rest, and only visible rows are ever built.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "model/info_document.h"

namespace apkinfo {

class TreeBuilder {
public:
    // Adds a file; missing parent directories are created on the way.
    // `detail` is a TreeNode::detail code (0 none, 1 symlink, 2 dex, ...).
    void add_file(std::string_view path, uint64_t size, uint8_t detail = 0);

    // Adds a directory explicitly (Alpine packages list directories as entries
    // too); parents are created as needed.
    void add_dir(std::string_view path);

    // Builds the flattened tree. Node 0 is the root, named after `root_name`.
    std::vector<TreeNode> finish(std::string root_name = "");

    size_t file_count() const { return file_count_; }
    bool empty() const { return file_count_ == 0 && directories_.empty(); }

private:
    uint32_t ensure_dir(std::string_view path);

    struct Temp {
        std::string name;
        std::string path;
        bool is_dir = false;
        uint64_t size = 0;
        uint8_t detail = 0;
        std::vector<uint32_t> children;
    };

    std::vector<Temp> nodes_{Temp{}};                       // node 0 = root
    std::vector<std::pair<std::string, uint32_t>> directories_;  // path -> node
    size_t file_count_ = 0;
};

}  // namespace apkinfo
