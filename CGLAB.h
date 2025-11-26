#pragma once

#include "d3dApp.h"
#include "MathHelper.h"
#include "UploadBuffer.h"
#include "GeometryGenerator.h"
#include "Camera.h"
#include "FrameResource.h"
#include "ShadowMap.h"
#include "GeometryManager.h"
#include "ResourceManager.h"
#include "Config.h"
#include "GBuffer.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;


extern const int gNumFrameResources; // Должно быть определено где-то

struct RenderItem
{
    RenderItem() = default;
    RenderItem(const RenderItem& rhs) = delete;

    XMFLOAT4X4 World = MathHelper::Identity4x4();
    XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

    int NumFramesDirty = gNumFrameResources;
    UINT ObjCBIndex = -1;

    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;
    std::string name;

    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
    Opaque = 0,
    Debug,
    Sky,
    Count
};

class CGLAB : public D3DApp
{
public:
    CGLAB(HINSTANCE hInstance);
    CGLAB(const CGLAB& rhs) = delete;
    CGLAB& operator=(const CGLAB& rhs) = delete;
    ~CGLAB();

    virtual bool Initialize() override;

private:
    /*
    D3DAPP OVERRIDES & APP CYCLE
    */
    virtual void CreateRtvAndDsvDescriptorHeaps() override;
    virtual void OnResize() override;
    virtual void Update(const GameTimer& gt) override;
    virtual void Draw(const GameTimer& gt) override;

    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void DrawSceneToShadowMap();

    /*
    UPDATES
    */
    void OnKeyboardInput(const GameTimer& gt);
    void AnimateMaterials(const GameTimer& gt);
    void UpdateObjectCBs(const GameTimer& gt);
    void UpdateMaterialBuffer(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateLightCBs(const GameTimer& gt);
    void ImguiUpdate();

    /*
	INITIALIZATION METHODS
    */
    void BuildRootSignature();
    void BuildShadowsRootSignature();
    void BuildGeometryRootSignature();
    void BuildLightingRootSignature();
    void BuildDescriptorHeaps();
    void BuildShadersAndInputLayout();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildGBuffer(); 
    void BuildLights();
    void SetLightShapes();
    void BuildRenderItems();

	// HELPERS
    void CreatePointLight(XMFLOAT3 pos, XMFLOAT3 color, float faloff_start, float faloff_end, float strength);
    void CreateSpotLight(XMFLOAT3 pos, XMFLOAT3 rot, XMFLOAT3 color, float faloff_start, float faloff_end, float strength, float spotpower);
    void CreateRenderItem(std::string name, std::string materialname, int RItemLayer, XMMATRIX& scaling, XMMATRIX& rotation, XMMATRIX& translation, XMMATRIX texTransform = XMMatrixIdentity(), std::string drawargs = "");
    void ImguiInit();

    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> GetStaticSamplers(); // Семплеры

    /*
	INPUT HANDLERS
    */
    virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y) override;

private:
    /*
	FRAME RESOURCES
    */
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr; // Текущий ресурс кадра
    int mCurrFrameResourceIndex = 0;

    /*
    SIGNATURES AND PSOS
    */
    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12RootSignature> mShadowsRootSignature;
    ComPtr<ID3D12RootSignature> mGeometryRootSignature;
    ComPtr<ID3D12RootSignature> mLightingRootSignature;

    std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // ==========================================================
    // 7. МЕНЕДЖЕРЫ РЕСУРСОВ И КУЧИ
    // ==========================================================

    std::unique_ptr<ResourceManager> mResourceMgr;
    std::unique_ptr<GeometryManager> mGeomMgr;

    ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr; // Общая SRV/CBV/UAV куча
    ComPtr<ID3D12DescriptorHeap> m_ImGuiSrvDescriptorHeap; // Куча для ImGui
    CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;
    int gBufferSrvOffset; // Смещение для SRV G-Buffer

    // ==========================================================
    // 8. СЦЕНА И ОБЪЕКТЫ
    // ==========================================================

    Camera mCamera;
    DirectX::BoundingSphere mSceneBounds;

    std::vector<std::unique_ptr<RenderItem>> mAllRitems;
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    PassConstants mMainPassCB; // Константы для основного прохода

    // ==========================================================
    // 9. ОТЛОЖЕННЫЙ РЕНДЕРИНГ (GBUFFER)
    // ==========================================================

    std::unique_ptr<GBuffer> mGBuffer;

    // ==========================================================
    // 10. СВЕТ И ТЕНИ (LIGHTS & SHADOWS)
    // ==========================================================

    std::vector<std::unique_ptr<Light>> mLights;

    // Параметры Shadow Map
    float mLightNearZ = 0.0f;
    float mLightFarZ = 0.0f;
    XMFLOAT3 mLightPosW;
    XMFLOAT4X4 mLightView = MathHelper::Identity4x4();
    XMFLOAT4X4 mLightProj = MathHelper::Identity4x4();
    XMFLOAT4X4 mShadowTransform = MathHelper::Identity4x4();

    float mLightRotationAngle = 0.0f; // Угол поворота для симуляции движения

    // ==========================================================
    // 11. ВВОД
    // ==========================================================

    POINT mLastMousePos;
};