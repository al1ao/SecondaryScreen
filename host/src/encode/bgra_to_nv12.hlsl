// BT.709, studio/limited range. BGRA SRVs present logical RGB channels to HLSL.
Texture2D<float4> image : register(t0);
SamplerState linearClamp : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex Fullscreen(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float Luma(Vertex v) : SV_Target {
    float3 rgb = image.Sample(linearClamp, v.uv).rgb;
    return (16.0 + 219.0 * dot(rgb, float3(0.2126, 0.7152, 0.0722))) / 255.0;
}
float2 Chroma(Vertex v) : SV_Target {
    // Half-resolution rendering plus linear sampling averages each chroma footprint.
    float3 rgb = image.Sample(linearClamp, v.uv).rgb;
    return (128.0 + 224.0 * float2(dot(rgb, float3(-0.114572, -0.385428, 0.5)),
        dot(rgb, float3(0.5, -0.454153, -0.045847)))) / 255.0;
}
