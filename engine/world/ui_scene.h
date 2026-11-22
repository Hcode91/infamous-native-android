// The game's interface screens ("Uno" scenes, class 0x18B): flat textured quads hung on the joints of an
// animated rig, plus text boxes (class 0x1D4). Units are pixels of a 1280x720 screen with the origin in the
// middle and y up. See docs/formats/ui.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "anim/clip.h"
#include "assets/pack.h"

namespace ina {

struct UiElement {
    std::string name;
    int joint = 0;                  // the rig joint it hangs on
    uint32_t textureOid = 0;        // colour texture (0: a flat colour, effect ui_color)
    float min[2] = {0, 0}, max[2] = {0, 0};   // the quad, around `offset`
    float offset[2] = {0, 0};
};

struct UiTextBox {
    std::string name;
    int joint = 0;
    float offset[2] = {0, 0};
    float size[2] = {0, 0};         // box width and height
    float fontSize = 0;
};

struct UiAnim {
    std::string name;
    std::vector<std::pair<std::string, AnimClip>> clips;   // in file order
};

struct UiScene {
    std::string name;
    std::vector<UiElement> elements;
    std::vector<UiTextBox> texts;
    std::vector<UiAnim> anims;
    int joints = 0;
};

// The Uno scene object named `name` (e.g. "ui_press_start"), with the elements, text boxes and animations of
// the proxy file it sits in. Empty (no elements) when not found.
UiScene readUiScene(const Pack& pack, const std::vector<std::string>& names, const std::string& name);

// The joints' transforms at a time of a clip: per joint scale x, y, rotation about z (radians), x, y, z.
// Joints the clip does not drive keep identity.
std::vector<float> uiPose(const UiScene& scene, const std::string& anim, const std::string& clip, float t, float* duration = nullptr);

// A bitmap font (class 0x15C, e.g. "font_main_latin" in lang_english.xpp): +0x14 texture, +0x1C line height
// (high half), +0x20 glyph table, +0x24 count. Glyphs are 20 bytes: code, x, y, width, height, x offset,
// y offset (from the line's top), advance (16-bit each; offsets signed), then two words not used here.
struct UiGlyph { uint32_t code = 0; int x = 0, y = 0, w = 0, h = 0, xoff = 0, yoff = 0, advance = 0; };
struct UiFont {
    uint32_t textureOid = 0;
    int lineHeight = 0;
    std::vector<UiGlyph> glyphs;
};
UiFont readUiFont(const Pack& pack, const std::vector<std::string>& names, const std::string& name);

}  // namespace ina
