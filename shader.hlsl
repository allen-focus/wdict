Texture2D glyph_atlas_texture : register(t0);
SamplerState mysampler : register(s0);

cbuffer cbuffer0 : register(b0)
{
    float4x4 projection_matrix;
};

struct VS_Input
{
    float4 target_rect : TARGET_RECT;
    float4 texture_rect : TEXTURE_RECT;
    float4 color : COLOR;
    uint vertex_id : SV_VertexID;
};

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float2 uv : UV;
    float4 color : COLOR;
};

PS_INPUT vs(VS_Input input)
{
    static float2 vertices[] =
    {
        {-1, -1},
        {+1, -1},
        {-1, +1},
        {+1, +1},
    };

    // Calculate target position (screen space)
    float2 target_rect_half_size = (input.target_rect.zw - input.target_rect.xy) / 2;
    float2 target_rect_center = (input.target_rect.xy + input.target_rect.zw) / 2;
    float2 target_position = vertices[input.vertex_id] * target_rect_half_size + target_rect_center;

    // Calculate texture position (clip space)
    float2 texture_rect_half_size = (input.texture_rect.zw - input.texture_rect.xy) / 2;
    float2 texture_rect_center = (input.texture_rect.xy + input.texture_rect.zw) / 2;
    float2 texture_position = vertices[input.vertex_id] * texture_rect_half_size + texture_rect_center;

    // Output
    PS_INPUT output;
    output.position = mul(projection_matrix, float4(target_position, 0.0f, 1.0f)); // convert to clip space
    output.uv = texture_position;
    output.color = input.color;
    return output;
}

float4 ps(PS_INPUT input) : SV_TARGET
{
    float glyph_texture_grayscale_ratio = glyph_atlas_texture.Sample(mysampler, input.uv).r;
    return float4(input.color.rgb, input.color.a * glyph_texture_grayscale_ratio);
}
