// A sector's placed entities and the lamps among them. A sector pack lists every entity it places (lamps,
// hydrants, benches, railings, plants) with a rotation, a position and the name of its prototype, a file
// merged into the same pack; a prototype's light objects become lights in the world. See docs/formats/lights.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "assets/pack.h"
#include "script/script_vm.h"
#include "world/scene.h"

namespace ina {

struct SectorInstance {
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // rows of the matrix from the prototype's space to the sector's
    float position[3] = {0, 0, 0};                     // in the sector's coordinates
    uint32_t nameOid = 0, proxyOid = 0;
};
// Found by the records' shape (an orthonormal matrix, a position, the word 0xFFF, a number and two names);
// the table that owns them is not located yet.
std::vector<SectorInstance> readSectorInstances(const Pack& pack, size_t nameCount);

struct WorldLight {
    float position[3] = {0, 0, 0};
    float inner = 0, outer = 0;        // full strength inside `inner`, none beyond `outer`
    float color[3] = {1, 1, 1};
};
// Every light of every placed entity, moved by `offset` (the sector's place in the city). Spot lights are
// returned as point lights; capsule lights are left out.
std::vector<WorldLight> readSectorLights(const Pack& pack, const ScriptImage& image, const std::vector<std::string>& names,
                                         const float offset[3]);

// The models of the placed entities that have one (bicycles, bins, benches, lamps), as one scene in city
// coordinates. A prototype is looked for in the sector's pack, then in `core`. `placed` counts the entities drawn.
Scene buildSectorProps(const Pack& sector, const Pack* core, const std::vector<std::string>& names, const float offset[3],
                       size_t* placed = nullptr);

// Adds the lights to a scene's vertices (the ninth float of each).
void lightScene(Scene& scene, const std::vector<WorldLight>& lights);

}  // namespace ina
