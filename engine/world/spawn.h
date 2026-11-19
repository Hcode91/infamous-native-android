// Models that scripts spawn: a "proxy" is a file merged into a pack (a car, a mailbox, a person), and a
// spawn site names the file to instantiate and where. See docs/formats/xpp-pack.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "assets/pack.h"
#include "assets/world_mesh.h"
#include "world/scene.h"

namespace ina {

inline constexpr uint32_t kProxyFileClass = 0x228;   // ProxyFileData: start of a merged file
inline constexpr uint32_t kProxySiteClass = 0x1CC;   // ProxySiteData: a place where a proxy can be spawned
inline constexpr uint32_t kBreakableClass = 0x17D;   // BreakableData: a prop that can be knocked about, laid out like a vehicle body
inline constexpr uint32_t kVehicleClass = 0x17F;     // VehicleData: a vehicle's body, with pieces like a static object

// Offset of the merged file with this name, 0 when the pack has none.
uint32_t findProxyFile(const Pack& pack, const std::vector<std::string>& names, const std::string& name);

// The model of one merged file at its nearest level of detail, in the file's own coordinates: the pieces of
// its static objects and vehicle body. `meshes` is readStaticMeshes(pack), or just the file's range of it.
// A vehicle body's pieces each carry a mask of damage states they show in; `damageState` picks one
// (0 = intact; on the sedan examined 8 has broken windows and 4 and 21 are wrecks).
Scene buildFileScene(const Pack& pack, const std::vector<StaticMesh>& meshes, uint32_t fileOffset, unsigned damageState = 0,
                     size_t* pieces = nullptr);

// A spawn site: the proxy it instantiates (name id) and its place.
struct ProxySite {
    uint32_t offset = 0, nameOid = 0, proxyOid = 0;
    float position[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 1};   // quaternion x, y, z, w
};
std::vector<ProxySite> readProxySites(const Pack& pack);

}  // namespace ina
