#pragma once

#if F4R_HAS_DLSS

namespace F4R_AA
{
	using PFun_slSetTagLegacy = sl::Result(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);

	struct Streamline
	{
		bool initialized = false;
		bool featureDLSS = false;

		HMODULE interposer = nullptr;

		sl::FrameToken* frameToken = nullptr;
		sl::ViewportHandle viewport = sl::ViewportHandle(0);

		sl::DLSSPreset preset = sl::DLSSPreset::ePresetK;

		PFun_slInit* slInit = nullptr;
		PFun_slShutdown* slShutdown = nullptr;
		PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
		PFun_slIsFeatureLoaded* slIsFeatureLoaded = nullptr;
		PFun_slSetFeatureLoaded* slSetFeatureLoaded = nullptr;
		PFun_slEvaluateFeature* slEvaluateFeature = nullptr;
		PFun_slAllocateResources* slAllocateResources = nullptr;
		PFun_slFreeResources* slFreeResources = nullptr;
		PFun_slSetTagLegacy* slSetTag = nullptr;
		PFun_slGetFeatureRequirements* slGetFeatureRequirements = nullptr;
		PFun_slGetFeatureVersion* slGetFeatureVersion = nullptr;
		PFun_slUpgradeInterface* slUpgradeInterface = nullptr;
		PFun_slSetConstants* slSetConstants = nullptr;
		PFun_slGetNativeInterface* slGetNativeInterface = nullptr;
		PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
		PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
		PFun_slSetD3DDevice* slSetD3DDevice = nullptr;

		PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings = nullptr;
		PFun_slDLSSGetState* slDLSSGetState = nullptr;
		PFun_slDLSSSetOptions* slDLSSSetOptions = nullptr;

		[[nodiscard]] static Streamline& GetSingleton();

		Streamline(const Streamline&) = delete;
		Streamline& operator=(const Streamline&) = delete;

		void LoadInterposer();
		void Initialize();
		void CheckFeatures(IDXGIAdapter* a_adapter);
		void PostDevice();
		void DestroyDLSSResources();

		void UpdateConstants(float a_jitterX, float a_jitterY);

		void Evaluate(
			Texture2D* a_color,
			Texture2D* a_motionVectors,
			float a_jitterX,
			float a_jitterY,
			uint32_t a_renderWidth,
			uint32_t a_renderHeight,
			uint32_t a_qualityMode);

	private:
		Streamline() = default;
	};
}
#endif
