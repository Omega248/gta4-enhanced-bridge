#pragma once

namespace gta4
{
	class game_lights final : public shared::common::loader::component_module
	{
	public:
		game_lights();

		static inline game_lights* p_this = nullptr;
		static game_lights* get() { return p_this; }

		static bool is_initialized()
		{
			if (const auto mod = get(); mod && mod->m_initialized) {
				return true;
			}
			return false;
		}

		void	draw_debug();
		void	iterate_all_game_lights();

	private:
		bool m_initialized = false;
	};
}
