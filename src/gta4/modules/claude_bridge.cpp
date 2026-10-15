#include "std_include.hpp"
#include <deque>
#include <shlobj.h>
#include "claude_bridge.hpp"
#include "d3d9_stats.hpp"
#include "xform_cache.hpp"
#include "dedupe_device.hpp"
#include "comp_settings.hpp"
#include "natives.hpp"
#include "remix_vars.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace gta4
{
	namespace
	{
		std::string json_escape(const std::string& in)
		{
			std::string o;
			o.reserve(in.size() + 8);
			for (const unsigned char ch : in)
			{
				switch (ch)
				{
				case '"':  o += "\\\""; break;
				case '\\': o += "\\\\"; break;
				case '\n': o += "\\n"; break;
				case '\r': break;
				case '\t': o += "\\t"; break;
				default:
					if (ch < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", ch); o += b; }
					else { o += static_cast<char>(ch); }
				}
			}
			return o;
		}

		std::string trim(std::string v)
		{
			const auto a = v.find_first_not_of(" \t\r\n");
			const auto b = v.find_last_not_of(" \t\r\n");
			return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
		}

		bool write_atomic(const std::filesystem::path& p, const std::string& data)
		{
			const auto tmp = p.wstring() + L".tmp";
			{
				std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
				if (!o) return false;
				o.write(data.data(), static_cast<std::streamsize>(data.size()));
			}
			return MoveFileExW(tmp.c_str(), p.wstring().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
		}

		natives::Ped player_ped()
		{
			natives::Ped ped = 0;
			const auto n = natives::get();
			n->GetPlayerChar(n->ConvertIntToPlayerindex(n->GetPlayerId()), &ped);
			return ped;
		}

		// Send a key chord to the focused window (used for the RTX Remix capture hotkey).
		void send_chord(std::initializer_list<WORD> keys)
		{
			std::vector<INPUT> in;
			for (const auto k : keys) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
			for (auto it = keys.end(); it != keys.begin();) { --it; INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i); }
			SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
		}

		// Grab the game window's on-screen pixels (works in borderless/windowed mode, which the
		// FusionFix RTX config uses) and save a 24-bit BMP.
		bool screenshot_bmp(const std::filesystem::path& out, int& w_out, int& h_out)
		{
			const HWND hwnd = shared::globals::main_window;
			if (!hwnd) return false;
			RECT rc; if (!GetClientRect(hwnd, &rc)) return false;
			POINT tl = { rc.left, rc.top }; ClientToScreen(hwnd, &tl);
			const int w = rc.right - rc.left, h = rc.bottom - rc.top;
			if (w <= 0 || h <= 0) return false;

			const HDC screen = GetDC(nullptr);
			const HDC mem = CreateCompatibleDC(screen);
			BITMAPINFO bi = {};
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;  // bottom-up, as BMP expects
			bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
			void* bits = nullptr;
			const HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
			bool ok = false;
			if (dib && bits)
			{
				const HGDIOBJ old = SelectObject(mem, dib);
				ok = BitBlt(mem, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY | CAPTUREBLT) != 0;
				SelectObject(mem, old);
				if (ok)
				{
					const int stride = ((w * 3 + 3) / 4) * 4;
					BITMAPFILEHEADER fh = {};
					fh.bfType = 0x4D42;
					fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
					fh.bfSize = fh.bfOffBits + stride * h;
					std::ofstream o(out, std::ios::binary | std::ios::trunc);
					o.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
					o.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(BITMAPINFOHEADER));
					o.write(static_cast<const char*>(bits), static_cast<std::streamsize>(stride) * h);
					ok = static_cast<bool>(o);
				}
			}
			if (dib) DeleteObject(dib);
			DeleteDC(mem);
			ReleaseDC(nullptr, screen);
			w_out = w; h_out = h;
			return ok;
		}

		// Remix options must not be changed from the game thread directly - the renderer runs on
		// another thread. Queue the change; the Comp Mod applies queued values on the render thread.
		bool set_remix(const std::string& name, const std::string& value)
		{
			const auto h = remix_vars::get_option(name);   // only options listed in rtx.conf
			if (!h || !remix_vars::get()) return false;
			return remix_vars::get()->add_queue_entry(h, remix_vars::string_to_option_value(h->second.type, value), 0.0f, true);
		}
	}

	// ------------------------------------------------------------------------- helpers

	float claude_bridge::f(const command& c, const char* k, float def)
	{
		const auto it = c.args.find(k);
		if (it == c.args.end()) return def;
		try { return std::stof(it->second); } catch (...) { return def; }
	}

	int claude_bridge::i(const command& c, const char* k, int def)
	{
		const auto it = c.args.find(k);
		if (it == c.args.end()) return def;
		try { return std::stoi(it->second); } catch (...) { return def; }
	}

	std::string claude_bridge::s(const command& c, const char* k, const std::string& def)
	{
		const auto it = c.args.find(k);
		return it == c.args.end() ? def : it->second;
	}

	bool claude_bridge::has(const command& c, const char* k) { return c.args.contains(k); }

	// ------------------------------------------------------------------------- setup

	void claude_bridge::init_dirs()
	{
		wchar_t env[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"GTA4_BRIDGE_DIR", env, MAX_PATH) > 0)
		{
			m_dir = env;
		}
		else
		{
			PWSTR dl = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl)) && dl)
			{
				m_dir = std::filesystem::path(dl) / L"GTA4-Enhanced-Setup" / L"bridge";
				CoTaskMemFree(dl);
			}
		}

		if (m_dir.empty()) return;
		std::error_code ec;
		std::filesystem::create_directories(m_dir / "inbox", ec);
		std::filesystem::create_directories(m_dir / "outbox", ec);
		std::filesystem::create_directories(m_dir / "done", ec);
		std::filesystem::create_directories(m_dir / "screens", ec);
		m_ready = !ec && std::filesystem::exists(m_dir / "inbox");
		shared::common::log("ClaudeBridge", m_ready ? "Bridge folder: " + m_dir.string() : "Bridge folder could not be created - bridge disabled.",
			m_ready ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
	}

	void claude_bridge::poll_inbox()
	{
		std::error_code ec;
		std::vector<std::filesystem::path> files;
		for (const auto& e : std::filesystem::directory_iterator(m_dir / "inbox", ec))
		{
			if (e.is_regular_file() && e.path().extension() == ".cmd") files.push_back(e.path());
		}
		std::sort(files.begin(), files.end());

		for (const auto& p : files)
		{
			command c;
			c.id = p.stem().string();
			std::ifstream in(p);
			std::string line;
			while (std::getline(in, line))
			{
				line = trim(line);
				if (line.empty() || line[0] == '#') continue;
				const auto eq = line.find('=');
				if (eq == std::string::npos) continue;
				c.args[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
			}
			in.close();
			std::filesystem::rename(p, m_dir / "done" / p.filename(), ec);
			if (ec) { std::filesystem::remove(p, ec); }
			m_queue.push_back(std::move(c));
		}
	}

	void claude_bridge::reply(const command& c, bool ok, const std::string& json_body)
	{
		std::string out = "{\"id\":\"" + json_escape(c.id) + "\",\"cmd\":\"" + json_escape(s(c, "cmd")) + "\",\"ok\":" + (ok ? "true" : "false");
		if (!json_body.empty()) out += "," + json_body;
		out += "}\n";
		write_atomic(m_dir / "outbox" / (c.id + ".json"), out);
	}

	void claude_bridge::write_status()
	{
		const auto n = natives::get();
		const auto player = n->ConvertIntToPlayerindex(n->GetPlayerId());
		const bool playing = n->IsPlayerPlaying(player);

		std::string j = "{\"frame\":" + std::to_string(m_frame) + ",\"playing\":" + (playing ? "true" : "false")
			+ ",\"queue\":" + std::to_string(m_queue.size());

		uint32_t hh = 0, mm = 0; n->GetTimeOfDay(&hh, &mm);
		uint32_t weather = 0; n->GetCurrentWeather(&weather);
		j += ",\"time\":\"" + std::to_string(hh) + ":" + (mm < 10 ? "0" : "") + std::to_string(mm) + "\",\"weather\":" + std::to_string(weather);

		if (playing)
		{
			const auto ped = player_ped();
			float x = 0, y = 0, z = 0, hd = 0;
			n->GetCharCoordinates(ped, &x, &y, &z);
			n->GetCharHeading(ped, &hd);
			uint32_t health = 0; n->GetCharHealth(ped, &health);
			const char* zone = n->GetNameOfInfoZone(x, y, z);
			char buf[256];
			sprintf_s(buf, ",\"pos\":[%.2f,%.2f,%.2f],\"heading\":%.1f,\"health\":%u,\"in_car\":%s", x, y, z, hd, health, n->IsCharInAnyCar(ped) ? "true" : "false");
			j += buf;
			j += ",\"zone\":\"" + json_escape(zone ? zone : "") + "\"";
		}
		{
			// per-frame D3D9 / bridge statistics since the last status write
			static std::uint64_t last = d3d9_stats::now_ticks();
			const std::uint64_t now = d3d9_stats::now_ticks();
			static std::uint32_t last_frame = m_frame;
			const double wall = d3d9_stats::ticks_to_ms(now - last);
			char gb[64]; sprintf_s(gb, ",\"game_fps\":%.1f", wall > 0 ? (m_frame - last_frame) * 1000.0 / wall : 0.0);
			j += gb;
			char xb[96]; sprintf_s(xb, ",\"xform_skipped\":%llu,\"xform_cache\":%s", xform_cache::g_skipped.exchange(0),
				xform_cache::g_enabled.load() ? "true" : "false");
			j += xb;
			{
				static const char* dn[] = { "rs", "ss", "tss", "tex", "vb", "ib", "shader", "decl", "xform", "const" };
				std::uint64_t tot = 0; std::string dj = ",\"dedupe\":{\"on\":"; dj += dedupe_device::g_enabled.load() ? "true" : "false";
				dj += ",\"stateblocks\":"; dj += dedupe_device::g_saw_state_blocks.load() ? "true" : "false";
				for (int k = 0; k < dedupe_device::S_COUNT; ++k)
				{
					const auto v = dedupe_device::g_skipped[k].exchange(0); tot += v;
					dj += ",\""; dj += dn[k]; dj += "\":" + std::to_string(v);
				}
				dj += ",\"total\":" + std::to_string(tot) + "}";
				j += dj;
			}
			j += ",\"d3d\":" + d3d9_stats::take_json(wall);
			last = now; last_frame = m_frame;
		}
		j += "}\n";
		write_atomic(m_dir / "status.json", j);
	}

	// ------------------------------------------------------------------------- commands

	bool claude_bridge::step_command(command& c)
	{
		const auto n = natives::get();
		const std::string cmd = s(c, "cmd");
		const auto player = n->ConvertIntToPlayerindex(n->GetPlayerId());
		const auto ped = player_ped();
		char buf[512];

		if (cmd == "ping") { reply(c, true, "\"msg\":\"pong\""); return true; }

		if (cmd == "status") { write_status(); reply(c, true, ""); return true; }

		// A/B switches for the bridge-traffic optimisations
		if (cmd == "xform_cache") { xform_cache::g_enabled = i(c, "on", 1) != 0; xform_cache::invalidate(); reply(c, true, ""); return true; }
		// live-tune compatibility-mod settings (no toml write): cmd=comp nocull_dist_far_static=250 nocull_map_areas=0 ...
		if (cmd == "comp")
		{
			auto gs = comp_settings::get();
			std::string set_list;
			auto fset = [&](const char* k, comp_settings::variable& v) { if (has(c, k)) { v.set_var(f(c, k), true); set_list += std::string(set_list.empty() ? "" : ",") + "\"" + k + "\""; } };
			auto bset = [&](const char* k, comp_settings::variable& v) { if (has(c, k)) { v.set_var(i(c, k) != 0, true); set_list += std::string(set_list.empty() ? "" : ",") + "\"" + k + "\""; } };
			auto iset = [&](const char* k, comp_settings::variable& v) { if (has(c, k)) { v.set_var(i(c, k), true); set_list += std::string(set_list.empty() ? "" : ",") + "\"" + k + "\""; } };
			fset("nocull_dist_near_static", gs->nocull_dist_near_static);
			fset("nocull_dist_medium_static", gs->nocull_dist_medium_static);
			fset("nocull_radius_medium_static", gs->nocull_radius_medium_static);
			fset("nocull_dist_far_static", gs->nocull_dist_far_static);
			fset("nocull_radius_far_static", gs->nocull_radius_far_static);
			fset("nocull_height_far_static", gs->nocull_height_far_static);
			fset("nocull_dist_lights", gs->nocull_dist_lights);
			fset("nocull_dist_sphere_interior", gs->nocull_dist_sphere_interior);
			bset("nocull_extended", gs->nocull_extended);
			bset("nocull_extended_auto", gs->nocull_extended_auto);
			fset("nocull_extended_dist", gs->nocull_extended_dist);
			fset("nocull_extended_radius", gs->nocull_extended_radius);
			fset("nocull_extended_height", gs->nocull_extended_height);
			bset("nocull_map_areas", gs->nocull_map_areas);
			iset("nocull_map_areas_count", gs->nocull_map_areas_count);
			bset("nocull_map_areas_high_lod_logic", gs->nocull_map_areas_high_lod_logic);
			reply(c, true, "\"set\":[" + set_list + "]"); return true;
		}
		if (cmd == "dedupe") { dedupe_device::g_enabled = i(c, "on", 1) != 0; reply(c, true, ""); return true; }
		if (cmd == "d3d_stats") { d3d9_stats::g_enabled = i(c, "on", 1) != 0; reply(c, true, ""); return true; }

		if (cmd == "teleport")
		{
			const float x = f(c, "x"), y = f(c, "y"), z = f(c, "z", 20.0f);
			if (c.step == 0)
			{
				n->RequestCollisionAtPosn(x, y, z);
				n->LoadScene(x, y, z);
				if (n->IsCharInAnyCar(ped))
				{
					natives::Vehicle car = 0; n->GetCarCharIsUsing(ped, &car);
					n->SetCarCoordinates(car, x, y, z);
					if (has(c, "heading")) n->SetCarHeading(car, f(c, "heading"));
				}
				else
				{
					n->SetCharCoordinates(ped, x, y, z);
					if (has(c, "heading")) n->SetCharHeading(ped, f(c, "heading"));
				}
				if (i(c, "ground", 0)) n->FreezeCharPosition(ped, true);   // hold still while the area streams in
				c.step = 1; c.wait_frames = i(c, "ground", 0) ? 120 : 30;   // let streaming catch up
				return false;
			}
			if (c.step == 1 && i(c, "ground", 0))
			{
				// snap to the ground once the area has streamed in (lets callers pass rough x/y only)
				float gz = 0.0f;
				n->GetGroundZFor3dCoord(x, y, 1000.0f, &gz);
				if (gz != 0.0f)
				{
					if (n->IsCharInAnyCar(ped)) { natives::Vehicle car = 0; n->GetCarCharIsUsing(ped, &car); n->SetCarCoordinates(car, x, y, gz + 1.0f); }
					else { n->SetCharCoordinates(ped, x, y, gz + 1.0f); }
				}
				n->FreezeCharPosition(ped, false);
				c.step = 2; c.wait_frames = 10;
				return false;
			}
			float px, py, pz; n->GetCharCoordinates(ped, &px, &py, &pz);
			sprintf_s(buf, "\"pos\":[%.2f,%.2f,%.2f]", px, py, pz);
			reply(c, true, buf);
			return true;
		}

		if (cmd == "time")
		{
			const int hh = std::clamp(i(c, "hour", 12), 0, 23), mm = std::clamp(i(c, "minute", 0), 0, 59);
			if (i(c, "freeze", 1)) n->ForceTimeOfDay(hh, mm); else n->SetTimeOfDay(hh, mm);
			reply(c, true, ""); return true;
		}
		if (cmd == "release_time") { n->ReleaseTimeOfDay(); reply(c, true, ""); return true; }

		if (cmd == "weather") { n->ForceWeatherNow(static_cast<uint32_t>(i(c, "id", 0))); reply(c, true, ""); return true; }
		if (cmd == "release_weather") { n->ReleaseWeather(); reply(c, true, ""); return true; }

		if (cmd == "hud")
		{
			const bool on = i(c, "on", 1) != 0;
			n->DisplayHud(on); n->DisplayRadar(on);
			reply(c, true, ""); return true;
		}

		if (cmd == "god")
		{
			const bool on = i(c, "on", 1) != 0;
			n->SetCharInvincible(ped, on);
			n->SetMaxWantedLevel(on ? 0 : 6);
			if (on) n->ClearWantedLevel(player);
			reply(c, true, ""); return true;
		}

		if (cmd == "freeze")
		{
			const bool on = i(c, "on", 1) != 0;
			n->FreezeCharPosition(ped, on);
			n->SetPlayerControl(player, !on);
			reply(c, true, ""); return true;
		}

		if (cmd == "camera")
		{
			if (!m_cam) { natives::Camera cam = 0; n->CreateCam(14, &cam); m_cam = cam; }
			n->SetCamPos(m_cam, f(c, "x"), f(c, "y"), f(c, "z"));
			if (has(c, "look_x")) n->PointCamAtCoord(m_cam, f(c, "look_x"), f(c, "look_y"), f(c, "look_z"));
			else n->SetCamRot(m_cam, f(c, "rx"), f(c, "ry"), f(c, "rz"));
			n->SetCamFov(m_cam, f(c, "fov", 50.0f));
			n->SetCamActive(m_cam, true);
			n->SetCamPropagate(m_cam, true);
			n->ActivateScriptedCams(true, true);
			reply(c, true, ""); return true;
		}
		if (cmd == "camera_off")
		{
			if (m_cam)
			{
				n->SetCamActive(m_cam, false);
				n->SetCamPropagate(m_cam, false);
				n->ActivateScriptedCams(false, false);
				n->DestroyCam(m_cam);
				m_cam = 0;
			}
			reply(c, true, ""); return true;
		}

		if (cmd == "spawn_car")
		{
			std::string model = s(c, "model", "sultanrs");
			const uint32_t hash = n->GetHashKey(model.data());
			if (c.step == 0) { n->RequestModel(hash); c.step = 1; return false; }
			if (!n->HasModelLoaded(hash))
			{
				if (c.guard > 600) { reply(c, false, "\"error\":\"model did not load - check the name\""); return true; }
				n->RequestModel(hash); return false;
			}
			float x, y, z; n->GetCharCoordinates(ped, &x, &y, &z);
			natives::Vehicle car = 0;
			n->CreateCar(hash, x, y, z + 1.0f, &car, true);
			if (car && i(c, "warp", 1)) n->WarpCharIntoCar(ped, car);
			n->MarkModelAsNoLongerNeeded(hash);
			reply(c, car != 0, car ? "" : "\"error\":\"spawn failed\"");
			return true;
		}

		if (cmd == "remix")
		{
			// any number of option=value pairs, e.g.  rtx.pathMaxBounces=4
			std::string results;
			bool all_ok = true;
			for (const auto& [k, v] : c.args)
			{
				if (!k.starts_with("rtx.")) continue;
				const bool ok = set_remix(k, v);
				all_ok &= ok;
				results += std::string(results.empty() ? "" : ",") + "\"" + json_escape(k) + "\":" + (ok ? "true" : "false");
			}
			reply(c, all_ok, "\"set\":{" + results + "}");
			return true;
		}

		if (cmd == "screenshot")
		{
			if (c.step == 0)
			{
				if (i(c, "hide_hud", 1)) { n->DisplayHud(false); n->DisplayRadar(false); }
				c.step = 1; c.wait_frames = std::max(2, i(c, "delay_frames", 10));
				return false;
			}
			int w = 0, h = 0;
			const auto path = m_dir / "screens" / (c.id + ".bmp");
			const bool ok = screenshot_bmp(path, w, h);
			if (i(c, "hide_hud", 1)) { n->DisplayHud(true); n->DisplayRadar(true); }
			sprintf_s(buf, "\"file\":\"screens/%s.bmp\",\"size\":[%d,%d]", c.id.c_str(), w, h);
			reply(c, ok, ok ? buf : "\"error\":\"screen grab failed (game minimised or exclusive fullscreen?)\"");
			return true;
		}

		if (cmd == "crawl")
		{
			// Capture crawler: visits a grid of points in short hops with Niko hidden and collision
			// off, waits for each area to stream in, frames it from above and triggers an RTX Remix
			// capture. The game must be in capture mode (replacement mods parked). Progress is saved
			// after every point (bridge/crawl_progress.txt) so a restart resumes where it stopped.
			// Stop early by creating bridge/crawl_stop.
			// args: x0 x1 y0 y1 step start height settle_s capture_s
			const auto now = GetTickCount64();
			const auto elapsed = [&]() { return static_cast<double>(now - std::stoull(s(c, "_t", "0"))) / 1000.0; };
			const auto mark = [&]() { c.args["_t"] = std::to_string(now); };
			const auto status = [&](const std::string& st)
			{
				char b[256];
				sprintf_s(b, "{\"index\":%d,\"total\":%zu,\"captured\":%d,\"water_skipped\":%d,\"state\":\"%s\"}\n",
					i(c, "_idx"), m_crawl.size(), i(c, "_caps"), i(c, "_water"), st.c_str());
				write_atomic(m_dir / "crawl_status.json", b);
			};

			if (c.step == 0)
			{
				const float x0 = f(c, "x0", -2300.0f), x1 = f(c, "x1", 2600.0f), y0 = f(c, "y0", -1800.0f), y1 = f(c, "y1", 2300.0f);
				const float st = std::max(50.0f, f(c, "step", 250.0f));
				m_crawl.clear();
				// serpentine order keeps every hop short
				int row = 0;
				for (float y = y0; y <= y1; y += st, ++row)
				{
					std::vector<crawl_pt> r;
					for (float x = x0; x <= x1; x += st) r.push_back({ x, y });
					if (row & 1) std::reverse(r.begin(), r.end());
					m_crawl.insert(m_crawl.end(), r.begin(), r.end());
				}
				int start = i(c, "start", -1);
				if (start < 0)   // resume from saved progress
				{
					std::ifstream in(m_dir / "crawl_progress.txt"); int v = 0;
					start = (in >> v) ? v : 0;
				}
				c.args["_idx"] = std::to_string(start); c.args["_caps"] = "0"; c.args["_water"] = "0";
				n->SetCharInvincible(ped, true); n->SetMaxWantedLevel(0); n->ClearWantedLevel(player);
				n->DisplayHud(false); n->DisplayRadar(false);
				n->ForceTimeOfDay(13, 0); n->ForceWeatherNow(1);
				c.step = 1;
				return false;
			}

			int idx = i(c, "_idx");
			std::error_code ec;
			const bool stop = std::filesystem::exists(m_dir / "crawl_stop", ec);

			if (c.step == 1)   // hop to the next point
			{
				if (idx >= static_cast<int>(m_crawl.size()) || stop)
				{
					if (m_cam) { n->SetCamActive(m_cam, false); n->SetCamPropagate(m_cam, false); n->ActivateScriptedCams(false, false); n->DestroyCam(m_cam); m_cam = 0; }
					n->SetCharVisible(ped, true); n->SetCharCollision(ped, true); n->FreezeCharPosition(ped, false);
					n->DisplayHud(true); n->DisplayRadar(true); n->ReleaseTimeOfDay(); n->ReleaseWeather();
					status(stop ? "stopped" : "finished");
					char b[160];
					sprintf_s(b, "\"index\":%d,\"total\":%zu,\"captured\":%d,\"stopped\":%s", idx, m_crawl.size(), i(c, "_caps"), stop ? "true" : "false");
					reply(c, true, b);
					return true;
				}
				const auto& p = m_crawl[idx];
				n->SetCharVisible(ped, false);
				n->SetCharCollision(ped, false);
				n->FreezeCharPosition(ped, true);
				n->SetCharCoordinatesNoOffset(ped, p.x, p.y, 150.0f);
				n->RequestCollisionAtPosn(p.x, p.y, 20.0f);
				n->LoadScene(p.x, p.y, 20.0f);
				mark(); status("streaming");
				c.step = 2;
				return false;
			}

			if (c.step == 2)   // wait for streaming, check for water, frame the shot
			{
				if (elapsed() < f(c, "settle_s", 7.0f)) return false;
				const auto& p = m_crawl[idx];
				float gz = 0.0f;
				n->GetGroundZFor3dCoord(p.x, p.y, 1000.0f, &gz);
				if (gz <= 0.5f)
				{
					c.args["_water"] = std::to_string(i(c, "_water") + 1);
					c.args["_idx"] = std::to_string(idx + 1);
					write_atomic(m_dir / "crawl_progress.txt", std::to_string(idx + 1) + "\n");
					c.step = 1;
					return false;
				}
				n->SetCharCoordinatesNoOffset(ped, p.x, p.y, gz + 1.0f);
				const float h = f(c, "height", 45.0f);
				if (!m_cam) { natives::Camera cam = 0; n->CreateCam(14, &cam); m_cam = cam; }
				n->SetCamPos(m_cam, p.x - h * 0.6f, p.y - h * 0.6f, gz + h);
				n->PointCamAtCoord(m_cam, p.x, p.y, gz);
				n->SetCamFov(m_cam, 70.0f);
				n->SetCamActive(m_cam, true); n->SetCamPropagate(m_cam, true); n->ActivateScriptedCams(true, true);
				mark(); status("framing");
				c.step = 3;
				return false;
			}

			if (c.step == 3)   // let the image and streaming settle, then capture
			{
				if (elapsed() < 4.0) return false;
				const HWND hwnd = shared::globals::main_window;
				if (hwnd && GetForegroundWindow() != hwnd)
				{
					status("waiting for game window focus");
					return false;   // retry until the window is focused again
				}
				send_chord({ VK_CONTROL, VK_SHIFT, 'Q' });
				mark(); status("capturing");
				c.step = 4;
				return false;
			}

			if (c.step == 4)   // capture export takes ~30 s
			{
				if (elapsed() < f(c, "capture_s", 45.0f)) return false;
				c.args["_caps"] = std::to_string(i(c, "_caps") + 1);
				c.args["_idx"] = std::to_string(idx + 1);
				write_atomic(m_dir / "crawl_progress.txt", std::to_string(idx + 1) + "\n");
				c.step = 1;
				return false;
			}
		}

		if (cmd == "capture")
		{
			// RTX Remix only captures while no replacement mods are loaded. Toggling replacements at
			// runtime crashes this setup, so the game must be started in capture mode (replacement
			// mods parked). This command only presses the capture hotkey - it never changes settings.
			if (c.step == 0)
			{
				const HWND hwnd = shared::globals::main_window;
				if (hwnd && GetForegroundWindow() != hwnd)
				{
					reply(c, false, "\"error\":\"game window is not focused - click into the game\"");
					return true;
				}
				send_chord({ VK_CONTROL, VK_SHIFT, 'Q' });
				c.step = 1; c.wait_frames = std::max(60, i(c, "capture_frames", 240));
				return false;
			}
			reply(c, true, "\"note\":\"capture hotkey sent - files appear in <game>\\\\rtx-remix\\\\captures\"");
			return true;
		}

		sprintf_s(buf, "\"error\":\"unknown command '%s'\"", json_escape(cmd).c_str());
		reply(c, false, buf);
		return true;
	}

	void claude_bridge::run_queue()
	{
		if (m_queue.empty()) return;
		auto& c = m_queue.front();
		if (c.wait_frames > 0) { --c.wait_frames; return; }
		++c.guard;
		bool finished = true;
		try { finished = step_command(c); }
		catch (...) { reply(c, false, "\"error\":\"exception while running command\""); finished = true; }
		if (!finished && c.guard > (s(c, "cmd") == "crawl" ? 100000000 : 3600)) { reply(c, false, "\"error\":\"timed out\""); finished = true; }
		if (finished) m_queue.pop_front();
	}

	// ------------------------------------------------------------------------- per frame

	void claude_bridge::on_frame()
	{
		const auto b = get();
		if (!b || !b->m_ready || !natives::is_initialized()) return;
		++b->m_frame;

		const auto n = natives::get();
		const bool playing = n->IsPlayerPlaying(n->ConvertIntToPlayerindex(n->GetPlayerId()));

		if (b->m_frame % 15 == 0) b->poll_inbox();      // ~4x per second at 60 fps
		if (b->m_frame % 30 == 0) b->write_status();    // ~2x per second

		if (playing) b->run_queue();                    // natives that move things need a live player
		else if (!b->m_queue.empty() && s(b->m_queue.front(), "cmd") == "ping") b->run_queue();
	}

	claude_bridge::claude_bridge()
	{
		p_this = this;
		init_dirs();
		m_initialized = true;
		shared::common::log("ClaudeBridge", "Module initialized.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}
}
