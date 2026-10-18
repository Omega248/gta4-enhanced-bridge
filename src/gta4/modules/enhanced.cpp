#include "std_include.hpp"
#include "enhanced.hpp"
#include "natives.hpp"
#include "claude_bridge.hpp"
#include "imgui.hpp"
#include <fstream>
#include <sstream>
#include <cmath>
#include <shlobj.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace gta4
{
	// implemented in claude_bridge.cpp (window capture that also works while other windows cover the game)
	bool claude_bridge_screenshot_bmp(const std::filesystem::path& out, int& w, int& h);
}

namespace gta4::enhanced
{
	namespace
	{
		settings_t g_settings;
		bool g_loaded = false;
		std::string g_message;
		ULONGLONG g_message_until = 0;

		// ------------------------------------------------------------------ helpers

		std::filesystem::path ini_path()
		{
			wchar_t exe[MAX_PATH] = {};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			return std::filesystem::path(exe).parent_path() / "GTA4Enhanced.ini";
		}

		void show(const std::string& text, uint32_t ms = 3000)
		{
			g_message = text;
			g_message_until = GetTickCount64() + ms;
			if (natives::is_initialized())
			{
				static char gxt[] = "STRING";
				static std::string buf; buf = text;
				natives::get()->PrintStringWithLiteralStringNow(gxt, buf.data(), ms, true);
			}
		}

		bool game_has_focus()
		{
			return shared::globals::main_window && GetForegroundWindow() == shared::globals::main_window;
		}

		// key edge detection that only fires while the game window has focus and no text box is active
		struct key_edge
		{
			bool down = false;
			bool pressed(int vk)
			{
				const bool now = game_has_focus() && !shared::globals::imgui_wants_text_input && (GetAsyncKeyState(vk) & 0x8000) != 0;
				const bool edge = now && !down;
				down = now;
				return edge;
			}
		};

		bool held(int vk) { return game_has_focus() && (GetAsyncKeyState(vk) & 0x8000) != 0; }

		natives::Player player() { const auto n = natives::get(); return n->ConvertIntToPlayerindex(n->GetPlayerId()); }
		natives::Ped player_ped() { natives::Ped p = 0; natives::get()->GetPlayerChar(player(), &p); return p; }

		uint32_t wanted_level() { uint32_t w = 0; natives::get()->StoreWantedLevel(player(), &w); return w; }

		const char* hud_names[] = { "Full", "Minimal (radar only when driving or wanted)", "Off (cinematic)" };

		// ------------------------------------------------------------------ photo mode

		struct photo_state
		{
			bool active = false;
			natives::Camera cam = 0;
			float x = 0, y = 0, z = 0;      // position
			float pitch = 0, roll = 0, heading = 0;
			float fov = 50.0f;
			bool frozen = false;
			bool hud_hidden = false;
			bool player_hidden = false;
			ULONGLONG last_tick = 0;
			int shot_pending = 0;           // frames until the screenshot is taken (HUD needs a frame to vanish)
			key_edge k_freeze, k_hud, k_shot, k_reset, k_player;
		} g_photo;

		int g_hud_last = -1;   // last applied HUD state (-1 = force re-apply)

		void apply_time_freeze(bool on)
		{
			natives::get()->SetTimeScale(on ? 0.001f : 1.0f);
			g_photo.frozen = on;
		}

		void apply_hud(bool visible)
		{
			const auto n = natives::get();
			n->DisplayHud(visible);
			n->DisplayRadar(visible);
		}

		void photo_enter()
		{
			const auto n = natives::get();
			natives::Camera game_cam = 0;
			n->GetGameCam(&game_cam);
			if (game_cam)
			{
				n->GetCamPos(game_cam, &g_photo.x, &g_photo.y, &g_photo.z);
				float rx = 0, ry = 0, rz = 0;
				n->GetCamRot(game_cam, &rx, &ry, &rz);
				g_photo.pitch = rx; g_photo.roll = 0.0f; g_photo.heading = rz;
				n->GetCamFov(game_cam, &g_photo.fov);
				if (g_photo.fov < 5.0f || g_photo.fov > 120.0f) g_photo.fov = 50.0f;
			}
			else
			{
				n->GetCharCoordinates(player_ped(), &g_photo.x, &g_photo.y, &g_photo.z);
				g_photo.z += 1.5f;
			}

			n->CreateCam(14, &g_photo.cam);
			n->SetCamPos(g_photo.cam, g_photo.x, g_photo.y, g_photo.z);
			n->SetCamRot(g_photo.cam, g_photo.pitch, g_photo.roll, g_photo.heading);
			n->SetCamFov(g_photo.cam, g_photo.fov);
			n->SetCamActive(g_photo.cam, true);
			n->SetCamPropagate(g_photo.cam, true);
			n->ActivateScriptedCams(true, true);
			n->SetPlayerControl(player(), false);

			g_photo.active = true;
			g_photo.last_tick = GetTickCount64();
			if (g_settings.photo_freeze_time) apply_time_freeze(true);
			if (g_settings.photo_hide_hud) { apply_hud(false); g_photo.hud_hidden = true; }
			if (g_settings.photo_hide_player) { n->SetCharVisible(player_ped(), false); g_photo.player_hidden = true; }

			show("Photo mode - WASD move, mouse look, Q/E down/up, Shift fast, Ctrl slow, Z/X zoom, [ ] tilt, "
				 "T freeze, H HUD, N hide Niko, R reset, SPACE take photo, F8 exit", 7000);
		}

		void photo_exit()
		{
			const auto n = natives::get();
			if (g_photo.cam)
			{
				n->SetCamActive(g_photo.cam, false);
				n->SetCamPropagate(g_photo.cam, false);
				n->ActivateScriptedCams(false, false);
				n->DestroyCam(g_photo.cam);
				g_photo.cam = 0;
			}
			if (g_photo.frozen) apply_time_freeze(false);
			if (g_photo.hud_hidden) { g_photo.hud_hidden = false; }
			if (g_photo.player_hidden) { n->SetCharVisible(player_ped(), true); g_photo.player_hidden = false; }
			n->SetPlayerControl(player(), true);
			g_photo.active = false;
			// let the HUD logic restore the HUD for the selected mode
			apply_hud(g_settings.hud != hud_mode::off);
			g_hud_last = -1;
		}

		bool save_jpeg(const std::filesystem::path& bmp, const std::filesystem::path& jpg)
		{
			static ULONG_PTR token = 0;
			if (!token) { Gdiplus::GdiplusStartupInput in; if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) return false; }

			UINT num = 0, size = 0;
			Gdiplus::GetImageEncodersSize(&num, &size);
			if (!size) return false;
			std::vector<uint8_t> buf(size);
			auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
			Gdiplus::GetImageEncoders(num, size, codecs);
			const CLSID* clsid = nullptr;
			for (UINT i = 0; i < num; ++i) if (wcscmp(codecs[i].MimeType, L"image/jpeg") == 0) { clsid = &codecs[i].Clsid; break; }
			if (!clsid) return false;

			bool ok = false;
			{
				Gdiplus::Bitmap img(bmp.wstring().c_str());
				Gdiplus::EncoderParameters params = {};
				ULONG quality = 95;
				params.Count = 1;
				params.Parameter[0].Guid = Gdiplus::EncoderQuality;
				params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
				params.Parameter[0].NumberOfValues = 1;
				params.Parameter[0].Value = &quality;
				ok = img.Save(jpg.wstring().c_str(), clsid, &params) == Gdiplus::Ok;
			}
			std::error_code ec;
			if (ok) std::filesystem::remove(bmp, ec);
			return ok;
		}

		void take_photo()
		{
			PWSTR pics = nullptr;
			std::filesystem::path dir;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics)) && pics) { dir = std::filesystem::path(pics) / "GTA IV Enhanced"; }
			if (pics) CoTaskMemFree(pics);
			if (dir.empty()) return;
			std::error_code ec; std::filesystem::create_directories(dir, ec);

			SYSTEMTIME t; GetLocalTime(&t);
			char name[64];
			sprintf_s(name, "GTAIV_%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
			const auto bmp = dir / (std::string(name) + ".bmp");
			int w = 0, h = 0;
			if (!claude_bridge_screenshot_bmp(bmp, w, h)) { show("Photo failed (game window not visible?)"); return; }
			const auto jpg = dir / (std::string(name) + ".jpg");
			show(save_jpeg(bmp, jpg) ? "Photo saved to Pictures\\GTA IV Enhanced" : "Photo saved (BMP) to Pictures\\GTA IV Enhanced", 2500);
		}

		void photo_frame()
		{
			const auto n = natives::get();
			const ULONGLONG now = GetTickCount64();
			const float dt = std::clamp(static_cast<float>(now - g_photo.last_tick) / 1000.0f, 0.0f, 0.1f);
			g_photo.last_tick = now;

			if (g_photo.shot_pending > 0)
			{
				if (--g_photo.shot_pending == 0)
				{
					take_photo();
					if (!g_photo.hud_hidden) apply_hud(true);
				}
				return; // keep the camera perfectly still for the shot
			}

			// look: mouse (game's own mouse delta) + arrow keys
			int mx = 0, my = 0;
			n->GetMouseInput(&mx, &my);
			const float zoom_scale = g_photo.fov / 50.0f; // slower look when zoomed in
			g_photo.heading -= static_cast<float>(mx) * g_settings.cam_mouse_sens * zoom_scale;
			g_photo.pitch -= static_cast<float>(my) * g_settings.cam_mouse_sens * zoom_scale;
			const float key_turn = 70.0f * dt * zoom_scale;
			if (held(VK_LEFT)) g_photo.heading += key_turn;
			if (held(VK_RIGHT)) g_photo.heading -= key_turn;
			if (held(VK_UP)) g_photo.pitch += key_turn;
			if (held(VK_DOWN)) g_photo.pitch -= key_turn;
			g_photo.pitch = std::clamp(g_photo.pitch, -89.0f, 89.0f);
			g_photo.heading = std::fmod(g_photo.heading + 360.0f, 360.0f);
			if (held(VK_OEM_4)) g_photo.roll -= 30.0f * dt;   // [
			if (held(VK_OEM_6)) g_photo.roll += 30.0f * dt;   // ]
			if (held('Z')) g_photo.fov = std::max(5.0f, g_photo.fov - 25.0f * dt);
			if (held('X')) g_photo.fov = std::min(110.0f, g_photo.fov + 25.0f * dt);

			// move
			float speed = g_settings.cam_speed * (held(VK_SHIFT) ? 4.0f : 1.0f) * (held(VK_CONTROL) ? 0.2f : 1.0f);
			const float h = g_photo.heading * 0.01745329252f, p = g_photo.pitch * 0.01745329252f;
			const float fx = -std::sin(h) * std::cos(p), fy = std::cos(h) * std::cos(p), fz = std::sin(p);
			const float rx = std::cos(h), ry = std::sin(h);
			float mvx = 0, mvy = 0, mvz = 0;
			if (held('W')) { mvx += fx; mvy += fy; mvz += fz; }
			if (held('S')) { mvx -= fx; mvy -= fy; mvz -= fz; }
			if (held('D')) { mvx += rx; mvy += ry; }
			if (held('A')) { mvx -= rx; mvy -= ry; }
			if (held('E')) mvz += 1.0f;
			if (held('Q')) mvz -= 1.0f;
			g_photo.x += mvx * speed * dt; g_photo.y += mvy * speed * dt; g_photo.z += mvz * speed * dt;

			// keep within range of the player so the city stays streamed in
			float px = 0, py = 0, pz = 0;
			n->GetCharCoordinates(player_ped(), &px, &py, &pz);
			const float dx = g_photo.x - px, dy = g_photo.y - py, dz = g_photo.z - pz;
			const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (d > g_settings.cam_max_distance && d > 0.0f)
			{
				const float k = g_settings.cam_max_distance / d;
				g_photo.x = px + dx * k; g_photo.y = py + dy * k; g_photo.z = pz + dz * k;
			}

			// toggles
			if (g_photo.k_freeze.pressed('T')) { apply_time_freeze(!g_photo.frozen); show(g_photo.frozen ? "Time frozen" : "Time running", 1200); }
			if (g_photo.k_hud.pressed('H')) { g_photo.hud_hidden = !g_photo.hud_hidden; apply_hud(!g_photo.hud_hidden); }
			if (g_photo.k_player.pressed('N')) { g_photo.player_hidden = !g_photo.player_hidden; n->SetCharVisible(player_ped(), !g_photo.player_hidden); }
			if (g_photo.k_reset.pressed('R')) { g_photo.roll = 0.0f; g_photo.fov = 50.0f; }
			if (g_photo.k_shot.pressed(VK_SPACE)) { apply_hud(false); n->ClearPrints(); g_photo.shot_pending = 3; }

			n->SetCamPos(g_photo.cam, g_photo.x, g_photo.y, g_photo.z);
			n->SetCamRot(g_photo.cam, g_photo.pitch, g_photo.roll, g_photo.heading);
			n->SetCamFov(g_photo.cam, g_photo.fov);
		}

		// ------------------------------------------------------------------ save anywhere

		void quick_save()
		{
			const auto n = natives::get();
			if (g_photo.active) { show("Leave photo mode first"); return; }
			if (n->GetMissionFlag()) { show("Can't save during a mission"); return; }
			if (!g_settings.save_allow_wanted && wanted_level() > 0) { show("Can't save while wanted - lose the cops first"); return; }
			if (!g_settings.save_allow_in_car && n->IsCharInAnyCar(player_ped())) { show("Get out of the vehicle to save"); return; }
			if (n->IsCharDead(player_ped()) || n->IsScreenFadedOut()) return;
			n->ActivateSaveMenu();   // the game's own save screen - every save slot is available
		}

		// ------------------------------------------------------------------ HUD modes

		void hud_frame()
		{
			if (g_photo.active) return;
			int& s_last = g_hud_last;
			const auto n = natives::get();
			int want = 1; // 1 = radar on
			if (g_settings.hud == hud_mode::off) want = 0;
			else if (g_settings.hud == hud_mode::minimal) want = (n->IsCharInAnyCar(player_ped()) || wanted_level() > 0) ? 1 : 0;

			// only touch the HUD on changes so missions/cutscenes keep control of it
			const int state = static_cast<int>(g_settings.hud) * 2 + want;
			if (state != s_last)
			{
				n->DisplayRadar(want != 0);
				n->DisplayHud(g_settings.hud != hud_mode::off);
				s_last = state;
			}
		}

		// ------------------------------------------------------------------ weather-aware city

		void city_frame()
		{
			if (!g_settings.living_city || claude_bridge::density_overridden()) return;
			const auto n = natives::get();

			uint32_t hh = 12, mm = 0; n->GetTimeOfDay(&hh, &mm);
			uint32_t w = 0; n->GetCurrentWeather(&w);
			//                              0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15   16   17   18   19   20   21   22   23
			static const float car[24] = { .55f,.45f,.35f,.30f,.35f,.50f,.75f,1.f, 1.f, .90f,.85f,.85f,.90f,.90f,.85f,.90f,1.f, 1.f, 1.f, .90f,.80f,.75f,.70f,.65f };
			static const float ped[24] = { .55f,.45f,.30f,.25f,.25f,.35f,.60f,.90f,1.f, .90f,.85f,.90f,1.f, 1.f, .95f,.95f,1.f, 1.f, 1.f, .95f,.90f,.85f,.75f,.65f };
			const float t = static_cast<float>(mm) / 60.0f; const uint32_t h1 = (hh + 1) % 24; hh %= 24;
			float cd = car[hh] + (car[h1] - car[hh]) * t, pd = ped[hh] + (ped[h1] - ped[hh]) * t, sd = pd;

			if (g_settings.weather_city)
			{
				// GTA IV weather ids: 0 extra sunny, 1 sunny, 2 sunny+windy, 3 cloudy, 4 rain, 5 drizzle, 6 fog, 7 thunderstorm
				const bool night = hh >= 21 || hh < 6;
				switch (w)
				{
				case 2: pd *= 0.90f; sd *= 0.85f; break;                          // windy: fewer people lingering
				case 3: sd *= 0.90f; break;
				case 4: cd *= 0.85f; pd *= 0.50f; sd *= 0.30f; break;             // rain: people head indoors, cafes/benches empty
				case 5: cd *= 0.95f; pd *= 0.75f; sd *= 0.55f; break;             // drizzle
				case 6: cd *= 0.75f; pd *= 0.80f; sd *= 0.75f; break;             // fog: fewer drivers out
				case 7: cd *= 0.70f; pd *= 0.35f; sd *= 0.20f; break;             // storm
				default: if (!night && hh >= 11 && hh <= 18) sd *= 1.15f; break;   // nice afternoon: more people out
				}
				if (night && (w == 4 || w == 7)) { cd *= 0.85f; pd *= 0.8f; }
			}
			cd *= g_settings.traffic_scale; pd *= g_settings.crowd_scale; sd *= g_settings.crowd_scale;

			// ease towards the target so weather changes don't make the street pop
			static float s_cd = -1, s_pd = -1, s_sd = -1;
			static ULONGLONG s_last = 0;
			const ULONGLONG now = GetTickCount64();
			const float dt = s_last ? std::clamp(static_cast<float>(now - s_last) / 1000.0f, 0.0f, 0.5f) : 0.0f; s_last = now;
			const float k = std::clamp(dt * 0.1f, 0.0f, 1.0f);   // ~10 s to settle
			if (s_cd < 0) { s_cd = cd; s_pd = pd; s_sd = sd; }
			s_cd += (cd - s_cd) * k; s_pd += (pd - s_pd) * k; s_sd += (sd - s_sd) * k;

			n->SetCarDensityMultiplier(s_cd);
			n->SetPedDensityMultiplier(s_pd);
			n->SetScenarioPedDensityMultiplier(s_sd, s_sd);
		}

		// ------------------------------------------------------------------ ini

		void ini_write(std::ofstream& o, const char* k, float v) { o << k << " = " << v << "\n"; }
		void ini_write(std::ofstream& o, const char* k, int v) { o << k << " = " << v << "\n"; }
		void ini_write(std::ofstream& o, const char* k, bool v) { o << k << " = " << (v ? 1 : 0) << "\n"; }

		key_edge g_k_menu, g_k_photo, g_k_save;
	}

	settings_t& settings() { return g_settings; }
	bool photo_active() { return g_photo.active; }
	const char* last_message() { return GetTickCount64() < g_message_until ? g_message.c_str() : ""; }

	namespace
	{
		void apply_performance() { claude_bridge::set_retain(g_settings.retain_k); }
	}

	void load_settings()
	{
		g_loaded = true;
		std::ifstream in(ini_path());
		if (!in) { save_settings(); apply_performance(); return; }
		std::string line;
		while (std::getline(in, line))
		{
			const auto eq = line.find('=');
			if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[' || eq == std::string::npos) continue;
			auto trim = [](std::string s) { while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back(); while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin()); return s; };
			const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
			const float f = static_cast<float>(atof(v.c_str())); const int i = atoi(v.c_str());
			auto& s = g_settings;
			if (k == "key_menu") s.key_menu = i;
			else if (k == "key_photo") s.key_photo = i;
			else if (k == "key_quicksave") s.key_quicksave = i;
			else if (k == "cam_speed") s.cam_speed = f;
			else if (k == "cam_mouse_sens") s.cam_mouse_sens = f;
			else if (k == "cam_max_distance") s.cam_max_distance = f;
			else if (k == "photo_freeze_time") s.photo_freeze_time = i != 0;
			else if (k == "photo_hide_hud") s.photo_hide_hud = i != 0;
			else if (k == "photo_hide_player") s.photo_hide_player = i != 0;
			else if (k == "save_allow_wanted") s.save_allow_wanted = i != 0;
			else if (k == "save_allow_in_car") s.save_allow_in_car = i != 0;
			else if (k == "hud") s.hud = static_cast<hud_mode>(std::clamp(i, 0, 2));
			else if (k == "living_city") s.living_city = i != 0;
			else if (k == "weather_city") s.weather_city = i != 0;
			else if (k == "traffic_scale") s.traffic_scale = std::clamp(f, 0.0f, 2.0f);
			else if (k == "crowd_scale") s.crowd_scale = std::clamp(f, 0.0f, 2.0f);
			else if (k == "retain_k") s.retain_k = std::clamp(i, 0, 16);
		}
		claude_bridge::living_city = g_settings.living_city;
		apply_performance();
	}

	void save_settings()
	{
		std::ofstream o(ini_path(), std::ios::trunc);
		if (!o) return;
		const auto& s = g_settings;
		o << "; GTA IV Enhanced player settings (edit here or in the F7 menu)\n; keys are Windows virtual-key codes: F5=116 F7=118 F8=119\n[keys]\n";
		ini_write(o, "key_menu", s.key_menu); ini_write(o, "key_photo", s.key_photo); ini_write(o, "key_quicksave", s.key_quicksave);
		o << "[photo]\n";
		ini_write(o, "cam_speed", s.cam_speed); ini_write(o, "cam_mouse_sens", s.cam_mouse_sens); ini_write(o, "cam_max_distance", s.cam_max_distance);
		ini_write(o, "photo_freeze_time", s.photo_freeze_time); ini_write(o, "photo_hide_hud", s.photo_hide_hud); ini_write(o, "photo_hide_player", s.photo_hide_player);
		o << "[saves]\n";
		ini_write(o, "save_allow_wanted", s.save_allow_wanted); ini_write(o, "save_allow_in_car", s.save_allow_in_car);
		o << "[hud]\n; 0 = full, 1 = minimal, 2 = off\n";
		ini_write(o, "hud", static_cast<int>(s.hud));
		o << "[city]\n";
		ini_write(o, "living_city", s.living_city); ini_write(o, "weather_city", s.weather_city);
		ini_write(o, "traffic_scale", s.traffic_scale); ini_write(o, "crowd_scale", s.crowd_scale);
		o << "[performance]\n; resend unchanged static draws every k-th frame (0 = off); needs rtx.numFramesToKeepInstances/BLAS > k\n";
		ini_write(o, "retain_k", s.retain_k);
	}

	bool on_key(UINT msg, WPARAM wparam)
	{
		if (!g_loaded) load_settings();
		if (msg == WM_KEYUP && static_cast<int>(wparam) == g_settings.key_menu)
		{
			menu_open = !menu_open;
			shared::globals::imgui_menu_open = menu_open;
			return true;
		}
		return false;
	}

	void on_frame()
	{
		if (!g_loaded) load_settings();
		if (!natives::is_initialized()) return;
		const auto n = natives::get();
		const bool playing = n->IsPlayerPlaying(player());
		if (!playing)
		{
			if (g_photo.active) photo_exit();
			return;
		}

		// the F4 developer menu closes the player menu
		if (!shared::globals::imgui_menu_open) menu_open = false;

		const bool menus_closed = !shared::globals::imgui_menu_open && !n->IsPauseMenuActive();

		if (g_k_photo.pressed(g_settings.key_photo) && menus_closed)
		{
			if (g_photo.active) photo_exit();
			else if (!n->IsCharDead(player_ped()) && !n->IsScreenFadedOut()) photo_enter();
		}
		if (g_k_save.pressed(g_settings.key_quicksave) && menus_closed) quick_save();

		if (g_photo.active)
		{
			if (n->IsPauseMenuActive()) photo_exit();
			else photo_frame();
		}

		hud_frame();
		city_frame();
	}

	// ------------------------------------------------------------------ menu

	void draw_tab_contents()
	{
		auto& s = g_settings;
		bool changed = false;
		const float w3 = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
		auto post = [](const char* lines) { claude_bridge::post_commands(lines); };

		if (ImGui::BeginTabBar("##enh_tabs"))
		{
			if (ImGui::BeginTabItem("  Photo  "))
			{
				ImGui::Spacing();
				if (ImGui::Button(photo_active() ? "Leave photo mode (F8)" : "Enter photo mode (F8)", ImVec2(-1, 40)))
				{
					post(photo_active() ? "photo on=0" : "photo on=1");
					menu_open = false; shared::globals::imgui_menu_open = false;
				}
				ImGui::TextDisabled("WASD move  |  mouse / arrows look  |  Q/E down/up  |  Shift fast, Ctrl slow");
				ImGui::TextDisabled("Z/X zoom  |  [ ] tilt  |  T freeze time  |  H HUD  |  N hide Niko  |  R reset  |  Space photo");
				ImGui::TextDisabled("Photos go to Pictures\\GTA IV Enhanced");
				ImGui::Spacing();
				changed |= ImGui::Checkbox("Freeze time when entering", &s.photo_freeze_time);
				changed |= ImGui::Checkbox("Hide HUD when entering", &s.photo_hide_hud);
				changed |= ImGui::Checkbox("Hide Niko when entering", &s.photo_hide_player);
				changed |= ImGui::SliderFloat("Camera speed (m/s)", &s.cam_speed, 0.5f, 40.0f, "%.1f");
				changed |= ImGui::SliderFloat("Mouse sensitivity", &s.cam_mouse_sens, 0.02f, 0.5f, "%.2f");
				changed |= ImGui::SliderFloat("Max distance from Niko (m)", &s.cam_max_distance, 20.0f, 600.0f, "%.0f");
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("  Saves  "))
			{
				ImGui::Spacing();
				if (ImGui::Button("Save game now (F5)", ImVec2(-1, 40))) { post("quicksave"); menu_open = false; shared::globals::imgui_menu_open = false; }
				ImGui::TextWrapped("Opens the game's own save screen anywhere outside missions, so every save slot is available. "
								   "Loading works from the pause menu (Game > Load Game) as usual.");
				ImGui::Spacing();
				changed |= ImGui::Checkbox("Allow saving while wanted", &s.save_allow_wanted);
				changed |= ImGui::Checkbox("Allow saving inside a vehicle", &s.save_allow_in_car);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("  HUD  "))
			{
				ImGui::Spacing();
				int mode = static_cast<int>(s.hud);
				for (int i = 0; i < 3; ++i) if (ImGui::RadioButton(hud_names[i], &mode, i)) { s.hud = static_cast<hud_mode>(mode); changed = true; }
				ImGui::TextDisabled("Minimal keeps the screen clean on foot; the radar comes back when you drive or get wanted.");
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("  World  "))
			{
				ImGui::SeparatorText("Time of day");
				if (ImGui::Button("Dawn 06:00", ImVec2(w3, 0))) post("time hour=6 minute=0"); ImGui::SameLine();
				if (ImGui::Button("Midday 13:00", ImVec2(w3, 0))) post("time hour=13 minute=0"); ImGui::SameLine();
				if (ImGui::Button("Golden hour 19:30", ImVec2(w3, 0))) post("time hour=19 minute=30");
				if (ImGui::Button("Night 23:30", ImVec2(w3, 0))) post("time hour=23 minute=30"); ImGui::SameLine();
				if (ImGui::Button("Release clock", ImVec2(w3, 0))) post("release_time");
				ImGui::SeparatorText("Weather");
				if (ImGui::Button("Clear", ImVec2(w3, 0))) post("weather id=0"); ImGui::SameLine();
				if (ImGui::Button("Cloudy", ImVec2(w3, 0))) post("weather id=3"); ImGui::SameLine();
				if (ImGui::Button("Rain", ImVec2(w3, 0))) post("weather id=4");
				if (ImGui::Button("Storm", ImVec2(w3, 0))) post("weather id=7"); ImGui::SameLine();
				if (ImGui::Button("Fog", ImVec2(w3, 0))) post("weather id=6"); ImGui::SameLine();
				if (ImGui::Button("Release weather", ImVec2(w3, 0))) post("release_weather");
				ImGui::SeparatorText("Police");
				if (ImGui::Button("No police", ImVec2(w3, 0))) post("calm max_wanted=0"); ImGui::SameLine();
				if (ImGui::Button("Police on", ImVec2(w3, 0))) post("calm max_wanted=6");
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("  City life  "))
			{
				ImGui::Spacing();
				changed |= ImGui::Checkbox("Living city - traffic and crowds follow the time of day", &s.living_city);
				ImGui::BeginDisabled(!s.living_city);
				changed |= ImGui::Checkbox("Weather aware - rain empties the streets, fog thins traffic, sunny afternoons bring people out", &s.weather_city);
				changed |= ImGui::SliderFloat("Traffic amount", &s.traffic_scale, 0.0f, 1.5f, "%.2fx");
				changed |= ImGui::SliderFloat("Pedestrian amount", &s.crowd_scale, 0.0f, 1.5f, "%.2fx");
				ImGui::EndDisabled();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("  More  "))
			{
				ImGui::Spacing();
				ImGui::TextWrapped("Car handling presets: run 'HANDLING - REALISTIC.bat' or 'HANDLING - CLASSIC.bat' in the game folder (applies next launch).");
				ImGui::Spacing();
				if (ImGui::Button("Run benchmark", ImVec2(w3, 0))) post("script file=bench_std");
				ImGui::SameLine(); ImGui::TextDisabled("results: Downloads\\GTA4-Enhanced-Setup\\bridge\\reports");
				ImGui::Spacing();
				ImGui::TextDisabled("Keys: F7 this menu, F8 photo mode, F5 save. Change them in GTA4Enhanced.ini (game folder).");
				ImGui::TextDisabled("F4 opens the full developer / RTX settings menu.");
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		if (changed) { claude_bridge::living_city = s.living_city; save_settings(); }
	}

	void draw_menu()
	{
		const auto vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
		bool open = true;
		if (ImGui::Begin("GTA IV Enhanced", &open, ImGuiWindowFlags_NoCollapse))
		{
			draw_tab_contents();
			if (const char* m = last_message(); m[0]) { ImGui::Separator(); ImGui::TextUnformatted(m); }
		}
		ImGui::End();
		if (!open) { menu_open = false; shared::globals::imgui_menu_open = false; }
	}

	// bridge / menu entry points that must run on the game thread
	void bridge_photo(bool on) { if (on && !g_photo.active) photo_enter(); else if (!on && g_photo.active) photo_exit(); }
	void bridge_quicksave() { quick_save(); }
}
