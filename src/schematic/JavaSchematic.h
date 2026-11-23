#pragma once

#include "exec/JobScheduler.h"
#include "schematic/JavaBlocks.h"
#include <filesystem>

namespace creatorx::schematic {
struct JavaWarnings {
    std::uint64_t blocks = 0;
    size_t blockEntities = 0;
    size_t actors = 0;
};

struct JavaRegion {
    core::Vec3i min;
    core::Vec3i size;
    std::vector<JavaBlock> palette;
    std::vector<std::uint32_t> blocks;
};

struct JavaStructure {
    core::Vec3i size{};
    core::Vec3i origin{};
    core::Vec3i offset{};
    std::vector<JavaRegion> regions;
    std::uint64_t total = 0;
    JavaWarnings warnings;
};

JavaStructure readJavaStructure(std::filesystem::path const& path, bool litematic, std::uint64_t limit);
std::string writeJavaStructure(edit::Clipboard const& clipboard, bool litematic, JavaWarnings& warnings);

class JavaImport {
public:
    explicit JavaImport(JavaStructure source);
    bool step(exec::Budget const& budget);
    std::uint64_t total() const { return mSource.total; }
    std::uint64_t progress() const { return mProgress; }
    JavaWarnings const& warnings() const { return mSource.warnings; }
    std::shared_ptr<edit::Clipboard const> clipboard() const { return mClipboard; }

private:
    struct Cell {
        edit::Clipboard::Cell blocks;
        bool approximate;
    };
    JavaStructure mSource;
    std::shared_ptr<edit::Clipboard> mClipboard;
    std::vector<Cell> mPalette;
    size_t mRegion = 0;
    size_t mIndex = 0;
    std::uint64_t mProgress = 0;
};
} // namespace creatorx::schematic
