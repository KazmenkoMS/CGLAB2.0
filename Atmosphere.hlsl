
Texture2D frameTex : register(t0);

SamplerState gsamPointWrap : register(s0);
SamplerState gsamPointClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);
SamplerState gsamAnisotropicWrap : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);
SamplerComparisonState gsamShadow : register(s6);
struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};



VSOut VS(uint vid : SV_VertexID)
{
    VSOut output;
    
    // Координаты вершин полноэкранного треугольника
    float2 positions[3] =
    {
        float2(-1, -1),
        float2(-1, 3),
        float2(3, -1)
    };
    
    output.PosH = float4(positions[vid], 0, 1);
    output.TexC = output.PosH.xy * 0.5 + 0.5;
    output.TexC.y = 1.0 - output.TexC.y;
    return output;
}

float4 PS(VSOut pin) : SV_Target
{
    float4 color = frameTex.Sample(gsamLinearClamp, pin.TexC);
    return float4(1, 1, 1, 1);

}
