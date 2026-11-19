// Leg placement from foot targets. Clips move a foot target joint per leg and a helper joint under it; the
// hip, knee and ankle rotations stored in clips do not follow the targets (they do not even loop in looping
// clips), so the legs are solved here. How the game itself solves them is not known: this reconstruction
// carries the ankle and the knee direction rigidly with the target joints, relative to the rest pose.
#pragma once
#include "assets/character_mesh.h"

#include <string>
#include <vector>

namespace ina {

struct LegChain {
    int target = -1;   // foot target joint
    int pole = -1;     // its helper joint, which carries the knee direction
    int hip = -1, knee = -1, ankle = -1;
};

// Finds the two leg chains by joint name; empty when the skeleton does not have them.
std::vector<LegChain> findLegChains(const Skeleton& skeleton, const std::vector<std::string>& names);

// A joint by name, or -1.
int findJoint(const Skeleton& skeleton, const std::vector<std::string>& names, const char* name);

// World matrices (12 floats per joint) of `localPose`, with the legs solved towards their targets.
// `restWorld` is poseToWorld of the rest pose. With no chains this is poseToWorld.
std::vector<float> poseToWorldWithLegs(const Skeleton& skeleton, const std::vector<float>& localPose,
                                       const std::vector<float>& restWorld, const std::vector<LegChain>& legs);

}  // namespace ina
