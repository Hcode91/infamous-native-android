// Which clip blocks each named animation uses. Animation nodes (plain clips and the blend nodes) point,
// through the pack's object table, at clip objects; a clip object owns one or more clip blocks.
#pragma once
#include "assets/pack.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ina {

struct ClipRef {
    uint32_t block = 0;        // data-area offset of a "CD" block
    float duration = 0;        // seconds, as stored beside the pointer
};

struct ClipObject {
    uint32_t offset = 0;
    uint32_t nameOid = 0;
    std::vector<ClipRef> blocks;   // in file order
};

struct AnimNode {
    uint32_t offset = 0;
    uint32_t nameOid = 0;
    uint32_t classId = 0;
    std::vector<uint32_t> clipObjects;   // indexes into AnimSet::clips, in the order the node lists them
};

struct AnimSet {
    std::vector<ClipObject> clips;
    std::vector<AnimNode> nodes;
    // First node with this name (names come from oid_manifest.txt), or nullptr.
    const AnimNode* find(const std::vector<std::string>& names, const std::string& name) const;
};

constexpr uint32_t kAnimClipClass = 0x229;

// `names` is used to recognise animation nodes: objects whose name starts with "anim_".
AnimSet readAnimSet(const Pack& pack, const std::vector<std::string>& names);

// Root movement stored in a clip block (translation over the whole clip, cm), or false when it has none.
bool clipRootMotion(const Pack& pack, uint32_t block, float out[3]);

}  // namespace ina
