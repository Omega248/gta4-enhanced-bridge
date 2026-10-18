#include "std_include.hpp"
#include "scene_probe.hpp"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

namespace gta4::scene_probe
{
	namespace
	{
		std::atomic<bool> g_enabled{ false };

		struct geom_key
		{
			const void* vb; const void* ib; UINT offset; UINT start; UINT prims;
			bool operator==(const geom_key& o) const { return vb == o.vb && ib == o.ib && offset == o.offset && start == o.start && prims == o.prims; }
		};
		struct geom_hash
		{
			size_t operator()(const geom_key& k) const
			{
				return std::hash<const void*>()(k.vb) ^ (std::hash<const void*>()(k.ib) << 1) ^ (size_t(k.start) * 2654435761u) ^ (size_t(k.prims) << 7) ^ (size_t(k.offset) << 13);
			}
		};
		struct shader_stat { std::uint64_t draws = 0, entity_draws = 0; };

		// current frame (render thread only)
		struct frame_t
		{
			std::uint32_t draws = 0, entity_draws = 0, model_draws = 0, vs_draws = 0, fixed_entity_draws = 0;
			std::unordered_set<const void*> entities, textures;
			std::unordered_set<std::uint32_t> models;
			std::unordered_set<geom_key, geom_hash> geoms, entity_geoms;
			std::uint32_t entity_type_draws[16] = {};
		} g_frame;

		// accumulated over finished frames, read by take_json (bridge thread)
		std::mutex g_mtx;
		double g_frames = 0;
		double g_sum[10] = {};   // per-frame sums in the order of names[] in take_json
		double g_type_sum[16] = {};
		std::unordered_map<std::string, shader_stat> g_shaders;   // summed over frames
		std::unordered_map<std::string, shader_stat> g_frame_shaders;
	}

	void set_enabled(bool on) { g_enabled = on; }
	bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

	void on_draw(IDirect3DDevice9* dev, const void* entity, std::uint32_t model_hash, std::uint32_t entity_type,
		bool entity_fixed, std::string_view shader, UINT start_index, UINT prims)
	{
		if (!enabled() || !dev) return;
		auto& f = g_frame;
		++f.draws;

		IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, stride = 0; dev->GetStreamSource(0, &vb, &off, &stride);
		IDirect3DIndexBuffer9* ib = nullptr; dev->GetIndices(&ib);
		IDirect3DBaseTexture9* t0 = nullptr; dev->GetTexture(0, &t0);
		IDirect3DVertexShader9* vs = nullptr; dev->GetVertexShader(&vs);
		const geom_key gk{ vb, ib, off, start_index, prims };
		f.geoms.insert(gk);
		if (t0) f.textures.insert(t0);
		if (vs) ++f.vs_draws;
		if (vb) vb->Release();
		if (ib) ib->Release();
		if (t0) t0->Release();
		if (vs) vs->Release();

		if (model_hash) { ++f.model_draws; f.models.insert(model_hash); }
		if (entity)
		{
			++f.entity_draws;
			f.entities.insert(entity);
			f.entity_geoms.insert(gk);
			f.entity_type_draws[entity_type & 15u]++;
			if (entity_fixed) ++f.fixed_entity_draws;
		}

		auto& s = g_frame_shaders[std::string(shader.empty() ? std::string_view("?") : shader)];
		++s.draws;
		if (entity) ++s.entity_draws;
	}

	void on_end_scene()
	{
		if (!enabled()) return;
		auto& f = g_frame;
		{
			std::lock_guard lock(g_mtx);
			g_frames += 1;
			const double v[10] = { double(f.draws), double(f.entity_draws), double(f.model_draws), double(f.vs_draws), double(f.fixed_entity_draws),
				double(f.entities.size()), double(f.models.size()), double(f.geoms.size()), double(f.entity_geoms.size()), double(f.textures.size()) };
			for (int i = 0; i < 10; ++i) g_sum[i] += v[i];
			for (int i = 0; i < 16; ++i) g_type_sum[i] += f.entity_type_draws[i];
			for (auto& [k, s] : g_frame_shaders) { auto& t = g_shaders[k]; t.draws += s.draws; t.entity_draws += s.entity_draws; }
		}
		g_frame_shaders.clear();
		f.draws = f.entity_draws = f.model_draws = f.vs_draws = f.fixed_entity_draws = 0;
		f.entities.clear(); f.textures.clear(); f.models.clear(); f.geoms.clear(); f.entity_geoms.clear();
		std::fill(std::begin(f.entity_type_draws), std::end(f.entity_type_draws), 0u);
	}

	std::string take_json()
	{
		if (!enabled()) return "{}";
		std::lock_guard lock(g_mtx);
		const double n = g_frames > 0 ? g_frames : 1;
		static const char* names[10] = { "draws", "entity_draws", "model_draws", "vs_draws", "fixed_entity_draws",
			"entities", "models", "geoms", "entity_geoms", "textures" };
		std::string j = "{\"frames\":" + std::to_string(int(g_frames));
		char buf[96];
		for (int i = 0; i < 10; ++i) { sprintf_s(buf, ",\"%s\":%.1f", names[i], g_sum[i] / n); j += buf; }
		j += ",\"entity_type_draws\":[";
		for (int i = 0; i < 16; ++i) { sprintf_s(buf, "%s%.1f", i ? "," : "", g_type_sum[i] / n); j += buf; }
		j += "],\"shaders\":[";
		std::vector<std::pair<std::string, shader_stat>> v(g_shaders.begin(), g_shaders.end());
		std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.draws > b.second.draws; });
		for (size_t i = 0; i < v.size() && i < 25; ++i)
		{
			std::string name;
			for (char c : v[i].first) name += (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 32) ? '_' : c;
			sprintf_s(buf, ",%.1f,%.1f]", v[i].second.draws / n, v[i].second.entity_draws / n);
			j += std::string(i ? "," : "") + "[\"" + name + "\"" + buf;
		}
		j += "]}";
		g_frames = 0;
		std::fill(std::begin(g_sum), std::end(g_sum), 0.0);
		std::fill(std::begin(g_type_sum), std::end(g_type_sum), 0.0);
		g_shaders.clear();
		return j;
	}
}
