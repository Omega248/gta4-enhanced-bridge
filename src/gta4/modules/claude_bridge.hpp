#pragma once
#include <deque>

// GTA IV Enhanced - Claude bridge
// A small, file-based command channel so an assistant (or any external tool) can drive the
// game: read state, teleport, set time/weather, take screenshots and RTX Remix captures.
//
// Folder (default %USERPROFILE%\Downloads\GTA4-Enhanced-Setup\bridge, override with the
// environment variable GTA4_BRIDGE_DIR):
//   inbox\<id>.cmd     key=value lines, first line "cmd=<name>"     (written by the tool)
//   outbox\<id>.json   result for that command                      (written by the game)
//   status.json        live game state, refreshed twice a second    (written by the game)
//   screens\<id>.jpg   screenshots
// Commands are only ever read from that folder on the local disk; nothing listens on the network.

namespace gta4
{
	class claude_bridge final : public shared::common::loader::component_module
	{
	public:
		claude_bridge();
		~claude_bridge() = default;

		static inline claude_bridge* p_this = nullptr;
		static claude_bridge* get() { return p_this; }

		// called every frame from the game's process hook (game thread, natives are safe here)
		static void on_frame();

		// queue bridge commands from inside the game (e.g. the in-game Enhanced panel): one command per
		// line, "cmd k=v k=v". Written to the inbox, so it runs on the game thread like any other command.
		static bool post_commands(const std::string& lines);
		static inline bool living_city = true;          // time / weather aware traffic and crowds (driven by enhanced.cpp)
		static bool density_overridden();               // a benchmark / "calm" command is holding traffic densities
		static void set_retain(int k);                  // retained static world: resend unchanged draws every k-th frame, k < 2 = off
		static int  retain_k();                         // active k, 0 when retain is off
		static void set_retain_gate(bool allow);        // per draw (render thread): this draw may be skipped

	private:
		struct command
		{
			std::string id;
			std::map<std::string, std::string> args;
			int wait_frames = 0;	// frames to wait before the next step
			int step = 0;			// for multi-frame commands
			int guard = 0;			// frames spent in this command (timeout)
			std::string note;
		};

		bool m_initialized = false;
		std::filesystem::path m_dir;
		std::deque<command> m_queue;
		bool m_ready = false;
		uint32_t m_frame = 0;
		int m_cam = 0;
		struct crawl_pt { float x, y; };
		std::vector<crawl_pt> m_crawl;                 // grid for the capture crawler

		void init_dirs();
		void poll_inbox();
		void write_status();
		void run_queue();
		bool step_command(command& c);  // returns true when finished
		void reply(const command& c, bool ok, const std::string& json_body);

		static float f(const command& c, const char* k, float def = 0.0f);
		static int   i(const command& c, const char* k, int def = 0);
		static std::string s(const command& c, const char* k, const std::string& def = "");
		static bool  has(const command& c, const char* k);
	};
}
