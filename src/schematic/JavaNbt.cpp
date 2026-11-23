#include "schematic/JavaNbt.h"
#include "ll/api/utils/CompressUtils.h"
#include <fstream>

namespace creatorx::schematic {
namespace {
constexpr size_t fileLimit = 256 * 1024 * 1024;

// 基岩 NBT 没有 TAG_Long_Array。使用字节数组承载同样的大端数据，交由现有 NBT 读写器处理其他字段。
class LongArrays {
public:
    LongArrays(std::string& bytes, bool writing) : mBytes(bytes), mWriting(writing) {}

    void convert() {
        if (number(1) != 10) invalid();
        string();
        payload(10, 0);
        if (mCursor != mBytes.size()) invalid();
    }

private:
    [[noreturn]] static void invalid() { throw std::runtime_error("invalid Java NBT"); }

    std::string_view take(size_t length) {
        if (length > mBytes.size() - mCursor) invalid();
        auto start = mCursor;
        mCursor += length;
        return {mBytes.data() + start, length};
    }

    std::uint32_t number(size_t length) {
        std::uint32_t value = 0;
        for (unsigned char byte : take(length)) value = (value << 8) | byte;
        return value;
    }

    std::string_view string() { return take(number(2)); }

    void array(size_t width, bool convertLength) {
        auto offset = mCursor;
        auto count = number(4);
        if (count > (mBytes.size() - mCursor) / width) invalid();
        take(count * width);
        if (!convertLength) return;
        if (mWriting && count % 8 != 0) invalid();
        auto replacement = mWriting ? count / 8 : count * 8;
        for (int i = 3; i >= 0; --i) {
            mBytes[offset + i] = char(replacement & 0xff);
            replacement >>= 8;
        }
    }

    void payload(unsigned type, unsigned depth, bool packed = false) {
        if (depth > 128) invalid();
        switch (type) {
        case 1: take(1); break;
        case 2: take(2); break;
        case 3: case 5: take(4); break;
        case 4: case 6: take(8); break;
        case 7: array(1, packed); break;
        case 8: string(); break;
        case 9: {
            auto typeOffset = mCursor;
            auto child = number(1);
            auto count = number(4);
            if (count > mBytes.size() - mCursor || (count != 0 && child == 0) || child > 12) invalid();
            if (child == 12) mBytes[typeOffset] = 7;
            for (std::uint32_t i = 0; i < count; ++i) payload(child, depth + 1);
            break;
        }
        case 10:
            while (true) {
                auto typeOffset = mCursor;
                auto child = number(1);
                if (child == 0) break;
                auto name = string();
                bool packedChild = mWriting && child == 7 && name == "BlockStates";
                if (packedChild) mBytes[typeOffset] = 12;
                if (child == 12) mBytes[typeOffset] = 7;
                payload(child, depth + 1, packedChild);
            }
            break;
        case 11: array(4, false); break;
        case 12: array(8, true); break;
        default: invalid();
        }
    }

    std::string& mBytes;
    size_t mCursor = 0;
    bool mWriting;
};
} // namespace

CompoundTag readJavaNbt(std::filesystem::path const& path) {
    auto size = std::filesystem::file_size(path);
    if (size > fileLimit) throw std::runtime_error("Java NBT exceeds 256 MiB");
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::failbit | std::ios::badbit);
    std::string bytes(size, '\0');
    input.read(bytes.data(), bytes.size());
    if (std::string_view(bytes).starts_with("\x1f\x8b")) {
        auto unpacked = ll::compress_utils::decompress(bytes, ll::compress_utils::CompressFormat::Gzip, fileLimit);
        if (!unpacked) throw std::runtime_error(unpacked.error().message());
        bytes = std::move(*unpacked);
    }
    LongArrays(bytes, false).convert();
    auto root = CompoundTag::fromBinaryNbt(bytes, false);
    if (!root) throw std::runtime_error(root.error().message());
    return std::move(*root);
}

std::string encodeJavaNbt(CompoundTag const& root, bool litematic) {
    auto bytes = root.toBinaryNbt(false);
    if (bytes.size() > fileLimit) throw std::runtime_error("Java NBT exceeds 256 MiB");
    if (litematic) LongArrays(bytes, true).convert();
    auto packed = ll::compress_utils::compress(bytes);
    if (!packed) throw std::runtime_error(packed.error().message());
    return std::move(*packed);
}
} // namespace creatorx::schematic
