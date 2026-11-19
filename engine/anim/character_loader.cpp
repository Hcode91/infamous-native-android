#include "anim/character_loader.h"

#include "core/bytes.h"

namespace ina {

const CharacterVariant* findVariant(const std::vector<CharacterVariant>& variants, const std::vector<std::string>& names, const std::string& name) {
    for (const CharacterVariant& v : variants)
        if (v.nameOid < names.size() && names[v.nameOid] == name) return &v;
    return nullptr;
}

std::vector<PackedMesh> variantMeshes(const Pack& pack, std::vector<PackedMesh> all, const CharacterVariant& variant, uint32_t skeletonOffset) {
    uint32_t anchor = skeletonOffset;
    for (const PackedMesh& m : all)
        if (m.optional && variantShows(variant, m)) {
            anchor = m.entryOffset;
            break;
        }
    const auto range = subFileRange(pack, anchor);
    std::vector<PackedMesh> out;
    for (PackedMesh& m : all)
        if (m.entryOffset >= range.first && m.entryOffset < range.second && variantShows(variant, m)) out.push_back(std::move(m));
    return out;
}

Skeleton skeletonForClips(const Pack& pack, uint32_t joints, uint32_t near) {
    Skeleton best;
    for (uint32_t at : findSkeletons(pack)) {
        Skeleton s = readSkeletonAt(pack, at);
        if (!s.jointCount || s.animatedJoints != joints) continue;
        if (!best.jointCount || (at <= near && at > best.offset) || (best.offset > near && at < best.offset)) best = std::move(s);
    }
    return best;
}

AnimClip readClipAt(const Pack& pack, uint32_t block) {
    const auto data = pack.data();
    if (size_t(block) + 0x60 > data.size()) throw FormatError("clip block offset");
    const uint32_t size = pack.word(size_t(block) + 8);
    if (size < 0x60 || size > data.size() - block) throw FormatError("clip block size");
    AnimClip c = parseAnimClip(data.subspan(block, size));
    c.offset = block;
    return c;
}

}  // namespace ina
