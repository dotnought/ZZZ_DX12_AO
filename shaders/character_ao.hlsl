Texture2D<float> SceneDepth : register(t0);
SamplerState DepthClamp : register(s0);

// Each entry contributes at both signs of its offset. The staggered radii
// cover the near contacts and the wider fade without distinct sampling rings.
// The last two columns scale the angular response and the radial falloff.
static const float4 Taps[] = {
    float4( -0.131,   1.695, 257.5, 0.818),
    float4( -1.850,  -1.606, 234.0, 0.783),
    float4(  3.329,  -0.690, 209.7, 0.739),
    float4( -3.675,   2.328, 190.0, 0.695),
    float4(  0.737,  -5.652, 167.7, 0.635),
    float4(  1.385,   6.913, 150.0, 0.575),
    float4( -6.058,  -6.034, 134.3, 0.511),
    float4( 10.050,   1.742, 120.4, 0.442),
    float4( -9.319,   7.560, 108.2, 0.370),
    float4( -3.545, -13.699,  96.5, 0.288),
    float4( 15.886,  -4.458,  86.4, 0.205),
    float4(-19.045,   1.453,  77.3, 0.120),
    float4( -6.199, -20.691,  70.3, 0.050),
};

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

float SampleOcclusion(float2 location, float2 pixel, float2 limit, float2 texelSize,
                      float center, float inverseCenter, float2 slope, float angleScale)
{
    const float4 depths = SceneDepth.GatherRed(DepthClamp, (location + 0.5) * texelSize);
    const float2 corner = floor(location);
    const float2 fraction = location - corner;
    const float4 weights = float4(
        (1.0 - fraction.x) * fraction.y, fraction.x * fraction.y,
        fraction.x * (1.0 - fraction.y), (1.0 - fraction.x) * (1.0 - fraction.y));

    // Gather returns bottom-left, bottom-right, top-right, then top-left.
    const float4 x = clamp(corner.x + float4(0, 1, 1, 0), 0.0, limit.x) - pixel.x;
    const float4 y = clamp(corner.y + float4(1, 1, 0, 0), 0.0, limit.y) - pixel.y;
    const float4 relativeDepth = (depths - center) * inverseCenter;
    const float4 obstruction = relativeDepth - (x * slope.x + y * slope.y);
    const float4 contact = smoothstep(0.0, 1.0, (obstruction - 0.00065) * angleScale);
    const float4 range = 1.0 - smoothstep(0.028, 0.074, max(abs(relativeDepth), obstruction));

    // Interpolate the occlusion contributions after depth rejection. Filtering
    // depth first would invent blockers in the gap at a character silhouette.
    return dot(weights, contact * range);
}

float4 CharacterVisibility(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    uint width, height;
    SceneDepth.GetDimensions(width, height);
    const int2 limit = int2(width, height) - 1;
    if (width == 0 || height == 0 || any(uv < 0.0) || any(uv > 1.0))
        return 1.0;

    const float2 dimensions = float2(width, height);
    const int2 pixel = min(int2(uv * dimensions), limit);
    const float center = LoadDepth(pixel, limit);
    if (center < 0.0001)
        return 1.0;

    // Reversed device depth is affine across projected planar surfaces.
    const float inverseCenter = rcp(center);
    const float2 slope = float2(
        SurfaceSlope(LoadDepth(pixel - int2(1, 0), limit), center, LoadDepth(pixel + int2(1, 0), limit)),
        SurfaceSlope(LoadDepth(pixel - int2(0, 1), limit), center, LoadDepth(pixel + int2(0, 1), limit))
    ) * inverseCenter;
    const float2 texelSize = rcp(dimensions);

    float occlusion = 0.0;
    float weight = 0.0;
    [loop]
    for (uint i = 0; i < sizeof(Taps) / sizeof(Taps[0]); ++i)
    {
        const float4 tap = Taps[i];
        const float forward = SampleOcclusion(pixel + tap.xy, pixel, limit, texelSize,
                                              center, inverseCenter, slope, tap.z);
        const float backward = SampleOcclusion(pixel - tap.xy, pixel, limit, texelSize,
                                               center, inverseCenter, slope, tap.z);
        occlusion += (forward + backward) * tap.w;
        weight += tap.w;
    }
    const float visibility = 1.0 - 0.27 * (occlusion / weight);
    return float4(visibility.xxx, 1.0);
}
