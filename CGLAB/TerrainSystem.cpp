#include "TerrainSystem.h"
#include "CGLAB.h"
// Реализация методов terrain системы

// 1. Реализация AABB::IntersectsFrustum
bool AABB::IntersectsFrustum(const XMFLOAT4 frustumPlanes[6]) const
{
    for (int i = 0; i < 6; i++)
    {
        XMVECTOR plane = XMLoadFloat4(&frustumPlanes[i]);

        // Найдем positive vertex для плоскости
        XMFLOAT3 positiveVertex;
        positiveVertex.x = (XMVectorGetX(plane) >= 0) ? maxPoint.x : minPoint.x;
        positiveVertex.y = (XMVectorGetY(plane) >= 0) ? maxPoint.y : minPoint.y;
        positiveVertex.z = (XMVectorGetZ(plane) >= 0) ? maxPoint.z : minPoint.z;

        XMVECTOR posVertex = XMLoadFloat3(&positiveVertex);

        // Если positive vertex за плоскостью, то AABB полностью вне frustum
        if (XMVectorGetX(XMPlaneDotCoord(plane, posVertex)) < 0)
        {
            return false;
        }
    }
    return true;
}

// 2. Извлечение плоскостей frustum из view-projection матрицы
void CGLAB::ExtractFrustumPlanes(const XMMATRIX& viewProj)
{
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, viewProj);

    // Left plane
    m_frustumPlanes[0].x = vp._14 + vp._11;
    m_frustumPlanes[0].y = vp._24 + vp._21;
    m_frustumPlanes[0].z = vp._34 + vp._31;
    m_frustumPlanes[0].w = vp._44 + vp._41;

    // Right plane
    m_frustumPlanes[1].x = vp._14 - vp._11;
    m_frustumPlanes[1].y = vp._24 - vp._21;
    m_frustumPlanes[1].z = vp._34 - vp._31;
    m_frustumPlanes[1].w = vp._44 - vp._41;

    // Top plane
    m_frustumPlanes[2].x = vp._14 - vp._12;
    m_frustumPlanes[2].y = vp._24 - vp._22;
    m_frustumPlanes[2].z = vp._34 - vp._32;
    m_frustumPlanes[2].w = vp._44 - vp._42;

    // Bottom plane
    m_frustumPlanes[3].x = vp._14 + vp._12;
    m_frustumPlanes[3].y = vp._24 + vp._22;
    m_frustumPlanes[3].z = vp._34 + vp._32;
    m_frustumPlanes[3].w = vp._44 + vp._42;

    // Near plane
    m_frustumPlanes[4].x = vp._13;
    m_frustumPlanes[4].y = vp._23;
    m_frustumPlanes[4].z = vp._33;
    m_frustumPlanes[4].w = vp._43;

    // Far plane
    m_frustumPlanes[5].x = vp._14 - vp._13;
    m_frustumPlanes[5].y = vp._24 - vp._23;
    m_frustumPlanes[5].z = vp._34 - vp._33;
    m_frustumPlanes[5].w = vp._44 - vp._43;

    // Нормализуем плоскости
    for (int i = 0; i < 6; i++)
    {
        XMVECTOR plane = XMLoadFloat4(&m_frustumPlanes[i]);
        plane = XMPlaneNormalize(plane);
        XMStoreFloat4(&m_frustumPlanes[i], plane);
    }
}

// 3. Логика определения LOD
bool QuadTreeNode::ShouldSplit(const XMFLOAT3& cameraPos, float maxScreenError,int nodeLODlevel) const
{
    // Вычисляем расстояние от камеры до центра тайла
    XMFLOAT3 center;
    center.x = (boundingBox.minPoint.x + boundingBox.maxPoint.x) * 0.5f;
    center.y = (boundingBox.minPoint.y + boundingBox.maxPoint.y) * 0.5f;
    center.z = (boundingBox.minPoint.z + boundingBox.maxPoint.z) * 0.5f;

    XMVECTOR camPos = XMLoadFloat3(&cameraPos);
    XMVECTOR centerPos = XMLoadFloat3(&center);

    float distance = XMVectorGetX(XMVector3Length(XMVectorSubtract(camPos, centerPos)));

    //// Размер тайла в мировых единицах
    float tileSize = boundingBox.maxPoint.x - boundingBox.minPoint.x;

    //// Эвристика: разделяем если тайл занимает больше определенного количества пикселей на экране
    //// Это упрощенная версия screen-space error calculation
    float screenSpaceSize = (tileSize / (distance + 1.0f)) * 250.0f; // упрощенная формула



    return screenSpaceSize > maxScreenError && depth < 4; // максимум 6 уровней
}

// 4. Обновление видимости в квадродереве
void QuadTreeNode::UpdateVisibility(const XMFLOAT4 frustumPlanes[6], const XMFLOAT3& cameraPos, std::vector<TerrainTile*>& visibleTiles)
{
    // Проверка на видимость по AABB
    if (!boundingBox.IntersectsFrustum(frustumPlanes))
    {
        return; // Узел полностью не виден, нет смысла идти дальше.
    }

    // Если узел является "листом" (нет дочерних узлов) или не нужно его разбивать
    if (!children[0] || !ShouldSplit(cameraPos, 100.0f,depth))
    {
        // Рендерим текущий узел (тайл)
        if (tile)
        {
            visibleTiles.push_back(tile);
        }
    }
    else // Если нужно разбивать
    {
        // Рекурсивно обновляем видимость дочерних узлов
        for (int i = 0; i < 4; i++)
        {
            if (children[i])
            {
                children[i]->UpdateVisibility(frustumPlanes, cameraPos, visibleTiles);
            }
        }
    }
}

// 5. Инициализация terrain системы
void TerrainSystem::Initialize(ID3D12Device* device, const std::wstring& heightmapPath,
    float worldSize, int maxLOD)
{
    m_worldSize = worldSize;
    m_maxLOD = maxLOD;
    m_heightScale = 50.0f; // настраиваемый параметр

    // Загрузка heightmap (упрощенная версия - нужно адаптировать под ваш код)
    // ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(device, cmdList, 
    //     heightmapPath.c_str(), m_heightmapTexture, uploadHeap));

    // Создаем корневой узел квадродерева
    m_rootNode = std::make_unique<QuadTreeNode>();
    m_rootNode->depth = 0;
    m_rootNode->boundingBox.minPoint = XMFLOAT3(-512 * 0.5f, -10.0f, -512 * 0.5f);
    m_rootNode->boundingBox.maxPoint = XMFLOAT3(512 * 0.5f, 40.0f, 512 * 0.5f);

    // Строим квадродерево рекурсивно
    int initialSize = worldSize; // 2^maxLOD
    BuildQuadTree(m_rootNode.get(), 0, 0, initialSize, 0);
}

void TerrainSystem::BuildQuadTree(QuadTreeNode* node, int x, int y, int size, int depth)
{
    node->depth = depth;
    // Вычисляем мировые координаты узла
    float tileSize = m_worldSize / (1 << depth);

    // Вычисляем AABB для этого узла.
    // Пока что упрощенно, без учета хайтмапы
    node->boundingBox = CalculateTileAABB(XMFLOAT3(x, 0, y), tileSize, -10.0f, 40.0f);
   
    auto tile = std::make_unique<TerrainTile>();
    tile->worldPos = XMFLOAT3(x, 0, y);
    tile->lodLevel = depth;
    tile->tileSize = tileSize;
    tile->isVisible = true;
    tile->boundingBox = node->boundingBox;
    tile->tileIndex = tileIndex++;
    m_allTiles.push_back(std::move(tile));
    node->tile = m_allTiles.back().get(); // Указываем на созданный тайл
    // Если мы достигли максимальной глубины, создаем тайл.
    if (depth != m_maxLOD)
    {
        int halfSize = size / 2;
        for (int i = 0; i < 4; i++)
        {
            node->children[i] = std::make_unique<QuadTreeNode>();
            int childX = x + (i % 2) * halfSize/32;
            int childY = y + (i / 2) * halfSize/32;
            BuildQuadTree(node->children[i].get(), childX, childY, halfSize, depth + 1);
        }
    }
}
AABB TerrainSystem::CalculateTileAABB(const XMFLOAT3& pos, float size, float minHeight, float maxHeight)
{
    AABB aabb;
    aabb.minPoint = XMFLOAT3(pos.x, minHeight, pos.z);
    aabb.maxPoint = XMFLOAT3(pos.x + size, maxHeight, pos.z + size);
    return aabb;
}

void TerrainSystem::Update(const XMFLOAT3& cameraPos, const XMFLOAT4 frustumPlanes[6])
{
    m_visibleTiles.clear();
    if (m_rootNode)
    {
        m_rootNode->UpdateVisibility(frustumPlanes, cameraPos, m_visibleTiles);
    }
}

std::vector<std::shared_ptr<TerrainTile>>& TerrainSystem::GetAllTiles()
{
    return m_allTiles;
}

void TerrainSystem::GetVisibleTiles(std::vector<TerrainTile*>& outTiles)
{
    outTiles = m_visibleTiles;
}
void CGLAB::GenerateTileGeometry(const XMFLOAT3& worldPos, float tileSize, int lodLevel,
    std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices)
{
    int baseResolution = 4;
    float Factor = 2; 
    int resolution = static_cast<int>(baseResolution * std::pow(Factor, lodLevel));
  // std::cout << "LOD " << lodLevel << "RES: " << resolution << "\n" : std::cout;
    vertices.clear();
    indices.clear();

    float stepSize = tileSize / (resolution - 1);

    // Генерируем вершины
    for (int z = 0; z < resolution; z++)
    {
        for (int x = 0; x < resolution; x++)
        {
            Vertex vertex;
            vertex.Pos = XMFLOAT3(worldPos.x + x * stepSize, 0.0f, worldPos.z + z * stepSize);
            vertex.TexC = XMFLOAT2((float)x / (resolution - 1), (float)z / (resolution - 1));
            vertex.Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
            vertex.Tangent = XMFLOAT3(1.0f, 0.0f, 0.0f);
            vertices.push_back(vertex);
        }
    }

    // Генерируем индексы
    for (int z = 0; z < resolution - 1; z++)
    {
        for (int x = 0; x < resolution - 1; x++)
        {
            UINT topLeft = z * resolution + x;
            UINT topRight = topLeft + 1;
            UINT bottomLeft = (z + 1) * resolution + x;
            UINT bottomRight = bottomLeft + 1;

            // Первый треугольник
            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);

            // Второй треугольник
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }
}

void CGLAB::BuildTerrainGeometry()
{
    auto terrainGeo = std::make_unique<MeshGeometry>();
    terrainGeo->Name = "terrainGeo";

    // Получаем тайлы из terrain системы
    auto& allTiles = m_terrainSystem->GetAllTiles();

    // Создаем один большой массив вершин для всех тайлов
    std::vector<Vertex> allVertices;
    std::vector<std::uint32_t> allIndices;

    for (int tileIdx = 0; tileIdx < allTiles.size(); tileIdx++)
    {
        auto& tile = allTiles[tileIdx];

        std::vector<Vertex> tileVertices;
        std::vector<std::uint32_t> tileIndices;

        GenerateTileGeometry(tile->worldPos, tile->tileSize/32, tile->lodLevel, tileVertices, tileIndices);

        // Смещаем индексы на количество уже добавленных вершин
        UINT baseVertex = allVertices.size();
        for (auto& index : tileIndices)
        {
            index += baseVertex;
        }

        // Сохраняем submesh
        SubmeshGeometry submesh;
        submesh.IndexCount = tileIndices.size();
        submesh.StartIndexLocation = allIndices.size();
        submesh.BaseVertexLocation = 0;

        std::string submeshName = "tile_" + std::to_string(tileIdx) + "_LOD_" + std::to_string(tile->lodLevel);
        terrainGeo->DrawArgs[submeshName] = submesh;

        // Добавляем в общие массивы
        allVertices.insert(allVertices.end(), tileVertices.begin(), tileVertices.end());
        allIndices.insert(allIndices.end(), tileIndices.begin(), tileIndices.end());
        
    }

    // 3. Создание GPU-буферов (остаётся как было)
    const UINT vbByteSize = (UINT)allVertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)allIndices.size() * sizeof(std::uint32_t);

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &terrainGeo->VertexBufferCPU));
    CopyMemory(terrainGeo->VertexBufferCPU->GetBufferPointer(), allVertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &terrainGeo->IndexBufferCPU));
    CopyMemory(terrainGeo->IndexBufferCPU->GetBufferPointer(), allIndices.data(), ibByteSize);

    terrainGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        allVertices.data(), vbByteSize,
        terrainGeo->VertexBufferUploader);

    terrainGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        allIndices.data(), ibByteSize,
        terrainGeo->IndexBufferUploader);

    terrainGeo->VertexByteStride = sizeof(Vertex);
    terrainGeo->VertexBufferByteSize = vbByteSize;
    terrainGeo->IndexFormat = DXGI_FORMAT_R32_UINT;
    terrainGeo->IndexBufferByteSize = ibByteSize;

    mGeometries[terrainGeo->Name] = std::move(terrainGeo);
}



void CGLAB::UpdateTerrain(const GameTimer& gt)
{
    if (!m_terrainSystem)
        return;

    // Обновляем позицию камеры
    XMVECTOR camPos = cam.GetPosition();
    XMFLOAT3 cameraPosition;
    XMStoreFloat3(&cameraPosition, camPos);

    // Извлекаем плоскости frustum
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    ExtractFrustumPlanes(viewProj);

    // Обновляем terrain систему. Это заполняет m_visibleTiles
    m_terrainSystem->Update(cameraPosition, m_frustumPlanes);

    // Получаем список видимых тайлов, которые определило квадродерево.
  
    for (auto& t : m_terrainSystem->GetAllTiles())
    {
        m_visibleTerrainTiles.push_back(t.get());
    }

    // Здесь можно передать m_visibleTerrainRenderItems в ваш рендер-лист.
}

