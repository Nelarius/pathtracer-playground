struct SkyState {
    params: array<f32, 27>,
    skyRadiances: array<f32, 3>,
    solarRadiances: array<f32, 3>,
    sunDirection: vec3<f32>,
};

struct Uniforms {
    inverseViewReverseZProjectionMat: mat4x4f,
    cameraEye: vec4f,
    framebufferSize: vec2f,
    frameCount: u32,
}

struct Aabb {
    min: vec3f,
    max: vec3f,
}

struct BvhNode {
    aabb: Aabb,
    trianglesOffset: u32,
    secondChildOffset: u32,
    triangleCount: u32,
    splitAxis: u32,
}

struct Positions {
    p0: vec3f,
    p1: vec3f,
    p2: vec3f,
}

struct VertexAttributes {
    n0: vec3f,
    n1: vec3f,
    n2: vec3f,

    uv0: vec2f,
    uv1: vec2f,
    uv2: vec2f,

    textureDescriptorIdx: u32,
}

struct TextureDescriptor {
    width: u32,
    height: u32,
    offset: u32,
}

struct Ray {
    origin: vec3f,
    direction: vec3f
}

struct BlueNoise {
    width: u32,
    height: u32,
    data: array<vec2f>,
}

const CHANNEL_R = 0u;
const CHANNEL_G = 1u;
const CHANNEL_B = 2u;

const EPSILON = 0.00001f;
const PI = 3.1415927f;
const FRAC_1_PI = 0.31830987f;
const FRAC_PI_2 = 1.5707964f;
const DEGREES_TO_RADIANS = PI / 180f;

const TERRESTRIAL_SOLAR_RADIUS = 0.255f * DEGREES_TO_RADIANS;
const SOLAR_COS_THETA_MAX = cos(TERRESTRIAL_SOLAR_RADIUS);
const SOLAR_INV_PDF = 2f * PI * (1f - SOLAR_COS_THETA_MAX);

const T_MIN = 0.001f;
const T_MAX = 10000f;

@group(0) @binding(0) var<storage, read_write> sampleBuffer: array<array<f32, 3>>;

@group(1) @binding(0) var<uniform> uniforms: Uniforms;

@group(2) @binding(0) var gbufferAlbedo: texture_2d<f32>;
@group(2) @binding(1) var gbufferNormal: texture_2d<f32>;
@group(2) @binding(2) var gbufferDepth: texture_depth_2d;

@group(3) @binding(0) var<storage, read> skyState: SkyState;
@group(3) @binding(1) var<storage, read> bvhNodes: array<BvhNode>;
@group(3) @binding(2) var<storage, read> positionAttributes: array<Positions>;
@group(3) @binding(3) var<storage, read> vertexAttributes: array<VertexAttributes>;
@group(3) @binding(4) var<storage, read> textureDescriptors: array<TextureDescriptor>;
@group(3) @binding(5) var<storage, read> textures: array<u32>;
@group(3) @binding(6) var<storage, read> blueNoise: BlueNoise;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) globalInvocationId: vec3<u32>) {
    if globalInvocationId.x >= u32(uniforms.framebufferSize.x) || globalInvocationId.y >= u32(uniforms.framebufferSize.y) {
        return;
    }

    let textureIdx = globalInvocationId.xy;
    let uv = (vec2f(globalInvocationId.xy) + vec2f(0.5)) / uniforms.framebufferSize;

    var color = vec3f(0.0, 0.0, 0.0);
    let depthSample = textureLoad(gbufferDepth, textureIdx, 0);
    if depthSample == 0.0 {
        let world = worldFromUv(uv, depthSample);
        let v = normalize(world - uniforms.cameraEye.xyz);
        let s = skyState.sunDirection;

        let theta = acos(v.y);
        let gamma = acos(clamp(dot(v, s), -1f, 1f));
        color = vec3f(
            skyRadiance(theta, gamma, CHANNEL_R),
            skyRadiance(theta, gamma, CHANNEL_G),
            skyRadiance(theta, gamma, CHANNEL_B)
        );
    } else {
        let coord = vec2u(uv * uniforms.framebufferSize);
        let position = worldFromUv(uv, depthSample);
        let encodedNormal = textureLoad(gbufferNormal, textureIdx, 0).rgb;
        let decodedNormal = 2f * encodedNormal - vec3(1f);
        let albedo = textureLoad(gbufferAlbedo, textureIdx, 0).rgb;
        color = surfaceColor(coord, offsetPosition(position, decodedNormal), decodedNormal, albedo);
    }

    let sampleBufferIdx = textureIdx.y * u32(uniforms.framebufferSize.x) + textureIdx.x;
    sampleBuffer[sampleBufferIdx] = array<f32, 3>(color.r, color.g, color.b);
}

@must_use
fn worldFromUv(uv: vec2f, depthSample: f32) -> vec3f {
    let ndc = vec4(2.0 * vec2(uv.x, 1.0 - uv.y) - vec2(1.0), depthSample, 1.0);
    let worldInvW = uniforms.inverseViewReverseZProjectionMat * ndc;
    let world = worldInvW / worldInvW.w;
    return world.xyz;
}

const NUM_BOUNCES = 2;

@must_use
fn surfaceColor(coord: vec2u, primaryPos: vec3f, primaryNormal: vec3f, primaryAlbedo: vec3f) -> vec3f {
    var position = primaryPos;
    var normal = primaryNormal;
    var albedo = primaryAlbedo;
    var radiance = vec3(0f);
    var throughput = vec3(1f);
    let blueNoise = animatedBlueNoise(coord, uniforms.frameCount, 1 << 20);

    radiance += throughput * lightSample(blueNoise, position, normal, albedo);

    for (var bounce = 1; bounce < NUM_BOUNCES; bounce += 1) {
        let wi = evalImplicitLambertian(blueNoise, normal);
        let ray = Ray(position, wi);
        throughput *= albedo;

        var hit: Intersection;
        if rayIntersectBvh(ray, T_MAX, &hit) {
            position = hit.p;
            normal = hit.n;
            let uv = hit.uv;
            let textureDescriptorIdx = hit.textureDescriptorIdx;
            albedo = evalTexture(textureDescriptorIdx, uv);
        } else {
            let v = ray.direction;
            let s = skyState.sunDirection;

            let theta = acos(v.y);
            let gamma = acos(clamp(dot(v, s), -1f, 1f));

            let skyRadiance = vec3f(
                skyRadiance(theta, gamma, CHANNEL_R),
                skyRadiance(theta, gamma, CHANNEL_G),
                skyRadiance(theta, gamma, CHANNEL_B)
            );

            radiance += throughput * skyRadiance;
            break;
        }

        radiance += throughput * lightSample(blueNoise, position, normal, albedo);
    }

    return radiance;
}

@must_use
fn lightSample(u: vec2f, position: vec3f, normal: vec3f, albedo: vec3f) -> vec3f {
    let lightDirection = sampleSolarDiskDirection(u, SOLAR_COS_THETA_MAX, skyState.sunDirection);
    let lightIntensity = vec3(
        skyState.solarRadiances[CHANNEL_R],
        skyState.solarRadiances[CHANNEL_G],
        skyState.solarRadiances[CHANNEL_B]
    );
    let brdf = albedo * FRAC_1_PI;
    let reflectance = brdf * dot(normal, lightDirection);
    let lightVisibility = shadowRay(Ray(position, lightDirection), T_MAX);
    return lightIntensity * reflectance * lightVisibility * SOLAR_INV_PDF;
}

@must_use
fn skyRadiance(theta: f32, gamma: f32, channel: u32) -> f32 {
    // Sky dome radiance
    let r = skyState.skyRadiances[channel];
    let idx = 9u * channel;
    let p0 = skyState.params[idx + 0u];
    let p1 = skyState.params[idx + 1u];
    let p2 = skyState.params[idx + 2u];
    let p3 = skyState.params[idx + 3u];
    let p4 = skyState.params[idx + 4u];
    let p5 = skyState.params[idx + 5u];
    let p6 = skyState.params[idx + 6u];
    let p7 = skyState.params[idx + 7u];
    let p8 = skyState.params[idx + 8u];

    let cosGamma = cos(gamma);
    let cosGamma2 = cosGamma * cosGamma;
    let cosTheta = abs(cos(theta));

    let expM = exp(p4 * gamma);
    let rayM = cosGamma2;
    let mieMLhs = 1.0 + cosGamma2;
    let mieMRhs = pow(1.0 + p8 * p8 - 2.0 * p8 * cosGamma, 1.5f);
    let mieM = mieMLhs / mieMRhs;
    let zenith = sqrt(cosTheta);
    let radianceLhs = 1.0 + p0 * exp(p1 / (cosTheta + 0.01));
    let radianceRhs = p2 + p3 * expM + p5 * rayM + p6 * mieM + p7 * zenith;
    let radianceDist = radianceLhs * radianceRhs;

    // Solar radiance
    let solarDiskRadius = gamma / TERRESTRIAL_SOLAR_RADIUS;
    let solarRadiance = select(0f, skyState.solarRadiances[channel], solarDiskRadius <= 1f);

    return r * radianceDist + solarRadiance;
}

@must_use
fn sampleSolarDiskDirection(u: vec2f, cosThetaMax: f32, direction: vec3f) -> vec3f {
    let v = directionInCone(u, cosThetaMax);
    let onb = pixarOnb(direction);
    return onb * v;
}

@must_use
fn evalImplicitLambertian(u: vec2f, n: vec3f) -> vec3f {
    let v = directionInCosineWeightedHemisphere(u);
    let onb = pixarOnb(n);
    return onb * v;
}

@must_use
fn evalTexture(textureDescriptorIdx: u32, uv: vec2f) -> vec3f {
    let textureDesc = textureDescriptors[textureDescriptorIdx];
    return textureLookup(textureDesc, uv);
}

@must_use
fn textureLookup(desc: TextureDescriptor, uv: vec2f) -> vec3f {
    let u = fract(uv.x);
    let v = fract(uv.y);

    let j = u32(u * f32(desc.width));
    let i = u32(v * f32(desc.height));
    let idx = i * desc.width + j;

    let bgra = textures[desc.offset + idx];
    let srgb = vec3(f32((bgra >> 16u) & 0xffu), f32((bgra >> 8u) & 0xffu), f32(bgra & 0xffu)) / 255f;
    let linearRgb = pow(srgb, vec3(2.2f));
    return linearRgb;
}

@must_use
fn pixarOnb(n: vec3f) -> mat3x3f {
    // https://www.jcgt.org/published/0006/01/01/paper-lowres.pdf
    let s = select(-1f, 1f, n.z >= 0f);
    let a = -1f / (s + n.z);
    let b = n.x * n.y * a;
    let u = vec3(1f + s * n.x * n.x * a, s * b, -s * n.x);
    let v = vec3(b, s + n.y * n.y * a, -n.y);

    return mat3x3(u, v, n);
}

struct Intersection {
    p: vec3f,
    n: vec3f,
    uv: vec2f,
    textureDescriptorIdx: u32,
}

@must_use
fn rayIntersectBvh(ray: Ray, rayTMax: f32, hit: ptr<function, Intersection>) -> bool {
    let intersector = rayAabbIntersector(ray);
    var toVisitOffset = 0u;
    var currentNodeIdx = 0u;
    var nodesToVisit: array<u32, 32u>;
    var didIntersect: bool = false;
    var tmax = rayTMax;

    loop {
        let node: BvhNode = bvhNodes[currentNodeIdx];

        if rayIntersectAabb(intersector, node.aabb, tmax) {
            if node.triangleCount > 0u {
                for (var idx = 0u; idx < node.triangleCount; idx = idx + 1u) {
                    let triangle: Positions = positionAttributes[node.trianglesOffset + idx];
                    var trihit: TriangleHit;
                    if rayIntersectTriangle(ray, triangle, tmax, &trihit) {
                        tmax = trihit.t;
                        didIntersect = true;

                        let b = trihit.b;
                        let triangleIdx = node.trianglesOffset + idx;
                        let vert = vertexAttributes[triangleIdx];

                        let p = trihit.p;
                        let n = b[0] * vert.n0 + b[1] * vert.n1 + b[2] * vert.n2;
                        let uv = b[0] * vert.uv0 + b[1] * vert.uv1 + b[2] * vert.uv2;
                        let textureDescriptorIdx = vertexAttributes[triangleIdx].textureDescriptorIdx;

                        *hit = Intersection(p, n, uv, textureDescriptorIdx);
                    }
                }
                if toVisitOffset == 0u {
                    break;
                }
                toVisitOffset -= 1u;
                currentNodeIdx = nodesToVisit[toVisitOffset];
            } else {
                // Is intersector.invDir[node.splitAxis] < 0f? If so, visit second child first.
                if intersector.dirNeg[node.splitAxis] == 1u {
                    nodesToVisit[toVisitOffset] = currentNodeIdx + 1u;
                    currentNodeIdx = node.secondChildOffset;
                } else {
                    nodesToVisit[toVisitOffset] = node.secondChildOffset;
                    currentNodeIdx = currentNodeIdx + 1u;
                }
                toVisitOffset += 1u;
            }
        } else {
            if toVisitOffset == 0u {
                break;
            }
            toVisitOffset -= 1u;
            currentNodeIdx = nodesToVisit[toVisitOffset];
        }
    }

    return didIntersect;
}

// Returns 1.0 if no forward intersections, 0.0 otherwise.
@must_use
fn shadowRay(ray: Ray, rayTMax: f32) -> f32 {
    let intersector = rayAabbIntersector(ray);
    var toVisitOffset = 0u;
    var currentNodeIdx = 0u;
    var nodesToVisit: array<u32, 32u>;

    loop {
        let node: BvhNode = bvhNodes[currentNodeIdx];

        if rayIntersectAabb(intersector, node.aabb, rayTMax) {
            if node.triangleCount > 0u {
                for (var idx = 0u; idx < node.triangleCount; idx = idx + 1u) {
                    let triangle: Positions = positionAttributes[node.trianglesOffset + idx];
                    // TODO: trihit not actually used. A different code path could be used?
                    var trihit: TriangleHit;
                    if rayIntersectTriangle(ray, triangle, rayTMax, &trihit) {
                        return 0f;
                    }
                }
                if toVisitOffset == 0u {
                    break;
                }
                toVisitOffset -= 1u;
                currentNodeIdx = nodesToVisit[toVisitOffset];
            } else {
                // Is intersector.invDir[node.splitAxis] < 0f? If so, visit second child first.
                if intersector.dirNeg[node.splitAxis] == 1u {
                    nodesToVisit[toVisitOffset] = currentNodeIdx + 1u;
                    currentNodeIdx = node.secondChildOffset;
                } else {
                    nodesToVisit[toVisitOffset] = node.secondChildOffset;
                    currentNodeIdx = currentNodeIdx + 1u;
                }
                toVisitOffset += 1u;
            }
        } else {
            if toVisitOffset == 0u {
                break;
            }
            toVisitOffset -= 1u;
            currentNodeIdx = nodesToVisit[toVisitOffset];
        }
    }

    return 1f;
}

struct RayAabbIntersector {
    origin: vec3f,
    invDir: vec3f,
    dirNeg: vec3u,
}

@must_use
fn rayAabbIntersector(ray: Ray) -> RayAabbIntersector {
    let invDirection = vec3f(1f / ray.direction.x, 1f / ray.direction.y, 1f / ray.direction.z);
    return RayAabbIntersector(
        ray.origin,
        invDirection,
        vec3u(select(0u, 1u, (invDirection.x < 0f)), select(0u, 1u, (invDirection.y < 0f)), select(0u, 1u, (invDirection.z < 0f)))
    );
}

@must_use
fn rayIntersectAabb(intersector: RayAabbIntersector, aabb: Aabb, rayTMax: f32) -> bool {
    let bounds: array<vec3f, 2> = array(aabb.min, aabb.max);

    var tmin: f32 = (bounds[intersector.dirNeg[0u]].x - intersector.origin.x) * intersector.invDir.x;
    var tmax: f32 = (bounds[1u - intersector.dirNeg[0u]].x - intersector.origin.x) * intersector.invDir.x;

    let tymin: f32 = (bounds[intersector.dirNeg[1u]].y - intersector.origin.y) * intersector.invDir.y;
    let tymax: f32 = (bounds[1 - intersector.dirNeg[1u]].y - intersector.origin.y) * intersector.invDir.y;

    if (tmin > tymax) || (tymin > tmax) {
        return false;
    }

    tmin = max(tymin, tmin);
    tmax = min(tymax, tmax);

    let tzmin: f32 = (bounds[intersector.dirNeg[2u]].z - intersector.origin.z) * intersector.invDir.z;
    let tzmax: f32 = (bounds[1 - intersector.dirNeg[2u]].z - intersector.origin.z) * intersector.invDir.z;

    if (tmin > tzmax) || (tzmin > tmax) {
        return false;
    }

    tmin = max(tzmin, tmin);
    tmax = min(tzmax, tmax);

    return (tmin < rayTMax) && (tmax > 0.0);
}

struct TriangleHit {
    p: vec3f,
    b: vec3f,
    t: f32,
}

@must_use
fn rayIntersectTriangle(ray: Ray, tri: Positions, tmax: f32, hit: ptr<function, TriangleHit>) -> bool {
    // Mäller-Trumbore algorithm
    // https://en.wikipedia.org/wiki/Möller–Trumbore_intersection_algorithm
    let e1 = tri.p1 - tri.p0;
    let e2 = tri.p2 - tri.p0;

    let h = cross(ray.direction, e2);
    let det = dot(e1, h);

    if det > -EPSILON && det < EPSILON {
        return false;
    }

    let invDet = 1.0f / det;
    let s = ray.origin - tri.p0;
    let u = invDet * dot(s, h);

    if u < 0.0f || u > 1.0f {
        return false;
    }

    let q = cross(s, e1);
    let v = invDet * dot(ray.direction, q);

    if v < 0.0f || u + v > 1.0f {
        return false;
    }

    let t = invDet * dot(e2, q);

    if t > EPSILON && t < tmax {
        // https://www.scratchapixel.com/lessons/3d-basic-rendering/ray-tracing-rendering-a-triangle/moller-trumbore-ray-triangle-intersection.html
        // e1 = v1 - v0
        // e2 = v2 - v0
        // -> p = v0 + u * e1 + v * e2
        let p = tri.p0 + u * e1 + v * e2;
        let n = normalize(cross(e1, e2));
        let b = vec3f(1f - u - v, u, v);
        *hit = TriangleHit(offsetPosition(p, n), b, t);
        return true;
    } else {
        return false;
    }
}

const ORIGIN = 1f / 32f;
const FLOAT_SCALE = 1f / 16384f;
const INT_SCALE = 1024;

@must_use
fn offsetPosition(p: vec3f, n: vec3f) -> vec3f {
    // Source: A Fast and Robust Method for Avoiding Self-Intersection, Ray Tracing Gems
    let offset = vec3i(i32(INT_SCALE * n.x), i32(INT_SCALE * n.y), i32(INT_SCALE * n.z));
    // Offset added straight into the mantissa bits to ensure the offset is scale-invariant,
    // except for when close to the origin, where we use FLOAT_SCALE as a small epsilon.
    let po = vec3f(
        bitcast<f32>(bitcast<i32>(p.x) + select(offset.x, -offset.x, (p.x < 0))),
        bitcast<f32>(bitcast<i32>(p.y) + select(offset.y, -offset.y, (p.y < 0))),
        bitcast<f32>(bitcast<i32>(p.z) + select(offset.z, -offset.z, (p.z < 0)))
    );

    return vec3f(
        select(po.x, p.x + FLOAT_SCALE * n.x, (abs(p.x) < ORIGIN)),
        select(po.y, p.y + FLOAT_SCALE * n.y, (abs(p.y) < ORIGIN)),
        select(po.z, p.z + FLOAT_SCALE * n.z, (abs(p.z) < ORIGIN))
    );
}

// `u` is a random number in [0, 1].
@must_use
fn directionInCone(u: vec2f, cosThetaMax: f32) -> vec3f {
    let cosTheta = 1f - u.x * (1f - cosThetaMax);
    let sinTheta = sqrt(1f - cosTheta * cosTheta);
    let phi = 2f * PI * u.y;

    let x = cos(phi) * sinTheta;
    let y = sin(phi) * sinTheta;
    let z = cosTheta;

    return vec3(x, y, z);
}

// `u` is a random number in [0, 1].
@must_use
fn directionInCosineWeightedHemisphere(u: vec2f) -> vec3f {
    let phi = 2f * PI * u.y;
    let sinTheta = sqrt(1f - u.x);

    let x = cos(phi) * sinTheta;
    let y = sin(phi) * sinTheta;
    let z = sqrt(u.x);

    return vec3(x, y, z);
}

@must_use
fn animatedBlueNoise(coord: vec2u, frameCount: u32, frameCountCycle: u32) -> vec2f {
    // Spatial component
    let idx = (coord.y % blueNoise.height) * blueNoise.width + (coord.x % blueNoise.width);
    let blueNoise = blueNoise.data[idx];

    // Temporal component
    // 2-dimensional golden ratio additive recurrence sequence
    // https://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
    let n = frameCount % frameCountCycle;
    let a1 = 0.7548776662466927f;
    let a2 = 0.5698402909980532f;
    let r2Seq = fract(vec2(
        a1 * f32(n),
        a2 * f32(n)
    ));

    return fract(blueNoise + r2Seq);
}
