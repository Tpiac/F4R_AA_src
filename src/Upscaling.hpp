#pragma once

#include "Common.hpp"

#if F4R_HAS_FSR3
	#include "FidelityFX.hpp"
#endif

namespace F4R_Upscaling
{
	class Upscaling
	{
	public:
		[[nodiscard]] static Upscaling& GetSingleton();

		Upscaling(const Upscaling&) = delete;
		Upscaling(Upscaling&&) = delete;
		Upscaling& operator=(const Upscaling&) = delete;
		Upscaling& operator=(Upscaling&&) = delete;
		~Upscaling();

		void Init();
		void LoadSettings(const std::string& a_iniPath);

		void InstallHooks();

		void Update();
		void Apply();

		void OverrideSamplerStates();
		void ResetSamplerStates();

		void UpdateGameSettings();
		void CheckResources();
        void RequestReset();
		void BuildFlareDepth(RE::BSGraphics::RenderTargetManager& a_rtMgr);
		void PushFlareDepth();
		void PopFlareDepth();

		Settings settings;

		bool upsclEnabled = false;
		bool resetHistory = false;

		std::unique_ptr<Texture2D> workingTexture;
		std::unique_ptr<Texture2D> motionVectorTexture;
		std::unique_ptr<Texture2D> tempTexture;
		ID3D11ComputeShader* mvFixShader = nullptr;
		ID3D11Buffer* mvFixCB = nullptr;
		ID3D11ComputeShader* rcasShader = nullptr;
		ID3D11Buffer* rcasCB = nullptr;

		std::unique_ptr<SharedTexture2D> xessColorTexture;
		std::unique_ptr<SharedTexture2D> xessMotionVectorTexture;
		std::unique_ptr<SharedTexture2D> xessDepthTexture;
		std::unique_ptr<SharedTexture2D> xessOutputTexture;
		ID3D11ComputeShader* depthCopyShader = nullptr;

		std::unique_ptr<Texture2D> flareDepthTexture;
		ID3D11ComputeShader* flareDepthShader = nullptr;
		ID3D11Buffer* flareDepthCB = nullptr;
		ID3D11ShaderResourceView* flareDepthBackup = nullptr;

		ID3D11SamplerState* biasedSamplerStates[320]{};
		ID3D11SamplerState* originalSamplerStates[320]{};

		float jitterX = 0.0f;
		float jitterY = 0.0f;

		float savedWidthRatio = 1.0f;
		float savedHeightRatio = 1.0f;
		float currentScale = 1.0f;

#if F4R_HAS_FSR3
		std::unique_ptr<FidelityFX> fidelityFX;
#endif

private:
		Upscaling() = default;

		bool resourcesCreated = false;
		uint32_t cachedWidth = 0;
		uint32_t cachedHeight = 0;
		DXGI_FORMAT cachedFormat = DXGI_FORMAT_UNKNOWN;
		int32_t cachedMethod = -1;
		float cachedSharpness = -1.0f;
		int32_t cachedQuality = -1;

		int startupFrameGuard = 0;
	};

	void InstallContextHooks();
}