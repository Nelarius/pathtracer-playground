#pragma once

#include "geometry.hpp"
#include "units/angle.hpp"

#include <glm/glm.hpp>

namespace pt
{
struct Camera
{
    glm::vec3 origin;
    glm::vec3 lowerLeftCorner;
    glm::vec3 horizontal;
    glm::vec3 vertical;
    float     lensRadius;
};

Camera createCamera(
    glm::vec3 origin,
    glm::vec3 lookAt,
    float     aperture,
    float     focusDistance,
    Angle     vfov,
    int       viewportWidth,
    int       viewportHeight);

// (u, v) are in [0, 1] range, where (0, 0) is the lower left corner and (1, 1) is the upper right
// corner.
Ray generateCameraRay(const Camera& camera, float u, float v);
} // namespace pt