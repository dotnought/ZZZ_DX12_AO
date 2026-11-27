Texture2D<float> SceneDepth : register(t0);
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
    // A feature only one pixel wide has no reliable tangent across its edges.
    if (backward * forward < 0.0 && min(abs(backward), abs(forward)) > 0.0052 * center)
        return 0.0;
    return abs(backward) < abs(forward) ? backward : forward;
}

float2 SmoothFormSlope(int2 pixel, int2 limit, float center, float inverseCenter, float2 localSlope)
{
    float2 sum = 0.32 * localSlope;
    float2 totalWeight = 0.32;
    static const float scaleWeights[] = {0.22, 0.5, 0.8, 1.0, 1.0, 0.85, 0.6, 0.35};
    float4 connected = 1.0;
    float4 lastDepth = 0.0;
    float4 lastStep = float4(-1, 1, -1, 1) * localSlope.x
                    + float4(-1, -1, 1, 1) * localSlope.y;

    // Check every texel along the four diagonals. A wide sample cannot
    // contribute if a closer sample crossed an edge on the same path.
    [loop]
    for (int radius = 1; radius <= 8; ++radius)
    {
        const int2 low = pixel - radius;
        const int2 high = pixel + radius;
        const float4 valid = float4(
            all(low >= 0), high.x <= limit.x && low.y >= 0,
            low.x >= 0 && high.y <= limit.y, all(high <= limit));
        const float4 depth = float4(
            LoadDepth(low, limit), LoadDepth(int2(high.x, low.y), limit),
            LoadDepth(int2(low.x, high.y), limit), LoadDepth(high, limit));
        const float4 delta = (depth - center) * inverseCenter;
        const float4 step = delta - lastDepth;
        connected = min(connected, valid * (1.0 - smoothstep(0.0017, 0.0052, abs(step - lastStep))));
        const float4 confidence = connected * scaleWeights[radius - 1]
            * (1.0 - smoothstep(0.028, 0.085, abs(delta)));
        lastDepth = delta;
        lastStep = step;

        // Both ends must belong to the receiving surface. The local slope
        // remains available for thin geometry with no reliable wide pair.
        const float2 horizontal = min(confidence.xz, confidence.yw);
        const float2 vertical = min(confidence.xy, confidence.zw);
        const float inverseSpan = 0.5 / radius;
        sum += inverseSpan * float2(dot(horizontal, delta.yw - delta.xz),
                                    dot(vertical, delta.zw - delta.xy));
        totalWeight += float2(horizontal.x + horizontal.y, vertical.x + vertical.y);
    }
    return sum / totalWeight;
}

float FormLighting(int2 pixel, int2 limit, float2 dimensions, float center, float inverseCenter, float2 slope)
{
    const float2 derivative = SmoothFormSlope(pixel, limit, center, inverseCenter, slope);

    // Approximate the camera's ray spacing. Inverse depth is affine on a
    // perspective plane, so account for the pixel's distance from screen center.
    const float pixelRaySpacing = 1.16 / dimensions.y;
    const float forward = 1.0 - dot(pixel + 0.5 - 0.5 * dimensions, derivative);
    const float3 normal = normalize(float3(-derivative / pixelRaySpacing, max(forward, 0.03)));
    // Native depth UVs are bottom-up in the observed scene.
    const float3 key = normalize(float3(-0.52, 0.59, 0.618));
    return lerp(0.68, 1.0, smoothstep(-0.36, 0.94, dot(normal, key)));
}

float OcclusionAlong(int2 pixel, int2 limit, float center, float inverseCenter, float2 slope, int2 axis)
{
    const bool diagonal = axis.x != 0 && axis.y != 0;
    const float pixelDistance = diagonal ? 1.41421356 : 1.0;
    const int steps = diagonal ? 11 : 15;
    const float axisSlope = dot(slope, float2(axis));
    float2 horizon = 0.0;

    // Scan both sides without gaps, so a narrow blocker fades with distance
    // instead of dropping out and reappearing at different tap radii.
    [loop]
    for (int i = 1; i <= steps; ++i)
    {
        const int2 offset = i * axis;
        const int2 high = pixel + offset;
        const int2 low = pixel - offset;
        const float2 valid = float2(all(high >= 0) && all(high <= limit),
                                    all(low >= 0) && all(low <= limit));
        const float2 relativeDepth = (float2(LoadDepth(high, limit), LoadDepth(low, limit)) - center) * inverseCenter;
        const float2 obstruction = relativeDepth - float2(i, -i) * axisSlope;
        const float distance = pixelDistance * i;
        const float2 angle = smoothstep(0.0, 0.003 + 0.00052 * distance, obstruction - 0.00065);
        const float2 shallow = smoothstep(0.001, 0.009, obstruction);
        const float2 range = 1.0 - smoothstep(0.028, 0.074, max(abs(relativeDepth), obstruction));
        const float falloff = 1.0 - smoothstep(0.0, 16.0, distance);
        horizon = max(horizon, valid * angle * shallow * range * falloff);
    }
    return horizon.x + horizon.y;
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
    static const int2 axes[] = {int2(1, 0), int2(0, 1), int2(1, 1), int2(-1, 1)};
    float occlusion = 0.0;
    [unroll]
    for (uint i = 0; i < sizeof(axes) / sizeof(axes[0]); ++i)
        occlusion += OcclusionAlong(pixel, limit, center, inverseCenter, slope, axes[i]);
    const float visibility = 1.0 - 0.52 * (occlusion / 8.0);
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
