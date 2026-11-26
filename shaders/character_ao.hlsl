Texture2D<float> SceneDepth : register(t0);

float LoadDepth(int2 pixel, int2 limit)
{
    return SceneDepth.Load(int3(clamp(pixel, int2(0, 0), limit), 0));
}

float SurfaceSlope(float low, float center, float high)
{
    const float backward = center - low;
    const float forward = high - center;
    return abs(backward) < abs(forward) ? backward : forward;
}

float4 CharacterVisibility(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    uint width, height;
    SceneDepth.GetDimensions(width, height);
    const int2 limit = int2(width, height) - 1;
    if (width == 0 || height == 0 || any(uv < 0.0) || any(uv > 1.0))
        return 1.0;
    const int2 pixel = min(int2(uv * float2(width, height)), limit);

    const float center = LoadDepth(pixel, limit);
    if (center < 0.0001)
        return 1.0;

    // The game's reversed device depth is affine on projected planar surfaces.
    const float2 slope = float2(
        SurfaceSlope(LoadDepth(pixel - int2(1, 0), limit), center, LoadDepth(pixel + int2(1, 0), limit)),
        SurfaceSlope(LoadDepth(pixel - int2(0, 1), limit), center, LoadDepth(pixel + int2(0, 1), limit)));
    const int2 offsets[16] = {
        int2(3, 0), int2(-3, 0), int2(0, 3), int2(0, -3),
        int2(2, 2), int2(-2, 2), int2(2, -2), int2(-2, -2),
        int2(7, 0), int2(-7, 0), int2(0, 7), int2(0, -7),
        int2(5, 5), int2(-5, 5), int2(5, -5), int2(-5, -5),
    };
    const float inverseCenter = rcp(center);
    float total = 0.0;
    float strongest = 0.0;
    [unroll]
    for (uint i = 0; i < 16; ++i)
    {
        const int2 location = clamp(pixel + offsets[i], int2(0, 0), limit);
        const float relativeDepth = (LoadDepth(location, limit) - center) * inverseCenter;
        const float relativeSlope = dot(slope, float2(location - pixel)) * inverseCenter;
        const float obstruction = relativeDepth - relativeSlope;
        // Fall off across depth discontinuities to suppress silhouette halos.
        const float range = 1.0 - smoothstep(0.018, 0.055, max(abs(relativeDepth), obstruction));
        const float contact = smoothstep(0.0008, 0.009, obstruction) * range * (i < 8 ? 1.0 : 0.75);
        total += contact;
        strongest = max(strongest, contact);
    }
    const float visibility = 1.0 - 0.58 * lerp(total / 14.0, strongest, 0.30);
    return float4(visibility.xxx, 1.0);
}
