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

float SurfaceSlope(int2 pixel, int2 axis, int2 limit, float center)
{
    const int2 low = pixel - axis;
    const int2 high = pixel + axis;
    const float backward = center - LoadDepth(low, limit);
    const float forward = LoadDepth(high, limit) - center;
    if (any(low < 0))
        return forward;
    if (any(high > limit))
        return backward;
    return abs(backward) < abs(forward) ? backward : forward;
}

float FormSlope(int2 pixel, int2 axis, int2 limit, float center, float inverseCenter, float localSlope)
{
    float sum = localSlope;
    float totalWeight = 1.0;
    [unroll]
    for (int radius = 5; radius <= 9; radius += 4)
    {
        const int2 low = pixel - radius * axis;
        const int2 high = pixel + radius * axis;
        const float2 valid = float2(all(low >= 0), all(high <= limit));
        const float2 delta = (float2(LoadDepth(low, limit), LoadDepth(high, limit)) - center) * inverseCenter;
        const float2 distances = float2(-radius, radius);
        const float2 weights = valid
            * (1.0 - smoothstep(0.0025, 0.009, abs(delta - localSlope * distances)))
            * (1.0 - smoothstep(0.028, 0.085, abs(delta)));
        sum += dot(weights, delta / distances);
        totalWeight += weights.x + weights.y;
    }
    return sum / totalWeight;
}

float FormLighting(int2 pixel, int2 limit, float2 dimensions, float center, float inverseCenter, float2 slope)
{
    const float2 derivative = float2(
        FormSlope(pixel, int2(1, 0), limit, center, inverseCenter, slope.x),
        FormSlope(pixel, int2(0, 1), limit, center, inverseCenter, slope.y));

    // Approximate the camera's ray spacing. Inverse depth is affine on a
    // perspective plane, so account for the pixel's distance from screen center.
    const float pixelRaySpacing = 1.16 / dimensions.y;
    const float forward = 1.0 - dot(pixel + 0.5 - 0.5 * dimensions, derivative);
    const float3 normal = normalize(float3(-derivative / pixelRaySpacing, max(forward, 0.03)));
    // Native depth UVs are bottom-up in the observed scene.
    const float3 key = normalize(float3(-0.52, 0.59, 0.618));
    return lerp(0.68, 1.0, smoothstep(-0.36, 0.94, dot(normal, key)));
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

float SceneShading(float2 uv, float background)
{
    uint width, height;
    SceneDepth.GetDimensions(width, height);
    const int2 limit = int2(width, height) - 1;
    if (width == 0 || height == 0 || any(uv < 0.0) || any(uv > 1.0))
        return background;

    const float2 dimensions = float2(width, height);
    const int2 pixel = min(int2(uv * dimensions), limit);
    const float center = LoadDepth(pixel, limit);
    if (center < 0.0001)
        return background;

    // Reversed device depth is affine across projected planar surfaces.
    const float inverseCenter = rcp(center);
    const float2 slope = float2(
        SurfaceSlope(pixel, int2(1, 0), limit, center),
        SurfaceSlope(pixel, int2(0, 1), limit, center)
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
    return visibility * FormLighting(pixel, limit, dimensions, center, inverseCenter, slope);
}

float4 CharacterVisibility(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    const float shading = SceneShading(uv, 1.0);
    return float4(shading.xxx, 1.0);
}

float4 ShadingView(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    // Expand display contrast while keeping the same form and occlusion terms.
    const float gray = saturate((SceneShading(uv, 0.48) - 0.48) / 0.52);
    return float4(gray.xxx, 1.0);
}
