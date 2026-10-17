#pragma once
// GTA IV Enhanced - player features (not graphics):
//   * photo mode   : free camera independent of Niko, time freeze, HUD toggle, screenshots
//   * save anywhere: quick-save key opens the game's own save menu (all of its slots) outside missions
//   * HUD modes    : full / minimal (radar only when driving or wanted) / off
//   * weather-aware city: traffic and crowds follow time of day AND weather, with smooth transitions
//   * player menu  : F7 opens a clean "GTA IV Enhanced" window (the F4 developer menu stays separate)
// Settings live in <game folder>\GTA4Enhanced.ini and are saved when changed from the menu.

namespace gta4::enhanced
{
	enum class hud_mode : int { full = 0, minimal = 1, off = 2 };

	struct settings_t
	{
		// keys (virtual-key codes)
		int key_menu = VK_F7;
		int key_photo = VK_F8;
		int key_quicksave = VK_F5;

		// photo mode
		float cam_speed = 6.0f;          // metres per second
		float cam_mouse_sens = 0.12f;    // degrees per mouse unit
		float cam_max_distance = 250.0f; // from the player (keeps the world streamed in)
		bool  photo_freeze_time = true;  // freeze the world when entering photo mode
		bool  photo_hide_hud = true;
		bool  photo_hide_player = false;

		// saves
		bool  save_allow_wanted = false; // allow quick-save while wanted
		bool  save_allow_in_car = true;

		// HUD
		hud_mode hud = hud_mode::full;

		// city
		bool  living_city = true;        // time-of-day traffic / crowds
		bool  weather_city = true;       // weather affects them too
		float traffic_scale = 1.0f;      // user multipliers on top
		float crowd_scale = 1.0f;

		// performance: resend unchanged static draws only every k-th frame (0/1 = off). Remix must keep
		// instances alive longer than k frames: rtx.numFramesToKeepInstances / numFramesToKeepBLAS > k in rtx.conf
		int   retain_k = 4;
	};

	settings_t& settings();
	void load_settings();
	void save_settings();

	// game thread, every frame (natives are safe)
	void on_frame();

	// window message hook (key edges for the menu toggle); returns true if the key was consumed
	bool on_key(UINT msg, WPARAM wparam);

	// ImGui
	inline bool menu_open = false;
	void draw_menu();               // the F7 player window
	void draw_tab_contents();       // same contents, used by the developer menu's "Enhanced" tab

	// game-thread entry points used by bridge commands ("photo on=", "quicksave")
	void bridge_photo(bool on);
	void bridge_quicksave();

	// status for the menu / bridge
	bool photo_active();
	const char* last_message();
}
