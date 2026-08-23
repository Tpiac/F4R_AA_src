#include "PCH.hpp"
#include "Streamline.hpp"
#include "AntiAliasing.hpp"

#include <psapi.h>
#include <xmmintrin.h>
#include <cmath>
#include <sl_matrix_helpers.h>

#if F4R_HAS_DLSS

namespace F4R_AA
{
	Streamline& Streamline::GetSingleton()
	{
		static Streamline instance;
		return instance;
	}

	namespace
	{
		void SLLogCallback(sl::LogType a_type, const char* a_msg)
		{
			switch (a_type) {
			case sl::LogType::eError:
				REX::LogError("[SL]: {}", a_msg);
				break;
			case sl::LogType::eWarn:
				REX::LogWarning("[SL]: {}", a_msg);
				break;
			default:
				REX::LogDebug("[SL]: {}", a_msg);
				break;
			}
		}

		sl::float4x4 ToSLMatrix(const __m128* a_mat)
		{
			sl::float4x4 result;
			for (int i = 0; i < 4; ++i) {
				alignas(16) float row[4];
				_mm_store_ps(row, a_mat[i]);
				result[i] = sl::float4(row[0], row[1], row[2], row[3]);
			}
			return result;
		}

		sl::float3 ToSLFloat3(const __m128* a_v)
		{
			alignas(16) float vals[4];
			_mm_store_ps(vals, *a_v);
			return sl::float3(vals[0], vals[1], vals[2]);
		}
	}

	void Streamline::LoadInterposer()
	{
		interposer = LoadLibraryW(L"Data/F4SE/Plugins/Streamline/sl.interposer.dll");
		if (!interposer) {
			REX::LogCritical("Failed to load interposer: Error Code {0:x}", GetLastError());
		}
	}

	void Streamline::Initialize()
	{
		REX::LogInformation("Initializing Streamline");

		if (!interposer) {
			REX::LogCritical("Initialize failed - interposer not loaded");
			return;
		}

		auto resolve = [&](const char* name) -> void* {
			return GetProcAddress(interposer, name);
		};

		slInit = reinterpret_cast<PFun_slInit*>(resolve("slInit"));
		slShutdown = reinterpret_cast<PFun_slShutdown*>(resolve("slShutdown"));
		slIsFeatureSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(resolve("slIsFeatureSupported"));
		slIsFeatureLoaded = reinterpret_cast<PFun_slIsFeatureLoaded*>(resolve("slIsFeatureLoaded"));
		slSetFeatureLoaded = reinterpret_cast<PFun_slSetFeatureLoaded*>(resolve("slSetFeatureLoaded"));
		slEvaluateFeature = reinterpret_cast<PFun_slEvaluateFeature*>(resolve("slEvaluateFeature"));
		slAllocateResources = reinterpret_cast<PFun_slAllocateResources*>(resolve("slAllocateResources"));
		slFreeResources = reinterpret_cast<PFun_slFreeResources*>(resolve("slFreeResources"));
		slSetTag = reinterpret_cast<PFun_slSetTagLegacy*>(resolve("slSetTag"));
		slGetFeatureRequirements = reinterpret_cast<PFun_slGetFeatureRequirements*>(resolve("slGetFeatureRequirements"));
		slGetFeatureVersion = reinterpret_cast<PFun_slGetFeatureVersion*>(resolve("slGetFeatureVersion"));
		slUpgradeInterface = reinterpret_cast<PFun_slUpgradeInterface*>(resolve("slUpgradeInterface"));
		slSetConstants = reinterpret_cast<PFun_slSetConstants*>(resolve("slSetConstants"));
		slGetNativeInterface = reinterpret_cast<PFun_slGetNativeInterface*>(resolve("slGetNativeInterface"));
		slGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(resolve("slGetFeatureFunction"));
		slGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(resolve("slGetNewFrameToken"));
		slSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(resolve("slSetD3DDevice"));

		if (!slInit) {
			REX::LogCritical("slInit not found in interposer");
			return;
		}

		static const sl::Feature kFeatures[] = { sl::kFeatureDLSS };

		sl::Preferences pref{};
		pref.showConsole = false;
		pref.logLevel = sl::LogLevel::eOff;
		pref.pathsToPlugins = nullptr;
		pref.numPathsToPlugins = 0;
		pref.pathToLogsAndData = nullptr;
		pref.allocateCallback = nullptr;
		pref.releaseCallback = nullptr;
		pref.logMessageCallback = &SLLogCallback;
		pref.flags = sl::PreferenceFlags::eUseManualHooking |
			sl::PreferenceFlags::eDisableCLStateTracking;
		pref.featuresToLoad = kFeatures;
		pref.numFeaturesToLoad = 1;
		pref.applicationId = 0;
		pref.engine = sl::EngineType::eCustom;
		pref.engineVersion = "1.0.0";
		pref.projectId = "4aa42191c0a946178e8a3318bc33ca24";
		pref.renderAPI = sl::RenderAPI::eD3D11;

		sl::Result result = slInit(pref, sl::kSDKVersion);
		if (result == sl::Result::eOk) {
			initialized = true;
			REX::LogInformation("Streamline initialized");
		} else {
			REX::LogCritical("Failed to initialize Streamline (result={})", static_cast<int>(result));
		}
	}

	void Streamline::CheckFeatures(IDXGIAdapter* a_adapter)
	{
		REX::LogInformation("Checking features");

		if (!a_adapter) {
			REX::LogWarning("CheckFeatures - adapter is null");
			return;
		}

		DXGI_ADAPTER_DESC desc{};
		a_adapter->GetDesc(&desc);

		sl::AdapterInfo adapterInfo{};
		adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
		adapterInfo.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
		adapterInfo.vkPhysicalDevice = nullptr;

		featureDLSS = false;
		slIsFeatureLoaded(sl::kFeatureDLSS, featureDLSS);

		if (!featureDLSS) {
			REX::LogWarning("DLSS feature is not loaded");

			sl::FeatureRequirements req{};
			sl::Result res = slGetFeatureRequirements(sl::kFeatureDLSS, req);
			if (res != sl::Result::eOk) {
				REX::LogError("slGetFeatureRequirements failed (result={})", static_cast<int>(res));
			}
			return;
		}

		REX::LogInformation("DLSS feature is loaded");

		sl::Result res = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
		featureDLSS = (res == sl::Result::eOk);
		if (!featureDLSS) {
			REX::LogError("DLSS is not supported (result={})", static_cast<int>(res));
			return;
		}

		REX::LogInformation("DLSS is available");
	}

	void Streamline::PostDevice()
	{
		if (featureDLSS) {
			slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&>(slDLSSGetOptimalSettings));
			slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", reinterpret_cast<void*&>(slDLSSGetState));
			slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>(slDLSSSetOptions));
		}
	}

	void Streamline::DestroyDLSSResources()
	{
		if (!featureDLSS || !slDLSSSetOptions || !slFreeResources)
			return;

		sl::DLSSOptions options{};
		options.mode = static_cast<sl::DLSSMode>(0xFFFFFFFFu);
		slDLSSSetOptions(viewport, options);
		slFreeResources(sl::kFeatureDLSS, viewport);
	}

	void Streamline::UpdateConstants(float a_jitterX, float a_jitterY)
	{
		sl::Constants constants{};

		auto& state = RE::BSGraphics::State::GetSingleton();
		const auto& camView = state.cameraState.camViewData;

		sl::float4x4 viewMatrix = ToSLMatrix(camView.viewMat.data());
		sl::float4x4 invView;
		sl::matrixFullInvert(invView, viewMatrix);
		sl::float4x4 vpUnjittered = ToSLMatrix(camView.viewProjUnjittered.data());
		sl::matrixMul(constants.cameraViewToClip, invView, vpUnjittered);
		sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);

		sl::float4x4 currentVP = ToSLMatrix(camView.currentViewProjUnjittered.data());
		sl::float4x4 previousVP = ToSLMatrix(camView.previousViewProjUnjittered.data());
		sl::float4x4 invCurrentVP;
		sl::matrixFullInvert(invCurrentVP, currentVP);
		sl::matrixMul(constants.clipToPrevClip, invCurrentVP, previousVP);
		sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);

		const auto& camState = state.cameraState;
		constants.cameraPos = sl::float3(camState.posAdjust.x, camState.posAdjust.y, camState.posAdjust.z);
		constants.cameraUp = ToSLFloat3(&camView.viewUp);
		constants.cameraRight = ToSLFloat3(&camView.viewRight);
		constants.cameraFwd = ToSLFloat3(&camView.viewDir);

		float cameraNear = 0.0f;
		float cameraFar = 1.0f;
		GetCameraNearFar(cameraNear, cameraFar);
		constants.cameraNear = cameraNear;
		constants.cameraFar = cameraFar;
		constants.cameraAspectRatio = (state.screenHeight != 0)
			? static_cast<float>(state.screenWidth) / static_cast<float>(state.screenHeight)
			: 1.0f;
		constants.cameraFOV = 2.0f * std::atan(1.0f / constants.cameraViewToClip[1].y);

		constants.cameraMotionIncluded = sl::Boolean::eTrue;
		constants.cameraPinholeOffset = { 0.0f, 0.0f };
		constants.depthInverted = sl::Boolean::eFalse;
		constants.motionVectors3D = sl::Boolean::eFalse;
		constants.reset = AntiAliasing::GetSingleton().resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		constants.jitterOffset = { -a_jitterX, -a_jitterY };
		constants.mvecScale = { 1.0f, 1.0f };
		constants.motionVectorsInvalidValue = 1.1754944e-38f;
		constants.orthographicProjection = sl::Boolean::eFalse;
		constants.motionVectorsDilated = sl::Boolean::eTrue;
		constants.motionVectorsJittered = sl::Boolean::eFalse;

		sl::Result res = slGetNewFrameToken(frameToken, nullptr);
		if (res != sl::Result::eOk) {
			REX::LogError("Could not get frame token (result={})", static_cast<int>(res));
		}

		res = slSetConstants(constants, *frameToken, viewport);
		if (res != sl::Result::eOk) {
			REX::LogError("Could not set constants (result={})", static_cast<int>(res));
		}
	}

	void Streamline::Evaluate(
		Texture2D* a_color,
		Texture2D* a_motionVectors,
		float a_jitterX,
		float a_jitterY,
		uint32_t a_renderWidth,
		uint32_t a_renderHeight,
		uint32_t a_qualityMode)
	{
		UpdateConstants(a_jitterX, a_jitterY);

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto& state = RE::BSGraphics::State::GetSingleton();

		auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
		if (!ctx) return;

		sl::DLSSMode mode = sl::DLSSMode::eDLAA;
		if (a_qualityMode == 1) mode = sl::DLSSMode::eMaxQuality;
		else if (a_qualityMode == 2) mode = sl::DLSSMode::eBalanced;
		else if (a_qualityMode == 3) mode = sl::DLSSMode::eMaxPerformance;
		else if (a_qualityMode == 4) mode = sl::DLSSMode::eUltraPerformance;

		bool isHDR = false;
		if (a_color->srv) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			a_color->srv->GetDesc(&srvDesc);
			isHDR = srvDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
				srvDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
				srvDesc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
		}

		sl::DLSSOptions options{};
		options.mode = mode;
		options.outputWidth = state.screenWidth;
		options.outputHeight = state.screenHeight;
		options.colorBuffersHDR = isHDR ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.dlaaPreset = preset;
		options.qualityPreset = preset;
		options.balancedPreset = preset;
		options.performancePreset = preset;
		options.ultraPerformancePreset = preset;

		sl::Result res = slDLSSSetOptions(viewport, options);
		if (res != sl::Result::eOk) {
			REX::LogCritical("Could not enable DLSS (result={})", static_cast<int>(res));
		}

		sl::Resource colorIn(sl::ResourceType::eTex2d, a_color->resource);
		sl::Resource colorOut(sl::ResourceType::eTex2d, a_color->resource);
		sl::Resource depth(sl::ResourceType::eTex2d, reinterpret_cast<void*>(rendererData->depthStencilTargets[2].texture));
		sl::Resource motionVectors(sl::ResourceType::eTex2d, a_motionVectors->resource);

		sl::Extent extent{ 0, 0, a_renderWidth, a_renderHeight };

		sl::ResourceTag tags[] = {
			sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::eOnlyValidNow, &extent),
			sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::eOnlyValidNow, &extent),
			sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::eOnlyValidNow, &extent),
			sl::ResourceTag(&motionVectors, sl::kBufferTypeMotionVectors, sl::eOnlyValidNow, &extent),
		};

		res = slSetTag(viewport, tags, 4, ctx);
		if (res != sl::Result::eOk) {
			REX::LogError("slSetTag failed (result={})", static_cast<int>(res));
		}

		const sl::BaseStructure* inputs[] = { &viewport };
		res = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, 1, ctx);
		if (res != sl::Result::eOk) {
			REX::LogError("slEvaluateFeature failed (result={})", static_cast<int>(res));
		}
	}
}
#endif
