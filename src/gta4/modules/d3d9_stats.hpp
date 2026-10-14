#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <windows.h>

// GTA IV Enhanced - per-frame D3D9 call statistics.
// Every call the game makes on the device goes through d3d9ex's wrapper and from there over the
// RTX Remix bridge (32-bit client -> 64-bit server). This counts the calls and measures the time
// the game's render thread spends inside each kind, so we can see how much of a frame is bridge
// overhead. Reported by claude_bridge in status.json ("d3d").

namespace gta4::d3d9_stats
{
#define GTA4_D3D9_STAT_LIST(X) \
	X(DrawIndexedPrimitive) X(DrawPrimitive) X(DrawPrimitiveUP) X(DrawIndexedPrimitiveUP) \
	X(SetTexture) X(SetRenderState) X(SetSamplerState) X(SetTextureStageState) \
	X(SetVertexShaderConstantF) X(SetPixelShaderConstantF) X(SetVertexShaderConstantI) X(SetPixelShaderConstantI) \
	X(SetVertexShaderConstantB) X(SetPixelShaderConstantB) \
	X(SetStreamSource) X(SetStreamSourceFreq) X(SetIndices) X(SetVertexDeclaration) X(SetFVF) \
	X(SetVertexShader) X(SetPixelShader) X(SetTransform) X(SetRenderTarget) X(SetDepthStencilSurface) \
	X(SetViewport) X(SetScissorRect) X(Clear) X(BeginScene) X(EndScene) X(StretchRect) \
	X(CreateTexture) X(CreateVertexBuffer) X(CreateIndexBuffer) X(UpdateSurface) X(UpdateTexture) \
	X(GetRenderTargetData) X(CreateQuery) X(GetRenderState) X(GetTexture) X(GetTransform)

	enum id : int {
#define GTA4_STAT_ENUM(n) n,
		GTA4_D3D9_STAT_LIST(GTA4_STAT_ENUM)
#undef GTA4_STAT_ENUM
		COUNT
	};

	inline const char* name(int i)
	{
		static const char* names[] = {
#define GTA4_STAT_NAME(n) #n,
			GTA4_D3D9_STAT_LIST(GTA4_STAT_NAME)
#undef GTA4_STAT_NAME
		};
		return i >= 0 && i < COUNT ? names[i] : "?";
	}

	inline std::atomic<std::uint64_t> g_count[COUNT];
	inline std::atomic<std::uint64_t> g_ticks[COUNT];
	inline std::atomic<std::uint64_t> g_frames{ 0 };     // EndScene calls
	inline std::atomic<std::uint64_t> g_draw_prims{ 0 }; // primitives submitted
	inline std::atomic<bool> g_enabled{ true };

	inline std::uint64_t now_ticks()
	{
		LARGE_INTEGER t; QueryPerformanceCounter(&t);
		return static_cast<std::uint64_t>(t.QuadPart);
	}

	inline double ticks_to_ms(std::uint64_t t)
	{
		static const double inv = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / static_cast<double>(f.QuadPart); }();
		return static_cast<double>(t) * inv;
	}

	struct scope
	{
		int m_id; std::uint64_t m_start;
		explicit scope(int id) : m_id(id), m_start(g_enabled.load(std::memory_order_relaxed) ? now_ticks() : 0) {}
		~scope()
		{
			if (!m_start) return;
			g_count[m_id].fetch_add(1, std::memory_order_relaxed);
			g_ticks[m_id].fetch_add(now_ticks() - m_start, std::memory_order_relaxed);
		}
	};

	// Snapshot-and-reset; returns a JSON object with per-frame averages since the last call.
	inline std::string take_json(double wall_ms)
	{
		const std::uint64_t frames = g_frames.exchange(0);
		const std::uint64_t prims = g_draw_prims.exchange(0);
		std::uint64_t c[COUNT], t[COUNT], total_calls = 0, total_ticks = 0;
		for (int i = 0; i < COUNT; ++i)
		{
			c[i] = g_count[i].exchange(0); t[i] = g_ticks[i].exchange(0);
			total_calls += c[i];
			// draw-call scopes include the compatibility mod's own per-draw work; Begin/EndScene include its frame work
			total_ticks += t[i];
		}
		const double f = frames ? static_cast<double>(frames) : 1.0;
		char buf[256];
		sprintf_s(buf, "{\"frames\":%llu,\"fps\":%.1f,\"wall_ms\":%.0f,\"calls_per_frame\":%.0f,\"prims_per_frame\":%.0f,\"d3d_ms_per_frame\":%.2f,\"top\":[",
			frames, wall_ms > 0 ? frames * 1000.0 / wall_ms : 0.0, wall_ms, total_calls / f, prims / f, ticks_to_ms(total_ticks) / f);
		std::string j = buf;

		// top 12 by time
		int order[COUNT]; for (int i = 0; i < COUNT; ++i) order[i] = i;
		for (int a = 0; a < COUNT; ++a) for (int b = a + 1; b < COUNT; ++b) if (t[order[b]] > t[order[a]]) std::swap(order[a], order[b]);
		bool first = true;
		for (int k = 0; k < 12 && k < COUNT; ++k)
		{
			const int i = order[k];
			if (!c[i]) continue;
			sprintf_s(buf, "%s[\"%s\",%.0f,%.3f]", first ? "" : ",", name(i), c[i] / f, ticks_to_ms(t[i]) / f);
			j += buf; first = false;
		}
		j += "]}";
		return j;
	}
}
