Texture2D<float> SceneDepth : register(t0);
SamplerState DepthClamp : register(s0);
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
    float2 correction = 0.0;
    float fullWeight = 0.32;
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
        const float2 rowCorrection = (delta.yw - delta.xz) * inverseSpan - localSlope.x;
        const float2 columnCorrection = (delta.zw - delta.xy) * inverseSpan - localSlope.y;
        correction += float2(dot(horizontal, rowCorrection), dot(vertical, columnCorrection));
        fullWeight += 2.0 * scaleWeights[radius - 1];
    }

    // Keep the original tangent as the prior for missing coverage. This
    // prevents one surviving diagonal from dominating next to a silhouette.
    return localSlope + correction / fullWeight;
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
    const float3 key = normalize(float3(-0.60, 0.66, 0.45));
    const float directional = dot(normal, key) - key.z;
    const float grazing = 1.0 - saturate(normal.z);
    return 1.0 + 0.42 * directional - 0.125 * grazing * grazing;
}

float OcclusionDisk(int2 pixel, int2 limit, float2 texelSize, float center, float inverseCenter, float2 slope)
{
    // Cover every texel in a circular footprint using 2x2 gathers. A thin
    // overlap fades continuously without picking out horizontal or diagonal rays.
    const int radius = 9;
    const float inverseRadiusSquared = 1.0 / (radius * radius);
    const float diskWeight = 84.8311233; // Sum of the squared falloff weights.
    const float2 halfExtent = 0.5 * limit;
    float occlusion = 0.0;
    [loop]
    for (int y = -radius; y < radius; y += 2)
    {
        float2 rowOcclusion = 0.0;
        float2 rowKnownWeight = 0.0;
        float2 rowWeight = 0.0;
        [loop]
        for (int x = -radius; x < radius; x += 2)
        {
            const int closestX = max(x, -x - 1);
            const int closestY = max(y, -y - 1);
            if (closestX * closestX + closestY * closestY >= radius * radius)
                continue;

            // Gather lanes are bottom-left, bottom-right, top-right, top-left.
            const float4 dx = x + float4(0, 1, 1, 0);
            const float4 dy = y + float4(1, 1, 0, 0);
            const float4 distanceSquared = dx * dx + dy * dy;
            const float4 falloff = saturate(1.0 - distanceSquared * inverseRadiusSquared);
            const float4 depths = SceneDepth.GatherRed(DepthClamp, (pixel + int2(x, y) + 1.0) * texelSize);
            const float4 dz = (depths - center) * inverseCenter;
            const float4 obstruction = dz - (slope.x * dx + slope.y * dy);
            const float4 angle = smoothstep(0.0, 0.0036 + 0.000055 * distanceSquared, obstruction - 0.00065);
            const float4 shallow = smoothstep(0.001, 0.009, obstruction);
            const float4 range = 1.0 - smoothstep(0.015, 0.05, max(abs(dz), obstruction));
            const float4 valid = float4(abs(pixel.x + dx - halfExtent.x) <= halfExtent.x)
                               * float4(abs(pixel.y + dy - halfExtent.y) <= halfExtent.y);
            const float4 spatial = falloff * falloff * valid;
            const float4 known = spatial * (1.0 - smoothstep(0.05, 0.075, abs(dz)));
            const float4 blocked = spatial * angle * shallow * range;
            rowOcclusion += blocked.xz + blocked.yw;
            rowKnownWeight += known.xz + known.yw;
            rowWeight += spatial.xz + spatial.yw;
        }
        // A detached foreground surface hides the depth behind it. Estimate
        // that contribution from the visible portion of the same rows so the
        // silhouette doesn't punch a bright wedge into an existing shadow.
        const float2 coverage = max(rowKnownWeight, 0.4 * rowWeight);
        occlusion += dot(rowOcclusion, rowWeight / max(coverage, 1e-6));
    }
    return 1.0 - 0.57 * occlusion / diskWeight;
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
    const float visibility = OcclusionDisk(pixel, limit, rcp(dimensions), center, inverseCenter, slope);
    return visibility * FormLighting(pixel, limit, dimensions, center, inverseCenter, slope);
}

float4 CharacterVisibility(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    const float shading = SceneShading(uv, 1.0);
    // The blend adds RGB * destination and retains alpha * destination.
    const float gain = max(shading - 1.0, 0.0);
    return float4(gain.xxx, min(shading, 1.0));
}

float4 ShadingView(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    // Reserve display headroom for the same lighting gains used in gameplay.
    const float gray = saturate((SceneShading(uv, 0.22) - 0.22) / 1.04);
    return float4(gray.xxx, 1.0);
}
