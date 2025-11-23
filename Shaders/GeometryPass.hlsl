//***************************************************************************************
// Default.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

// Defaults for number of lights.
#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

// Include common HLSL code.
#include "Common.hlsl"

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
    float3 TangentU : TANGENT;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float4 ShadowPosH : POSITION0;
    float3 PosW : POSITION1;
    float3 NormalW : NORMAL;
    float3 TangentW : TANGENT;
    float2 TexC : TEXCOORD;
};

// Выходная структура Пиксельного Шейдера
struct PixelOut
{
    float4 AlbedoRoughness : SV_Target0; // RT0
    float4 NormalFresnel : SV_Target1; // RT1
    float4 Position : SV_Target2; // RT2
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout = (VertexOut) 0.0f;

	// Fetch the material data.
    MaterialData matData = gMaterialData[gMaterialIndex];
	
    // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3) gWorld);
	
    vout.TangentW = mul(vin.TangentU, (float3x3) gWorld);

    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);
	
	// Output vertex attributes for interpolation across triangle.
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = mul(texC, matData.MatTransform).xy;

    // Generate projective tex-coords to project shadow map onto scene.
    vout.ShadowPosH = mul(posW, gShadowTransform);
	
    return vout;
}

PixelOut PS(VertexOut pin) : SV_Target
{
    PixelOut pout;
    
	// Fetch the material data.
    MaterialData matData = gMaterialData[gMaterialIndex];
    float4 diffuseAlbedo = matData.DiffuseAlbedo;
    float3 fresnelR0 = matData.FresnelR0;
    float roughness = matData.Roughness;
    uint diffuseMapIndex = matData.DiffuseMapIndex;
    uint normalMapIndex = matData.NormalMapIndex;
	
    // 1. Sample Albedo
    float4 texColor = gTextureMaps[NonUniformResourceIndex(diffuseMapIndex)].Sample(gsamAnisotropicWrap, pin.TexC);
    diffuseAlbedo *= texColor;
    
#ifdef ALPHA_TEST
    clip(diffuseAlbedo.a - 0.1f);
#endif

	// 2. Normal Mapping
    pin.NormalW = normalize(pin.NormalW);
    float4 normalMapSample = gTextureMaps[NonUniformResourceIndex(normalMapIndex)].Sample(gsamAnisotropicWrap, pin.TexC);
    float3 bumpedNormalW = NormalSampleToWorldSpace(normalMapSample.rgb, pin.NormalW, pin.TangentW);
	
    // RT0: Цвет (RGB) + Шероховатость (A)
    pout.AlbedoRoughness = float4(diffuseAlbedo.rgb, roughness);

    // RT1: Нормаль (RGB) + Френель (A) 
    pout.NormalFresnel = float4(bumpedNormalW, fresnelR0.r);

    // RT2: Мировая позиция (RGB) + Unused
    pout.Position = float4(pin.PosW, 1.0f);

    return pout;
    
}


