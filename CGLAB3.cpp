#include <windows.h>
#include "CGLAB.h"

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "imgui.h"

const int gNumFrameResources = 3;

// Main application entry point.
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
	PSTR cmdLine, int showCmd)
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	try
	{
		CGLAB theApp(hInstance);
		if (!theApp.Initialize())
			return 0;

		return theApp.Run();
	}
	catch (DxException& e)
	{
		MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
		return 0;
	}
}


CGLAB::CGLAB(HINSTANCE hInstance)
	: D3DApp(hInstance)
{
	mSceneBounds.Center = mConfig.SceneBoundsCenter;
	mSceneBounds.Radius = mConfig.SceneBoundsRadius;
}
CGLAB::~CGLAB()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}




//==============================================================FRAMEWORK METHODS==============================================================


/*
INITIALIZATION
*/
bool CGLAB::Initialize()
{
	if (!D3DApp::Initialize())
		return false;

	// Reset the command list to prep for initialization commands.
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	mCamera.SetPosition(0.0f, 2.0f, -15.0f);

	mShadowMap = std::make_unique<ShadowMap>(
		md3dDevice.Get(), mConfig.ShadowMapHeight, mConfig.ShadowMapWidth);
	mGeomMgr = std::make_unique<GeometryManager>();
	mResourceMgr = std::make_unique<ResourceManager>();
	mResourceMgr->Init(md3dDevice.Get(), mCommandList.Get());
	mResourceMgr->LoadTextures();
	//LoadTextures();
	BuildRootSignature();
	BuildDescriptorHeaps();
	BuildShadersAndInputLayout();
	
	mGeomMgr->BuildBasicGeometry(md3dDevice.Get(), mCommandList.Get());
	mGeomMgr->BuildGeometryFromFile(md3dDevice.Get(), mCommandList.Get(), "Models/skull.obj", "skullGeo");
	BuildGeometryRootSignature();
	BuildLightingRootSignature();
	BuildGBuffer();
	mResourceMgr->BuildMaterials();
	BuildRenderItems();
	BuildFrameResources();
	BuildPSOs();
	ImguiInit();
	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	OnResize();

	// Wait until initialization is complete.
	FlushCommandQueue();

	return true;
}

void CGLAB::OnResize()
{
	D3DApp::OnResize();
	BuildDescriptorHeaps();
	BuildGBuffer();
	mCamera.SetLens(mConfig.CameraFovY, AspectRatio(), mConfig.CameraNearZ, mConfig.CameraFarZ);
}

void CGLAB::Update(const GameTimer& gt)
{
	OnKeyboardInput(gt);

	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	//
	// Animate the lights (and hence shadows).
	//

	mLightRotationAngle += 0.1f * gt.DeltaTime();

	XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
	for (int i = 0; i < 3; ++i)
	{
		XMFLOAT3 a(1, -1, 0);
		XMVECTOR lightDir = XMLoadFloat3(&a);
		XMStoreFloat3(&mConfig.RotatedLightDirections[i], lightDir);
	}

	AnimateMaterials(gt);
	UpdateObjectCBs(gt);
	UpdateMaterialBuffer(gt);
	UpdateShadowTransform(gt);
	UpdateMainPassCB(gt);
	UpdateShadowPassCB(gt);
	ImguiUpdate();
}

void CGLAB::Draw(const GameTimer& gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

	ThrowIfFailed(cmdListAlloc->Reset());

	// Сбрасываем список команд, используя PSO для теней
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["shadow_opaque"].Get()));

	// Устанавливаем кучу дескрипторов (один раз за кадр)
	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	// Получаем адрес буфера материалов (он нужен почти везде)
	auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();

	// ==========================================
	// 1. Shadow Pass
	// ==========================================
	
	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

	// !!! ИСПРАВЛЕНИЕ 1: Привязываем Material Buffer (Slot 2) !!!
	// Без этого шейдер читает мусор и GPU виснет
	mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

	// !!! ИСПРАВЛЕНИЕ 2: Привязываем Текстуры (Slot 4) !!!
	// Main Root Signature имеет 2 таблицы (Slot 3 и Slot 4). 
	// Slot 4 - это unbounded array текстур. Даже если тени их не используют, 
	// лучше привязать кучу, чтобы избежать неопределенного поведения.
	mCommandList->SetGraphicsRootDescriptorTable(3, mNullSrv); // Slot 3
	mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart()); // Slot 4

	// Выполняем Shadow Pass
	DrawSceneToShadowMap();

	// ==========================================
	// 2. Geometry Pass (G-Buffer)
	// ==========================================

	// Переводим ресурсы GBuffer из GENERIC_READ в RENDER_TARGET
	D3D12_RESOURCE_BARRIER barriers[3];
	for (int i = 0; i < 3; ++i)
		barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer->Resource(i),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
	mCommandList->ResourceBarrier(3, barriers);

	// Чистим G-Buffer и ставим его как цель
	float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandles[3];
	for (int i = 0; i < 3; ++i) {
		rtvHandles[i] = mGBuffer->Rtv(i);
		mCommandList->ClearRenderTargetView(rtvHandles[i], clearColor, 0, nullptr);
	}
	
	// Используем основной Depth Buffer сцены
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	mCommandList->OMSetRenderTargets(3, rtvHandles, false, &DepthStencilView());
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	mCommandList->SetPipelineState(mPSOs["GeometryPass"].Get());
	mCommandList->SetGraphicsRootSignature(mGeometryRootSignature.Get());

	// Биндим Pass CBV (Slot 1)
	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

	// Биндим Material Buffer (Slot 2)
	mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

	// Биндим текстуры (Slot 3 для GeometryRootSignature)
	mCommandList->SetGraphicsRootDescriptorTable(3, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	// Отрисовка непрозрачной геометрии
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	// Переводим GBuffer обратно в GENERIC_READ для Light Pass
	for (int i = 0; i < 3; ++i)
		barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer->Resource(i),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
	mCommandList->ResourceBarrier(3, barriers);

	// ==========================================
	// 3. Lighting Pass
	// ==========================================

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr); // Без depth buffer!
	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::Black, 0, nullptr);

	mCommandList->SetGraphicsRootSignature(mLightingRootSignature.Get());
	mCommandList->SetPipelineState(mPSOs["LightingPass"].Get());

	// 0: PassCB
	mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

	// 1: Global Textures Table (CubeMap, ShadowMap)
	CD3DX12_GPU_DESCRIPTOR_HANDLE globalTexStart(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	globalTexStart.Offset(mResourceMgr->TexOffsets["skyCubeMap"], mCbvSrvUavDescriptorSize);
	mCommandList->SetGraphicsRootDescriptorTable(1, globalTexStart);

	// 2: GBuffer Table
	mCommandList->SetGraphicsRootDescriptorTable(2, mGBuffer->Srv());

	// Рисуем Full Screen Quad
	mCommandList->IASetVertexBuffers(0, 0, nullptr);
	mCommandList->IASetIndexBuffer(nullptr);
	mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	mCommandList->DrawInstanced(3, 1, 0, 0);

	// ==========================================
	// 4. Skybox Pass
	// ==========================================

	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);
	
	mCommandList->SetGraphicsRootSignature(mRootSignature.Get()); // Возвращаем Main Signature
	mCommandList->SetPipelineState(mPSOs["sky"].Get());

	// Биндинг PassCB (Slot 1)
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress()); 
	
	// !!! ИСПРАВЛЕНИЕ 3: Привязываем Material Buffer (Slot 2) и для Skybox !!!
	mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

	// Биндинг CubeMap (Slot 3)
	CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	skyTexDescriptor.Offset(mResourceMgr->TexOffsets["skyCubeMap"], mCbvSrvUavDescriptorSize);
	mCommandList->SetGraphicsRootDescriptorTable(3, skyTexDescriptor);
	
	// Биндинг Textures (Slot 4) - формально нужен для сигнатуры, даже если скайбокс не использует
	mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

	// ==========================================
	// 6. IMGUI Render
	// ==========================================

	ID3D12DescriptorHeap* imguiheaps[] = { m_ImGuiSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(imguiheaps), imguiheaps);
	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

	// ==========================================
	// 7. Final Presentation
	// ==========================================

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(mCommandList->Close());

	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	mCurrFrameResource->Fence = ++mCurrentFence;
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

/*
INPUT HANDLING
*/
void CGLAB::OnMouseDown(WPARAM btnState, int x, int y)
{
	mLastMousePos.x = x;
	mLastMousePos.y = y;

	SetCapture(mhMainWnd);
}

void CGLAB::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void CGLAB::OnMouseMove(WPARAM btnState, int x, int y)
{
	if (!ImGui::GetIO().WantCaptureMouse)
	{
		if ((btnState & MK_LBUTTON) != 0)
		{
			// Make each pixel correspond to a quarter of a degree.
			float dx = XMConvertToRadians(mConfig.CameraRotationSpeed * static_cast<float>(x - mLastMousePos.x));
			float dy = XMConvertToRadians(mConfig.CameraRotationSpeed * static_cast<float>(y - mLastMousePos.y));

			mCamera.Pitch(dy);
			mCamera.RotateY(dx);
		}

		mLastMousePos.x = x;
		mLastMousePos.y = y;
	}

}

void CGLAB::OnKeyboardInput(const GameTimer& gt)
{
	const float dt = gt.DeltaTime();

	if (GetAsyncKeyState('W') & 0x8000)
		mCamera.Walk(mConfig.CameraWalkSpeed * dt);

	if (GetAsyncKeyState('S') & 0x8000)
		mCamera.Walk(-mConfig.CameraWalkSpeed * dt);

	if (GetAsyncKeyState('A') & 0x8000)
		mCamera.Strafe(-mConfig.CameraWalkSpeed * dt);

	if (GetAsyncKeyState('D') & 0x8000)
		mCamera.Strafe(mConfig.CameraWalkSpeed * dt);

	mCamera.UpdateViewMatrix();
}

/*
UPDATE FUNCTIONS
*/
void CGLAB::AnimateMaterials(const GameTimer& gt)
{

}

void CGLAB::UpdateObjectCBs(const GameTimer& gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for (auto& e : mAllRitems)
	{
		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		if (e->NumFramesDirty > 0)
		{
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
			objConstants.MaterialIndex = e->Mat->MatCBIndex;

			currObjectCB->CopyData(e->ObjCBIndex, objConstants);

			// Next FrameResource need to be updated too.
			e->NumFramesDirty--;
		}
	}
}

void CGLAB::UpdateMaterialBuffer(const GameTimer& gt)
{
	auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
	for (auto& e : mResourceMgr->mMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MaterialData matData;
			matData.DiffuseAlbedo = mat->DiffuseAlbedo;
			matData.FresnelR0 = mat->FresnelR0;
			matData.Roughness = mat->Roughness;
			XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
			matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;
			matData.NormalMapIndex = mat->NormalSrvHeapIndex;

			currMaterialBuffer->CopyData(mat->MatCBIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void CGLAB::UpdateShadowTransform(const GameTimer& gt)
{
	// Only the first "main" light casts a shadow.
	XMVECTOR lightDir = XMLoadFloat3(&mMainPassCB.Lights[0].Direction);
	XMVECTOR lightPos = -2.0f * mSceneBounds.Radius * lightDir;
	XMVECTOR targetPos = XMLoadFloat3(&mSceneBounds.Center);
	XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

	XMStoreFloat3(&mLightPosW, lightPos);

	// Transform bounding sphere to light space.
	XMFLOAT3 sphereCenterLS;
	XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

	// Ortho frustum in light space encloses scene.
	float l = sphereCenterLS.x - mSceneBounds.Radius;
	float b = sphereCenterLS.y - mSceneBounds.Radius;
	float n = sphereCenterLS.z - mSceneBounds.Radius;
	float r = sphereCenterLS.x + mSceneBounds.Radius;
	float t = sphereCenterLS.y + mSceneBounds.Radius;
	float f = sphereCenterLS.z + mSceneBounds.Radius;

	mLightNearZ = n;
	mLightFarZ = f;
	XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	XMMATRIX S = lightView * lightProj * T;
	XMStoreFloat4x4(&mLightView, lightView);
	XMStoreFloat4x4(&mLightProj, lightProj);
	XMStoreFloat4x4(&mShadowTransform, S);
}

void CGLAB::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX proj = mCamera.GetProj();

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransform);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	XMStoreFloat4x4(&mMainPassCB.ShadowTransform, XMMatrixTranspose(shadowTransform));
	mMainPassCB.EyePosW = mCamera.GetPosition3f();
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();
	mMainPassCB.AmbientLight = { 0, 0, 0, 1.0f };

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void CGLAB::UpdateShadowPassCB(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mLightView);
	XMMATRIX proj = XMLoadFloat4x4(&mLightProj);

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	UINT w = mShadowMap->Width();
	UINT h = mShadowMap->Height();

	XMStoreFloat4x4(&mShadowPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mShadowPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mShadowPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mShadowPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mShadowPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mShadowPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	mShadowPassCB.EyePosW = mLightPosW;
	mShadowPassCB.RenderTargetSize = XMFLOAT2((float)w, (float)h);
	mShadowPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / w, 1.0f / h);
	mShadowPassCB.NearZ = mLightNearZ;
	mShadowPassCB.FarZ = mLightFarZ;

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(1, mShadowPassCB);
}

void CGLAB::ImguiUpdate()
{
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGui::Begin("Settings");
	if (ImGui::BeginTabBar("Light Settings"))
	{
		if (ImGui::BeginTabItem("Lights"))
		{
			ImGui::ColorEdit3("Directional Light Color", (float*)&mMainPassCB.Lights[0].Strength);
			ImGui::SliderFloat3("Directional Light Direction", (float*)&mMainPassCB.Lights[0].Direction, -1.0f, 1.0f);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();
}



void CGLAB::BuildDescriptorHeaps()
{
	//
	// Create the SRV heap.
	//
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = 50 + 3;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

	//
	// Fill out the heap with actual descriptors.
	//
	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());


	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	// srv for textures
	int mTextureCount = 0;
	for (const auto& tex : mResourceMgr->mTextures)
	{
		if (tex.first == "skyCubeMap")
			continue;
		else
		{
			auto text = tex.second->Resource;
			srvDesc.Format = text->GetDesc().Format;
			srvDesc.Texture2D.MipLevels = text->GetDesc().MipLevels;
			md3dDevice->CreateShaderResourceView(text.Get(), &srvDesc, hDescriptor);
			mResourceMgr->TexOffsets[tex.first] = mTextureCount;
			mTextureCount++;
			// next descriptor
			hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);
		}
	}

	if (mResourceMgr->mTextures.size() != 0)
	{
		auto skyCubeMap = mResourceMgr->mTextures["skyCubeMap"]->Resource;
		// srv for cube map
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		srvDesc.TextureCube.MostDetailedMip = 0;
		srvDesc.TextureCube.MipLevels = skyCubeMap->GetDesc().MipLevels;
		srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
		srvDesc.Format = skyCubeMap->GetDesc().Format;
		mResourceMgr->TexOffsets["skyCubeMap"] = mTextureCount;
		md3dDevice->CreateShaderResourceView(skyCubeMap.Get(), &srvDesc, hDescriptor);
	}

	mResourceMgr->TexOffsets["shadow"] = mTextureCount + 1;

	mResourceMgr->TexOffsets["nullCube"] = mTextureCount + 2;
	mResourceMgr->TexOffsets["nullTex"] = mTextureCount + 3;

	auto srvCpuStart = mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	auto srvGpuStart = mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
	auto dsvCpuStart = mDsvHeap->GetCPUDescriptorHandleForHeapStart();


	auto nullSrv = CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, mResourceMgr->TexOffsets["nullCube"], mCbvSrvUavDescriptorSize);
	mNullSrv = CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, mResourceMgr->TexOffsets["nullCube"], mCbvSrvUavDescriptorSize);

	md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);
	nullSrv.Offset(1, mCbvSrvUavDescriptorSize);

	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);

	mShadowMap->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, mResourceMgr->TexOffsets["shadow"], mCbvSrvUavDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, mResourceMgr->TexOffsets["shadow"], mCbvSrvUavDescriptorSize),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(dsvCpuStart, 1, mDsvDescriptorSize));
	gBufferSrvOffset = 50;

}

void CGLAB::BuildRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable0;
	texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);

	CD3DX12_DESCRIPTOR_RANGE texTable1;
	texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, -1, 2, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[5];

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsConstantBufferView(0);
	slotRootParameter[1].InitAsConstantBufferView(1);
	slotRootParameter[2].InitAsShaderResourceView(0, 1);
	slotRootParameter[3].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[4].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);


	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void CGLAB::BuildGeometryRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable1;
	texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, -1, 0, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[4];

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsConstantBufferView(0);
	slotRootParameter[1].InitAsConstantBufferView(1);
	slotRootParameter[2].InitAsShaderResourceView(0, 1);
	slotRootParameter[3].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);


	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mGeometryRootSignature.GetAddressOf())));
}

void CGLAB::BuildLightingRootSignature()
{
	// *** 1. ТАБЛИЦЫ ДЕСКРИПТОРОВ ***

	// Таблица 1: G-Buffer SRVs (t2, t3, t4)
	CD3DX12_DESCRIPTOR_RANGE gBufferTable;
	gBufferTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 2); // Binds t2, t3, t4

	// Таблица 2: Global Textures (CubeMap t0, ShadowMap t1)
	// Текстуры, которые не являются G-Buffer'ом, но нужны для Light Pass
	CD3DX12_DESCRIPTOR_RANGE globalTexTable[2];

	// t0: CubeMap
	globalTexTable[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	// t1: ShadowMap
	globalTexTable[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

	// *** 2. КОРНЕВЫЕ ПАРАМЕТРЫ ***

	CD3DX12_ROOT_PARAMETER slotRootParameter[3];

	// 0: CBV PassConstants (b1) - Прямой Root CBV
	slotRootParameter[0].InitAsConstantBufferView(0);

	// 1: Global Textures Table (t0, t1) - Таблица, содержащая CubeMap и ShadowMap
	slotRootParameter[1].InitAsDescriptorTable(2, globalTexTable);

	// 2: GBuffer Table (t3, t4, t5) - Таблица, содержащая G-Buffer
	slotRootParameter[2].InitAsDescriptorTable(1, &gBufferTable);

	// ... (Остальная часть кода, включая статические семплеры, остается прежней) ...

	// Обратите внимание: теперь у нас 3 Root Parameters, а не 4!
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter,
		(UINT)GetStaticSamplers().size(), GetStaticSamplers().data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mLightingRootSignature.GetAddressOf())));
}

void CGLAB::CreateRtvAndDsvDescriptorHeaps()
{
	// Add +6 RTV for cube render target.
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 3; // +3 for GBuffer RTVs
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

	// Add +1 DSV for shadow map.
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 2;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
}

void CGLAB::BuildShadersAndInputLayout()
{
	const D3D_SHADER_MACRO alphaTestDefines[] =
	{
		"ALPHA_TEST", "1",
		NULL, NULL
	};

	mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["shadowVS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["shadowOpaquePS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "PS", "ps_5_1");
	mShaders["shadowAlphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", alphaTestDefines, "PS", "ps_5_1");

	mShaders["debugVS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["debugPS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["skyVS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["skyPS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["GPassVS"] = d3dUtil::CompileShader(L"Shaders\\GeometryPass.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["GPassPS"] = d3dUtil::CompileShader(L"Shaders\\GeometryPass.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["DeferredLightVS"] = d3dUtil::CompileShader(L"Shaders\\LightingPass.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["DeferredLightPS"] = d3dUtil::CompileShader(L"Shaders\\LightingPass.hlsl", nullptr, "PS", "ps_5_1");


	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
}

void CGLAB::BuildPSOs()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;

	//
	// PSO for opaque objects.
	//
	ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	opaquePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	opaquePsoDesc.pRootSignature = mRootSignature.Get();
	opaquePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
		mShaders["standardVS"]->GetBufferSize()
	};
	opaquePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	opaquePsoDesc.SampleMask = UINT_MAX;
	opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	opaquePsoDesc.NumRenderTargets = 1;
	opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
	opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	opaquePsoDesc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

	//
	// PSO for shadow map pass.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC smapPsoDesc = opaquePsoDesc;
	smapPsoDesc.RasterizerState.DepthBias = 100000;
	smapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
	smapPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
	smapPsoDesc.pRootSignature = mRootSignature.Get();
	smapPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["shadowVS"]->GetBufferPointer()),
		mShaders["shadowVS"]->GetBufferSize()
	};
	smapPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["shadowOpaquePS"]->GetBufferPointer()),
		mShaders["shadowOpaquePS"]->GetBufferSize()
	};

	// Shadow map pass does not have a render target.
	smapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
	smapPsoDesc.NumRenderTargets = 0;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&smapPsoDesc, IID_PPV_ARGS(&mPSOs["shadow_opaque"])));

	//
	// PSO for debug layer.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc = opaquePsoDesc;
	debugPsoDesc.pRootSignature = mRootSignature.Get();
	debugPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["debugVS"]->GetBufferPointer()),
		mShaders["debugVS"]->GetBufferSize()
	};
	debugPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["debugPS"]->GetBufferPointer()),
		mShaders["debugPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&debugPsoDesc, IID_PPV_ARGS(&mPSOs["debug"])));

	//
	// PSO for sky.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = opaquePsoDesc;

	// The camera is inside the sky sphere, so just turn off culling.
	skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	// Make sure the depth function is LESS_EQUAL and not just LESS.  
	// Otherwise, the normalized depth values at z = 1 (NDC) will 
	// fail the depth test if the depth buffer was cleared to 1.
	skyPsoDesc.DepthStencilState.DepthEnable = true; // Включен!
	skyPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	skyPsoDesc.pRootSignature = mRootSignature.Get();
	skyPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["skyVS"]->GetBufferPointer()),
		mShaders["skyVS"]->GetBufferSize()
	};
	skyPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["skyPS"]->GetBufferPointer()),
		mShaders["skyPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["sky"])));

	//
	// PSO for G-Buffer pass.
	//

	DXGI_FORMAT GBUFFER_FORMATS[3] = {
		DXGI_FORMAT_R8G8B8A8_UNORM,      // RT0: Albedo + Roughness
		DXGI_FORMAT_R16G16B16A16_FLOAT,  // RT1: Normal + Fresnel
		DXGI_FORMAT_R32G32B32A32_FLOAT   // RT2: Position
	};

	//==========================================================
	// PSO 1: G-Pass (Geometry Pass)
	//==========================================================
	D3D12_GRAPHICS_PIPELINE_STATE_DESC gPassPsoDesc = opaquePsoDesc;

	// 1. Shaders
	gPassPsoDesc.VS = { reinterpret_cast<BYTE*>(mShaders["GPassVS"]->GetBufferPointer()), mShaders["GPassVS"]->GetBufferSize() };
	gPassPsoDesc.PS = { reinterpret_cast<BYTE*>(mShaders["GPassPS"]->GetBufferPointer()), mShaders["GPassPS"]->GetBufferSize() };

	// 2. Render Targets
	gPassPsoDesc.NumRenderTargets = 3; // Мы пишем в 3 G-Buffer'а
	gPassPsoDesc.RTVFormats[0] = GBUFFER_FORMATS[0];
	gPassPsoDesc.RTVFormats[1] = GBUFFER_FORMATS[1];
	gPassPsoDesc.RTVFormats[2] = GBUFFER_FORMATS[2];
	gPassPsoDesc.RTVFormats[3] = DXGI_FORMAT_UNKNOWN; // Остальные не используются

	// 3. Depth-Stencil State
	// Глубину нужно писать, чтобы отсекать скрытую геометрию
	gPassPsoDesc.DepthStencilState.DepthEnable = true;
	gPassPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

	// 4. Root Signature (используем старую, так как она содержит cbPerObject, gMaterialData и gTextureMaps)
	gPassPsoDesc.pRootSignature = mGeometryRootSignature.Get();

	// Создаем PSO для G-Pass
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&gPassPsoDesc, IID_PPV_ARGS(&mPSOs["GeometryPass"])));


	//==========================================================
	// PSO 2: Light Pass (Deferred Shading Pass)
	//==========================================================
	D3D12_GRAPHICS_PIPELINE_STATE_DESC lightPassPsoDesc = opaquePsoDesc;

	// 1. Shaders
	lightPassPsoDesc.VS = { reinterpret_cast<BYTE*>(mShaders["DeferredLightVS"]->GetBufferPointer()), mShaders["DeferredLightVS"]->GetBufferSize() };
	lightPassPsoDesc.PS = { reinterpret_cast<BYTE*>(mShaders["DeferredLightPS"]->GetBufferPointer()), mShaders["DeferredLightPS"]->GetBufferSize() };

	// 2. Input Layout
	lightPassPsoDesc.InputLayout = { nullptr, 0 }; // <--- ВАЖНО: Вершины генерируются в VS по SV_VertexID, Input Layout не нужен.

	// 3. Render Targets
	lightPassPsoDesc.NumRenderTargets = 1;
	lightPassPsoDesc.RTVFormats[0] = mBackBufferFormat; // Пишем в Back Buffer

	// 4. Depth-Stencil State
	// В Light Pass мы просто рисуем полноэкранный квадрат. Глубину ни читать, ни писать не нужно.
	lightPassPsoDesc.DepthStencilState.DepthEnable = false;
	lightPassPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

	// 5. Blending State (обычно для Light Pass используем аддитивный блендинг, но для первого прохода оставляем Opaque)
	// Поскольку мы очищаем буфер до черного, используем стандартный Opaque (D3D12_BLEND_DESC по умолчанию)

	// 6. Primitive Topology
	lightPassPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	// 7. Root Signature (используем новую сигнатуру Light Pass, чтобы забиндить G-Buffer как текстуры)
	lightPassPsoDesc.pRootSignature = mLightingRootSignature.Get();

	// Создаем PSO для Light Pass
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&lightPassPsoDesc, IID_PPV_ARGS(&mPSOs["LightingPass"])));
}

void CGLAB::BuildFrameResources()
{
	for (int i = 0; i < gNumFrameResources; ++i)
	{
		mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
			2, (UINT)mAllRitems.size(), (UINT)mResourceMgr->mMaterials.size()));
	}
}

void CGLAB::CreateRenderItem(std::string name, std::string materialname, int RItemLayer, XMMATRIX& scaling, XMMATRIX& rotation, XMMATRIX& translation, XMMATRIX texTransform, std::string drawargs)
{
	if (drawargs == "")
	{
		for (const auto& drawArg : mGeomMgr->mGeometries[name]->DrawArgs)
		{
			auto ritem_child = std::make_unique<RenderItem>();
			XMStoreFloat4x4(&ritem_child->World, scaling * rotation * translation);
			XMStoreFloat4x4(&ritem_child->TexTransform, texTransform);
			ritem_child->name = name + "_" + drawArg.first;
			ritem_child->ObjCBIndex = mAllRitems.size();
			ritem_child->Mat = mResourceMgr->mMaterials[materialname].get();
			ritem_child->Geo = mGeomMgr->mGeometries[name].get();
			ritem_child->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			ritem_child->IndexCount = ritem_child->Geo->DrawArgs[drawargs].IndexCount;
			ritem_child->StartIndexLocation = ritem_child->Geo->DrawArgs[drawargs].StartIndexLocation;
			ritem_child->BaseVertexLocation = ritem_child->Geo->DrawArgs[drawargs].BaseVertexLocation;
			mRitemLayer[RItemLayer].push_back(ritem_child.get());
			mAllRitems.push_back(std::move(ritem_child));
		}
	}
	else
	{
		auto ritem = std::make_unique<RenderItem>();
		XMStoreFloat4x4(&ritem->World, scaling * rotation * translation);
		XMStoreFloat4x4(&ritem->TexTransform, texTransform);
		ritem->ObjCBIndex = mAllRitems.size();
		ritem->Mat = mResourceMgr->mMaterials[materialname].get();
		ritem->Geo = mGeomMgr->mGeometries[name].get();
		ritem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		ritem->IndexCount = ritem->Geo->DrawArgs[drawargs].IndexCount;
		ritem->StartIndexLocation = ritem->Geo->DrawArgs[drawargs].StartIndexLocation;
		ritem->BaseVertexLocation = ritem->Geo->DrawArgs[drawargs].BaseVertexLocation;
		mRitemLayer[RItemLayer].push_back(ritem.get());
		mAllRitems.push_back(std::move(ritem));
	}
}

void CGLAB::BuildRenderItems()
{
	CreateRenderItem("shapeGeo", "sky", (int)RenderLayer::Sky,
		XMMatrixScaling(5000.0f, 5000.0f, 5000.0f),
		XMMatrixIdentity(),
		XMMatrixIdentity(),
		XMMatrixIdentity(),
		"sphere");

	CreateRenderItem("skullGeo", "skullMat", (int)RenderLayer::Opaque,
		XMMatrixScaling(2.0f, 2.0f, 2.0f),
		XMMatrixIdentity(),
		XMMatrixTranslation(0.0f, 3.0f, 0.0f),
		XMMatrixScaling(1.0f, 1.0f, 1.0f),
		"Group5732");

	CreateRenderItem("shapeGeo", "bricks0", (int)RenderLayer::Debug,
		XMMatrixScaling(1.0f, 1.0f, 1.0f),
		XMMatrixIdentity(),
		XMMatrixIdentity(),
		XMMatrixIdentity(),
		"quad");

	CreateRenderItem("shapeGeo", "bricks0", (int)RenderLayer::Opaque,
		XMMatrixScaling(2.0f, 1.0f, 2.0f),
		XMMatrixIdentity(),
		XMMatrixTranslation(0.0f, 0.5f, 0.0f),
		XMMatrixScaling(1.0f, 0.5f, 1.0f),
		"box");

	CreateRenderItem("shapeGeo", "tile0", (int)RenderLayer::Opaque,
		XMMatrixScaling(1.0f, 1.0f, 1.0f),
		XMMatrixIdentity(),
		XMMatrixIdentity(),
		XMMatrixScaling(8.0f, 8.0f, 1.0f),
		"grid");

	XMMATRIX scaleIdentity = XMMatrixScaling(1.0f, 1.0f, 1.0f);
	XMMATRIX rotIdentity = XMMatrixIdentity();
	XMMATRIX texIdentity = XMMatrixIdentity();
	XMMATRIX brickTexTransform = XMMatrixScaling(1.5f, 2.0f, 1.0f);
	int layer = (int)RenderLayer::Opaque;
	std::string geoName = "shapeGeo";

	for (int i = 0; i < 5; ++i)
	{
		XMMATRIX leftCylTranslation = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f);
		XMMATRIX rightCylTranslation = XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f);
		XMMATRIX leftSphereTranslation = XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f);
		XMMATRIX rightSphereTranslation = XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f);

		// 1. Left Cylinder (bricks0)
		CreateRenderItem(geoName, "bricks0", layer,
			scaleIdentity,
			rotIdentity,
			leftCylTranslation,
			brickTexTransform,
			"cylinder");

		// 2. Right Cylinder (bricks0)
		CreateRenderItem(geoName, "bricks0", layer,
			scaleIdentity,
			rotIdentity,
			rightCylTranslation,
			brickTexTransform,
			"cylinder");

		// 3. Left Sphere (mirror0)
		CreateRenderItem(geoName, "mirror0", layer,
			scaleIdentity,
			rotIdentity,
			leftSphereTranslation,
			texIdentity,
			"sphere");

		// 4. Right Sphere (mirror0)
		CreateRenderItem(geoName, "mirror0", layer,
			scaleIdentity,
			rotIdentity,
			rightSphereTranslation,
			texIdentity,
			"sphere");
	}
}

void CGLAB::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

	auto objectCB = mCurrFrameResource->ObjectCB->Resource();

	// For each render item...
	for (size_t i = 0; i < ritems.size(); ++i)
	{
		auto ri = ritems[i];

		cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;

		cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

		cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
}

void CGLAB::DrawSceneToShadowMap()
{
	mCommandList->RSSetViewports(1, &mShadowMap->Viewport());
	mCommandList->RSSetScissorRects(1, &mShadowMap->ScissorRect());

	// Change to DEPTH_WRITE.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

	// Clear the back buffer and depth buffer.
	mCommandList->ClearDepthStencilView(mShadowMap->Dsv(),
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Set null render target because we are only going to draw to
	// depth buffer.  Setting a null render target will disable color writes.
	// Note the active PSO also must specify a render target count of 0.
	mCommandList->OMSetRenderTargets(0, nullptr, false, &mShadowMap->Dsv());

	// Bind the pass constant buffer for the shadow map pass.
	auto passCB = mCurrFrameResource->PassCB->Resource();
	D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + 1 * passCBByteSize;
	mCommandList->SetGraphicsRootConstantBufferView(1, passCBAddress);

	mCommandList->SetPipelineState(mPSOs["shadow_opaque"].Get());

	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	// Change back to GENERIC_READ so we can read the texture in a shader.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void CGLAB::ImguiInit()
{
	D3D12_DESCRIPTOR_HEAP_DESC imGuiHeapDesc = {};
	imGuiHeapDesc.NumDescriptors = 1;
	imGuiHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	imGuiHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	imGuiHeapDesc.NodeMask = 0; // Or the appropriate node mask if you have multiple GPUs
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&imGuiHeapDesc, IID_PPV_ARGS(&m_ImGuiSrvDescriptorHeap)));

	// INITIALIZE IMGUI ////////////////////
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	////////////////////////////////////////
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	ImGui_ImplDX12_InitInfo init_info = {};
	init_info.Device = md3dDevice.Get();
	init_info.CommandQueue = mCommandQueue.Get();
	init_info.NumFramesInFlight = gNumFrameResources;
	init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // Or your render target format.
	init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
	init_info.SrvDescriptorHeap = m_ImGuiSrvDescriptorHeap.Get();
	init_info.LegacySingleSrvCpuDescriptor = m_ImGuiSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	init_info.LegacySingleSrvGpuDescriptor = m_ImGuiSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
	ImGui_ImplWin32_Init(mhMainWnd);
	ImGui_ImplDX12_Init(&init_info);
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> CGLAB::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers.  So just define them all up front
	// and keep them available as part of the root signature.  

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC shadow(
		6, // shaderRegister
		D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
		0.0f,                               // mipLODBias
		16,                                 // maxAnisotropy
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp,
		shadow
	};
}

void CGLAB::BuildGBuffer()
{
	mGBuffer = std::make_unique<GBuffer>(md3dDevice.Get(), mClientWidth, mClientHeight);

	CD3DX12_CPU_DESCRIPTOR_HANDLE srvHeapHandle(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	srvHeapHandle.Offset(gBufferSrvOffset, mCbvSrvUavDescriptorSize);

	// 2. Получаем GPU хендл (для биндинга в шейдер) и ТОЖЕ оборачиваем в CD3DX12
	CD3DX12_GPU_DESCRIPTOR_HANDLE gpuSrvHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	gpuSrvHandle.Offset(gBufferSrvOffset, mCbvSrvUavDescriptorSize); // Теперь Offset работает!

	// 3. RTV хендл тоже оборачиваем
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	rtvHeapHandle.Offset(SwapChainBufferCount, mRtvDescriptorSize);

	// 4. Вызываем функцию (она теперь примет аргументы, так как типы совпадают)
	mGBuffer->BuildDescriptors(
		srvHeapHandle,
		gpuSrvHandle,
		rtvHeapHandle,
		mCbvSrvUavDescriptorSize,
		mRtvDescriptorSize
	);
}
