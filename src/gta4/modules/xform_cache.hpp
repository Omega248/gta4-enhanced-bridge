#pragma once
#include <d3d9.h>
#include <cstring>
#include "d3d9_stats.hpp"

// GTA IV Enhanced - redundant transform filter.
// The compatibility mod sets VIEW and PROJECTION (and often an unchanged WORLD) before every draw.
// Under RTX Remix each of those is a command sent across the 32->64-bit bridge, so skipping
// re-sends of an identical matrix removes thousands of bridge commands per frame.
// The cache is cleared every BeginScene and on device Reset, so a stale entry can live at most
// one frame even if something sets transforms behind our back (e.g. a state block).
namespace gta4::xform_cache
{
	struct slot_t { D3DMATRIX m; bool valid; };
	inline slot_t g_slots[3] = {};
	inline std::atomic<std::uint64_t> g_skipped{ 0 };
	inline std::atomic<bool> g_enabled{ true };

	inline int slot_of(const D3DTRANSFORMSTATETYPE t)
	{
		return t == D3DTS_WORLD ? 0 : t == D3DTS_VIEW ? 1 : t == D3DTS_PROJECTION ? 2 : -1;
	}

	inline void invalidate() { for (auto& s : g_slots) s.valid = false; }

	// record a transform that was sent by someone else (keeps the cache truthful)
	inline void note(const D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m)
	{
		const int s = slot_of(t);
		if (s < 0) return;
		if (m) { g_slots[s].m = *m; g_slots[s].valid = true; } else g_slots[s].valid = false;
	}

	inline HRESULT set(IDirect3DDevice9* dev, const D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m)
	{
		const int s = slot_of(t);
		if (s >= 0 && m && g_enabled.load(std::memory_order_relaxed))
		{
			if (g_slots[s].valid && std::memcmp(&g_slots[s].m, m, sizeof(D3DMATRIX)) == 0)
			{
				g_skipped.fetch_add(1, std::memory_order_relaxed);
				return D3D_OK;
			}
			g_slots[s].m = *m; g_slots[s].valid = true;
		}
		else if (s >= 0) g_slots[s].valid = false;
		return dev->SetTransform(t, m);
	}
}
