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
    float corner_radius : CORNER_RADIUS;
    uint vertex_id : SV_VertexID;
};

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float2 uv : UV;
    float4 color : COLOR;
    float2 target_rect_half_size : TARGET_RECT_HALF_SIZE;
    float2 target_rect_center : TARGET_RECT_CENTER;
    float corner_radius : CORNER_RADIUS;
};

PS_INPUT vs(VS_Input input)
{
    static float2 vertices[] =
    {
        { -1, -1 },
        { + 1, -1 },
        { -1, + 1 },
        { + 1, + 1 },
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

    output.target_rect_half_size = target_rect_half_size;
    output.target_rect_center = target_rect_center;

    output.corner_radius = input.corner_radius;
    return output;
}

// See:
//   https://www.youtube.com/watch?v=62-pRVZuS5c
//   https://zed.dev/blog/videogame#drawing-rectangles
float2 calculate_distance_to_shrunk_corner(float2 pixel_position, float2 rect_center, float2 rect_half_size, float corner_radius)
{
    return abs(rect_center - pixel_position) - rect_half_size + float2(corner_radius, corner_radius);
}

float rect_sdf(float2 distance_to_shrunk_corner, float corner_radius)
{
    return length(max(distance_to_shrunk_corner, 0.0)) +
        min(max(distance_to_shrunk_corner.x, distance_to_shrunk_corner.y), 0.0) -
        corner_radius;
}

float4 ps(PS_INPUT input) : SV_TARGET
{
    float glyph_texture_grayscale_ratio = glyph_atlas_texture.Sample(mysampler, input.uv).r;
    float4 texture_based_color = float4(input.color.rgb, input.color.a * glyph_texture_grayscale_ratio);
    
    // NOTE: The SDF function calculates distance to the geometric boundary of the shape,
    // but we sample at pixel centers. Need to create a half-pixel (0.5) offset to distance.
    float2 distance_to_shrunk_corner = calculate_distance_to_shrunk_corner(
        input.position.xy, input.target_rect_center, input.target_rect_half_size, input.corner_radius
    );
    float distance = 0.5 + rect_sdf(distance_to_shrunk_corner, input.corner_radius);
    float alpha_mask = 1.0 - smoothstep(0.0, 1.0, distance);
    texture_based_color.a *= alpha_mask;
    
    return texture_based_color;
}