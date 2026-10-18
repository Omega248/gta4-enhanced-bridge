#include "std_include.hpp"
#include "flags.hpp"

namespace shared::common
{
	std::vector<std::string> flags::m_enabled_flags;

	// gets the singleton instance
	flags& flags::get()
	{
		static flags instance;
		return instance;
	}

	bool flags::has_flag(const std::string& flag)
	{
		get().parse_flags();
		for (const auto& entry : m_enabled_flags)
		{
			if (utils::str_to_lower(entry) == utils::str_to_lower(flag)) {
				return true;
			}
		}

		return false;
	}

	void flags::parse_flags()
	{
		// only parse flags once
		if (static auto flags_parsed = false; !flags_parsed)
		{
			flags_parsed = true;

			int num_args;
			auto* const argv = CommandLineToArgvW(GetCommandLineW(), &num_args);

			if (argv)
			{
				for (auto i = 0; i < num_args; ++i)
				{
					std::wstring wide_flag(argv[i]);
					if (wide_flag[0] == L'-')
					{
						wide_flag.erase(wide_flag.begin());
						auto str = utils::convert_wstring(wide_flag);
						shared::common::log("Flags", std::format("Found flag: {}", str), shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
						m_enabled_flags.emplace_back(std::move(str));
					}
				}

				LocalFree(argv);
			}

			// also read flags from gta4e_flags.txt next to the exe (one per line, '-' optional): the Rockstar
			// launcher starts the game itself, so command-line flags do not always reach it
			char exe[MAX_PATH]{};
			if (GetModuleFileNameA(nullptr, exe, MAX_PATH))
			{
				std::string path(exe);
				path = path.substr(0, path.find_last_of("\\/") + 1) + "gta4e_flags.txt";
				if (FILE* fp = nullptr; fopen_s(&fp, path.c_str(), "r") == 0 && fp)
				{
					char line[256];
					while (fgets(line, sizeof(line), fp))
					{
						std::string s(line);
						while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
						if (!s.empty() && s[0] == '-') s.erase(s.begin());
						if (s.empty() || s[0] == '#') continue;
						shared::common::log("Flags", std::format("Found flag (gta4e_flags.txt): {}", s), shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
						m_enabled_flags.emplace_back(std::move(s));
					}
					fclose(fp);
				}
			}
		}
	}
}
