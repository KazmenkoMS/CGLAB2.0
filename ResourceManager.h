#pragma once
#include <string>
#include <d3dUtil.h>
class ResourceManager {
public:
	static ResourceManager& GetInstance() {
		static ResourceManager instance;
		return instance;
	}

	void Initialize(const std::wstring& basePath) {
		mBasePath = basePath;
		if (!mBasePath.empty() && mBasePath.back() != L'\\') {
			mBasePath += L"\\";
		}
	}

	std::wstring GetModelPath(const std::string& filename) const {
		return mBasePath + L"Models\\" + AnsiToWString(filename);
	}

	std::wstring GetTexturePath(const std::string& filename) const {
		return mBasePath + L"Textures\\" + AnsiToWString(filename);
	}

	std::wstring GetShaderPath(const std::string& filename) const {
		return mBasePath + L"Shaders\\" + AnsiToWString(filename);
	}

	bool FileExists(const std::wstring& path) const {
		std::ifstream file(path);
		return file.good();
	}

private:
	std::wstring mBasePath = L"";
};
