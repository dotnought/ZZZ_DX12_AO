cbuffer SceneConstants : register(b0) { float2 Scale; }
Texture2D<float> NativeDepth : register(t3);
SamplerState NativeSampler : register(s0);

struct Vertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Vertex SceneVertex(uint id : SV_VertexID)
{
    Vertex result;
    result.uv = float2((id << 1) & 2, id & 2);
    result.position = float4((result.uv * float2(2.0, -2.0) + float2(-1.0, 1.0)) * Scale, 0.0, 1.0);
    return result;
}

float4 SceneColor(Vertex input) : SV_Target0
{
    const float depth = NativeDepth.SampleLevel(NativeSampler, input.uv, 0);
    return float4(depth, 0.5, 0.75, 1.0);
}

float CurvedDepth(Vertex input) : SV_Depth
{
    const float2 pixel = input.position.xy - 0.5;
    if (pixel.x >= 213.0)
    {
        // A separated silhouette in front of shallow staircase depth steps.
        const float2 detached = pixel - float2(235, 231);
        return dot(detached, detached) <= 64.0 ? 0.29 : 0.25 + 0.0015 * floor((pixel.y - 192.0) / 8.0);
    }
    if (pixel.x >= 174.0)
    {
        const float2 overlay = pixel - float2(193, 211);
        return dot(overlay, overlay) <= 64.0 ? 0.253 : 0.25;
    }

    // Piecewise-linear depth across a cylinder reproduces a faceted limb.
    const float acrossCylinder = (input.uv.x - 0.28) / 0.095;
    if (abs(acrossCylinder) < 1.0)
    {
        const float facet = acrossCylinder / 0.24;
        const float2 ends = (floor(facet) + float2(0.0, 1.0)) * 0.24;
        const float2 curve = sqrt(saturate(1.0 - ends * ends));
        return 0.235 + 0.024 * lerp(curve.x, curve.y, frac(facet));
    }

    const float3 ray = normalize(float3((input.uv * 2.0 - 1.0) * 0.58, 1.0));
    const float3 center = float3(0.0, 0.87, 2.0);
    const float projection = dot(ray, center);
    const float discriminant = projection * projection - (dot(center, center) - 0.21 * 0.21);
    if (discriminant < 0.0)
        return 0.0;
    return 0.5 / ((projection - sqrt(discriminant)) * ray.z);
}
