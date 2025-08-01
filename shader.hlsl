Texture2D glyph_atlas_texture : register(t0);
SamplerState mysampler : register(s0);

cbuffer cbuffer0 : register(b0)
{
    float4x4 projection_matrix;
};

//
// Input struct
//

struct VS_Input
{
    float4 target_rect : TARGET_RECT;
    float4 texture_rect : TEXTURE_RECT;
    float4 color : COLOR;
    float corner_radius : CORNER_RADIUS;
    float border_thickness : BORDER_THICKNESS;
    float4 border_color : BORDER_COLOR;
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
    float border_thickness : BORDER_THICKNESS;
    float4 border_color : BORDER_COLOR;
};

//
// vertex shader
//

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
    output.border_thickness = input.border_thickness;
    output.border_color = input.border_color;
    return output;
}

//
// SDF: rounded corner
//

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

//
// sRGB decode & encode
//

float3 sRGBToLinear(float3 srgb)
{
    srgb = saturate(srgb);
    return float3(
        srgb.r < 0.04045 ? srgb.r / 12.92 : pow((srgb.r + 0.055) / 1.055, 2.4),
        srgb.g < 0.04045 ? srgb.g / 12.92 : pow((srgb.g + 0.055) / 1.055, 2.4),
        srgb.b < 0.04045 ? srgb.b / 12.92 : pow((srgb.b + 0.055) / 1.055, 2.4)
    );
}

float3 linearToSRGB(float3 lin)
{
    lin = saturate(lin);
    return float3(
        lin.r < 0.0031308 ? lin.r * 12.92 : 1.055 * pow(lin.r, 1.0/2.4) - 0.055,
        lin.g < 0.0031308 ? lin.g * 12.92 : 1.055 * pow(lin.g, 1.0/2.4) - 0.055,
        lin.b < 0.0031308 ? lin.b * 12.92 : 1.055 * pow(lin.b, 1.0/2.4) - 0.055
    );
}

//
// pixel shader
//

float4 ps(PS_INPUT input) : SV_TARGET
{
    float glyph_texture_grayscale_ratio = glyph_atlas_texture.Sample(mysampler, input.uv).r;
    float4 texture_based_color = float4(input.color.rgb, input.color.a * glyph_texture_grayscale_ratio);
    
    // SDF-based rounded rectangle generation
    float2 distance_to_shrunk_corner = calculate_distance_to_shrunk_corner(
        input.position.xy, 
        input.target_rect_center, 
        input.target_rect_half_size, 
        input.corner_radius
    );
    
    // NOTE: The SDF function calculates distance to the geometric boundary of the shape,
    // but we sample at pixel centers. Need to create a half-pixel (0.5) offset to distance.
    // See: https://www.shadertoy.com/view/dtsXzH#
    float sdf_outer = rect_sdf(distance_to_shrunk_corner, input.corner_radius) + 0.5;
    float sdf_inner = sdf_outer + input.border_thickness;
    
    // Anti-aliased alpha mask generation
    float is_outer = 1.0 - smoothstep(0.0, 1.0, sdf_outer);
    float is_inner = 1.0 - smoothstep(0.0, 1.0, sdf_inner);

    // Color space conversion and blending
    float3 border_linear = sRGBToLinear(input.border_color.rgb);
    float3 texture_linear = sRGBToLinear(texture_based_color.rgb);
    float3 mixed_linear = lerp(border_linear, texture_linear, is_inner);
    float3 final_srgb = linearToSRGB(mixed_linear);

    // The outer mask ensures pixels outside the rounded rectangle are fully transparent
    float4 final_color = float4(final_srgb, texture_based_color.a * is_outer);
    return final_color;
}