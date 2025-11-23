#include "Common.hlsl"

// Входные текстуры G-Buffer (Привяжем их в слоты t3, t4, t5 в новом Root Signature)
Texture2D gGBuffer0 : register(t3); // Albedo + Roughness
Texture2D gGBuffer1 : register(t4); // Normal + Fresnel
Texture2D gGBuffer2 : register(t5); // Position

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

// Вершинный шейдер генерирует квадрат без VertexBuffer (по ID вершин)
VertexOut VS(uint vid : SV_VertexID)
{
    VertexOut vout;
    vout.TexC = float2((vid << 1) & 2, vid & 2);
    vout.PosH = float4(vout.TexC * float2(2, -2) + float2(-1, 1), 0, 1);
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    // 1. Читаем данные из G-Buffer
    // Используем Load, так как координаты пиксельные (0..Widht, 0..Height)
    int3 texCoord = int3(pin.PosH.xy, 0);

    float4 data0 = gGBuffer0.Load(texCoord);
    float4 data1 = gGBuffer1.Load(texCoord);
    float4 data2 = gGBuffer2.Load(texCoord);

    float3 albedo = data0.rgb;
    float roughness = data0.a;
    float3 normalW = data1.rgb;
    float fresnelR0_scalar = data1.a;
    float3 posW = data2.rgb;
    
    // Восстанавливаем Fresnel (упрощение: считаем его серым)
    float3 fresnelR0 = float3(fresnelR0_scalar, fresnelR0_scalar, fresnelR0_scalar);

    // 2. Расчет векторов
    float3 toEyeW = normalize(gEyePosW - posW);

    // 3. Тени (код из старого Default.hlsl)
    // Нам нужно трансформировать позицию в пространство тени
    float4 shadowPosH = mul(float4(posW, 1.0f), gShadowTransform);
    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = CalcShadowFactor(shadowPosH);

    // 4. Освещение (используем ту же функцию ComputeLighting)
    // shininess вычисляем как раньше
    float shininess = (1.0f - roughness);
    Material mat = { float4(albedo, 1.0f), fresnelR0, shininess };
    
    float4 directLight = ComputeLighting(gLights, mat, posW, normalW, toEyeW, shadowFactor);
    float4 ambient = gAmbientLight * float4(albedo, 1.0f);

    float4 litColor = ambient + directLight;

    // 5. Отражения (Cube Map)
    float3 r = reflect(-toEyeW, normalW);
    float4 reflectionColor = gCubeMap.Sample(gsamLinearWrap, r);
    float3 fresnelFactor = SchlickFresnel(fresnelR0, normalW, r);
    litColor.rgb += shininess * fresnelFactor * reflectionColor.rgb;

    litColor.a = 1.0f;
    return litColor;
}