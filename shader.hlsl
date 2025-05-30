cbuffer cbuffer0 : register(b0)
{
    float4x4 projection_matrix;
};

struct VS_Input
{
    float4 rect : RECT;
    float4 color : COLOR;
    uint vertex_id : SV_VertexID;
};

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PS_INPUT vs(VS_Input input)
{
    // Select current vertex (based on vertex id) and transform to screen space
    static float2 vertices[] =
    {
      {-1, -1},
      {+1, -1},
      {-1, +1},
      {+1, +1},
    };
    float2 rect_half_size = (input.rect.zw - input.rect.xy) / 2;
    float2 rect_center = (input.rect.xy + input.rect.zw) / 2;
    float2 position = vertices[input.vertex_id] * rect_half_size + rect_center;

    // Convert to clip space
    PS_INPUT output;
    output.position = mul(projection_matrix, float4(position, 0.0f, 1.0f));
    output.color = input.color;
    return output;
}

float4 ps(PS_INPUT input) : SV_TARGET
{
    return input.color;
}

