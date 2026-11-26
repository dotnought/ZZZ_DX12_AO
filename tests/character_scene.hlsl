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

float4 SceneColor(float2 uv : TEXCOORD0) : SV_Target0
{
    const float depth = NativeDepth.SampleLevel(NativeSampler, uv, 0);
    return float4(depth, 0.5, 0.75, 1.0);
}
