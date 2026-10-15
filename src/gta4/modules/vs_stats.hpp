#pragma once
// GTA IV Enhanced - which draws still reach Remix with a vertex shader bound (= vertex capture cost),
// per shader preset and per render category. Read by claude_bridge (status.json "vs").
#include <atomic>
#include <cstdint>
#include <string>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <string_view>
#include <cstring>

namespace gta4::vs_stats
{
	enum cat : int { C_STATIC, C_VEHICLE, C_INSTANCE, C_FX, C_SKY, C_WATER, C_MIRROR, C_OTHER, C_COUNT };
	inline const char* cat_name[C_COUNT] = { "static", "vehicle", "instance", "fx", "sky", "water", "mirror", "other" };
	inline std::atomic<std::uint32_t> g_cat[C_COUNT];
	inline std::atomic<std::uint32_t> g_cat_ff[C_COUNT];
	inline std::atomic<std::uint32_t> g_preset[256];
	inline std::atomic<std::uint32_t> g_prims[256];
	inline char g_preset_name[256][40] = {};

	inline void track(int category, int preset, std::string_view name, unsigned prims, bool ff)
	{
		if (ff) { g_cat_ff[category].fetch_add(1, std::memory_order_relaxed); return; }
		g_cat[category].fetch_add(1, std::memory_order_relaxed);
		const int p = preset & 255;
		g_preset[p].fetch_add(1, std::memory_order_relaxed); g_prims[p].fetch_add(prims, std::memory_order_relaxed);
		if (!g_preset_name[p][0] && !name.empty()) { const size_t n = std::min<size_t>(name.size(), 39); std::memcpy(g_preset_name[p], name.data(), n); g_preset_name[p][n] = 0; }
	}

	inline std::string take_json(double frames)
	{
		const double f = frames > 0 ? frames : 1.0; char b[200];
		std::string j = "{\"vs\":{";
		for (int i = 0; i < C_COUNT; ++i) { sprintf_s(b, "%s\"%s\":%.0f", i ? "," : "", cat_name[i], g_cat[i].exchange(0) / f); j += b; }
		j += "},\"ff\":{";
		for (int i = 0; i < C_COUNT; ++i) { sprintf_s(b, "%s\"%s\":%.0f", i ? "," : "", cat_name[i], g_cat_ff[i].exchange(0) / f); j += b; }
		j += "},\"presets\":[";
		std::vector<std::pair<std::uint32_t, int>> v;
		std::uint32_t pr[256];
		for (int i = 0; i < 256; ++i) { const auto n = g_preset[i].exchange(0); pr[i] = g_prims[i].exchange(0); if (n) v.emplace_back(n, i); }
		std::sort(v.rbegin(), v.rend());
		for (size_t k = 0; k < v.size() && k < 16; ++k)
		{
			const int i = v[k].second; const char* nm = g_preset_name[i][0] ? g_preset_name[i] : "?";
			std::string safe; for (const char* c = nm; *c && safe.size() < 40; ++c) safe += (*c == '"' || *c == '\\') ? '_' : *c;
			sprintf_s(b, "%s[%d,\"%s\",%.0f,%.0f]", k ? "," : "", i, safe.c_str(), v[k].first / f, pr[i] / f); j += b;
		}
		return j + "]}";
	}
}
