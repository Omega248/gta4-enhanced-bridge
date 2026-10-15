#pragma once
// GTA IV Enhanced - redundant-state filter in front of the RTX Remix bridge.
// Originally generated from d3d9ex.hpp; now maintained by hand.
//
// Every IDirect3DDevice9 call is a command sent across the 32-bit -> 64-bit Remix bridge.
// GTA IV and the compatibility mod re-set a lot of state that is already set (same texture, same
// render state, same shader constants...). This device sits between them and the bridge and drops
// any Set* call whose value equals what the bridge already has. Device state is identical either way.
//
// Safety: if the game ever uses state blocks (which change state behind our back) the filter
// clears itself every BeginScene; Reset clears it too. The filter can be switched off live.
#include <d3d9.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <string>
#include <cstdio>

namespace gta4
{
	class dedupe_device final : public IDirect3DDevice9
	{
	public:
		enum stat : int { S_RS, S_SS, S_TSS, S_TEX, S_VB, S_IB, S_SH, S_DECL, S_XF, S_CONST, S_COUNT };
		static inline std::atomic<std::uint64_t> g_skipped[S_COUNT];
		static inline std::atomic<bool> g_enabled{ false }; // off by default: no measured FPS gain and the game uses state blocks
		static inline std::atomic<bool> g_saw_state_blocks{ false };

		// --- what actually crosses the bridge, per render target ("pass") -------------------------
		struct pass_bucket { char name[40]; std::atomic<std::uint64_t> draws{ 0 }, prims{ 0 }, shader_draws{ 0 }, nocolor_draws{ 0 }; };
		static constexpr int MAX_PASS = 24;
		static inline pass_bucket g_pass[MAX_PASS];
		static inline std::atomic<int> g_pass_n{ 0 };
		static inline std::atomic<bool> g_pass_stats{ true };
		static int pass_index(const char* name)
		{
			const int n = g_pass_n.load();
			for (int i = 0; i < n; ++i) if (!std::strcmp(g_pass[i].name, name)) return i;
			if (n >= MAX_PASS) return MAX_PASS - 1;
			strncpy_s(g_pass[n].name, name, _TRUNCATE); g_pass_n.store(n + 1); return n;
		}
		// [name, draws/frame, prims/frame, draws with a shader bound, draws with colour writes off]
		static std::string pass_json(double frames)
		{
			std::string j = "["; const int n = g_pass_n.load(); bool first = true; char b[160];
			const double f = frames > 0 ? frames : 1.0;
			for (int i = 0; i < n; ++i)
			{
				const auto d = g_pass[i].draws.exchange(0), p = g_pass[i].prims.exchange(0), s = g_pass[i].shader_draws.exchange(0), c = g_pass[i].nocolor_draws.exchange(0);
				if (!d) continue;
				sprintf_s(b, "%s[\"%s\",%.0f,%.0f,%.0f,%.0f]", first ? "" : ",", g_pass[i].name, d / f, p / f, s / f, c / f);
				j += b; first = false;
			}
			return j + "]";
		}

		// --- retained static world ("round-robin resubmission") ------------------------------------
		// Remix keeps a ray-traced instance alive for rtx.numFramesToKeepInstances frames after its last
		// draw (and indefinitely outside the frustum with rtx.antiCulling.object). So a static object that
		// was drawn identically for a couple of frames does not need to be re-sent every frame: we send
		// each one only every K-th frame (staggered by its hash) and drop the other draws before they cross
		// the bridge. Requires rtx.numFramesToKeepInstances / numFramesToKeepBLAS > K on the Remix side.
		// Only fixed-function draws (no shader bound) with a static world matrix and static buffers qualify;
		// peds (skinned), moving vehicles, particles and anything new or changed are always sent.
		static inline std::atomic<bool> g_retain{ false };
		static inline std::atomic<int> g_retain_k{ 4 };
		static inline std::atomic<std::uint64_t> g_retain_skipped{ 0 }, g_retain_sent_static{ 0 }, g_retain_sent_other{ 0 };
		static inline std::atomic<std::uint32_t> g_retain_tracked{ 0 };
		static std::string retain_json(double frames)
		{
			const double f = frames > 0 ? frames : 1.0; char b[200];
			sprintf_s(b, "{\"on\":%s,\"k\":%d,\"skipped\":%.0f,\"sent_static\":%.0f,\"sent_other\":%.0f,\"tracked\":%u}",
				g_retain.load() ? "true" : "false", g_retain_k.load(), g_retain_skipped.exchange(0) / f,
				g_retain_sent_static.exchange(0) / f, g_retain_sent_other.exchange(0) / f, g_retain_tracked.load());
			return b;
		}

		// time spent inside the bridge client (m_real) for draws and Present, to split the render-thread
		// cost between the compatibility mod's per-draw work and the bridge/Remix side
		static inline std::atomic<std::uint64_t> g_bt_draw{ 0 }, g_bt_present{ 0 }, g_bt_set{ 0 };
		static inline std::atomic<bool> g_bt_on{ true };
		static inline std::atomic<bool> g_bt_wrap_swapchain{ true };
		static std::int64_t qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
		static std::string bridge_time_json(double frames)
		{
			LARGE_INTEGER f; QueryPerformanceFrequency(&f);
			const double k = 1000.0 / static_cast<double>(f.QuadPart) / (frames > 0 ? frames : 1.0); char b[160];
			sprintf_s(b, "{\"draw_ms\":%.2f,\"present_ms\":%.2f,\"setters_ms\":%.2f}",
				g_bt_draw.exchange(0) * k, g_bt_present.exchange(0) * k, g_bt_set.exchange(0) * k);
			return b;
		}
		struct bt_scope { std::atomic<std::uint64_t>& a; std::int64_t t0; explicit bt_scope(std::atomic<std::uint64_t>& x) : a(x), t0(g_bt_on.load(std::memory_order_relaxed) ? qpc() : 0) {}
			~bt_scope() { if (t0) a.fetch_add(static_cast<std::uint64_t>(qpc() - t0), std::memory_order_relaxed); } };

		explicit dedupe_device(IDirect3DDevice9* real) : m_real(real) { invalidate(); m_pass = pass_index("backbuffer"); }
		IDirect3DDevice9* real() const { return m_real; }

		void invalidate()
		{
			m_rs.clear(); m_ss.clear(); m_ts.clear(); m_tex.clear();
			for (auto& v : m_vb) v.valid = false;
			m_ib_valid = m_vs_valid = m_ps_valid = m_decl_valid = m_fvf_valid = false;
			for (auto& x : m_xf) x.valid = false;
			m_vsc.clear(); m_psc.clear();
		}

		HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObj) override
		{ const HRESULT hr = m_real->QueryInterface(riid, ppvObj); if (hr == S_OK && ppvObj && *ppvObj == m_real) *ppvObj = this; return hr; }

		ULONG __stdcall AddRef() override
		{ return m_real->AddRef(); }

		ULONG __stdcall Release() override
		{ return m_real->Release(); }

		HRESULT __stdcall TestCooperativeLevel() override
		{ return m_real->TestCooperativeLevel(); }

		UINT __stdcall GetAvailableTextureMem() override
		{ return m_real->GetAvailableTextureMem(); }

		HRESULT __stdcall EvictManagedResources() override
		{ return m_real->EvictManagedResources(); }

		HRESULT __stdcall GetDirect3D(IDirect3D9** ppD3D9) override
		{ return m_real->GetDirect3D(ppD3D9); }

		HRESULT __stdcall GetDeviceCaps(D3DCAPS9* pCaps) override
		{ return m_real->GetDeviceCaps(pCaps); }

		HRESULT __stdcall GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) override
		{ return m_real->GetDisplayMode(iSwapChain, pMode); }

		HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override
		{ return m_real->GetCreationParameters(pParameters); }

		HRESULT __stdcall SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override
		{ return m_real->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }

		BOOL __stdcall ShowCursor(BOOL bShow) override
		{ return m_real->ShowCursor(bShow); }

		HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) override
		{ return m_real->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain); }

		// the game presents through the swap chain: hand out a thin wrapper that times Present
		struct timed_swapchain final : IDirect3DSwapChain9
		{
			IDirect3DSwapChain9* r; IDirect3DDevice9* dev; ULONG refs = 1;
			timed_swapchain(IDirect3DSwapChain9* real, IDirect3DDevice9* d) : r(real), dev(d) {}
			HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) override
			{ if (riid == __uuidof(IDirect3DSwapChain9) || riid == __uuidof(IUnknown)) { *ppv = this; AddRef(); return S_OK; } return r->QueryInterface(riid, ppv); }
			ULONG __stdcall AddRef() override { r->AddRef(); return ++refs; }
			ULONG __stdcall Release() override { const ULONG n = --refs; r->Release(); if (!n) delete this; return n; }
			HRESULT __stdcall Present(CONST RECT* a, CONST RECT* b, HWND c, CONST RGNDATA* d, DWORD e) override
			{ bt_scope _t(g_bt_present); return r->Present(a, b, c, d, e); }
			HRESULT __stdcall GetFrontBufferData(IDirect3DSurface9* p) override { return r->GetFrontBufferData(p); }
			HRESULT __stdcall GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** p) override { return r->GetBackBuffer(i, t, p); }
			HRESULT __stdcall GetRasterStatus(D3DRASTER_STATUS* p) override { return r->GetRasterStatus(p); }
			HRESULT __stdcall GetDisplayMode(D3DDISPLAYMODE* p) override { return r->GetDisplayMode(p); }
			HRESULT __stdcall GetDevice(IDirect3DDevice9** p) override { if (!p) return D3DERR_INVALIDCALL; dev->AddRef(); *p = dev; return D3D_OK; }
			HRESULT __stdcall GetPresentParameters(D3DPRESENT_PARAMETERS* p) override { return r->GetPresentParameters(p); }
		};

		HRESULT __stdcall GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override
		{
			const HRESULT hr = m_real->GetSwapChain(iSwapChain, pSwapChain);
			if (SUCCEEDED(hr) && pSwapChain && *pSwapChain && g_bt_wrap_swapchain.load()) *pSwapChain = new timed_swapchain(*pSwapChain, this);
			return hr;
		}

		UINT __stdcall GetNumberOfSwapChains() override
		{ return m_real->GetNumberOfSwapChains(); }

		HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override
		{ invalidate(); return m_real->Reset(pPresentationParameters); }

		HRESULT __stdcall Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) override
		{ on_present(); bt_scope _t(g_bt_present); return m_real->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion); }

		HRESULT __stdcall GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override
		{ return m_real->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }

		HRESULT __stdcall GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override
		{ return m_real->GetRasterStatus(iSwapChain, pRasterStatus); }

		HRESULT __stdcall SetDialogBoxMode(BOOL bEnableDialogs) override
		{ return m_real->SetDialogBoxMode(bEnableDialogs); }

		HRESULT __stdcall CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override
		{ return m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle); }

		HRESULT __stdcall CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) override
		{ return m_real->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }

		HRESULT __stdcall CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override
		{ return m_real->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }

		HRESULT __stdcall CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override
		{ return m_real->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }

		HRESULT __stdcall CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override
		{ return m_real->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }

		HRESULT __stdcall CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override
		{ return m_real->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }

		HRESULT __stdcall CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override
		{ return m_real->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle); }

		HRESULT __stdcall UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) override
		{ return m_real->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }

		HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) override
		{ return m_real->UpdateTexture(pSourceTexture, pDestinationTexture); }

		HRESULT __stdcall GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override
		{ return m_real->GetRenderTargetData(pRenderTarget, pDestSurface); }

		HRESULT __stdcall GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override
		{ return m_real->GetFrontBufferData(iSwapChain, pDestSurface); }

		HRESULT __stdcall StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) override
		{ return m_real->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter); }

		HRESULT __stdcall ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) override
		{ return m_real->ColorFill(pSurface, pRect, color); }

		HRESULT __stdcall CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override
		{ return m_real->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }

		HRESULT __stdcall SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override
		{
			if (RenderTargetIndex == 0 && g_pass_stats.load(std::memory_order_relaxed))
			{
				char nm[40] = "none";
				D3DSURFACE_DESC d{};
				if (pRenderTarget && SUCCEEDED(pRenderTarget->GetDesc(&d))) sprintf_s(nm, "%ux%u f%u", d.Width, d.Height, static_cast<unsigned>(d.Format));
				m_pass = pass_index(nm);
			}
			return m_real->SetRenderTarget(RenderTargetIndex, pRenderTarget);
		}

		HRESULT __stdcall GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override
		{ return m_real->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }

		HRESULT __stdcall SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override
		{ return m_real->SetDepthStencilSurface(pNewZStencil); }

		HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override
		{ return m_real->GetDepthStencilSurface(ppZStencilSurface); }

		HRESULT __stdcall BeginScene() override
		{ if (m_saw_state_blocks) invalidate(); return m_real->BeginScene(); }

		HRESULT __stdcall EndScene() override
		{ return m_real->EndScene(); }

		HRESULT __stdcall Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override
		{ return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil); }

		HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) override
		{ const int s = State == D3DTS_WORLD ? 0 : State == D3DTS_VIEW ? 1 : State == D3DTS_PROJECTION ? 2 : -1;
			if (s >= 0 && pMatrix) { if (on() && m_xf[s].valid && !std::memcmp(&m_xf[s].m, pMatrix, sizeof(D3DMATRIX))) return skip(S_XF); m_xf[s].m = *pMatrix; m_xf[s].valid = true; }
			return m_real->SetTransform(State, pMatrix); }

		HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override
		{ return m_real->GetTransform(State, pMatrix); }

		HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) override
		{ const int s = State == D3DTS_WORLD ? 0 : State == D3DTS_VIEW ? 1 : State == D3DTS_PROJECTION ? 2 : -1; if (s >= 0) m_xf[s].valid = false; return m_real->MultiplyTransform(State, pMatrix); }

		HRESULT __stdcall SetViewport(CONST D3DVIEWPORT9* pViewport) override
		{ return m_real->SetViewport(pViewport); }

		HRESULT __stdcall GetViewport(D3DVIEWPORT9* pViewport) override
		{ return m_real->GetViewport(pViewport); }

		HRESULT __stdcall SetMaterial(CONST D3DMATERIAL9* pMaterial) override
		{ return m_real->SetMaterial(pMaterial); }

		HRESULT __stdcall GetMaterial(D3DMATERIAL9* pMaterial) override
		{ return m_real->GetMaterial(pMaterial); }

		HRESULT __stdcall SetLight(DWORD Index, CONST D3DLIGHT9* pLight) override
		{ return m_real->SetLight(Index, pLight); }

		HRESULT __stdcall GetLight(DWORD Index, D3DLIGHT9* pLight) override
		{ return m_real->GetLight(Index, pLight); }

		HRESULT __stdcall LightEnable(DWORD Index, BOOL Enable) override
		{ return m_real->LightEnable(Index, Enable); }

		HRESULT __stdcall GetLightEnable(DWORD Index, BOOL* pEnable) override
		{ return m_real->GetLightEnable(Index, pEnable); }

		HRESULT __stdcall SetClipPlane(DWORD Index, CONST float* pPlane) override
		{ return m_real->SetClipPlane(Index, pPlane); }

		HRESULT __stdcall GetClipPlane(DWORD Index, float* pPlane) override
		{ return m_real->GetClipPlane(Index, pPlane); }

		HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override
		{ if (State == D3DRS_COLORWRITEENABLE) m_colorwrite = Value;
			if (on() && m_rs.same(State, Value)) return skip(S_RS); m_rs.put(State, Value); { bt_scope _t(g_bt_set); return m_real->SetRenderState(State, Value); } }

		HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override
		{ return m_real->GetRenderState(State, pValue); }

		HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override
		{ note_state_blocks(); return m_real->CreateStateBlock(Type, ppSB); }

		HRESULT __stdcall BeginStateBlock() override
		{ note_state_blocks(); return m_real->BeginStateBlock(); }

		HRESULT __stdcall EndStateBlock(IDirect3DStateBlock9** ppSB) override
		{ note_state_blocks(); return m_real->EndStateBlock(ppSB); }

		HRESULT __stdcall SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus) override
		{ return m_real->SetClipStatus(pClipStatus); }

		HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override
		{ return m_real->GetClipStatus(pClipStatus); }

		HRESULT __stdcall GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override
		{ return m_real->GetTexture(Stage, ppTexture); }

		HRESULT __stdcall SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) override
		{ if (on() && m_tex.same(Stage, reinterpret_cast<std::uintptr_t>(pTexture))) return skip(S_TEX); m_tex.put(Stage, reinterpret_cast<std::uintptr_t>(pTexture)); { bt_scope _t(g_bt_set); return m_real->SetTexture(Stage, pTexture); } }

		HRESULT __stdcall GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override
		{ return m_real->GetTextureStageState(Stage, Type, pValue); }

		HRESULT __stdcall SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override
		{ const std::uint64_t k = (std::uint64_t(Stage) << 32) | Type; if (on() && m_ts.same(k, Value)) return skip(S_TSS); m_ts.put(k, Value); return m_real->SetTextureStageState(Stage, Type, Value); }

		HRESULT __stdcall GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override
		{ return m_real->GetSamplerState(Sampler, Type, pValue); }

		HRESULT __stdcall SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override
		{ const std::uint64_t k = (std::uint64_t(Sampler) << 32) | Type; if (on() && m_ss.same(k, Value)) return skip(S_SS); m_ss.put(k, Value); return m_real->SetSamplerState(Sampler, Type, Value); }

		HRESULT __stdcall ValidateDevice(DWORD* pNumPasses) override
		{ return m_real->ValidateDevice(pNumPasses); }

		HRESULT __stdcall SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries) override
		{ return m_real->SetPaletteEntries(PaletteNumber, pEntries); }

		HRESULT __stdcall GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) override
		{ return m_real->GetPaletteEntries(PaletteNumber, pEntries); }

		HRESULT __stdcall SetCurrentTexturePalette(UINT PaletteNumber) override
		{ return m_real->SetCurrentTexturePalette(PaletteNumber); }

		HRESULT __stdcall GetCurrentTexturePalette(UINT *PaletteNumber) override
		{ return m_real->GetCurrentTexturePalette(PaletteNumber); }

		HRESULT __stdcall SetScissorRect(CONST RECT* pRect) override
		{ return m_real->SetScissorRect(pRect); }

		HRESULT __stdcall GetScissorRect(RECT* pRect) override
		{ return m_real->GetScissorRect(pRect); }

		HRESULT __stdcall SetSoftwareVertexProcessing(BOOL bSoftware) override
		{ return m_real->SetSoftwareVertexProcessing(bSoftware); }

		BOOL __stdcall GetSoftwareVertexProcessing() override
		{ return m_real->GetSoftwareVertexProcessing(); }

		HRESULT __stdcall SetNPatchMode(float nSegments) override
		{ return m_real->SetNPatchMode(nSegments); }

		HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override
		{ note_draw(PrimitiveCount); bt_scope _t(g_bt_draw); return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); }

		HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override
		{ if (g_retain.load(std::memory_order_relaxed) && retain_skip(BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)) return D3D_OK;
			note_draw(primCount); bt_scope _t(g_bt_draw); return m_real->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); }

		HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override
		{ note_draw(PrimitiveCount); return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }

		HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override
		{ note_draw(PrimitiveCount); return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }

		HRESULT __stdcall ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override
		{ return m_real->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }

		HRESULT __stdcall CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) override
		{ return m_real->CreateVertexDeclaration(pVertexElements, ppDecl); }

		HRESULT __stdcall SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override
		{ if (on() && m_decl_valid && m_decl == pDecl) return skip(S_DECL); m_decl = pDecl; m_decl_valid = true; m_fvf_valid = false; return m_real->SetVertexDeclaration(pDecl); }

		HRESULT __stdcall GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override
		{ return m_real->GetVertexDeclaration(ppDecl); }

		HRESULT __stdcall SetFVF(DWORD FVF) override
		{ if (on() && m_fvf_valid && m_fvf == FVF) return skip(S_DECL); m_fvf = FVF; m_fvf_valid = true; m_decl_valid = false; return m_real->SetFVF(FVF); }

		HRESULT __stdcall GetFVF(DWORD* pFVF) override
		{ return m_real->GetFVF(pFVF); }

		HRESULT __stdcall CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) override
		{ return m_real->CreateVertexShader(pFunction, ppShader); }

		HRESULT __stdcall SetVertexShader(IDirect3DVertexShader9* pShader) override
		{ if (on() && m_vs_valid && m_vs == pShader) return skip(S_SH); m_vs = pShader; m_vs_valid = true; return m_real->SetVertexShader(pShader); }

		HRESULT __stdcall GetVertexShader(IDirect3DVertexShader9** ppShader) override
		{ return m_real->GetVertexShader(ppShader); }

		HRESULT __stdcall SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) override
		{ if (on() && m_vsc.same(StartRegister, pConstantData, Vector4fCount)) return skip(S_CONST); m_vsc.put(StartRegister, pConstantData, Vector4fCount); { bt_scope _t(g_bt_set); return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); } }

		HRESULT __stdcall GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override
		{ return m_real->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }

		HRESULT __stdcall SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) override
		{ return m_real->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }

		HRESULT __stdcall GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override
		{ return m_real->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }

		HRESULT __stdcall SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount) override
		{ return m_real->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }

		HRESULT __stdcall GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override
		{ return m_real->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }

		HRESULT __stdcall SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) override
		{ if (StreamNumber < 16 && on() && m_vb[StreamNumber].valid && m_vb[StreamNumber].vb == pStreamData && m_vb[StreamNumber].off == OffsetInBytes && m_vb[StreamNumber].stride == Stride) return skip(S_VB);
			if (StreamNumber < 16) m_vb[StreamNumber] = { pStreamData, OffsetInBytes, Stride, true }; { bt_scope _t(g_bt_set); return m_real->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); } }

		HRESULT __stdcall GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes, UINT* pStride) override
		{ return m_real->GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride); }

		HRESULT __stdcall SetStreamSourceFreq(UINT StreamNumber, UINT Divider) override
		{ return m_real->SetStreamSourceFreq(StreamNumber, Divider); }

		HRESULT __stdcall GetStreamSourceFreq(UINT StreamNumber, UINT* Divider) override
		{ return m_real->GetStreamSourceFreq(StreamNumber, Divider); }

		HRESULT __stdcall SetIndices(IDirect3DIndexBuffer9* pIndexData) override
		{ if (on() && m_ib_valid && m_ib == pIndexData) return skip(S_IB); m_ib = pIndexData; m_ib_valid = true; { bt_scope _t(g_bt_set); return m_real->SetIndices(pIndexData); } }

		HRESULT __stdcall GetIndices(IDirect3DIndexBuffer9** ppIndexData) override
		{ return m_real->GetIndices(ppIndexData); }

		HRESULT __stdcall CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) override
		{ return m_real->CreatePixelShader(pFunction, ppShader); }

		HRESULT __stdcall SetPixelShader(IDirect3DPixelShader9* pShader) override
		{ if (on() && m_ps_valid && m_ps == pShader) return skip(S_SH); m_ps = pShader; m_ps_valid = true; return m_real->SetPixelShader(pShader); }

		HRESULT __stdcall GetPixelShader(IDirect3DPixelShader9** ppShader) override
		{ return m_real->GetPixelShader(ppShader); }

		HRESULT __stdcall SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) override
		{ if (on() && m_psc.same(StartRegister, pConstantData, Vector4fCount)) return skip(S_CONST); m_psc.put(StartRegister, pConstantData, Vector4fCount); { bt_scope _t(g_bt_set); return m_real->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); } }

		HRESULT __stdcall GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override
		{ return m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }

		HRESULT __stdcall SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) override
		{ return m_real->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }

		HRESULT __stdcall GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override
		{ return m_real->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }

		HRESULT __stdcall SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount) override
		{ return m_real->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }

		HRESULT __stdcall GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override
		{ return m_real->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }

		HRESULT __stdcall DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo) override
		{ return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }

		HRESULT __stdcall DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo) override
		{ return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }

		HRESULT __stdcall DeletePatch(UINT Handle) override
		{ return m_real->DeletePatch(Handle); }

		HRESULT __stdcall CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override
		{ return m_real->CreateQuery(Type, ppQuery); }

		void __stdcall SetCursorPosition(int X, int Y, DWORD Flags) override
		{ m_real->SetCursorPosition(X, Y, Flags); }

		void __stdcall SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp) override
		{ m_real->SetGammaRamp(iSwapChain, Flags, pRamp); }

		void __stdcall GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) override
		{ m_real->GetGammaRamp(iSwapChain, pRamp); }

		float __stdcall GetNPatchMode() override
		{ return m_real->GetNPatchMode(); }

	private:
		template <typename K, typename V> struct kv
		{
			std::unordered_map<K, V> m;
			bool same(K k, V v) const { const auto it = m.find(k); return it != m.end() && it->second == v; }
			void put(K k, V v) { m[k] = v; }
			void clear() { m.clear(); }
		};
		struct consts
		{
			float v[256][4]; bool valid[256];
			void clear() { std::memset(valid, 0, sizeof(valid)); }
			bool same(UINT start, const float* d, UINT n) const
			{
				if (!d || start + n > 256) return false;
				for (UINT i = 0; i < n; ++i) if (!valid[start + i] || std::memcmp(v[start + i], d + i * 4, 16)) return false;
				return true;
			}
			void put(UINT start, const float* d, UINT n)
			{
				if (!d) return;
				for (UINT i = 0; i < n && start + i < 256; ++i) { std::memcpy(v[start + i], d + i * 4, 16); valid[start + i] = true; }
			}
		};
		struct vb_t { IDirect3DVertexBuffer9* vb; UINT off; UINT stride; bool valid; };
		struct xf_t { D3DMATRIX m; bool valid; };

		bool on() const { return g_enabled.load(std::memory_order_relaxed); }
		struct retain_entry { std::uint32_t last_frame; std::uint32_t streak; };
		std::unordered_map<std::uint64_t, retain_entry> m_retain;
		std::unordered_map<const void*, bool> m_static_res;   // buffer/texture -> may be retained (not dynamic, not a render target)
		std::uint32_t m_frame_no = 1;

		static std::uint64_t fnv(std::uint64_t h, const void* p, size_t n)
		{
			const auto* b = static_cast<const unsigned char*>(p);
			for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
			return h;
		}

		bool res_static_vb(IDirect3DVertexBuffer9* vb)
		{
			if (!vb) return false;
			const auto it = m_static_res.find(vb); if (it != m_static_res.end()) return it->second;
			D3DVERTEXBUFFER_DESC d{}; const bool ok = SUCCEEDED(vb->GetDesc(&d)) && !(d.Usage & D3DUSAGE_DYNAMIC);
			m_static_res[vb] = ok; return ok;
		}
		bool res_static_ib(IDirect3DIndexBuffer9* ib)
		{
			if (!ib) return false;
			const auto it = m_static_res.find(ib); if (it != m_static_res.end()) return it->second;
			D3DINDEXBUFFER_DESC d{}; const bool ok = SUCCEEDED(ib->GetDesc(&d)) && !(d.Usage & D3DUSAGE_DYNAMIC);
			m_static_res[ib] = ok; return ok;
		}
		bool res_static_tex(IDirect3DBaseTexture9* t)
		{
			if (!t) return true;
			const auto it = m_static_res.find(t); if (it != m_static_res.end()) return it->second;
			bool ok = true;
			if (t->GetType() == D3DRTYPE_TEXTURE)
			{
				D3DSURFACE_DESC d{};
				if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) ok = !(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC | D3DUSAGE_DEPTHSTENCIL));
			}
			else ok = false;   // cube/volume textures: leave alone
			m_static_res[t] = ok; return ok;
		}

		void on_present()
		{
			++m_frame_no;
			if ((m_frame_no & 255) == 0)
			{
				for (auto it = m_retain.begin(); it != m_retain.end(); ) it = (m_frame_no - it->second.last_frame > 120) ? m_retain.erase(it) : std::next(it);
				m_static_res.clear();          // re-check resources now and then (pointers get reused after streaming)
				g_retain_tracked.store(static_cast<std::uint32_t>(m_retain.size()));
			}
		}

		// true = drop this draw (Remix still holds the identical instance from a recent frame).
		// All inputs are read back from the bridge client's own device state (local, no IPC) so that
		// state blocks, which change state without passing through this wrapper, cannot fool it.
		bool retain_skip(INT base, UINT minv, UINT numv, UINT start, UINT prims)
		{
			const auto other = [] { g_retain_sent_other.fetch_add(1, std::memory_order_relaxed); return false; };
			struct rel { IUnknown* p = nullptr; ~rel() { if (p) p->Release(); } };

			IDirect3DVertexShader9* vs = nullptr; m_real->GetVertexShader(&vs); rel r_vs{ vs };
			IDirect3DPixelShader9* ps = nullptr; m_real->GetPixelShader(&ps); rel r_ps{ ps };
			if (vs || ps) return other();                                   // shader draws: skinned peds, special effects

			IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, stride = 0; m_real->GetStreamSource(0, &vb, &off, &stride); rel r_vb{ vb };
			IDirect3DIndexBuffer9* ib = nullptr; m_real->GetIndices(&ib); rel r_ib{ ib };
			if (!res_static_vb(vb) || !res_static_ib(ib)) return other();     // dynamic buffers: water, trails, particles

			IDirect3DBaseTexture9* t0 = nullptr; m_real->GetTexture(0, &t0); rel r_t0{ t0 };
			IDirect3DBaseTexture9* t1 = nullptr; m_real->GetTexture(1, &t1); rel r_t1{ t1 };
			if (!res_static_tex(t0) || !res_static_tex(t1)) return other();   // render-target or dynamic textures

			D3DMATRIX world{}; if (FAILED(m_real->GetTransform(D3DTS_WORLD, &world))) return other();
			IDirect3DVertexDeclaration9* decl = nullptr; m_real->GetVertexDeclaration(&decl); rel r_decl{ decl };
			DWORD fvf = 0; m_real->GetFVF(&fvf);

			std::uint64_t h = 1469598103934665603ull;
			const void* ptrs[6] = { vb, ib, t0, t1, decl, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(fvf)) };
			h = fnv(h, ptrs, sizeof(ptrs));
			const UINT nums[8] = { off, stride, static_cast<UINT>(base), minv, numv, start, prims, static_cast<UINT>(m_pass) };
			h = fnv(h, nums, sizeof(nums));
			h = fnv(h, &world, sizeof(D3DMATRIX));
			// per-draw data the compatibility mod passes to its Remix runtime through unused render states
			// (texture category, modifiers, forced hash, emissive scale, decal contrast, wetness, animated
			// UVs, hash seed). Animated UVs change every frame, so those draws never count as unchanged.
			static constexpr DWORD k_custom_rs[] = { 42, 149, 150, 169, 177, 210, 211, 212, 213, 214, 215, 216, 220 };
			DWORD rs[sizeof(k_custom_rs) / sizeof(DWORD)] = {};
			for (size_t i = 0; i < sizeof(k_custom_rs) / sizeof(DWORD); ++i) m_real->GetRenderState(static_cast<D3DRENDERSTATETYPE>(k_custom_rs[i]), &rs[i]);
			h = fnv(h, rs, sizeof(rs));

			auto& e = m_retain[h];
			if (e.last_frame == m_frame_no) { g_retain_sent_static.fetch_add(1, std::memory_order_relaxed); return false; }   // repeated within a frame: send
			e.streak = (e.last_frame + 1 == m_frame_no) ? e.streak + 1 : 0;
			e.last_frame = m_frame_no;
			const int k = g_retain_k.load(std::memory_order_relaxed);
			if (e.streak >= 2 && k > 1 && ((m_frame_no + static_cast<std::uint32_t>(h >> 40)) % static_cast<std::uint32_t>(k)) != 0)
			{
				g_retain_skipped.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			g_retain_sent_static.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		void note_draw(UINT prims)
		{
			if (!g_pass_stats.load(std::memory_order_relaxed)) return;
			auto& b = g_pass[m_pass];
			b.draws.fetch_add(1, std::memory_order_relaxed); b.prims.fetch_add(prims, std::memory_order_relaxed);
			if (m_vs || m_ps) b.shader_draws.fetch_add(1, std::memory_order_relaxed);
			if ((m_colorwrite & 0xF) == 0) b.nocolor_draws.fetch_add(1, std::memory_order_relaxed);
		}
		static HRESULT skip(stat s) { g_skipped[s].fetch_add(1, std::memory_order_relaxed); return D3D_OK; }
		void note_state_blocks() { m_saw_state_blocks = true; g_saw_state_blocks = true; invalidate(); }

		IDirect3DDevice9* m_real;
		bool m_saw_state_blocks = false;
		int m_pass = 0;
		DWORD m_colorwrite = 0xF;
		kv<DWORD, DWORD> m_rs;
		kv<std::uint64_t, DWORD> m_ss, m_ts;
		kv<DWORD, std::uintptr_t> m_tex;
		vb_t m_vb[16] = {};
		IDirect3DIndexBuffer9* m_ib = nullptr; bool m_ib_valid = false;
		IDirect3DVertexShader9* m_vs = nullptr; bool m_vs_valid = false;
		IDirect3DPixelShader9* m_ps = nullptr; bool m_ps_valid = false;
		IDirect3DVertexDeclaration9* m_decl = nullptr; bool m_decl_valid = false;
		DWORD m_fvf = 0; bool m_fvf_valid = false;
		xf_t m_xf[3] = {};
		consts m_vsc = {}, m_psc = {};
	};
}
