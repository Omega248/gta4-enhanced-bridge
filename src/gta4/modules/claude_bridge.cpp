#include "std_include.hpp"
#include "vs_stats.hpp"
#include "imgui.hpp"
#include <thread>
#include <sstream>
#include <fstream>
#include <atomic>
#include <deque>
#include <shlobj.h>
#include <tlhelp32.h>
#include "claude_bridge.hpp"
#include "d3d9_stats.hpp"
#include "xform_cache.hpp"
#include "dedupe_device.hpp"
#include "comp_settings.hpp"
#include "natives.hpp"
#include "remix_vars.hpp"
#include "shared/common/remix_api.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace gta4
{
	// benchmark traffic: < 0 = leave the game alone; applied every frame (the game resets densities)
	static float g_bench_peds = -1.0f, g_bench_cars = -1.0f;

	// ---- GTA IV Enhanced tooling: crash log, session telemetry, scripted benchmarks -------------
	static std::filesystem::path g_tool_dir;            // bridge folder (set in the constructor)
	static bool g_telemetry_on = true;                  // passive session log while you play
	static int g_telemetry_interval = 5;                // seconds
	static std::atomic<uint32_t> g_tool_frame{ 0 };
	static char g_last_cmd[64] = "";
	static float g_last_pos[3] = {};
	static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

	static LONG WINAPI gta4e_crash_filter(EXCEPTION_POINTERS* ep)
	{
		if (!g_tool_dir.empty() && ep && ep->ExceptionRecord)
		{
			FILE* f = nullptr;
			if (_wfopen_s(&f, (g_tool_dir / L"crash_log.txt").c_str(), L"a") == 0 && f)
			{
				const auto* er = ep->ExceptionRecord;
				HMODULE mod = nullptr; char modname[MAX_PATH] = "?";
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					static_cast<LPCSTR>(er->ExceptionAddress), &mod) && mod) GetModuleFileNameA(mod, modname, MAX_PATH);
				SYSTEMTIME t; GetLocalTime(&t);
				fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u crash code 0x%08lX at %p (%s +0x%llX) frame %u last_cmd '%s' pos %.1f %.1f %.1f\n",
					t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, er->ExceptionCode, er->ExceptionAddress, modname,
					static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(er->ExceptionAddress) - reinterpret_cast<uintptr_t>(mod)),
					g_tool_frame.load(), g_last_cmd, g_last_pos[0], g_last_pos[1], g_last_pos[2]);
				fclose(f);
			}
		}
		return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
	}

	// ---- autostart: gets from the launcher screen into the game with no clicks -----------------
	// Active only while <bridge>/autostart exists. Runs from DLL load (before the game window exists),
	// posts Enter to this process's own launcher window until the game proper is running.
	// Never touches other processes, dialogs (#32770) or the console.
	static std::filesystem::path resolve_bridge_dir()
	{
		wchar_t env[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"GTA4_BRIDGE_DIR", env, MAX_PATH) > 0) return env;
		PWSTR dl = nullptr; std::filesystem::path out;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl)) && dl)
		{ out = std::filesystem::path(dl) / L"GTA4-Enhanced-Setup" / L"bridge"; CoTaskMemFree(dl); }
		return out;
	}

	struct own_window { HWND hwnd; std::string cls; bool visible; };
	static std::vector<own_window> list_own_windows()
	{
		std::vector<own_window> v;
		EnumWindows([](HWND h, LPARAM lp) -> BOOL
		{
			DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
			if (pid == GetCurrentProcessId())
			{
				char cls[128] = {}; GetClassNameA(h, cls, sizeof(cls));
				reinterpret_cast<std::vector<own_window>*>(lp)->push_back({ h, cls, IsWindowVisible(h) != FALSE });
			}
			return TRUE;
		}, reinterpret_cast<LPARAM>(&v));
		return v;
	}

	static void post_key(HWND h, UINT vk)
	{
		const UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
		PostMessageA(h, WM_KEYDOWN, vk, 1 | (sc << 16));
		Sleep(60);
		PostMessageA(h, WM_KEYUP, vk, 1 | (sc << 16) | (1u << 30) | (1u << 31));
	}

	void start_autostart_watcher()
	{
		std::thread([]
		{
			const auto dir = resolve_bridge_dir();
			std::error_code ec;
			if (dir.empty() || !std::filesystem::exists(dir / "autostart", ec)) return;
			std::ofstream(dir / "autostart_log.txt", std::ios::app) << "autostart watcher started\n";
			const ULONGLONG t0 = GetTickCount64();
			while (GetTickCount64() - t0 < 240000)
			{
				Sleep(2500);
				if (g_tool_frame.load() > 120) { std::ofstream(dir / "autostart_log.txt", std::ios::app) << "in game, watcher done\n"; return; }
				for (const auto& w : list_own_windows())
				{
					if (!w.visible || w.cls == "#32770" || w.cls == "ConsoleWindowClass") continue;
					if (w.cls.find("grcWindow") != std::string::npos) continue;   // the game itself: SkipMenu loads the save
					post_key(w.hwnd, VK_RETURN);
					std::ofstream(dir / "autostart_log.txt", std::ios::app) << "Enter -> window class '" << w.cls << "'\n";
				}
			}
		}).detach();
	}

	static std::string read_text(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary); if (!in) return {};
		std::stringstream ss; ss << in.rdbuf(); return ss.str();
	}

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
			j += ",\"passes\":" + dedupe_device::pass_json(static_cast<double>(m_frame - last_frame));
			j += ",\"retain\":" + dedupe_device::retain_json(static_cast<double>(m_frame - last_frame));
			j += ",\"bridge_time\":" + dedupe_device::bridge_time_json(static_cast<double>(m_frame - last_frame));
			j += ",\"vs_draws\":" + vs_stats::take_json(static_cast<double>(m_frame - last_frame));
			j += ",\"near_dupes\":" + dedupe_device::near_json(static_cast<double>(m_frame - last_frame));
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

		strncpy_s(g_last_cmd, cmd.c_str(), _TRUNCATE);

		// script file=<name>  : runs <bridge>/scripts/<name>.txt, one command per line ("cmd k=v k=v").
		// Lines: any bridge command, plus  wait s=<sec>,  measure name=<label> s=<sec>,  exit_game.
		if (cmd == "script")
		{
			const auto path = m_dir / "scripts" / (s(c, "file", "autorun") + ".txt");
			std::ifstream in(path);
			if (!in) { reply(c, false, "\"error\":\"script not found\""); return true; }
			std::vector<command> cmds; std::string line; int n_line = 0;
			const std::string report = s(c, "report", c.id);
			while (std::getline(in, line))
			{
				line = trim(line);
				if (line.empty() || line[0] == '#') continue;
				command sc; sc.id = c.id + "_" + std::to_string(++n_line);
				std::istringstream ls(line); std::string tok; bool first = true;
				while (ls >> tok)
				{
					const auto eq = tok.find('=');
					if (first && eq == std::string::npos) { sc.args["cmd"] = tok; first = false; continue; }
					first = false;
					if (eq != std::string::npos) sc.args[tok.substr(0, eq)] = tok.substr(eq + 1);
				}
				sc.args["report"] = report;
				if (sc.args.contains("cmd")) cmds.push_back(std::move(sc));
			}
			reply(c, true, "\"steps\":" + std::to_string(cmds.size()) + ",\"report\":\"" + json_escape(report) + "\"");
			// insert right after this command (the caller pops this one)
			for (auto it = cmds.rbegin(); it != cmds.rend(); ++it) m_queue.insert(m_queue.begin() + 1, std::move(*it));
			return true;
		}

		// wait_idle [max_s=900] [quiet_s=10] : waits until RTX Remix has finished loading replacement
		// materials/assets (no UsdMod::processMaterial activity in rtx-remix/gta4e_profile.txt for quiet_s).
		// Benchmarks taken while "Loading enhancements" runs are meaningless, so scripts call this first.
		if (cmd == "wait_idle")
		{
			const ULONGLONG now = GetTickCount64();
			if (c.step == 0) { c.note = std::to_string(now) + ";" + std::to_string(now); c.step = 1; }
			const auto sep = c.note.find(';');
			const ULONGLONG t0 = std::stoull(c.note.substr(0, sep));
			ULONGLONG last_busy = std::stoull(c.note.substr(sep + 1));
			static ULONGLONG s_next_poll = 0;
			if (now >= s_next_poll)
			{
				s_next_poll = now + 2000;
				const std::string prof = read_text(std::filesystem::path("rtx-remix") / "gta4e_profile.txt");
				bool busy = false;
				if (const auto at = prof.find("processMaterial"); at != std::string::npos)
				{
					// "Z dxvk::UsdMod::Impl::processMaterial   <ms>   <calls>"
					std::istringstream ls(prof.substr(at + 15, 80)); float ms = 0, calls = 0; ls >> ms >> calls;
					busy = calls > 0.02f || ms > 0.5f;
				}
				if (busy) { last_busy = now; c.note = std::to_string(t0) + ";" + std::to_string(last_busy); }
			}
			const bool quiet = now - last_busy >= static_cast<ULONGLONG>(f(c, "quiet_s", 10.0f) * 1000.0f);
			const bool timeout = now - t0 >= static_cast<ULONGLONG>(f(c, "max_s", 900.0f) * 1000.0f);
			if (!quiet && !timeout) return false;
			char b2[96]; sprintf_s(b2, "\"waited_s\":%.0f,\"timeout\":%s", (now - t0) / 1000.0, timeout && !quiet ? "true" : "false");
			reply(c, true, b2); return true;
		}

		if (cmd == "wait")
		{
			if (c.step == 0) { c.note = std::to_string(GetTickCount64()); c.step = 1; }
			if (GetTickCount64() - std::stoull(c.note) < static_cast<ULONGLONG>(f(c, "s", 1.0f) * 1000.0f)) return false;
			reply(c, true, ""); return true;
		}

		// measure name=<label> s=<sec> : average fps over the window, then appends the label, fps,
		// the latest status.json and the Remix zone profile to <bridge>/reports/<report>.jsonl
		if (cmd == "measure")
		{
			if (c.step == 0) { c.note = std::to_string(GetTickCount64()) + " " + std::to_string(m_frame); c.step = 1; return false; }
			std::istringstream ns(c.note); ULONGLONG t0 = 0; uint32_t f0 = 0; ns >> t0 >> f0;
			const ULONGLONG el = GetTickCount64() - t0;
			if (el < static_cast<ULONGLONG>(f(c, "s", 8.0f) * 1000.0f)) return false;
			const double fps = el ? (m_frame - f0) * 1000.0 / static_cast<double>(el) : 0.0;
			std::string status = read_text(m_dir / "status.json"); while (!status.empty() && (status.back() == '\n' || status.back() == '\r')) status.pop_back();
			if (status.empty()) status = "null";
			const std::string prof = read_text(std::filesystem::path("rtx-remix") / "gta4e_profile.txt");
			char fb[64]; sprintf_s(fb, "%.2f", fps);
			const std::string rec = "{\"name\":\"" + json_escape(s(c, "name", "m")) + "\",\"fps\":" + fb + ",\"frames\":" + std::to_string(m_frame - f0)
				+ ",\"ms\":" + std::to_string(el) + ",\"status\":" + status + ",\"remix_profile\":\"" + json_escape(prof) + "\"}\n";
			std::error_code ec; std::filesystem::create_directories(m_dir / "reports", ec);
			std::ofstream out(m_dir / "reports" / (s(c, "report", "bench") + ".jsonl"), std::ios::app | std::ios::binary); out << rec;
			reply(c, true, std::string("\"fps\":") + fb); return true;
		}

		if (cmd == "exit_game")
		{
			reply(c, true, "");
			std::ofstream(m_dir / "killed.txt") << "exit_game command\n";
			TerminateProcess(GetCurrentProcess(), 0);
			return true;
		}

		// telemetry on=0/1 interval=<sec>
		// what is the game doing right now (no screen reading needed)
		if (cmd == "game_state")
		{
			std::string wl;
			for (const auto& w : list_own_windows()) wl += std::string(wl.empty() ? "" : ",") + "[\"" + json_escape(w.cls) + "\"," + (w.visible ? "true" : "false") + "]";
			natives::Vehicle car = 0; const bool in_car = n->IsCharInAnyCar(ped); if (in_car) n->GetCarCharIsUsing(ped, &car);
			sprintf_s(buf, "\"playing\":%s,\"pause_menu\":%s,\"control\":%s,\"faded_out\":%s,\"cutscene_done\":%s,\"dead\":%s,\"in_car\":%s,\"mission\":%s,\"episode\":%d",
				n->IsPlayerPlaying(player) ? "true" : "false", n->IsPauseMenuActive() ? "true" : "false", n->IsPlayerControlOn(player) ? "true" : "false",
				n->IsScreenFadedOut() ? "true" : "false", n->HasCutsceneFinished() ? "true" : "false", n->IsCharDead(ped) ? "true" : "false",
				in_car ? "true" : "false", n->GetMissionFlag() ? "true" : "false", static_cast<int>(n->GetCurrentEpisode()));
			reply(c, true, std::string(buf) + ",\"windows\":[" + wl + "]"); return true;
		}

		// key vk=<virtual key code> [window=game|launcher] : posts a key to this game's own window (menus)
		if (cmd == "key")
		{
			const UINT vk = static_cast<UINT>(i(c, "vk", VK_RETURN)); const bool launcher = s(c, "window", "game") == "launcher"; int sent = 0;
			for (const auto& w : list_own_windows())
			{
				if (!w.visible || w.cls == "#32770" || w.cls == "ConsoleWindowClass") continue;
				const bool is_game = w.cls.find("grcWindow") != std::string::npos;
				if (is_game != launcher) { post_key(w.hwnd, vk); ++sent; }
			}
			reply(c, sent > 0, "\"sent\":" + std::to_string(sent)); return true;
		}

		// player control without input: drive_wander speed=<m/s>, walk_to x= y= z=, stop_tasks, control on=0/1
		if (cmd == "drive_wander")
		{
			if (!n->IsCharInAnyCar(ped)) { reply(c, false, "\"error\":\"not in a car\""); return true; }
			natives::Vehicle car = 0; n->GetCarCharIsUsing(ped, &car);
			n->TaskCarDriveWander(ped, car, f(c, "speed", 15.0f), static_cast<uint32_t>(i(c, "style", 2)));
			reply(c, true, ""); return true;
		}
		if (cmd == "walk_to") { n->TaskGoStraightToCoord(ped, f(c, "x"), f(c, "y"), f(c, "z"), static_cast<uint32_t>(i(c, "move", 2)), 45000); reply(c, true, ""); return true; }
		if (cmd == "stop_tasks") { n->ClearCharTasks(ped); reply(c, true, ""); return true; }
		if (cmd == "control") { n->SetPlayerControl(player, i(c, "on", 1) != 0); reply(c, true, ""); return true; }

		if (cmd == "living_city") { living_city = i(c, "on", 1) != 0; reply(c, true, ""); return true; }
		if (cmd == "telemetry") { g_telemetry_on = i(c, "on", 1) != 0; g_telemetry_interval = std::max(1, i(c, "interval", 5)); reply(c, true, ""); return true; }

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
		if (cmd == "calm")
		{
			// repeatable benchmarks: no police, optional fixed wind/traffic. max_wanted=0..6, wind=0.., cars=0..1
			n->ClearWantedLevel(n->ConvertIntToPlayerindex(n->GetPlayerId()));
			n->SetMaxWantedLevel(static_cast<uint32_t>(i(c, "max_wanted", 0)));
			if (has(c, "wind")) n->ForceWind(f(c, "wind"));
			// cars / peds >= 0 hold a fixed density every frame (benchmarks); < 0 hands control back (living city / game)
			if (has(c, "cars")) { g_bench_cars = f(c, "cars"); if (g_bench_cars >= 0.0f) n->SetCarDensityMultiplier(g_bench_cars); }
			if (has(c, "peds")) g_bench_peds = f(c, "peds");
			if (has(c, "clear"))   // clear=<radius>: remove cars and peds around the player now
			{
				const auto me = player_ped();
				float x = 0, y = 0, z = 0; n->GetCharCoordinates(me, &x, &y, &z);
				const float r = f(c, "clear");
				n->ClearAreaOfCars(x, y, z, r); n->ClearAreaOfChars(x, y, z, r);
			}
			reply(c, true, ""); return true;
		}

		if (cmd == "remix_raw")
		{
			// any Remix option as a raw string (strings, hash rules, options not listed in rtx.conf)
			std::string results; bool all_ok = true;
			for (const auto& [k, v] : c.args)
			{
				if (!k.starts_with("rtx.") && !k.starts_with("dxvk.")) continue;
				const auto r = shared::common::remix_api::get().m_bridge.SetConfigVariable(k.c_str(), v.c_str());
				const bool ok = r == REMIXAPI_ERROR_CODE_SUCCESS;
				all_ok &= ok;
				results += std::string(results.empty() ? "" : ",") + "\"" + json_escape(k) + "\":" + (ok ? "true" : "false");
			}
			reply(c, all_ok, "\"set\":{" + results + "}");
			return true;
		}

		if (cmd == "retain")
		{
			// retained static world: on=0/1, k=<send every k-th frame>. Pair with rtx.numFramesToKeepInstances/BLAS > k.
			if (has(c, "k")) dedupe_device::g_retain_k = std::max(1, std::min(16, i(c, "k", 4)));
			if (has(c, "on")) dedupe_device::g_retain = i(c, "on", 1) != 0;
			if (has(c, "shaders")) dedupe_device::g_retain_shaders = i(c, "shaders", 1) != 0;
			reply(c, true, "\"on\":" + std::string(dedupe_device::g_retain.load() ? "true" : "false") + ",\"k\":" + std::to_string(dedupe_device::g_retain_k.load()));
			return true;
		}

		if (cmd == "skin_probe")
		{
			// skin_probe n=<skinned draws to log> -> <bridge>/skin_probe.txt
			dedupe_device::g_skin_probe_path = (m_dir / L"skin_probe.txt").wstring();
			dedupe_device::g_skin_probe = std::max(0, std::min(200, i(c, "n", 8)));
			reply(c, true, ""); return true;
		}
		if (cmd == "dupe_skip") { dedupe_device::g_dupe_skip = i(c, "on", 1) != 0; reply(c, true, ""); return true; }
		if (cmd == "hide")
		{
			// cost attribution: hide=<category> on=0/1  (static, vehicle, instances, foliage, fx, ff, vs, water)
			const auto im = imgui::get(); const bool on = i(c, "on", 1) != 0; const auto what = s(c, "what");
			bool* flag = what == "static" ? &im->m_dbg_do_not_render_static : what == "vehicle" ? &im->m_dbg_do_not_render_vehicle
				: what == "instances" ? &im->m_dbg_do_not_render_instances : what == "foliage" ? &im->m_dbg_do_not_render_tree_foliage
				: what == "fx" ? &im->m_dbg_do_not_render_fx : what == "ff" ? &im->m_dbg_do_not_render_ff
				: what == "vs" ? &im->m_dbg_do_not_render_indexed_prims_with_vertexshader : what == "water" ? &im->m_dbg_do_not_render_water : nullptr;
			if (!flag) { reply(c, false, "\"error\":\"unknown category\""); return true; }
			*flag = on; reply(c, true, ""); return true;
		}
		if (cmd == "vs_skip") { vs_stats::g_skip_mask = static_cast<std::uint32_t>(i(c, "mask", 0)); reply(c, true, ""); return true; }
		if (cmd == "pad") { dedupe_device::g_pad_rs = i(c, "rs", 0); dedupe_device::g_pad_vsc = i(c, "vsc", 0); reply(c, true, ""); return true; }
		if (cmd == "bridge_time") { dedupe_device::g_bt_on = i(c, "on", 1) != 0; reply(c, true, ""); return true; }
		if (cmd == "pass_stats") { dedupe_device::g_pass_stats = i(c, "on", 1) != 0; reply(c, true, ""); return true; }

		if (cmd == "affinity")
		{
			// live CPU placement test: game=<hex mask> remix=<hex mask> game_prio/remix_prio = idle|below|normal|above|high
			auto prio = [](const std::string& s) -> DWORD {
				return s == "idle" ? IDLE_PRIORITY_CLASS : s == "below" ? BELOW_NORMAL_PRIORITY_CLASS : s == "above" ? ABOVE_NORMAL_PRIORITY_CLASS
					: s == "high" ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS; };
			std::string res; bool ok = true;
			if (has(c, "game"))
			{
				const DWORD_PTR m = static_cast<DWORD_PTR>(std::stoull(s(c, "game"), nullptr, 16));
				ok &= SetProcessAffinityMask(GetCurrentProcess(), m) != 0;
				res += "\"game\":" + std::string(ok ? "true" : "false");
			}
			if (has(c, "game_prio")) SetPriorityClass(GetCurrentProcess(), prio(s(c, "game_prio")));
			if (has(c, "remix") || has(c, "remix_prio"))
			{
				int found = 0;
				HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
				PROCESSENTRY32W pe{ sizeof(pe) };
				if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe))
				{
					do
					{
						if (_wcsicmp(pe.szExeFile, L"NvRemixBridge.exe")) continue;
						HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
						if (!h) { ok = false; continue; }
						if (has(c, "remix")) ok &= SetProcessAffinityMask(h, static_cast<DWORD_PTR>(std::stoull(s(c, "remix"), nullptr, 16))) != 0;
						if (has(c, "remix_prio")) ok &= SetPriorityClass(h, prio(s(c, "remix_prio"))) != 0;
						CloseHandle(h); ++found;
					} while (Process32NextW(snap, &pe));
				}
				if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
				res += std::string(res.empty() ? "" : ",") + "\"remix_processes\":" + std::to_string(found);
			}
			reply(c, ok, res);
			return true;
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

	bool claude_bridge::post_commands(const std::string& lines)
	{
		const auto b = get();
		if (!b || !b->m_ready) return false;
		static std::atomic<uint32_t> n{ 0 };
		std::istringstream in(lines); std::string line; bool ok = true;
		while (std::getline(in, line))
		{
			line = trim(line);
			if (line.empty()) continue;
			std::string body = "cmd=";
			std::istringstream ls(line); std::string tok; bool first = true;
			while (ls >> tok) { body += (first ? tok : "\n" + tok); first = false; }
			char id[64]; sprintf_s(id, "ui_%010llu_%04u", static_cast<unsigned long long>(GetTickCount64()), n.fetch_add(1) % 10000);
			const auto tmp = b->m_dir / "inbox" / (std::string(id) + ".tmp");
			ok &= write_atomic(tmp, body + "\n");
			std::error_code ec; std::filesystem::rename(tmp, b->m_dir / "inbox" / (std::string(id) + ".cmd"), ec); ok &= !ec;
		}
		return ok;
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

		if (playing && g_bench_peds >= 0.0f) { n->SetPedDensityMultiplier(g_bench_peds); n->SetScenarioPedDensityMultiplier(g_bench_peds, g_bench_peds); }
		if (playing && g_bench_cars >= 0.0f) n->SetCarDensityMultiplier(g_bench_cars);

		// living city: traffic and crowds follow the clock and the weather (benchmarks override it)
		if (playing && living_city && g_bench_peds < 0.0f && g_bench_cars < 0.0f)
		{
			uint32_t hh = 12, mm = 0; n->GetTimeOfDay(&hh, &mm);
			uint32_t w = 0; n->GetCurrentWeather(&w);
			//                         0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15   16   17   18   19   20   21   22   23
			static const float car[24] = { .55f,.45f,.35f,.30f,.35f,.50f,.75f,1.f, 1.f, .90f,.85f,.85f,.90f,.90f,.85f,.90f,1.f, 1.f, 1.f, .90f,.80f,.75f,.70f,.65f };
			static const float ped[24] = { .55f,.45f,.30f,.25f,.25f,.35f,.60f,.90f,1.f, .90f,.85f,.90f,1.f, 1.f, .95f,.95f,1.f, 1.f, 1.f, .95f,.90f,.85f,.75f,.65f };
			const float t = static_cast<float>(mm) / 60.0f; const uint32_t h1 = (hh + 1) % 24; hh %= 24;
			float cd = car[hh] + (car[h1] - car[hh]) * t, pd = ped[hh] + (ped[h1] - ped[hh]) * t;
			if (w == 4 || w == 7) pd *= 0.55f;          // raining / thunder: people stay inside
			else if (w == 5) pd *= 0.8f;                // drizzle
			n->SetCarDensityMultiplier(cd);
			n->SetPedDensityMultiplier(pd);
			n->SetScenarioPedDensityMultiplier(pd, pd);
		}

		g_tool_frame = b->m_frame;

		// passive session telemetry: one line every few seconds while playing -> <bridge>/session_log.jsonl
		{
			static ULONGLONG t_last = GetTickCount64(); static uint32_t f_last = b->m_frame; static uint32_t playing_frames = 0;
			playing_frames = playing ? playing_frames + 1 : 0;
			const ULONGLONG now = GetTickCount64();
			if (now - t_last >= static_cast<ULONGLONG>(g_telemetry_interval) * 1000ull)
			{
				const double fps = (b->m_frame - f_last) * 1000.0 / static_cast<double>(now - t_last);
				t_last = now; f_last = b->m_frame;
				if (playing && g_telemetry_on)
				{
					const auto ped = player_ped();
					float x = 0, y = 0, z = 0; n->GetCharCoordinates(ped, &x, &y, &z);
					g_last_pos[0] = x; g_last_pos[1] = y; g_last_pos[2] = z;
					uint32_t hh = 0, mm = 0; n->GetTimeOfDay(&hh, &mm); uint32_t w = 0; n->GetCurrentWeather(&w);
					const char* zone = n->GetNameOfInfoZone(x, y, z);
					SYSTEMTIME t; GetLocalTime(&t);
					char line[512];
					sprintf_s(line, "{\"t\":\"%02u:%02u:%02u\",\"fps\":%.1f,\"pos\":[%.0f,%.0f,%.0f],\"zone\":\"%s\",\"car\":%d,\"tod\":\"%u:%02u\",\"weather\":%u}\n",
						t.wHour, t.wMinute, t.wSecond, fps, x, y, z, zone ? json_escape(zone).c_str() : "", n->IsCharInAnyCar(ped) ? 1 : 0, hh, mm, w);
					std::ofstream(b->m_dir / "session_log.jsonl", std::ios::app | std::ios::binary) << line;
				}
			}

			// autorun: <bridge>/scripts/autorun.txt runs once, 8 s after the player is in control; renamed afterwards
			if (playing_frames == 240)
			{
				std::error_code ec;
				const auto ar = b->m_dir / "scripts" / "autorun.txt";
				if (std::filesystem::exists(ar, ec))
				{
					command sc; SYSTEMTIME t; GetLocalTime(&t); char id[64];
					sprintf_s(id, "autorun_%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
					sc.id = id; sc.args["cmd"] = "script"; sc.args["file"] = "autorun.last"; sc.args["report"] = id;
					b->m_queue.push_front(std::move(sc));
					// consume it (runs from autorun.last.txt) so it only runs on the launch it was meant for
					std::filesystem::remove(b->m_dir / "scripts" / "autorun.last.txt", ec);
					std::filesystem::rename(ar, b->m_dir / "scripts" / "autorun.last.txt", ec);
				}
			}
		}

		if (playing) b->run_queue();                    // natives that move things need a live player
		else if (!b->m_queue.empty() && s(b->m_queue.front(), "cmd") == "ping") b->run_queue();
	}

	claude_bridge::claude_bridge()
	{
		p_this = this;
		init_dirs();
		m_initialized = true;
		if (m_ready)
		{
			g_tool_dir = m_dir;
			std::error_code ec; std::filesystem::create_directories(m_dir / "scripts", ec); std::filesystem::create_directories(m_dir / "reports", ec);
			g_prev_filter = SetUnhandledExceptionFilter(gta4e_crash_filter);
		}

		// Kill switch on its own thread, so it works even when the render thread is stuck (GPU hang,
		// Remix bridge waiting on Present): creating <bridge>/kill ends the game process immediately.
		if (m_ready)
		{
			const auto dir = m_dir;
			std::thread([dir]
			{
				std::error_code ec;
				std::filesystem::remove(dir / "kill", ec);
				for (;;)
				{
					Sleep(1000);
					if (std::filesystem::exists(dir / "kill", ec))
					{
						std::filesystem::remove(dir / "kill", ec);
						write_atomic(dir / "killed.txt", "killed by bridge kill switch\n");
						TerminateProcess(GetCurrentProcess(), 0);
					}
				}
			}).detach();
		}
		shared::common::log("ClaudeBridge", "Module initialized.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}
}
