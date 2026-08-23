#pragma once

#include "Common.hpp"

#if F4R_HAS_FSR3
	#include "FidelityFX.hpp"
#endif

namespace F4R_AA
{
	class AntiAliasing
	{
	public:
		[[nodiscard]] static AntiAliasing& GetSingleton();

		AntiAliasing(const AntiAliasing&) = delete;
		AntiAliasing(AntiAliasing&&) = delete;
		AntiAliasing& operator=(const AntiAliasing&) = delete;
		AntiAliasing& operator=(AntiAliasing&&) = delete;
		~AntiAliasing();

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

		Settings settings;

		bool aaEnabled = false;
		bool resetHistory = false;

		std::unique_ptr<Texture2D> workingTexture;
		std::unique_ptr<Texture2D> motionVectorTexture;
		std::unique_ptr<Texture2D> sharpenTexture;
		ID3D11ComputeShader* mvFixShader = nullptr;
		ID3D11Buffer* mvFixCB = nullptr;
		ID3D11ComputeShader* rcasShader = nullptr;
		ID3D11Buffer* rcasCB = nullptr;

		std::unique_ptr<SharedTexture2D> xessColorTexture;
		std::unique_ptr<SharedTexture2D> xessMotionVectorTexture;
		std::unique_ptr<SharedTexture2D> xessDepthTexture;
		std::unique_ptr<SharedTexture2D> xessOutputTexture;
		ID3D11ComputeShader* depthCopyShader = nullptr;

		ID3D11SamplerState* biasedSamplerStates[320]{};
		ID3D11SamplerState* originalSamplerStates[320]{};

		float jitterX = 0.0f;
		float jitterY = 0.0f;

		float savedWidthRatio = 1.0f;
		float savedHeightRatio = 1.0f;

#if F4R_HAS_FSR3
		std::unique_ptr<FidelityFX> fidelityFX;
#endif

	private:
		AntiAliasing() = default;

		bool resourcesCreated = false;
		uint32_t cachedWidth = 0;
		uint32_t cachedHeight = 0;
		DXGI_FORMAT cachedFormat = DXGI_FORMAT_UNKNOWN;
		int32_t cachedMode = -1;

		int startupFrameGuard = 0;
	};

	void InstallContextHooks();
}
