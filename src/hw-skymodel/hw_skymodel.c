#include "hw_skymodel.h"

#include "params_r.h"
#include "params_g.h"
#include "params_b.h"
#include "radiances_r.h"
#include "radiances_g.h"
#include "radiances_b.h"

#include <assert.h>
#include <stddef.h>
#include <math.h>

static const float PI = (float)M_PI;

static float quintic_9(const float* const data, const float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float t4 = t2 * t2;
    const float t5 = t4 * t;

    const float inv_t = 1.0 - t;
    const float inv_t2 = inv_t * inv_t;
    const float inv_t3 = inv_t2 * inv_t;
    const float inv_t4 = inv_t2 * inv_t2;
    const float inv_t5 = inv_t4 * inv_t;

    const float m0 = data[0] * inv_t5;
    const float m1 = data[9] * 5.0 * inv_t4 * t;
    const float m2 = data[2 * 9] * 10.0 * inv_t3 * t2;
    const float m3 = data[3 * 9] * 10.0 * inv_t2 * t3;
    const float m4 = data[4 * 9] * 5.0 * inv_t * t4;
    const float m5 = data[5 * 9] * t5;

    return m0 + m1 + m2 + m3 + m4 + m5;
}

static float quintic_1(const float* const data, const float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float t4 = t2 * t2;
    const float t5 = t4 * t;

    const float inv_t = 1.0 - t;
    const float inv_t2 = inv_t * inv_t;
    const float inv_t3 = inv_t2 * inv_t;
    const float inv_t4 = inv_t2 * inv_t2;
    const float inv_t5 = inv_t4 * inv_t;

    const float m0 = data[0] * inv_t5;
    const float m1 = data[1] * 5.0 * inv_t4 * t;
    const float m2 = data[2 * 1] * 10.0 * inv_t3 * t2;
    const float m3 = data[3 * 1] * 10.0 * inv_t2 * t3;
    const float m4 = data[4 * 1] * 5.0 * inv_t * t4;
    const float m5 = data[5 * 1] * t5;

    return m0 + m1 + m2 + m3 + m4 + m5;
}

static void initParams(
    float* const       outParams,
    const float* const data,
    const float        turbidity,
    const float        albedo,
    const float        t)
{
    const size_t turbidityInt = (size_t)turbidity;
    const float  turbidityRem = fmodf(turbidity, 1.0f);
    assert(turbidityInt > 0);
    const size_t turbidityMin = turbidityInt - 1;
    const size_t turbidityMax = turbidityInt < 9 ? turbidityInt : 9;

    const float* const p0 = data + (9 * 6 * turbidityMin);
    const float* const p1 = data + (9 * 6 * turbidityMax);
    const float* const p2 = data + (9 * 6 * 10 + 9 * 6 * turbidityMin);
    const float* const p3 = data + (9 * 6 * 10 + 9 * 6 * turbidityMax);

    const float s0 = (1.0 - albedo) * (1.0 - turbidityRem);
    const float s1 = (1.0 - albedo) * turbidityRem;
    const float s2 = albedo * (1.0 - turbidityRem);
    const float s3 = albedo * turbidityRem;

    for (size_t i = 0; i < 9; ++i)
    {
        outParams[i] = 0.0f;
        outParams[i] += s0 * quintic_9(p0 + i, t);
        outParams[i] += s1 * quintic_9(p1 + i, t);
        outParams[i] += s2 * quintic_9(p2 + i, t);
        outParams[i] += s3 * quintic_9(p3 + i, t);
    }
}

static void initRadiances(
    float* const       outRadiance,
    const float* const data,
    const float        turbidity,
    const float        albedo,
    const float        t)
{
    const size_t turbidity_int = (size_t)turbidity;
    const float  turbidity_rem = fmodf(turbidity, 1.0f);
    assert(turbidity_int > 0);
    const size_t turbidity_min = turbidity_int - 1;
    const size_t turbidity_max = turbidity_int < 9 ? turbidity_int : 9;

    const float* const p0 = data + (6 * turbidity_min);
    const float* const p1 = data + (6 * turbidity_max);
    const float* const p2 = data + (6 * 10 + 6 * turbidity_min);
    const float* const p3 = data + (6 * 10 + 6 * turbidity_max);

    const float s0 = (1.0 - albedo) * (1.0 - turbidity_rem);
    const float s1 = (1.0 - albedo) * turbidity_rem;
    const float s2 = albedo * (1.0 - turbidity_rem);
    const float s3 = albedo * turbidity_rem;

    *outRadiance = 0.0f;
    *outRadiance += s0 * quintic_1(p0, t);
    *outRadiance += s1 * quintic_1(p1, t);
    *outRadiance += s2 * quintic_1(p2, t);
    *outRadiance += s3 * quintic_1(p3, t);
}

SkyStateResult skyStateNew(const SkyParams* const skyParams, SkyState* const skyState)
{
    const float elevation = skyParams->elevation;
    const float turbidity = skyParams->turbidity;
    const float albedo[3] = {skyParams->albedo[0], skyParams->albedo[1], skyParams->albedo[2]};

    // Validate

    if (elevation < 0.0f || elevation > PI)
    {
        return SkyStateResult_ElevationOutOfRange;
    }

    if (turbidity < 1.0f || turbidity > 10.0f)
    {
        return SkyStateResult_TurbidityOutOfRange;
    }

    if ((albedo[0] < 0.0f || albedo[0] > 1.0f) || (albedo[1] < 0.0f || albedo[1] > 1.0f) ||
        (albedo[2] < 0.0f || albedo[2] > 1.0f))
    {
        return SkyStateResult_AlbedoOutOfRange;
    }

    // Init state.

    const float t = powf((elevation / (0.5 * PI)), (1.0f / 3.0f));

    initParams(skyState->params + 0, paramsR, turbidity, albedo[0], t);
    initParams(skyState->params + 9, paramsG, turbidity, albedo[1], t);
    initParams(skyState->params + (9 * 2), paramsB, turbidity, albedo[2], t);
    initRadiances(skyState->radiances + 0, radiancesR, turbidity, albedo[0], t);
    initRadiances(skyState->radiances + 1, radiancesG, turbidity, albedo[1], t);
    initRadiances(skyState->radiances + 2, radiancesB, turbidity, albedo[2], t);

    return SkyStateResult_Success;
}

float skyStateRadiance(
    const SkyState* const skyState,
    const float           theta,
    const float           gamma,
    const Channel         channel)
{
    const size_t channelIdx = (size_t)channel;
    const float  r = skyState->radiances[channelIdx];
    const float* p = skyState->params + (9 * channelIdx);
    const float  p0 = p[0];
    const float  p1 = p[1];
    const float  p2 = p[2];
    const float  p3 = p[3];
    const float  p4 = p[4];
    const float  p5 = p[5];
    const float  p6 = p[6];
    const float  p7 = p[7];
    const float  p8 = p[8];

    const float cosGamma = cosf(gamma);
    const float cosGamma2 = cosGamma * cosGamma;
    const float cos_theta = fabsf(cosf(theta));

    const float expM = expf(p4 * gamma);
    const float rayM = cosGamma2;
    const float mieMLhs = 1.0 + cosGamma2;
    const float mieMRhs = powf(1.0 + p8 * p8 - 2.0 * p8 * cosGamma, 1.5f);
    const float mieM = mieMLhs / mieMRhs;
    const float zenith = sqrtf(cos_theta);
    const float radiance_lhs = 1.0 + p0 * expf(p1 / (cos_theta + 0.01));
    const float radiance_rhs = p2 + p3 * expM + p5 * rayM + p6 * mieM + p7 * zenith;
    const float radiance_dist = radiance_lhs * radiance_rhs;

    return r * radiance_dist;
}