// ZZZ 3.2 native visibility resolve. Slots and the two constant offsets come
// from the captured resolve shader and the matching pixel root signature.
Texture2D<float> SceneDepth : register(t0);
Texture2D<float> HalfDepth : register(t1);
Texture2D<float4> MaterialFlags : register(t2);
Texture2D<float> HalfVisibility : register(t3);
SamplerState MaterialSampler : register(s0);
SamplerState DepthSampler : register(s1);

cbuffer SceneConstants : register(b0)
{
    float MaterialMipBias : packoffset(c61.x);
    float2 EyeDepthUnpack : packoffset(c62);
}

float InverseEyeDepth(float depth)
{
    return mad(depth, EyeDepthUnpack.x, EyeDepthUnpack.y);
}

float ResolveNativeVisibility(float2 position, float centerEyeDepth)
{
    uint width, height;
    HalfVisibility.GetDimensions(width, height);
    if (width == 0 || height == 0)
        return 1.0;
    const int2 limit = int2(width, height) - 1;
    const int2 origin = int2(floor(position * 0.5 - 0.5));
    float weightedVisibility = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        const int2 location = clamp(origin + int2(i & 1u, i >> 1), int2(0, 0), limit);
        float depth = HalfDepth.Load(int3(location, 0));
        float weight = 0.01 + saturate(1.0 - 20.0 * abs(depth / centerEyeDepth - 1.0));
        weightedVisibility += HalfVisibility.Load(int3(location, 0)) * weight;
        totalWeight += weight;
    }
    return saturate(weightedVisibility / totalWeight);
}

float LoadInverseEyeDepth(int2 pixel, int2 limit)
{
    const float rawDepth = SceneDepth.Load(int3(clamp(pixel, int2(0, 0), limit), 0));
    return rawDepth > 0.0 ? InverseEyeDepth(rawDepth) : 0.0;
}

float SurfaceSlope(float backward, float center, float forward)
{
    const float a = center - backward;
    const float b = forward - center;
    return abs(a) < abs(b) ? a : b;
}

float FineOcclusion(float2 uv)
{
    uint width, height;
    SceneDepth.GetDimensions(width, height);
    if (width == 0 || height == 0)
        return 0.0;
    const int2 limit = int2(width, height) - 1;
    const int2 pixel = clamp(int2(uv * float2(width, height)), int2(0, 0), limit);
    const float center = LoadInverseEyeDepth(pixel, limit);
    if (center <= 1e-6)
        return 0.0;

    const float2 slope = float2(
        SurfaceSlope(LoadInverseEyeDepth(pixel - int2(1, 0), limit), center,
                     LoadInverseEyeDepth(pixel + int2(1, 0), limit)),
        SurfaceSlope(LoadInverseEyeDepth(pixel - int2(0, 1), limit), center,
                     LoadInverseEyeDepth(pixel + int2(0, 1), limit)));

    // Reciprocal eye depth is affine across a projected plane. Subtract its
    // local slope so an unoccluded tilted surface remains neutral. Reject
    // distant foreground/background edges to limit silhouette darkening.
    const int2 offsets[8] = {
        int2(3, 0), int2(-3, 0), int2(0, 3), int2(0, -3),
        int2(3, 3), int2(-3, 3), int2(3, -3), int2(-3, -3),
    };
    float occlusionSum = 0.0;
    float occlusionMax = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        const int2 location = clamp(pixel + offsets[i], int2(0, 0), limit);
        const float sampleDepth = LoadInverseEyeDepth(location, limit);
        const float difference = (sampleDepth - center) / center;
        const float predicted = dot(slope, float2(location - pixel)) / center;
        const float obstruction = difference - predicted;
        const float rangeWeight = 1.0 - smoothstep(0.012, 0.035, max(abs(difference), obstruction));
        const float amount = sampleDepth > 0.0
            ? smoothstep(0.0006, 0.004, obstruction) * rangeWeight : 0.0;
        occlusionSum += amount;
        occlusionMax = max(occlusionMax, amount);
    }
    return 0.42 * lerp(occlusionSum * (1.0 / 8.0), occlusionMax, 0.35);
}

float4 NativeDetailAo(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    const float rawDepth = SceneDepth.SampleLevel(DepthSampler, uv, 0);
    const float inverseDepth = InverseEyeDepth(rawDepth);
    if (rawDepth <= 0.0 || inverseDepth <= 1e-6)
        return 1.0;

    const bool nativeExclusion = MaterialFlags.SampleBias(MaterialSampler, uv, MaterialMipBias).a > 0.5;
    const float nativeVisibility = nativeExclusion ? 1.0 : ResolveNativeVisibility(position.xy, rcp(inverseDepth));
    return nativeVisibility * (1.0 - FineOcclusion(uv));
}
