// Vertex layouts of static meshes. The mesh record does not describe its vertices; the layout follows from
// the material's effect (the vertex program it is drawn with). This table was measured over every sector
// of the EU disc release: for each effect, the size of each stream (from the space between pointers) and
// which columns hold texture coordinates. See docs/formats/world-geometry.md.
#pragma once
#include <cstdint>
#include <string_view>

namespace ina {

enum class DrawMode : uint8_t {
    Opaque,      // ordinary surface
    Cutout,      // alpha-tested (fences, foliage cards)
    Decal,       // drawn over other surfaces, blended, no depth writes
    Invisible,   // light volumes and ambient-occlusion shapes: not drawn as surfaces
};

struct VertexLayout {
    uint8_t strideA = 12, strideB = 0;   // bytes per vertex in the first and second stream (0: no stream)
    bool halfPosition = false;           // position is three half-floats instead of three floats
    uint8_t uvStream = 0;                // 0 or 1; 0xFF: no texture coordinates known
    uint8_t uvOffset = 0;                // byte offset of the coordinates inside a vertex of that stream
    bool uvFloat = false;                // two floats instead of two half-floats
    DrawMode mode = DrawMode::Opaque;
};

// The layout for an effect, by name; null when the effect is not in the table.
const VertexLayout* layoutForEffect(std::string_view effect);

// Layouts to try, in order, when the effect name is not known: one per distinct pair of stream sizes.
const VertexLayout* fallbackLayouts(size_t* count);

// How an effect is drawn, by name (also for effects not in the layout table).
DrawMode drawModeForEffect(std::string_view effect);

}  // namespace ina
