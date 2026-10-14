#pragma once

namespace gta4
{
	class timecycle final : public shared::common::loader::component_module
	{
	public:
		timecycle();

		static inline timecycle* p_this = nullptr;
		static timecycle* get() { return p_this; }

		static bool is_initialized()
		{
			if (const auto mod = get(); mod && mod->m_initialized) {
				return true;
			}
			return false;
		}

		static float	get_bad_weather_factor();
		static void		translate_and_apply_timecycle_settings();

	private:
		bool m_initialized = false;
	};
}
