#include "PCH.hpp"
#include "AntiAliasing.hpp"
#include <Detours.h>
#include <unordered_map>

namespace
{
	using namespace RE;
	using namespace RE::BSGraphics;
	using namespace F4R_AA;

	inline std::uintptr_t GetAEAddr(std::uintptr_t a_aeID, std::ptrdiff_t a_subOffset)
	{
		return REL::Relocation{ REL::Id<>{ a_aeID } }.GetAddress() + a_subOffset;
	}

	inline std::uintptr_t GetOGAddr(std::uintptr_t a_ogRVA)
	{
		return REL::Module::GetSingleton()->GetBaseAddress() + a_ogRVA;
	}

	inline std::uintptr_t ResolveAddr(std::uintptr_t a_ogRVA, std::uintptr_t a_aeID, std::ptrdiff_t a_aeSub)
	{
		if (IsAE() || IsNG())
			return GetAEAddr(a_aeID, a_aeSub);
		return GetOGAddr(a_ogRVA);
	}

#if F4R_HAS_FSR3
	inline bool IsFSR3Mode()
	{
		return AntiAliasing::GetSingleton().settings.iAAMode == static_cast<int32_t>(AAMode::FSR3);
	}
#endif

	inline void LogHookResult(const char* a_name, std::uintptr_t a_original)
	{
		if (a_original)
			REX::LogDebug("{} installed", a_name);
		else
			REX::LogWarning("{} FAILED", a_name);
	}

	using TemporalAA_IsActiveFunc = bool(ImageSpaceEffectTemporalAA*);
	TemporalAA_IsActiveFunc* g_originalTemporalAA_IsActive = nullptr;

	bool Hook_TemporalAA_IsActive(ImageSpaceEffectTemporalAA* a_this)
	{
		auto& aa = AntiAliasing::GetSingleton();
		if (aa.aaEnabled) {
			return false;
		}
		return g_originalTemporalAA_IsActive(a_this);
	}

	using PreRender_UpdateFunc = void(RenderTargetManager*, void*, void*, void*, void*);
	PreRender_UpdateFunc* g_originalPreRender_Update = nullptr;

	void Hook_PreRender_Update(RenderTargetManager* a_this, void* a_p2, void* a_p3, void* a_p4, void* a_p5)
	{
		g_originalPreRender_Update(a_this, a_p2, a_p3, a_p4, a_p5);
		AntiAliasing::GetSingleton().Update();
	}

	using PostRender_ApplyAAFunc = void(RenderTargetManager*, bool);
	PostRender_ApplyAAFunc* g_originalPostRender_ApplyAA = nullptr;

	void Hook_PostRender_ApplyAA(RenderTargetManager* a_this, bool a_p2)
	{
		g_originalPostRender_ApplyAA(a_this, a_p2);

		auto& aa = AntiAliasing::GetSingleton();
		aa.Apply();

		aa.savedWidthRatio = GetDynWidthRatio(*a_this);
		aa.savedHeightRatio = GetDynHeightRatio(*a_this);
		GetDynWidthRatio(*a_this) = 1.0f;
		GetDynHeightRatio(*a_this) = 1.0f;
		GetDynResActivated(*a_this) = false;
	}

	using SamplerOverride_BeginFunc = void(void*);
	SamplerOverride_BeginFunc* g_originalSamplerOverride_Begin = nullptr;

	void Hook_SamplerOverride_Begin(void* a_this)
	{
		AntiAliasing::GetSingleton().OverrideSamplerStates();
		g_originalSamplerOverride_Begin(a_this);
		AntiAliasing::GetSingleton().ResetSamplerStates();
	}

	using SamplerOverride_EndFunc = void(void*);
	SamplerOverride_EndFunc* g_originalSamplerOverride_End = nullptr;

	void Hook_SamplerOverride_End(void* a_this)
	{
		AntiAliasing::GetSingleton().OverrideSamplerStates();
		g_originalSamplerOverride_End(a_this);
		AntiAliasing::GetSingleton().ResetSamplerStates();

#if F4R_HAS_FSR3
		auto& aa = AntiAliasing::GetSingleton();
		if (IsFSR3Mode() && aa.aaEnabled && aa.fidelityFX) {
			aa.fidelityFX->GenerateReactiveMask();
		}
#endif
	}

#if F4R_HAS_FSR3
	using Capture_OpaqueFunc = void(BSShaderAccumulator*);
	Capture_OpaqueFunc* g_originalCapture_Opaque = nullptr;

	void Hook_Capture_Opaque(BSShaderAccumulator* a_this)
	{
		g_originalCapture_Opaque(a_this);

		if (!IsFSR3Mode()) return;

		auto& aa = AntiAliasing::GetSingleton();
		if (!aa.aaEnabled || !aa.fidelityFX ||
			!aa.fidelityFX->colorOpaqueOnlyTexture ||
			!aa.fidelityFX->colorOpaqueOnlyTexture->resource)
			return;

		auto* rendererData = BSGraphics::RendererData::GetSingleton();
		if (!rendererData) return;

		auto* ctx = GetImmediateContext();
		if (!ctx) return;

		auto* rt4 = reinterpret_cast<ID3D11Texture2D*>(rendererData->renderTargets[F4R_AA::RenderTarget::kMainTemp].texture);
		if (!rt4) return;

		ctx->CopyResource(aa.fidelityFX->colorOpaqueOnlyTexture->resource, rt4);
	}
#endif

	using PreserveJitter_EffectsFunc = void(RenderTargetManager*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
	PreserveJitter_EffectsFunc* g_originalPreserveJitter_Effects = nullptr;

	void Hook_PreserveJitter_Effects(RenderTargetManager* a_this, std::uint32_t a_p2, std::uint32_t a_p3, std::uint32_t a_p4, std::uint32_t a_p5)
	{
		auto& state = State::GetSingleton();
		float savedX = state.offsetX;
		float savedY = state.offsetY;

		g_originalPreserveJitter_Effects(a_this, a_p2, a_p3, a_p4, a_p5);

		state.offsetX = savedX;
		state.offsetY = savedY;
	}

	using Loading_ClearHistoryFunc = void();
	Loading_ClearHistoryFunc* g_originalLoading_ClearHistory = nullptr;

	void Hook_Loading_ClearHistory()
	{
		g_originalLoading_ClearHistory();

		auto& rtMgr = RenderTargetManager::GetSingleton();
		GetDynWidthRatio(rtMgr) = 1.0f;
		GetDynHeightRatio(rtMgr) = 1.0f;
		GetDynResActivated(rtMgr) = false;
		AntiAliasing::GetSingleton().RequestReset();
	}

	using Frame_ImagespaceEntryFunc = void(void*);
	Frame_ImagespaceEntryFunc* g_originalFrame_ImagespaceEntry = nullptr;

	void Hook_Frame_ImagespaceEntry(void* a_this)
	{
		g_originalFrame_ImagespaceEntry(a_this);

		auto& rtMgr = RenderTargetManager::GetSingleton();
		auto& aa = AntiAliasing::GetSingleton();
		GetDynWidthRatio(rtMgr) = aa.savedWidthRatio;
		GetDynHeightRatio(rtMgr) = aa.savedHeightRatio;
		GetDynResActivated(rtMgr) =
			(aa.savedWidthRatio != 1.0f || aa.savedHeightRatio != 1.0f);
	}

	std::unordered_map<ID3D11SamplerState*, ID3D11SamplerState*> g_samplerCache;

	ID3D11SamplerState* EnsureCappedSampler(ID3D11SamplerState* a_original)
	{
		if (!a_original) return nullptr;
		auto it = g_samplerCache.find(a_original);
		if (it != g_samplerCache.end()) return it->second;

		D3D11_SAMPLER_DESC desc;
		a_original->GetDesc(&desc);
		if (desc.Filter == D3D11_FILTER_ANISOTROPIC && desc.MaxAnisotropy > 8) {
			desc.MaxAnisotropy = 8;
			auto* device = GetRenderer();
			ID3D11SamplerState* capped = nullptr;
			if (device && SUCCEEDED(device->CreateSamplerState(&desc, &capped))) {
				g_samplerCache[a_original] = capped;
				return capped;
			}
		}
		g_samplerCache[a_original] = a_original;
		return a_original;
	}

	using PSSetSamplersFunc = void(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
	PSSetSamplersFunc* g_originalPSSetSamplers = nullptr;
	bool g_contextHooksInstalled = false;

	void Hook_PSSetSamplers(ID3D11DeviceContext* a_ctx, UINT a_startSlot, UINT a_numSamplers, ID3D11SamplerState* const* a_samplers)
	{
		ID3D11SamplerState* capped[16] = {};
		bool changed = false;
		for (UINT i = 0; i < a_numSamplers && i < 16; i++) {
			capped[i] = EnsureCappedSampler(a_samplers[i]);
			if (capped[i] != a_samplers[i]) changed = true;
		}
		g_originalPSSetSamplers(a_ctx, a_startSlot, a_numSamplers, changed ? capped : a_samplers);
	}
}

namespace F4R_AA
{
	void InstallContextHooks()
	{
		if (g_contextHooksInstalled) return;
		auto* ctx = GetImmediateContext();
		if (!ctx) return;

		auto vtableAddr = reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(ctx));
		auto result = Detours::X64::DetourVTable(vtableAddr,
			reinterpret_cast<uintptr_t>(&Hook_PSSetSamplers), 13);
		g_originalPSSetSamplers = reinterpret_cast<PSSetSamplersFunc*>(result);
		g_contextHooksInstalled = true;
		REX::LogDebug("PSSetSamplers hook installed");
	}

	void AntiAliasing::InstallHooks()
	{
		REX::LogDebug("Installing hooks...");

		{
			std::uintptr_t vtableAddr;
			if (IsAE() || IsNG()) {
				vtableAddr = REL::Relocation{ REL::Id<>{ 0x1473CC } }.GetAddress();
			} else {
				vtableAddr = REL::Relocation<std::uintptr_t>{ RE::VTABLE::ImageSpaceEffectTemporalAA[0] }.GetAddress();
			}
			auto result = Detours::X64::DetourVTable(vtableAddr, reinterpret_cast<std::uintptr_t>(&Hook_TemporalAA_IsActive), 8);
			g_originalTemporalAA_IsActive = reinterpret_cast<TemporalAA_IsActiveFunc*>(result);
			LogHookResult("TemporalAA_IsActive", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x14b, 0x235FF1, 0x29F);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_PreRender_Update));
			g_originalPreRender_Update = reinterpret_cast<PreRender_UpdateFunc*>(result);
			LogHookResult("PreRender_Update", result);
		}

		{
			auto addr = ResolveAddr(0x2857110 + 0xe1, 0x235FF2, 0xC5);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_PostRender_ApplyAA));
			g_originalPostRender_ApplyAA = reinterpret_cast<PostRender_ApplyAAFunc*>(result);
			LogHookResult("PostRender_ApplyAA", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x17f, 0x235FF1, 0x2E3);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SamplerOverride_Begin));
			g_originalSamplerOverride_Begin = reinterpret_cast<SamplerOverride_BeginFunc*>(result);
			LogHookResult("SamplerOverride_Begin", result);
		}

		{
			auto addr = ResolveAddr(0x2857480 + 0x1c9, 0x235FF1, 0x3A6);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_SamplerOverride_End));
			g_originalSamplerOverride_End = reinterpret_cast<SamplerOverride_EndFunc*>(result);
			LogHookResult("SamplerOverride_End", result);
		}

#if F4R_HAS_FSR3
		{
			auto addr = ResolveAddr(0x28568B0 + 0x1dc, 0x235FEB, 0x4C6);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_Capture_Opaque));
			g_originalCapture_Opaque = reinterpret_cast<Capture_OpaqueFunc*>(result);
			LogHookResult("Capture_Opaque", result);
		}
#endif

		{
			auto addr = ResolveAddr(0x2857110 + 0x9f, 0x235FF2, 0x83);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_PreserveJitter_Effects));
			g_originalPreserveJitter_Effects = reinterpret_cast<PreserveJitter_EffectsFunc*>(result);
			LogHookResult("PreserveJitter_Effects", result);
		}

		{
			auto addr = ResolveAddr(0x1297BE0 + 0x2bd, 0x225209, 0x275);
			auto result = REL::GetTrampoline()->WriteCall5(addr, reinterpret_cast<std::uintptr_t>(&Hook_Loading_ClearHistory));
			g_originalLoading_ClearHistory = reinterpret_cast<Loading_ClearHistoryFunc*>(result);
			LogHookResult("Loading_ClearHistory", result);
		}

		{
			auto addr = ResolveAddr(0x2857110, 0x235FF2, 0);
			auto result = Detours::X64::DetourFunction(addr, reinterpret_cast<std::uintptr_t>(&Hook_Frame_ImagespaceEntry));
			g_originalFrame_ImagespaceEntry = reinterpret_cast<Frame_ImagespaceEntryFunc*>(result);
			LogHookResult("Frame_ImagespaceEntry", result);
		}

		REX::LogDebug("All hooks installed");
	}
}
