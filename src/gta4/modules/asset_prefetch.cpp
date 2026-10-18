#include "std_include.hpp"
#include "shared/common/flags.hpp"
#include <atomic>
#include <thread>
#include <vector>
#include <fstream>

// GTA IV Enhanced - warm the OS file cache for RTX Remix mod textures at launch.
// Remix opens every replacement DDS one at a time on its asset thread to read the header
// (AssetDataManager::findAsset), which took 4-5 minutes for ~75k files (2.4 ms per cold file).
// Reading the first 4 KB of each file in parallel first costs ~0.2 ms per file and leaves the
// headers in the cache (0.06 ms warm). Read-only; disable with the "no_asset_prefetch" flag.
namespace gta4
{
	void start_asset_prefetch()
	{
		if (shared::common::flags::has_flag("no_asset_prefetch")) return;

		std::thread([]
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			const ULONGLONG t0 = GetTickCount64();

			wchar_t exe[MAX_PATH]{};
			if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
			const auto root = std::filesystem::path(exe).parent_path();
			const auto mods = root / "rtx-remix" / "mods";

			std::vector<std::wstring> files;
			std::error_code ec;
			for (auto it = std::filesystem::recursive_directory_iterator(mods, std::filesystem::directory_options::skip_permission_denied, ec);
				 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
			{
				if (it->is_regular_file(ec) && _wcsicmp(it->path().extension().c_str(), L".dds") == 0) {
					files.emplace_back(it->path().wstring());
				}
			}
			const ULONGLONG t_list = GetTickCount64();

			std::atomic<size_t> next{ 0 }, ok{ 0 };
			const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 2u, 16u);
			std::vector<std::thread> pool;
			for (unsigned w = 0; w < workers; ++w)
			{
				pool.emplace_back([&]
				{
					char buf[4096];
					for (size_t i; (i = next.fetch_add(1)) < files.size(); )
					{
						const HANDLE h = CreateFileW(files[i].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
						if (h == INVALID_HANDLE_VALUE) continue;
						DWORD got = 0;
						if (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) ok.fetch_add(1);
						CloseHandle(h);
					}
				});
			}
			for (auto& t : pool) t.join();

			const ULONGLONG t1 = GetTickCount64();
			std::ofstream(root / "rtx-remix" / "logs" / "gta4e_prefetch.log", std::ios::app)
				<< "prefetched " << ok.load() << "/" << files.size() << " DDS headers with " << workers << " threads: list "
				<< (t_list - t0) << " ms, read " << (t1 - t_list) << " ms\n";
		}).detach();
	}
}
