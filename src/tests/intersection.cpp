#include <common/bvh.hpp>
#include <common/geometry.hpp>
#include <common/ray_intersection.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

TEST_CASE("Ray intersects triangle", "[intersection]")
{
    const pt::Ray ray{
        .origin = glm::vec3{0.0f, 0.0f, 0.0f},
        .direction = glm::vec3{0.0f, 0.0f, 1.0f},
    };
    const pt::Triangle triangle{
        .v0 = glm::vec3{0.0f, 0.0f, 1.0f},
        .v1 = glm::vec3{1.0f, 0.0f, 1.0f},
        .v2 = glm::vec3{0.0f, 1.0f, 1.0f},
    };

    pt::Intersection isect;
    const bool intersects = rayIntersectTriangle(ray, pt::Triangle48(triangle), 1000.0f, isect);

    REQUIRE(intersects);
    REQUIRE(isect.p.x == Catch::Approx(0.0f));
    REQUIRE(isect.p.y == Catch::Approx(0.0f));
    REQUIRE(isect.p.z == Catch::Approx(1.0f));
}