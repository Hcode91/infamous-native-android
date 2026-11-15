// PSARC archive reader (the game's two world archives are PSARC 1.3, zlib, 64 KiB blocks).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ina {

class Psarc {
public:
    struct Entry {
        std::string name;      // path from the archive's manifest, leading '/' removed
        uint64_t size = 0;     // uncompressed size
        uint64_t offset = 0;   // file offset of the first block
        uint32_t firstBlock = 0;
    };

    explicit Psarc(const std::string& path);
    ~Psarc();
    Psarc(const Psarc&) = delete;
    Psarc& operator=(const Psarc&) = delete;

    const std::vector<Entry>& entries() const { return entries_; }   // manifest itself is not listed
    int find(const std::string& name) const;                         // -1 when absent
    std::vector<uint8_t> read(size_t index) const;

private:
    std::vector<uint8_t> readRaw(const Entry& e) const;
    FILE* f_ = nullptr;
    uint32_t blockSize_ = 0;
    std::vector<uint32_t> blockSizes_;
    std::vector<Entry> entries_;
};

}  // namespace ina
