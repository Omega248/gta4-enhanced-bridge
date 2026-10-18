#pragma once
// GTA IV Enhanced - scene probe (phase 0 of docs/SCENE_SUBMISSION.md).
// Read-only statistics about what the game draws each frame: entities, models, unique geometry,
// textures and shader types per D3D9 draw. Sizes the work for submitting the scene through the
// Remix API. Off by default (bridge: probe on=1); never changes rendering.
#include <d3d9.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace gta4::scene_probe
{
	void set_enabled(bool on);
	bool enabled();

	// one D3D9 indexed draw; entity is the CEntity the game is drawing (static-world path) or null
	void on_draw(IDirect3DDevice9* dev, const void* entity, std::uint32_t model_hash, std::uint32_t entity_type,
		bool entity_fixed, std::string_view shader, UINT start_index, UINT prims);

	// frame boundary (the game issues one EndScene per frame)
	void on_end_scene();

	// per-frame averages since the last call, as a JSON object; "{}" when disabled
	std::string take_json();
}
