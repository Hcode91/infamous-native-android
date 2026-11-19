// Putting a character together from a pack: the meshes one variant shows, the joint hierarchy its clips
// drive, and the clip blocks of its named animations.
#pragma once
#include "anim/anim_set.h"
#include "anim/character_model.h"
#include "anim/clip.h"
#include "anim/leg_ik.h"

#include <string>
#include <vector>

namespace ina {

// The variant with this name, or nullptr.
const CharacterVariant* findVariant(const std::vector<CharacterVariant>& variants, const std::vector<std::string>& names, const std::string& name);

// The meshes `variant` shows. Variants and meshes can sit in different files of a merged pack: the meshes
// are taken from the file holding a mesh the variant names, or, when it names none, the file holding
// `skeletonOffset`. Of those, the ones that are not optional or that the variant names.
std::vector<PackedMesh> variantMeshes(const Pack& pack, std::vector<PackedMesh> all, const CharacterVariant& variant, uint32_t skeletonOffset);

// The joint hierarchy whose animated joint count is `joints`; with several, the one nearest before `near`.
Skeleton skeletonForClips(const Pack& pack, uint32_t joints, uint32_t near);

// Parses the clip block at a data-area offset.
AnimClip readClipAt(const Pack& pack, uint32_t block);

}  // namespace ina
