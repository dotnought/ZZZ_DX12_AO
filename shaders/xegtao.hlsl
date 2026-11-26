// Intel XeGTAO integration. See third_party/xegtao/LICENSE for the upstream license.
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS
#define XE_GTAO_USE_DEFAULT_CONSTANTS 0
#define VA_SATURATE(x) saturate(x)
float AdjustVisibility(int2 pixel, float visibility);
#define XE_GTAO_ADJUST_VISIBILITY(pixel, visibility) AdjustVisibility(pixel, visibility)
#include "../third_party/xegtao/XeGTAO.hlsli"

cbuffer Config : register(b0)
{
    GTAOConstants gtao;
    uint2 activeSize;
    uint2 depthOrigin;
    float intensity;
    float fadeStart;
    float fadeEnd;
    float backgroundDepth;
};

SamplerState pointClamp : register(s0);
Texture2D<float> depth : register(t0);
Texture2D<float4> viewNormals : register(t1);
Texture2D<uint> sourceAO : register(t0);
Texture2D<float> sourceEdges : register(t1);
Texture2D<float> rawDepth : register(t2);

RWTexture2D<float> mip0 : register(u0);
RWTexture2D<float> mip1 : register(u1);
RWTexture2D<float> mip2 : register(u2);
RWTexture2D<float> mip3 : register(u3);
RWTexture2D<float> mip4 : register(u4);
RWTexture2D<uint> targetAO : register(u0);
RWTexture2D<unorm float> targetEdges : register(u1);

groupshared float depthTile[8][8];

float ReadLinearDepth(uint2 pixel)
{
    float deviceZ = depth.Load(int3(min(pixel, activeSize - 1u) + depthOrigin, 0));
    float linearZ = XeGTAO_ScreenSpaceToViewSpaceDepth(deviceZ, gtao);
    return isfinite(linearZ) && linearZ > 0.0 ? min(linearZ, 100000.0) : 100000.0;
}

// Explicit active-rectangle clamping handles odd sizes, subrects and aligned mip storage.
[numthreads(8, 8, 1)]
void Prefilter(uint2 thread : SV_DispatchThreadID, uint2 group : SV_GroupThreadID)
{
    uint2 pixel = thread * 2;
    float a = ReadLinearDepth(pixel);
    float b = ReadLinearDepth(pixel + uint2(1, 0));
    float c = ReadLinearDepth(pixel + uint2(0, 1));
    float d = ReadLinearDepth(pixel + uint2(1, 1));
    mip0[pixel] = a;
    mip0[pixel + uint2(1, 0)] = b;
    mip0[pixel + uint2(0, 1)] = c;
    mip0[pixel + 1] = d;
    float filtered = XeGTAO_DepthMIPFilter(a, b, c, d, gtao);
    mip1[thread] = filtered;
    depthTile[group.y][group.x] = filtered;
    GroupMemoryBarrierWithGroupSync();

    // Each level reads four entries that are exclusive to the reducing thread.
    [unroll]
    for (uint step = 1; step <= 4; step *= 2)
    {
        if (all((group & (step * 2 - 1)) == 0))
        {
            float next = XeGTAO_DepthMIPFilter(
                depthTile[group.y][group.x], depthTile[group.y][group.x + step],
                depthTile[group.y + step][group.x], depthTile[group.y + step][group.x + step], gtao);
            depthTile[group.y][group.x] = next;
            if (step == 1)
                mip2[thread / 2] = next;
            else if (step == 2)
                mip3[thread / 4] = next;
            else
                mip4[thread / 8] = next;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

float2 Noise(uint2 pixel)
{
    uint index = HilbertIndex(pixel.x, pixel.y) + 288u * uint(gtao.NoiseIndex);
    return frac(0.5 + index * float2(0.7548776662, 0.5698402910));
}

void Horizon(uint2 pixel, float slices, float steps)
{
    // Padded work pixels use the nearest active pixel for the input normal.
    uint2 activePixel = min(pixel, activeSize - 1u);
    float3 normal = 0.0;
    normal = viewNormals.Load(int3(activePixel + depthOrigin, 0)).xyz;
    float lengthSquared = dot(normal, normal);
    normal = lengthSquared > 1e-6 ? normal * rsqrt(lengthSquared) : 0.0;
    if (dot(normal, normal) < 0.5)
    {
        const uint2 size = activeSize - 1u;
        const int2 p = int2(activePixel);
        float dl = depth.Load(int3(max(p - int2(1, 0), 0), 0));
        float dr = depth.Load(int3(min(activePixel + uint2(1, 0), size), 0));
        float dt = depth.Load(int3(max(p - int2(0, 1), 0), 0));
        float db = depth.Load(int3(min(activePixel + uint2(0, 1), size), 0));
        float dc = depth.Load(int3(activePixel, 0));
        normal = XeGTAO_CalculateNormal(XeGTAO_CalculateEdges(dc, dl, dr, dt, db),
            XeGTAO_ComputeViewspacePosition((float2(p) + 0.5) * gtao.ViewportPixelSize, dc, gtao),
            XeGTAO_ComputeViewspacePosition((float2(p) + float2(-0.5, 0.5)) * gtao.ViewportPixelSize, dl, gtao),
            XeGTAO_ComputeViewspacePosition((float2(p) + float2(1.5, 0.5)) * gtao.ViewportPixelSize, dr, gtao),
            XeGTAO_ComputeViewspacePosition((float2(p) + float2(0.5, -0.5)) * gtao.ViewportPixelSize, dt, gtao),
            XeGTAO_ComputeViewspacePosition((float2(p) + float2(0.5, 1.5)) * gtao.ViewportPixelSize, db, gtao));
    }

    if (!all(isfinite(normal)) || dot(normal, normal) < 0.5)
        normal = float3(0, 0, -1);

    // For storage-padding pixels use the raw value computed at the padding coordinate,
    // with clamped depth already present in its mip chain.
    XeGTAO_MainPass(pixel, slices, steps, Noise(activePixel), normal, gtao,
                    depth, pointClamp, targetAO, targetEdges);
}

[numthreads(8, 8, 1)]
void HorizonLow(uint2 pixel : SV_DispatchThreadID) { Horizon(pixel, 1, 2); }
[numthreads(8, 8, 1)]
void HorizonMedium(uint2 pixel : SV_DispatchThreadID) { Horizon(pixel, 2, 2); }
[numthreads(8, 8, 1)]
void HorizonHigh(uint2 pixel : SV_DispatchThreadID) { Horizon(pixel, 3, 3); }
[numthreads(8, 8, 1)]
void HorizonUltra(uint2 pixel : SV_DispatchThreadID) { Horizon(pixel, 9, 3); }

[numthreads(8, 8, 1)]
void Denoise(uint2 thread : SV_DispatchThreadID)
{
    XeGTAO_Denoise(thread * uint2(2, 1), gtao, sourceAO, sourceEdges, pointClamp, targetAO, false);
}

[numthreads(8, 8, 1)]
void DenoiseFinal(uint2 thread : SV_DispatchThreadID)
{
    uint2 p = thread * uint2(2, 1);
    if (gtao.DenoiseBlurBeta >= 10000.0)
    {
        [unroll]
        for (uint side = 0; side < 2; ++side)
        {
            uint2 pixel = p + uint2(side, 0);
            float visibility = saturate(sourceAO.Load(int3(pixel, 0)) * (XE_GTAO_OCCLUSION_TERM_SCALE / 255.0));
            targetAO[pixel] = uint(AdjustVisibility(int2(pixel), visibility) * 255.0 + 0.5);
        }
        return;
    }
    XeGTAO_Denoise(p, gtao, sourceAO, sourceEdges, pointClamp, targetAO, true);
}

float AdjustVisibility(int2 pixel, float visibility)
{
    uint2 source = min(uint2(pixel), activeSize - 1u) + depthOrigin;
    float deviceZ = rawDepth.Load(int3(source, 0));
    if (deviceZ == backgroundDepth)
        return 1.0;

    float linearZ = XeGTAO_ScreenSpaceToViewSpaceDepth(deviceZ, gtao);
    float weight = isfinite(linearZ) && linearZ > 0.0
        ? 1.0 - smoothstep(fadeStart, fadeEnd, linearZ)
        : 0.0;
    return 1.0 - saturate((1.0 - visibility) * intensity * weight);
}
