#include "world/ui_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "anim/anim_set.h"
#include "anim/character_loader.h"
#include "assets/character_mesh.h"
#include "assets/world_mesh.h"

namespace ina {

namespace {

float wf(const Pack& p, size_t at) {
    const uint32_t w = p.word(at);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

uint16_t half(const Pack& p, size_t at) {
    const uint32_t w = p.word(at & ~size_t(3));
    return (at & 2) ? uint16_t(w & 0xFFFF) : uint16_t(w >> 16);
}

constexpr uint32_t kUnoClass = 0x18B, kTextBoxClass = 0x1D4, kFileClass = 0x228;

}  // namespace

UiScene readUiScene(const Pack& pack, const std::vector<std::string>& names, const std::string& name) {
    UiScene s;
    s.name = name;
    const auto& d = pack.data();
    uint32_t uno = 0, file = 0;
    for (uint32_t o : pack.objects()) {
        if (size_t(o) + 0xB0 > d.size()) continue;
        const uint32_t oid = pack.word(size_t(o) + 0x0C);
        if (pack.word(o) == kUnoClass && oid < names.size() && names[oid] == name) { uno = o; break; }
    }
    if (!uno) return s;
    for (uint32_t o : pack.objects())   // the file the scene sits in: the nearest file start before it
        if (o <= uno && o > file && pack.word(o) == kFileClass) file = o;
    const auto range = file ? subFileRange(pack, file, kFileClass) : std::pair<uint32_t, uint32_t>{uno, uno + 0x1000};

    // elements: a list of 0x60-byte records (+0x00 name, +0x08 material, +0x10 box min x y z, +0x1C box max,
    // +0x28 offset x y); which joint each hangs on comes from a table of 10-byte rows (joint + 1, 0,
    // element index, 0, ...)
    const uint32_t list = pack.word(size_t(uno) + 0xA8), count = pack.word(size_t(uno) + 0xAC);
    for (uint32_t i = 0; i < count && i < 64 && size_t(list) + 0x60 * (i + 1) <= d.size(); i++) {
        const size_t e = size_t(list) + 0x60 * i;
        UiElement el;
        const uint32_t oid = pack.word(e);
        el.name = oid < names.size() ? names[oid] : "";
        const uint32_t mat = pack.word(e + 8);
        for (const auto& [slot, tex] : readMaterialTextures(pack, mat))
            if (slot <= 1 && !el.textureOid) el.textureOid = tex;
        el.min[0] = wf(pack, e + 0x10);
        el.min[1] = wf(pack, e + 0x14);
        el.max[0] = wf(pack, e + 0x1C);
        el.max[1] = wf(pack, e + 0x20);
        el.offset[0] = wf(pack, e + 0x28);
        el.offset[1] = wf(pack, e + 0x2C);
        s.elements.push_back(el);
    }
    const uint32_t table = pack.word(size_t(uno) + 0x90), rows = pack.word(size_t(uno) + 0x94);
    for (uint32_t r = 0; r < rows && r < 64; r++) {
        const size_t at = size_t(table) + 10 * size_t(r);
        if (at + 10 > d.size()) break;
        const int joint = int(half(pack, at)) - 1, element = half(pack, at + 4);
        if (element < int(s.elements.size()) && joint >= 0) s.elements[size_t(element)].joint = joint;
    }

    // text boxes of the same file: +0x30 joint, +0x60 offset x y, +0x90 width, height, font size
    for (uint32_t o : pack.objects()) {
        if (o < range.first || o >= range.second || pack.word(o) != kTextBoxClass || size_t(o) + 0xA0 > d.size()) continue;
        UiTextBox t;
        const uint32_t oid = pack.word(size_t(o) + 0x0C);
        t.name = oid < names.size() ? names[oid] : "";
        t.joint = int(pack.word(size_t(o) + 0x30));
        t.offset[0] = wf(pack, size_t(o) + 0x60);
        t.offset[1] = wf(pack, size_t(o) + 0x64);
        t.size[0] = wf(pack, size_t(o) + 0x90);
        t.size[1] = wf(pack, size_t(o) + 0x94);
        t.fontSize = wf(pack, size_t(o) + 0x98);
        s.texts.push_back(t);
    }

    // the rig's animations: the file's animation nodes and their clips
    const AnimSet set = readAnimSet(pack, names);
    for (const AnimNode& node : set.nodes) {
        if (node.offset < range.first || node.offset >= range.second) continue;
        UiAnim a;
        a.name = node.nameOid < names.size() ? names[node.nameOid] : "";
        for (uint32_t k : node.clipObjects) {
            const ClipObject& c = set.clips[k];
            if (c.blocks.empty()) continue;
            try {
                AnimClip clip = readClipAt(pack, c.blocks[0].block);
                s.joints = std::max<int>(s.joints, clip.joints);
                a.clips.emplace_back(c.nameOid < names.size() ? names[c.nameOid] : "", std::move(clip));
            } catch (const std::exception&) {
            }
        }
        s.anims.push_back(std::move(a));
    }
    for (const UiElement& e : s.elements) s.joints = std::max(s.joints, e.joint + 1);
    return s;
}

UiFont readUiFont(const Pack& pack, const std::vector<std::string>& names, const std::string& name) {
    UiFont f;
    const auto& d = pack.data();
    for (uint32_t o : pack.objects()) {
        if (size_t(o) + 0x28 > d.size() || pack.word(o) != 0x15C) continue;
        const uint32_t oid = pack.word(size_t(o) + 0x0C);
        if (oid >= names.size() || names[oid] != name) continue;
        f.textureOid = pack.word(size_t(o) + 0x14);
        f.lineHeight = int(pack.word(size_t(o) + 0x1C) >> 16);
        const uint32_t table = pack.word(size_t(o) + 0x20), n = pack.word(size_t(o) + 0x24);
        for (uint32_t i = 0; i < n && i < 4096 && size_t(table) + 20 * (i + 1) <= d.size(); i++) {
            const size_t g = size_t(table) + 20 * i;
            UiGlyph gl;
            gl.code = half(pack, g);
            gl.x = half(pack, g + 2);
            gl.y = half(pack, g + 4);
            gl.w = half(pack, g + 6);
            gl.h = half(pack, g + 8);
            gl.xoff = int16_t(half(pack, g + 10));
            gl.yoff = int16_t(half(pack, g + 12));
            gl.advance = half(pack, g + 14);
            f.glyphs.push_back(gl);
        }
        break;
    }
    return f;
}

std::vector<float> uiPose(const UiScene& scene, const std::string& anim, const std::string& clip, float t, float* duration) {
    std::vector<float> pose(size_t(scene.joints) * 12, 0.0f);
    for (int j = 0; j < scene.joints; j++) {
        float* p = &pose[size_t(j) * 12];
        p[0] = p[1] = p[2] = 1.0f;
        p[7] = 1.0f;
    }
    if (duration) *duration = 0;
    for (const UiAnim& a : scene.anims) {
        if (!anim.empty() && a.name != anim) continue;
        for (const auto& [n, c] : a.clips) {
            if (n != clip) continue;
            if (c.joints > scene.joints) pose.resize(size_t(c.joints) * 12, 0.0f);
            const float frame = std::clamp(t * c.fps, 0.0f, float(std::max<int>(c.frames, 1) - 1));
            c.evaluate(frame, pose);
            if (duration) *duration = c.duration();
            break;
        }
        break;
    }
    // per joint: scale x, y, rotation about z, x, y, z
    std::vector<float> out;
    for (size_t j = 0; j * 12 + 11 < pose.size(); j++) {
        const float* p = &pose[j * 12];
        const float angle = 2.0f * std::atan2(p[6], p[7]);
        out.insert(out.end(), {p[0], p[1], angle, p[8], p[9], p[10]});
    }
    return out;
}

}  // namespace ina
