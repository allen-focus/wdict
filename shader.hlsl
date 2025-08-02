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
    float4 border_color : BORDER_COLOR;
    float corner_radius : CORNER_RADIUS;
    float border_thickness : BORDER_THICKNESS;
    float enable_shadow : ENABLE_SHADOW;
    uint vertex_id : SV_VertexID;
};

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float2 uv : UV;
    float4 color : COLOR;
    float2 target_rect_half_size : TARGET_RECT_HALF_SIZE;
    float2 target_rect_center : TARGET_RECT_CENTER;
    float2 original_rect_half_size : ORIGINAL_RECT_HALF_SIZE;
    float2 original_rect_center : ORIGINAL_RECT_CENTER;
    float4 border_color : BORDER_COLOR;
    float corner_radius : CORNER_RADIUS;
    float border_thickness : BORDER_THICKNESS;
    float shadow_sigma : SHADOW_SIGMA;
    float2 shadow_offset : SHADOW_OFFSET;
};

//
// vertex shader
//

PS_INPUT vs(VS_Input input)
{
    static float2 vertices[] =
    {
        { -1, -1 },
        { +1, -1 },
        { -1, +1 },
        { +1, +1 },
    };

    // Calculate target position (screen space)
    float2 target_rect_half_size = (input.target_rect.zw - input.target_rect.xy) / 2;
    float2 target_rect_center = (input.target_rect.xy + input.target_rect.zw) / 2;
    float2 target_position = vertices[input.vertex_id] * target_rect_half_size + target_rect_center;

    // Calculate texture position (clip space)
    float2 texture_rect_half_size = (input.texture_rect.zw - input.texture_rect.xy) / 2;
    float2 texture_rect_center = (input.texture_rect.xy + input.texture_rect.zw) / 2;
    float2 texture_position = vertices[input.vertex_id] * texture_rect_half_size + texture_rect_center;

    // Calculate original rect based shadow
    float shadow_sigma = 0;
    float2 shadow_offset = float2(0, 0);
    float2 original_rect_half_size = target_rect_half_size;
    float2 original_rect_center = target_rect_center;
    if (input.enable_shadow >= 1)
    {
        shadow_sigma = 4;
        shadow_offset = float2(0, 2);

        // NOTE: As we hard-coded shadow sigma and offset, we could just use the 
        // pre-calculated original rect. The detail of that calculation is below:
        // ```
        // float shadow_radius = 3.0 * shadow_sigma;
        //
        // // Calculate how much we expanded in each direction
        // float expand_left = shadow_radius + max(0, -input.shadow_offset.x);
        // float expand_right = shadow_radius + max(0, input.shadow_offset.x);
        // float expand_top = shadow_radius + max(0, -input.shadow_offset.y);
        // float expand_bottom = shadow_radius + max(0, input.shadow_offset.y);
        //
        // // Shrink back to original size
        // original_rect_half_size.x -= (expand_left + expand_right) * 0.5;
        // original_rect_half_size.y -= (expand_top + expand_bottom) * 0.5;
        // original_rect_center.x -= input.shadow_offset.x * 0.5;
        // original_rect_center.y -= input.shadow_offset.y * 0.5;
        // ```
        original_rect_half_size.x -= 12;
        original_rect_half_size.y -= 13;
        original_rect_center.y -= 1;
    }

    // Output
    PS_INPUT output;
    output.position = mul(projection_matrix, float4(target_position, 0.0f, 1.0f)); // convert to clip space
    output.uv = texture_position;
    output.color = input.color;

    output.target_rect_half_size = target_rect_half_size;
    output.target_rect_center = target_rect_center;

    output.original_rect_half_size = original_rect_half_size;
    output.original_rect_center = original_rect_center;

    output.corner_radius = input.corner_radius;
    output.border_thickness = input.border_thickness;
    output.border_color = input.border_color;
    output.shadow_sigma = shadow_sigma;
    output.shadow_offset = shadow_offset;
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
// sRGB decode & encode (TODO: Need to learn about sRGB)
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
// Rounded Rectangle Blur (see: https://madebyevan.com/shaders/fast-rounded-rectangle-shadows)
//

// NOTE: Raph Levien post a relative blog that step further based Evan Wallace's method.
// See: https://raphlinus.github.io/graphics/2020/04/21/blurred-rounded-rects.html

// A standard gaussian function, used for weighting samples
float gaussian(float x, float sigma)
{
    const float pi = 3.141592653589793;
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (sqrt(2.0 * pi) * sigma);
}

// Approximate the error function (erf) for a float2 input
float2 erf(float2 x)
{
    float2 s = sign(x);
    float2 a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}

// Return the blurred mask along the x dimension
float roundedBoxShadowX(float x, float y, float sigma, float corner, float2 halfSize)
{
    float delta = min(halfSize.y - corner - abs(y), 0.0);
    float curved = halfSize.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    float2 erfArgs = (x + float2(-curved, curved)) * (sqrt(0.5) / sigma);
    float2 integral = 0.5 + 0.5 * erf(erfArgs);
    return integral.y - integral.x;
}

// Return the mask for the shadow of a box from lower to upper
float roundedBoxShadow(float2 lower, float2 upper, float2 pixel, float sigma, float corner)
{
    // Center everything
    float2 center = (lower + upper) * 0.5;
    float2 halfSize = (upper - lower) * 0.5;
    pixel -= center;

    float low = pixel.y - halfSize.y;
    float high = pixel.y + halfSize.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end = clamp(3.0 * sigma, low, high);

    float step = (end - start) / 4.0;
    float y = start + step * 0.5;
    float value = 0.0;

    [unroll]
    for (int i = 0; i < 4; i++) {
        value += roundedBoxShadowX(pixel.x, pixel.y - y, sigma, corner, halfSize) * gaussian(y, sigma) * step;
        y += step;
    }

    return value;
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
        input.original_rect_center,
        input.original_rect_half_size,
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
    float3 base_linear = lerp(border_linear, texture_linear, is_inner);
    float base_alpha = texture_based_color.a * is_outer;

    // Calculate shadow
    float shadow_alpha = 0;
    if (input.shadow_sigma)
    {
        float2 rect_min = input.original_rect_center - input.original_rect_half_size;
        float2 rect_max = input.original_rect_center + input.original_rect_half_size;
        shadow_alpha = roundedBoxShadow(
            rect_min + input.shadow_offset,
            rect_max + input.shadow_offset,
            input.position.xy,
            input.shadow_sigma,
            input.corner_radius
        );
    }

    // Alpha compositing using Porter-Duff "Over" operation
    float3 shadow_color = float3(0.8, 0.8, 0.8);
    float3 composed_rgb_linear = base_alpha * base_linear + shadow_alpha * shadow_color * (1.0 - base_alpha);
    float composed_alpha = base_alpha + shadow_alpha * (1.0 - base_alpha);

    // Convert back to sRGB and handle premultiplied alpha (1e-6 to avoid division by zero when alpha is 0)
    float3 shadowed_srgb = linearToSRGB(composed_rgb_linear / max(composed_alpha, 1e-6));
    return float4(shadowed_srgb, composed_alpha);
}
