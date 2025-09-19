#pragma once
#include "RenderItem.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;





struct AABB
{
	XMFLOAT3 minPoint;
	XMFLOAT3 maxPoint;

	bool IntersectsFrustum(const XMFLOAT4 frustumPlanes[6]) const;
};
struct TerrainTile
{
	XMFLOAT3 worldPos;          // Позиция тайла в мире
	int lodLevel;               // Уровень детализации (0 - самый детальный)
	float tileSize;             // Размер тайла в мировых единицах
	AABB boundingBox;           // Ограничивающий параллелепипед
	bool isVisible = true;             // Видимость после culling
	int tileIndex;
	int renderItemIndex; // Render item для тайла
	int NumFramesDirty;
};


struct QuadTreeNode
{
	TerrainTile* tile;          
	std::unique_ptr<QuadTreeNode> children[4]; // Дочерние узлы
	AABB boundingBox;           // AABB всего поддерева
	int depth;                  // Глубина в дереве

	// Методы
	bool ShouldSplit(const XMFLOAT3& cameraPos, float maxScreenError,int nodeLODlevel) const;
	void UpdateVisibility(const XMFLOAT4 frustumPlanes[6], const XMFLOAT3& cameraPos, std::vector<TerrainTile*>& visibleTiles);
};

class TerrainSystem
{
public:
	TerrainSystem() {};

	void Initialize(ID3D12Device* device, const std::wstring& heightmapPath,
		float worldSize, int maxLOD);
	void Update(const XMFLOAT3& cameraPos, const XMFLOAT4 frustumPlanes[6]);
	std::vector<std::shared_ptr<TerrainTile>>& GetAllTiles();
	void GetVisibleTiles(std::vector<TerrainTile*>& outTiles);
	float m_worldSize;

private:
	std::unique_ptr<QuadTreeNode> m_rootNode;
	ComPtr<ID3D12Resource> m_heightmapTexture;
	std::vector<std::shared_ptr<TerrainTile>>m_allTiles;
	std::vector<TerrainTile*> m_visibleTiles;
	int m_maxLOD;
	float m_heightScale;
	int tileIndex = 0;
	void BuildQuadTree(QuadTreeNode* node, int x, int y, int size, int depth);
	AABB CalculateTileAABB(const XMFLOAT3& pos, float size, float minHeight, float maxHeight);
};
