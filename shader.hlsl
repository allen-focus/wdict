#define CLIP_QUEUE_CAPACITY 256 // 1. must matches C header's define; 2. must be a power of two

Texture2D glyph_atlas_texture : register(t0);
SamplerState mysampler : register(s0);

cbuffer mvp_buffer : register(b0)
{
    float4x4 projection_matrix;
};

cbuffer clip_rect_buffer : register(b1)
{
    float4 clip_rects[CLIP_QUEUE_CAPACITY];
};

//
// Input struct
//

struct VS_Input
{
    float4 target_rect : TARGET_RECT;
    float4 texture_rect : TEXTURE_RECT;
    float4 corner_color00 : COLOR0;
    float4 corner_color10 : COLOR1;
    float4 corner_color01 : COLOR2;
    float4 corner_color11 : COLOR3;
    float4 border_color : BORDER_COLOR;
    float4 shadow_color : SHADOW_COLOR;
    float4 style_params : STYLE_PARAMS; // x: corner_radius, y: border_thickness, z: shadow_offset_x, w: shadow_offset_y
    float shear : SHEAR;
    float shadow_sigma : SHADOW_SIGMA;
    float is_text : IS_TEXT;
    int clip_index : CLIP_INDEX;
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
    float4 shadow_color : SHADOW_COLOR;
    float corner_radius : CORNER_RADIUS;
    float border_thickness : BORDER_THICKNESS;
    float shadow_sigma : SHADOW_SIGMA;
    float2 shadow_offset : SHADOW_OFFSET;
    float is_text : IS_TEXT;
    int clip_index : CLIP_INDEX;
};

//
// vertex shader
//

PS_INPUT vs(VS_Input input)
{
    static float2 base_vertices[] =
    {
        { -1, -1 },
        { +1, -1 },
        { -1, +1 },
        { +1, +1 },
    };

    // Apply shear to right-side vertices
    float2 shear_offsets[] =
    {
        { 0, 0 },
        { 0, input.shear },
        { 0, 0 },
        { 0, input.shear },
    };

    // Calculate target position (screen space)
    float2 target_rect_half_size = (input.target_rect.zw - input.target_rect.xy) / 2;
    float2 target_rect_center = (input.target_rect.xy + input.target_rect.zw) / 2;
    float2 target_position = (base_vertices[input.vertex_id] * target_rect_half_size + target_rect_center)
                             + shear_offsets[input.vertex_id] * target_rect_half_size.y;

    // Calculate texture position (clip space)
    float2 texture_rect_half_size = (input.texture_rect.zw - input.texture_rect.xy) / 2;
    float2 texture_rect_center = (input.texture_rect.xy + input.texture_rect.zw) / 2;
    float2 texture_position = base_vertices[input.vertex_id] * texture_rect_half_size + texture_rect_center;

    // Calculate original rect based shadow
    float shadow_sigma = 0;
    float2 shadow_offset = float2(input.style_params.z, input.style_params.w);
    float2 original_rect_half_size = target_rect_half_size;
    float2 original_rect_center = target_rect_center;
    if (input.shadow_sigma)
    {
        shadow_sigma = input.shadow_sigma;
        float shadow_radius = 3.0 * shadow_sigma;

        // Calculate how much we expanded in each direction
        float expand_left = shadow_radius + max(0, -shadow_offset.x);
        float expand_right = shadow_radius + max(0, shadow_offset.x);
        float expand_top = shadow_radius + max(0, -shadow_offset.y);
        float expand_bottom = shadow_radius + max(0, shadow_offset.y);

        // Shrink back to original size
        original_rect_half_size.x -= (expand_left + expand_right) * 0.5;
        original_rect_half_size.y -= (expand_top + expand_bottom) * 0.5;
        original_rect_center.x -= shadow_offset.x * 0.5;
        original_rect_center.y -= shadow_offset.y * 0.5;
    }

    float4 per_corner_color;
    switch (input.vertex_id)
    {
        case 0: per_corner_color = input.corner_color00; break;
        case 1: per_corner_color = input.corner_color10; break;
        case 2: per_corner_color = input.corner_color01; break;
        default: per_corner_color = input.corner_color11; break;
    }

    // Output
    PS_INPUT output;
    output.position = mul(projection_matrix, float4(target_position, 0.0f, 1.0f)); // convert to clip space
    output.uv = texture_position;
    output.color = per_corner_color;

    output.target_rect_half_size = target_rect_half_size;
    output.target_rect_center = target_rect_center;

    output.original_rect_half_size = original_rect_half_size;
    output.original_rect_center = original_rect_center;

    output.border_color = input.border_color;
    output.shadow_color = input.shadow_color;
    output.corner_radius = input.style_params.x;
    output.border_thickness = input.style_params.y;
    output.shadow_sigma = shadow_sigma;
    output.shadow_offset = shadow_offset;
    output.is_text = input.is_text;
    output.clip_index = input.clip_index;
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
// sRGB encoding (for composition path, where RTV is non-sRGB UNORM)
//

float linear_to_srgb_channel(float x)
{
    if (x <= 0.0031308f)
        return 12.92f * x;
    return 1.055f * pow(abs(x), 1.0f / 2.4f) - 0.055f;
}

float3 linear_to_srgb(float3 c)
{
    return float3(linear_to_srgb_channel(c.r), linear_to_srgb_channel(c.g), linear_to_srgb_channel(c.b));
}

//
// pixel shader
//

float4 ps(PS_INPUT input) : SV_TARGET
{
    if (input.clip_index >= 0)
    {
        int idx = input.clip_index;
        float2 pos = input.position.xy;
        float2 clip_min = float2(clip_rects[idx].x, clip_rects[idx].y);
        float2 clip_max = float2(clip_rects[idx].z, clip_rects[idx].w);
        if (pos.x < clip_min.x || pos.x > clip_max.x || pos.y < clip_min.y || pos.y > clip_max.y)
            return float4(0, 0, 0, 0);
    }

    float glyph_texture_grayscale_ratio = glyph_atlas_texture.Sample(mysampler, input.uv).r;

    // Text rendering: texture already contains shape, just apply color
    if (input.is_text == 1.0)
    {
        float text_alpha = input.color.a * glyph_texture_grayscale_ratio;
#ifdef COMPOSITION_PATH
        return float4(linear_to_srgb(input.color.rgb) * text_alpha, text_alpha);
#else
        return float4(input.color.rgb, text_alpha);
#endif
    }

    // If the pixel is completely inside, skip subsequent corner‑radius and shadow calculations.
    float2 d = abs(input.position.xy - input.original_rect_center)
               - input.original_rect_half_size + max(input.corner_radius, input.border_thickness);
    if (d.x < -1.0 && d.y < -1.0)
#ifdef COMPOSITION_PATH
        return float4(linear_to_srgb(input.color.rgb) * input.color.a, input.color.a);
#else
        return input.color;
#endif

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

    // NOTE: Inner corners become sharp when border_thickness >= corner_radius,
    // because the effective inner corner radius (corner_radius - border_thickness) goes negative.
    float sdf_inner = sdf_outer + input.border_thickness;

    // Anti-aliased alpha mask generation
    // Use sub-pixel smoothing range for sharper edges without visible gray borders
    float is_outer = 1.0 - smoothstep(0, 1, sdf_outer);
    float is_inner = 1.0 - smoothstep(0, 1, sdf_inner);

    // Blending
    float3 texture_linear = input.color.rgb;
    float3 base_linear;
    if (input.border_thickness > 0.0)
    {
        float3 border_linear = input.border_color.rgb;
        base_linear = lerp(border_linear, texture_linear, is_inner);
    }
    else
    {
        // No border - use only texture color
        base_linear = texture_linear;
    }
    float base_alpha = lerp(input.border_color.a, input.color.a, is_inner) * is_outer;

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
    float3 shadow_color = input.shadow_color.rgb;
    float effective_shadow_alpha = shadow_alpha * input.shadow_color.a;
    float3 composed_rgb_linear = base_alpha * base_linear + effective_shadow_alpha * shadow_color * (1.0 - base_alpha);
    float composed_alpha = base_alpha + effective_shadow_alpha * (1.0 - base_alpha);
    float3 straight_linear = composed_rgb_linear / max(composed_alpha, 1e-6);

    // Handle premultiplied alpha
#ifdef COMPOSITION_PATH
    float3 straight_srgb = linear_to_srgb(straight_linear);
    return float4(straight_srgb * composed_alpha, composed_alpha);
#else
    return float4(straight_linear, composed_alpha);
#endif
}
