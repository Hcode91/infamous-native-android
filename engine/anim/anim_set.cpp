#include "anim/anim_set.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace ina {
namespace {

bool isClipBlock(std::span<const uint8_t> d, uint32_t at) {
    return (at & 15) == 0 && size_t(at) + 0x60 <= d.size() && d[at] == 'C' && d[at + 1] == 'D' && d[at + 2] == 1 && d[at + 3] == 7;
}

float f32at(const Pack& p, size_t at) {
    const uint32_t w = p.word(at);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

}  // namespace

const AnimNode* AnimSet::find(const std::vector<std::string>& names, const std::string& name) const {
    for (const AnimNode& n : nodes)
        if (n.nameOid < names.size() && names[n.nameOid] == name) return &n;
    return nullptr;
}

AnimSet readAnimSet(const Pack& pack, const std::vector<std::string>& names) {
    AnimSet set;
    const auto data = pack.data();
    std::vector<uint32_t> objects = pack.objects();
    objects.erase(std::remove_if(objects.begin(), objects.end(), [&](uint32_t o) { return size_t(o) + 0x40 > data.size(); }), objects.end());
    std::sort(objects.begin(), objects.end());
    const std::set<uint32_t> objectSet(objects.begin(), objects.end());

    // Clip objects, and where each one's own data starts (the pointer at +0x34, to its joint masks).
    std::vector<std::pair<uint32_t, size_t>> starts;   // data start, clip index
    std::vector<std::pair<uint32_t, size_t>> byOffset; // object offset, clip index
    for (uint32_t o : objects) {
        if (pack.word(o) != kAnimClipClass) continue;
        ClipObject c;
        c.offset = o;
        c.nameOid = pack.word(o + 0x0C);
        if (pack.isPointer(o + 0x34)) starts.push_back({pack.word(o + 0x34), set.clips.size()});
        byOffset.push_back({o, set.clips.size()});
        set.clips.push_back(c);
    }
    std::sort(starts.begin(), starts.end());
    // Every pointer to a clip block belongs to the clip object whose data starts last before it.
    if (!starts.empty()) {
        for (uint32_t at : pack.pointerOffsets()) {
            if (size_t(at) + 8 > data.size()) continue;
            const uint32_t target = pack.word(at);
            if (!isClipBlock(data, target)) continue;
            auto it = std::upper_bound(starts.begin(), starts.end(), std::make_pair(at, size_t(-1)));
            if (it == starts.begin()) continue;
            --it;
            set.clips[it->second].blocks.push_back({target, f32at(pack, at + 4)});
        }
    }
    auto clipIndex = [&](uint32_t offset) -> int {
        auto it = std::lower_bound(byOffset.begin(), byOffset.end(), std::make_pair(offset, size_t(0)));
        return it != byOffset.end() && it->first == offset ? int(it->second) : -1;
    };

    // Nodes: follow a node's pointers two levels down (to its lists, then to object table slots).
    for (size_t i = 0; i < objects.size(); i++) {
        const uint32_t o = objects[i];
        const uint32_t nameOid = pack.word(o + 0x0C), cls = pack.word(o);
        if (cls == kAnimClipClass || nameOid >= names.size() || names[nameOid].rfind("anim_", 0) != 0) continue;
        const size_t end = std::min<size_t>(i + 1 < objects.size() ? objects[i + 1] : data.size(), size_t(o) + 0x200);
        AnimNode node;
        node.offset = o;
        node.nameOid = nameOid;
        node.classId = cls;
        auto add = [&](uint32_t target) {
            const int k = clipIndex(target);
            if (k < 0) return false;
            if (std::find(node.clipObjects.begin(), node.clipObjects.end(), uint32_t(k)) == node.clipObjects.end()) node.clipObjects.push_back(uint32_t(k));
            return true;
        };
        for (size_t a = size_t(o) + 0x10; a + 4 <= end; a += 4) {
            if (!pack.isPointer(a)) continue;
            const uint32_t t = pack.word(a);
            if (add(t) || objectSet.count(t) || t == 0 || size_t(t) + 0x40 > data.size()) continue;
            for (size_t b = t; b < size_t(t) + 0x40; b += 4) {   // a list of pointers
                if (!pack.isPointer(b)) continue;
                const uint32_t u = pack.word(b);
                if (add(u) || objectSet.count(u) || u == 0 || size_t(u) + 4 > data.size()) continue;
                if (pack.isPointer(u)) add(pack.word(u));        // an object table slot
            }
        }
        set.nodes.push_back(std::move(node));
    }
    return set;
}

bool clipRootMotion(const Pack& pack, uint32_t block, float out[3]) {
    const auto data = pack.data();
    if (!isClipBlock(data, block)) return false;
    const uint32_t at = pack.word(size_t(block) + 0x18);
    if (at == 0 || size_t(block) + at + 0x30 > data.size()) return false;
    for (int k = 0; k < 3; k++) out[k] = f32at(pack, size_t(block) + at + 0x20 + size_t(k) * 4);
    return true;
}

}  // namespace ina
