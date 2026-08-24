#include "PCH.hpp"
#include "AntiAliasing.hpp"
#include "Streamline.hpp"
#include "XeSS.hpp"

#include "Shaders/MVFix.hpp"
#include "Shaders/RCAS.hpp"
#include "Shaders/DepthCopy.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace F4R_AA
{
	namespace
	{
		float Halton(int32_t a_index, int32_t a_base)
		{
			float f = 1.0f, result = 0.0f;
			for (int32_t currentIndex = a_index; currentIndex > 0;) {
				f /= (float)a_base;
				result = result + f * (float)(currentIndex % a_base);
				currentIndex = (uint32_t)(floorf((float)currentIndex / (float)a_base));
			}
			return result;
		}

		int32_t GetJitterPhaseCount(int32_t a_renderWidth, int32_t a_displayWidth, float a_basePhaseCount)
		{
			const int32_t jitterPhaseCount =
				int32_t(a_basePhaseCount * pow((float(a_displayWidth) / a_renderWidth), 2.0f));
			return jitterPhaseCount;
		}

		void GetJitterOffset(float* a_outX, float* a_outY, int32_t a_index, int32_t a_phaseCount)
		{
			const float x = Halton((a_index % a_phaseCount) + 1, 2) - 0.5f;
			const float y = Halton((a_index % a_phaseCount) + 1, 3) - 0.5f;
			*a_outX = x;
			*a_outY = y;
		}

		ID3D11ComputeShader* CreateComputeShaderFromBytecode(
			const unsigned char* a_bytecode,
			unsigned int a_bytecodeSize,
			ID3D11Device* a_device)
		{
			if (!a_bytecode || !a_bytecodeSize || !a_device) return nullptr;

			ID3D11ComputeShader* shader = nullptr;
			HRESULT hr = a_device->CreateComputeShader(a_bytecode, a_bytecodeSize, nullptr, &shader);
			if (FAILED(hr)) {
				REX::LogError("CreateComputeShader failed hr=0x{:x}", static_cast<uint32_t>(hr));
				return nullptr;
			}
			return shader;
		}

		std::unique_ptr<Texture2D> CreateSharpenTexture(
			ID3D11Device* a_device,
			uint32_t a_width,
			uint32_t a_height,
			DXGI_FORMAT a_backBufferFormat,
			DXGI_FORMAT a_srvFormat)
		{
			DXGI_FORMAT uavFormat = a_srvFormat;
			if (uavFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
				uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			}

			auto texture = std::make_unique<Texture2D>();
			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = a_width;
			texDesc.Height = a_height;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = a_backBufferFormat;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			HRESULT hr = a_device->CreateTexture2D(&texDesc, nullptr, &texture->resource);
			if (FAILED(hr)) {
				REX::LogError("CreateTexture2D(sharpenTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				return nullptr;
			}

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = uavFormat;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			hr = a_device->CreateUnorderedAccessView(texture->resource, &uavDesc, &texture->uav);
			if (FAILED(hr)) {
				REX::LogError("CreateUnorderedAccessView(sharpenTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				return nullptr;
			}

			return texture;
		}

		void RunComputePass(
			ID3D11DeviceContext* a_ctx,
			ID3D11ComputeShader* a_shader,
			ID3D11Buffer* a_constants,
			ID3D11ShaderResourceView* const* a_srvs,
			uint32_t a_numSRVs,
			ID3D11UnorderedAccessView* a_uav,
			uint32_t a_groupX,
			uint32_t a_groupY)
		{
			if (a_constants) {
				a_ctx->CSSetConstantBuffers(0, 1, &a_constants);
			}
			a_ctx->CSSetShaderResources(0, a_numSRVs, a_srvs);
			ID3D11UnorderedAccessView* uavs[1] = { a_uav };
			a_ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			a_ctx->CSSetShader(a_shader, nullptr, 0);
			a_ctx->Dispatch(a_groupX, a_groupY, 1);

			ID3D11UnorderedAccessView* nullUav[1] = { nullptr };
			a_ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
			ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
			a_ctx->CSSetShaderResources(0, a_numSRVs, nullSrvs);
			a_ctx->CSSetShader(nullptr, nullptr, 0);
		}

		ID3D11Buffer* CreateConstantBuffer(ID3D11Device* a_device, const char* a_name, uint32_t a_size)
		{
			D3D11_BUFFER_DESC desc = {};
			desc.ByteWidth = a_size;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			ID3D11Buffer* buffer = nullptr;
			HRESULT hr = a_device->CreateBuffer(&desc, nullptr, &buffer);
			if (FAILED(hr)) {
				REX::LogError("CreateBuffer({}) failed hr=0x{:x}", a_name, static_cast<uint32_t>(hr));
				return nullptr;
			}
			return buffer;
		}

		int32_t ParseInt32(const char* a_value, int32_t a_default)
		{
			char* end = nullptr;
			unsigned long value = std::strtoul(a_value, &end, 10);
			return (end != a_value && end && *end == '\0') ? static_cast<int32_t>(value) : a_default;
		}

		float ParseFloat(const char* a_value, float a_default)
		{
			char* end = nullptr;
			float value = std::strtof(a_value, &end);
			return (end != a_value && end && *end == '\0') ? value : a_default;
		}
	}

	struct MotionVectorConstants
	{
		uint32_t screenWidth;
		uint32_t screenHeight;
		uint32_t renderWidth;
		uint32_t renderHeight;
		float cameraFar;
		float cameraNear;
		float cameraFarMinusNear;
		float cameraFarTimesNear;
	};

	struct RCASConstants
	{
		float sharpness;
		float pad0;
		float pad1;
		float pad2;
	};

	AntiAliasing& AntiAliasing::GetSingleton()
	{
		static AntiAliasing instance;
		return instance;
	}

	AntiAliasing::~AntiAliasing() = default;

	void AntiAliasing::LoadSettings(const std::string& a_iniPath)
	{
		char buf[64];

		GetPrivateProfileStringA("Settings", "iAAMode", F4R_STRINGIFY(F4R_DEFAULT_AAMODE), buf, sizeof(buf), a_iniPath.c_str());
		settings.iAAMode = ParseInt32(buf, F4R_DEFAULT_AAMODE);

		GetPrivateProfileStringA("Settings", "fSharpness", "0.5", buf, sizeof(buf), a_iniPath.c_str());
		settings.fSharpness = ParseFloat(buf, 0.5f);

		GetPrivateProfileStringA("Settings", "iDLSSPreset", "11", buf, sizeof(buf), a_iniPath.c_str());
		settings.iDLSSPreset = ParseInt32(buf, 11);

		GetPrivateProfileStringA("Advanced", "fAnisotropicMipBias", "-0.0001", buf, sizeof(buf), a_iniPath.c_str());
		settings.fAnisotropicMipBias = ParseFloat(buf, -0.0001f);

#if F4R_HAS_DLSS
	Streamline::GetSingleton().preset = static_cast<sl::DLSSPreset>(settings.iDLSSPreset);

	GetPrivateProfileStringA("Settings", "bEnableReflex", "1", buf, sizeof(buf), a_iniPath.c_str());
	settings.bEnableReflex = ParseInt32(buf, 1) != 0;

	GetPrivateProfileStringA("Settings", "bReflexBoost", "0", buf, sizeof(buf), a_iniPath.c_str());
	settings.bReflexBoost = ParseInt32(buf, 0) != 0;

	GetPrivateProfileStringA("Advanced", "bReflexUseFPSLimit", "0", buf, sizeof(buf), a_iniPath.c_str());
	settings.bReflexUseFPSLimit = ParseInt32(buf, 0) != 0;

	GetPrivateProfileStringA("Advanced", "fReflexFPSLimit", "60", buf, sizeof(buf), a_iniPath.c_str());
	float reflexFPSLimit = ParseFloat(buf, 60.0f);
	if (reflexFPSLimit < 20.0f) reflexFPSLimit = 20.0f;
	if (reflexFPSLimit > 240.0f) reflexFPSLimit = 240.0f;
	settings.fReflexFPSLimit = reflexFPSLimit;
#endif

	const auto mode = static_cast<AAMode>(settings.iAAMode);
	if (mode == AAMode::DLAA) {
#if F4R_HAS_DLSS
		REX::LogInformation("Settings loaded - mode=DLAA sharpness={} mipBias={} reflex={} dlssPreset={}",
			settings.fSharpness, settings.fAnisotropicMipBias,
			settings.bEnableReflex ? "enabled" : "disabled", settings.iDLSSPreset);
#else
		REX::LogInformation("Settings loaded - mode=DLAA sharpness={} mipBias={} dlssPreset={}",
			settings.fSharpness, settings.fAnisotropicMipBias, settings.iDLSSPreset);
#endif
	} else if (mode == AAMode::FSR3) {
			REX::LogInformation("Settings loaded - mode=FSR3 sharpness={} mipBias={}",
				settings.fSharpness, settings.fAnisotropicMipBias);
		} else if (mode == AAMode::XeSS) {
			REX::LogInformation("Settings loaded - mode=XeSS sharpness={} mipBias={}",
				settings.fSharpness, settings.fAnisotropicMipBias);
		} else {
			REX::LogInformation("Settings loaded - mode=Off");
		}
	}

	void AntiAliasing::RequestReset()
	{
		resetHistory = true;
	}

	void AntiAliasing::Init()
{
		static bool s_initialized = false;
		if (s_initialized) {
			REX::LogWarning("Init called twice - ignoring (hooks already installed)");
			return;
		}
		s_initialized = true;

		REX::LogDebug("Init called");

		if (settings.iAAMode == static_cast<int32_t>(AAMode::Off)) {
			REX::LogInformation("Off mode - no hooks installed");
			return;
		}

		auto* branchPool = F4SE::GetTrampolineInterface()->AllocateFromBranchPool(256);
		REL::GetTrampoline()->Init(branchPool, 256);

		InstallHooks();
		UpdateGameSettings();

		REX::LogDebug("Init complete");
	}

	bool IsMenuBlocked()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) return false;

		static const std::initializer_list<const char*> blockedMenus = {
			"PipboyMenu", "BarterMenu", "InventoryMenu",
			"CraftingMenu", "MainMenu", "MapMenu",
			"PauseMenu", "LoadingMenu",
			"ExamineMenu", "TerminalMenu"
		};
		for (auto name : blockedMenus) {
			auto result = ui->IsMenuOpen(RE::BSFixedString(name));
			if (result.value_or(false)) return true;
		}
		return false;
	}

	void AntiAliasing::Update()
	{
		const auto mode = static_cast<AAMode>(settings.iAAMode);
		aaEnabled = false;
		if (mode == AAMode::Off) {
			return;
		}

#if F4R_HAS_DLSS
		if (mode == AAMode::DLAA) {
			auto& streamline = Streamline::GetSingleton();
			aaEnabled = streamline.initialized && streamline.featureDLSS;
		}
#endif
#if F4R_HAS_FSR3
		if (mode == AAMode::FSR3) {
			aaEnabled = true;
		}
#endif
#if F4R_HAS_XESS
		if (mode == AAMode::XeSS) {
			aaEnabled = XeSS::GetSingleton().initialized;
		}
#endif

		auto* main = RE::Main::GetSingleton();
		bool prevEnabled = aaEnabled;
		if (main && (!main->gameActive || IsMenuBlocked())) {
			aaEnabled = false;
		}

		if (!prevEnabled && aaEnabled) {
			resetHistory = true;
		}
		if ((mode == AAMode::XeSS || mode == AAMode::FSR3 || mode == AAMode::DLAA) && !aaEnabled) {
			resetHistory = true;
		}

#if F4R_HAS_DLSS
		if (mode == AAMode::DLAA) {
			Streamline::GetSingleton().UpdateLatency();
		}
#endif

		if (mode == AAMode::FSR3 && g_enbLoaded && !g_realDevice && !g_enbExtractionFailed) {
			ExtractRealD3D11();
		}

		InstallContextHooks();

		auto& state = RE::BSGraphics::State::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();

		if (aaEnabled) {
			int32_t renderWidth = static_cast<int32_t>(state.screenWidth);
			int32_t displayWidth = static_cast<int32_t>(state.screenWidth);
#if F4R_HAS_FSR3
			if (mode == AAMode::FSR3) {
				int32_t phaseCount = ffxFsr3GetJitterPhaseCount(renderWidth, displayWidth);
				ffxFsr3GetJitterOffset(&jitterX, &jitterY, state.frameCount, phaseCount);
			} else
#endif
			{
#if F4R_HAS_XESS
				float basePhaseCount = (mode == AAMode::XeSS) ? 16.0f : 8.0f;
#else
				float basePhaseCount = 8.0f;
#endif
				int32_t phaseCount = GetJitterPhaseCount(renderWidth, displayWidth, basePhaseCount);
				GetJitterOffset(&jitterX, &jitterY, state.frameCount, phaseCount);
			}

			state.offsetX = (jitterX * -2.0f) / static_cast<float>(state.screenWidth);
			state.offsetY = (jitterY * 2.0f) / static_cast<float>(state.screenHeight);
		}

		if (aaEnabled) {
			auto* samplerStates = GetGlobalSamplers();

			if (samplerStates) {
				constexpr float mipBias = -1.0f;

				static float s_previousMipBias = 0.0f;
				bool needsRebuild = (s_previousMipBias != mipBias);

				auto* device = GetRenderer();

				for (int i = 0; i < 320; i++) {
					if (originalSamplerStates[i]) {
						originalSamplerStates[i]->Release();
					}
					originalSamplerStates[i] = samplerStates->a[i];
					if (originalSamplerStates[i]) {
						originalSamplerStates[i]->AddRef();
					}

					if (needsRebuild) {
						if (biasedSamplerStates[i]) {
							biasedSamplerStates[i]->Release();
							biasedSamplerStates[i] = nullptr;
						}

						ID3D11SamplerState* src = samplerStates->a[i];
						if (src && device) {
							D3D11_SAMPLER_DESC desc;
							src->GetDesc(&desc);
							bool shouldClone = false;
							if (desc.Filter == D3D11_FILTER_ANISOTROPIC) {
								desc.MaxAnisotropy = 8;
								desc.MipLODBias = settings.fAnisotropicMipBias;
								shouldClone = true;
							} else if (desc.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR) {
								desc.MipLODBias = mipBias;
								shouldClone = true;
							}
							if (shouldClone) {
								HRESULT hr = device->CreateSamplerState(&desc, &biasedSamplerStates[i]);
								if (FAILED(hr)) {
									biasedSamplerStates[i] = nullptr;
								}
							}
						}
					}
				}

				if (needsRebuild) {
					s_previousMipBias = mipBias;
				}
			}
		}

		GetDynHeightRatio(rtMgr) = 1.0f;
		GetDynWidthRatio(rtMgr) = 1.0f;
		GetDynResActivated(rtMgr) = false;

		UpdateGameSettings();
		CheckResources();
	}

	void AntiAliasing::Apply()
	{
		const auto mode = static_cast<AAMode>(settings.iAAMode);
		if (mode == AAMode::Off) return;

		auto* main = RE::Main::GetSingleton();

		if (main && main->gameActive) {
			if (startupFrameGuard < 5) {
				startupFrameGuard++;
				return;
			}
		}

		if (!aaEnabled) return;

		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		if (!rendererData) return;

		auto* ctx = GetImmediateContext();
		if (!ctx) return;

		ctx->OMSetRenderTargets(0, nullptr, nullptr);

		auto& state = RE::BSGraphics::State::GetSingleton();
		auto& rtMgr = RE::BSGraphics::RenderTargetManager::GetSingleton();

		auto* backBufferSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[RenderTarget::kFrameBuffer].srView);
		if (!backBufferSRV) return;

		ID3D11Resource* backBufferResource = nullptr;
		backBufferSRV->GetResource(&backBufferResource);
		if (!backBufferResource) return;

#if F4R_HAS_XESS
		if (mode == AAMode::XeSS) {
			if (!xessColorTexture || !xessColorTexture->resource) {
				backBufferResource->Release();
				return;
			}
			ctx->CopyResource(reinterpret_cast<ID3D11Resource*>(xessColorTexture->resource), backBufferResource);
		} else
#endif
		{
			if (!workingTexture || !workingTexture->resource) {
				backBufferResource->Release();
				return;
			}
			ctx->CopyResource(reinterpret_cast<ID3D11Resource*>(workingTexture->resource), backBufferResource);
		}

		uint32_t renderW = static_cast<uint32_t>(static_cast<float>(state.screenWidth) * GetDynWidthRatio(rtMgr));
		uint32_t renderH = static_cast<uint32_t>(static_cast<float>(state.screenHeight) * GetDynHeightRatio(rtMgr));

#if F4R_HAS_DLSS
		if (mode == AAMode::DLAA) {
			auto& streamline = Streamline::GetSingleton();

			if (motionVectorTexture && motionVectorTexture->resource &&
				motionVectorTexture->uav && motionVectorTexture->srv &&
				mvFixShader && mvFixCB) {

				float cameraNear = 0.0f;
				float cameraFar = 1.0f;
				GetCameraNearFar(cameraNear, cameraFar);

				MotionVectorConstants constants{};
				constants.screenWidth = state.screenWidth;
				constants.screenHeight = state.screenHeight;
				constants.renderWidth = renderW;
				constants.renderHeight = renderH;
				constants.cameraFar = cameraFar;
				constants.cameraNear = cameraNear;
				constants.cameraFarMinusNear = cameraFar - cameraNear;
				constants.cameraFarTimesNear = cameraFar * cameraNear;
				ctx->UpdateSubresource(mvFixCB, 0, nullptr, &constants, 0, 0);

				ID3D11ShaderResourceView* mvSRV =
					reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[RenderTarget::kMotionVectors].srView);
				ID3D11ShaderResourceView* depthSRV =
					reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);

				if (mvSRV && depthSRV) {
					ID3D11ShaderResourceView* srvs[2] = { mvSRV, depthSRV };
					RunComputePass(ctx, mvFixShader, mvFixCB, srvs, 2, motionVectorTexture->uav,
						(renderW + 7) / 8, (renderH + 7) / 8);
				}
			}

			streamline.Evaluate(
				workingTexture.get(),
				motionVectorTexture.get(),
				jitterX, jitterY, renderW, renderH, 0);
			resetHistory = false;

			Texture2D* finalTexture = workingTexture.get();

			if (settings.fSharpness > 0.0f && sharpenTexture && sharpenTexture->resource &&
				sharpenTexture->uav && rcasShader && rcasCB && workingTexture->srv) {

				float sharpness = settings.fSharpness;
				if (sharpness < 0.0f) sharpness = 0.0f;
				if (sharpness > 1.0f) sharpness = 1.0f;

				RCASConstants constants{};
				constants.sharpness = exp2f(2.0f * sharpness - 2.0f);
				ctx->UpdateSubresource(rcasCB, 0, nullptr, &constants, 0, 0);

				ID3D11ShaderResourceView* srvs[1] = { workingTexture->srv };
				RunComputePass(ctx, rcasShader, rcasCB, srvs, 1, sharpenTexture->uav,
					(state.screenWidth + 7) / 8, (state.screenHeight + 7) / 8);

				finalTexture = sharpenTexture.get();
			}

			ctx->CopyResource(backBufferResource, reinterpret_cast<ID3D11Resource*>(finalTexture->resource));
		}
#endif
#if F4R_HAS_FSR3
		if (mode == AAMode::FSR3) {
			if (fidelityFX) {
				fidelityFX->Apply(workingTexture->resource, jitterX, jitterY, renderW, renderH);
			}

			ctx->CopyResource(backBufferResource, reinterpret_cast<ID3D11Resource*>(workingTexture->resource));
		}
#endif
#if F4R_HAS_XESS
		if (mode == AAMode::XeSS) {
			if (xessMotionVectorTexture && xessMotionVectorTexture->resource) {
				ID3D11Resource* rawMV = reinterpret_cast<ID3D11Resource*>(rendererData->renderTargets[RenderTarget::kMotionVectors].texture);
				if (rawMV) {
					ctx->CopyResource(xessMotionVectorTexture->resource, rawMV);
				}
			}

			if (xessDepthTexture && xessDepthTexture->uav && depthCopyShader) {
				ID3D11ShaderResourceView* depthSRV =
					reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->depthStencilTargets[DepthStencil::kMain].srViewDepth);

				if (depthSRV) {
					ID3D11ShaderResourceView* srvs[1] = { depthSRV };
					RunComputePass(ctx, depthCopyShader, nullptr, srvs, 1, xessDepthTexture->uav,
						(renderW + 7) / 8, (renderH + 7) / 8);
				}
			}

			uint32_t reset = resetHistory ? 1u : 0u;
			resetHistory = false;

			XeSS::GetSingleton().Execute(
				xessColorTexture.get(),
				xessMotionVectorTexture.get(),
				xessDepthTexture.get(),
				xessOutputTexture.get(),
				jitterX, jitterY, renderW, renderH, reset);

			if (settings.fSharpness > 0.0f && sharpenTexture && sharpenTexture->uav &&
				xessOutputTexture && xessOutputTexture->srv && rcasShader && rcasCB) {

				float sharpness = settings.fSharpness;
				if (sharpness < 0.0f) sharpness = 0.0f;
				if (sharpness > 1.0f) sharpness = 1.0f;

				RCASConstants constants{};
				constants.sharpness = 0.25f + 0.75f * powf(sharpness, 0.2f);
				ctx->UpdateSubresource(rcasCB, 0, nullptr, &constants, 0, 0);

				ID3D11ShaderResourceView* srvs[1] = { xessOutputTexture->srv };
				RunComputePass(ctx, rcasShader, rcasCB, srvs, 1, sharpenTexture->uav,
					(state.screenWidth + 7) / 8, (state.screenHeight + 7) / 8);

				ctx->CopyResource(backBufferResource, sharpenTexture->resource);
			} else {
				ctx->CopyResource(backBufferResource, xessOutputTexture->resource);
			}
		}
#endif

		backBufferResource->Release();
	}

	void AntiAliasing::UpdateGameSettings()
	{
		auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		if (imageSpaceManager && imageSpaceManager->effectList[0x11]) {
			imageSpaceManager->effectList[0x11]->isActive = false;
		}

		auto enableTAAReloc = IsAE() || IsNG()
			? REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x294512 } }
			: REL::Relocation<std::uintptr_t>{ REL::Id<>{ 0x70681 } };
		*reinterpret_cast<bool*>(enableTAAReloc.GetAddress()) = true;
	}

	void AntiAliasing::OverrideSamplerStates()
	{
		if (!aaEnabled) return;

		auto* samplerStates = GetGlobalSamplers();
		if (!samplerStates) return;

		for (int i = 0; i < 320; i++) {
			if (biasedSamplerStates[i]) {
				samplerStates->a[i] = biasedSamplerStates[i];
			}
		}
	}

	void AntiAliasing::ResetSamplerStates()
	{
		if (!aaEnabled) return;

		auto* samplerStates = GetGlobalSamplers();
		if (!samplerStates) return;

		for (int i = 0; i < 320; i++) {
			samplerStates->a[i] = originalSamplerStates[i];
		}
	}

	void AntiAliasing::CheckResources()
	{
		auto* rendererData = RE::BSGraphics::RendererData::GetSingleton();
		auto& state = RE::BSGraphics::State::GetSingleton();

		auto* device = GetRenderer();
		if (!device) return;

		DXGI_FORMAT backBufferFormat = DXGI_FORMAT_R16G16B16A16_TYPELESS;
		DXGI_FORMAT typedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		auto* bbSRV = reinterpret_cast<ID3D11ShaderResourceView*>(rendererData->renderTargets[RenderTarget::kFrameBuffer].srView);
		if (bbSRV) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			bbSRV->GetDesc(&srvDesc);
			if (srvDesc.Format != DXGI_FORMAT_UNKNOWN) {
				typedFormat = srvDesc.Format;
			}

			ID3D11Resource* bbRes = nullptr;
			bbSRV->GetResource(&bbRes);
			if (bbRes) {
				D3D11_RESOURCE_DIMENSION dim;
				bbRes->GetType(&dim);
				if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
					D3D11_TEXTURE2D_DESC texDesc;
					reinterpret_cast<ID3D11Texture2D*>(bbRes)->GetDesc(&texDesc);
					backBufferFormat = texDesc.Format;
				}
				bbRes->Release();
			}
		}

		if (resourcesCreated) {
			if (cachedWidth == state.screenWidth && cachedHeight == state.screenHeight && cachedFormat == backBufferFormat && cachedMode == settings.iAAMode) {
				return;
			}
			REX::LogInformation("CheckResources - mode/resolution/format changed {}x{} fmt{} mode{} -> {}x{} fmt{} mode{} - recreating",
				cachedWidth, cachedHeight, static_cast<int>(cachedFormat), cachedMode,
				state.screenWidth, state.screenHeight, static_cast<int>(backBufferFormat), settings.iAAMode);
			workingTexture.reset();
			motionVectorTexture.reset();
			sharpenTexture.reset();
#if F4R_HAS_FSR3
			fidelityFX.reset();
#endif
#if F4R_HAS_XESS
			xessColorTexture.reset();
			xessMotionVectorTexture.reset();
			xessDepthTexture.reset();
			xessOutputTexture.reset();
			{
				auto& xess = XeSS::GetSingleton();
				if (xess.context && xess.xessDestroyContext) {
					xess.xessDestroyContext(xess.context);
					xess.context = nullptr;
					xess.initialized = false;
				}
			}
#endif
			if (mvFixCB) { mvFixCB->Release(); mvFixCB = nullptr; }
			if (rcasCB) { rcasCB->Release(); rcasCB = nullptr; }
			resourcesCreated = false;
		}

		const auto mode = static_cast<AAMode>(settings.iAAMode);

		REX::LogDebug("CheckResources - creating resources (mode={})", settings.iAAMode);

		if (!workingTexture) {
			workingTexture = std::make_unique<Texture2D>();
			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = state.screenWidth;
			texDesc.Height = state.screenHeight;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = backBufferFormat;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
#if F4R_HAS_FSR3
			if (mode == AAMode::FSR3) {
				texDesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
			}
#endif
			HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &workingTexture->resource);
			if (FAILED(hr)) {
				REX::LogError("CreateTexture2D(workingTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				workingTexture.reset();
				return;
			}
			REX::LogDebug("workingTexture {}x{} fmt={}", texDesc.Width, texDesc.Height, static_cast<int>(backBufferFormat));
		}

#if F4R_HAS_DLSS || F4R_HAS_XESS
		if (mode == AAMode::DLAA || mode == AAMode::XeSS) {
			if (!rcasCB) {
				rcasCB = CreateConstantBuffer(device, "rcasCB", sizeof(RCASConstants));
			}
			if (!rcasShader) {
				rcasShader = CreateComputeShaderFromBytecode(kRCAS, kRCASSize, device);
			}
		}
#endif

#if F4R_HAS_DLSS
		if (mode == AAMode::DLAA) {
			if (!motionVectorTexture) {
				motionVectorTexture = std::make_unique<Texture2D>();
				D3D11_TEXTURE2D_DESC texDesc = {};
				texDesc.Width = state.screenWidth;
				texDesc.Height = state.screenHeight;
				texDesc.MipLevels = 1;
				texDesc.ArraySize = 1;
				texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
				texDesc.SampleDesc.Count = 1;
				texDesc.Usage = D3D11_USAGE_DEFAULT;
				texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &motionVectorTexture->resource);
				if (FAILED(hr)) {
					REX::LogError("CreateTexture2D(motionVector) failed hr=0x{:x}", static_cast<uint32_t>(hr));
					motionVectorTexture.reset();
					return;
				}

				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
				uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				uavDesc.Texture2D.MipSlice = 0;
				hr = device->CreateUnorderedAccessView(motionVectorTexture->resource, &uavDesc, &motionVectorTexture->uav);
				if (FAILED(hr)) {
					REX::LogError("CreateUnorderedAccessView(motionVector) failed hr=0x{:x}", static_cast<uint32_t>(hr));
					motionVectorTexture.reset();
					return;
				}

				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
				hr = device->CreateShaderResourceView(motionVectorTexture->resource, &srvDesc, &motionVectorTexture->srv);
				if (FAILED(hr)) {
					REX::LogError("CreateShaderResourceView(motionVector) failed hr=0x{:x}", static_cast<uint32_t>(hr));
					motionVectorTexture.reset();
					return;
				}

				REX::LogDebug("motionVectorTexture {}x{} R16G16_FLOAT", texDesc.Width, texDesc.Height);
			}

			if (!mvFixCB) {
				mvFixCB = CreateConstantBuffer(device, "mvFixCB", sizeof(MotionVectorConstants));
			}

			if (!mvFixShader) {
				mvFixShader = CreateComputeShaderFromBytecode(kMVFix, kMVFixSize, device);
			}

			if (workingTexture && workingTexture->resource && !workingTexture->srv) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = typedFormat;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
				HRESULT hr = device->CreateShaderResourceView(workingTexture->resource, &srvDesc, &workingTexture->srv);
				if (FAILED(hr)) {
					REX::LogError("CreateShaderResourceView(workingTexture) failed hr=0x{:x}", static_cast<uint32_t>(hr));
				}
			}

			if (!sharpenTexture) {
				sharpenTexture = CreateSharpenTexture(device, state.screenWidth, state.screenHeight, backBufferFormat, typedFormat);
			}
		}
#endif
#if F4R_HAS_FSR3
		if (mode == AAMode::FSR3) {
			fidelityFX = std::make_unique<FidelityFX>();
			if (!fidelityFX->CreateFSRResources(
					device,
					state.screenWidth, state.screenHeight,
					backBufferFormat)) {
				REX::LogError("CheckResources - CreateFSRResources failed");
				fidelityFX.reset();
			}
		}
#endif
#if F4R_HAS_XESS
		if (mode == AAMode::XeSS) {
			auto* context = GetImmediateContext();
			auto& xess = XeSS::GetSingleton();
			if (!xess.loaded) {
				xess.Load();
			}
			if (!xess.loaded) {
				return;
			}

			if (!xess.device) {
				if (!xess.CreateD3D12(device, context)) {
					REX::LogError("CheckResources - D3D12 interop failed");
					return;
				}
			}

			uint32_t width = state.screenWidth;
			uint32_t height = state.screenHeight;

			if (!xessColorTexture) {
				xessColorTexture = std::make_unique<SharedTexture2D>();
				if (!xess.CreateSharedTexture(xessColorTexture.get(), width, height, backBufferFormat, D3D11_BIND_SHADER_RESOURCE, typedFormat)) {
					REX::LogError("CreateSharedTexture(color) failed");
					xessColorTexture.reset();
					return;
				}
			}

			if (!xessMotionVectorTexture) {
				xessMotionVectorTexture = std::make_unique<SharedTexture2D>();
				if (!xess.CreateSharedTexture(xessMotionVectorTexture.get(), width, height, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT)) {
					REX::LogError("CreateSharedTexture(motionVector) failed");
					xessMotionVectorTexture.reset();
					return;
				}
			}

			if (!xessDepthTexture) {
				xessDepthTexture = std::make_unique<SharedTexture2D>();
				if (!xess.CreateSharedTexture(xessDepthTexture.get(), width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT)) {
					REX::LogError("CreateSharedTexture(depth) failed");
					xessDepthTexture.reset();
					return;
				}
			}

			if (!xessOutputTexture) {
				DXGI_FORMAT uavFormat = typedFormat;
				if (uavFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
					uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
				}
				xessOutputTexture = std::make_unique<SharedTexture2D>();
				if (!xess.CreateSharedTexture(xessOutputTexture.get(), width, height, backBufferFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, typedFormat, uavFormat)) {
					REX::LogError("CreateSharedTexture(output) failed");
					xessOutputTexture.reset();
					return;
				}
			}

			if (!sharpenTexture) {
				sharpenTexture = CreateSharpenTexture(device, width, height, backBufferFormat, typedFormat);
			}

			if (!depthCopyShader) {
				depthCopyShader = CreateComputeShaderFromBytecode(kDepthCopy, kDepthCopySize, device);
			}

			if (!xess.initialized) {
				if (!xess.CreateContext(width, height)) {
					REX::LogError("CheckResources - CreateContext failed");
					return;
				}
			}
		}
#endif

		resourcesCreated = true;
		cachedWidth = state.screenWidth;
		cachedHeight = state.screenHeight;
		cachedFormat = backBufferFormat;
		cachedMode = settings.iAAMode;
	}
}
