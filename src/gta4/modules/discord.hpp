#pragma once

namespace gta4
{
	class discord final : public shared::common::loader::component_module
	{
	public:
		discord();
		~discord();

		static inline discord* p_this = nullptr;
		static discord* get() { return p_this; }

		static bool is_initialized()
		{
			if (const auto mod = get(); mod && mod->m_initialized) {
				return true;
			}
			return false;
		}

		static void first_frame_setup();
		static void	update_discord();
		static void	init();
		static void	shutdown();

		static inline bool g_enable_discord_rpc = true;

	private:
		bool m_initialized = false;
	};
}
