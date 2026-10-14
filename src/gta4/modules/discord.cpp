#include "std_include.hpp"
#include "discord.hpp"

#include <discord_rpc.h>

#include "comp_settings.hpp"
#include "natives.hpp"

namespace gta4
{
	DiscordRichPresence discord_presence;
	bool discord_presence_initialized = false;

	void discord::first_frame_setup()
	{
		Discord_RunCallbacks();

		static bool time_init = false;
		if (!time_init)
		{
			time_init = true;

			discord_presence.startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
		}

		discord_presence.state = "Loading ...";
		discord_presence.partySize = 0;
		discord_presence.partyMax = 0;
		discord_presence.largeImageKey = "icon";
		Discord_UpdatePresence(&discord_presence);
	}

	void discord::update_discord()
	{
		static int frame_counter = -1;
		static std::string state_str;

		Discord_RunCallbacks();

		// run on very first frame - then only on each 5th frame
		if (!game::CMenuManager__m_MenuActive && (frame_counter < 0 || frame_counter > 5))
		{
			state_str = "";
			const auto n = natives::get();

			natives::Ped ped;
			n->GetPlayerChar(n->ConvertIntToPlayerindex(n->GetPlayerId()), &ped);

			std::string zone_name;
			{
				float x, y, z;
				n->GetCharCoordinates(ped, &x, &y, &z);
				zone_name = n->GetNameOfZone(x, y, z);
			}

			if (n->IsCharSittingInAnyCar(ped))
			{
				state_str = "In Vehicle";

				natives::Vehicle veh;
				n->GetCarCharIsUsing(ped, &veh);

				if (veh)
				{
					uint32_t veh_hash = 0u;
					n->GetCarModel(veh, &veh_hash);

					if (veh_hash)
					{
						std::string name = n->GetDisplayNameFromVehicleModel(veh_hash);
						if (!name.empty()) {
							state_str = "In Vehicle (" + name + ") @ " + zone_name; // shared::utils::va("In Vehicle (%s)", name.c_str()
						}
					}
				}
			} else {
				state_str = "On Foot @ " + zone_name;
			}

			discord_presence.state = state_str.c_str();
			discord_presence.partySize = 0;
			discord_presence.partyMax = 0;
			discord_presence.largeImageKey = "icon";
			Discord_UpdatePresence(&discord_presence);
		}

		if (++frame_counter > 5) {
			frame_counter = 0;
		}
	}

	static void ready([[maybe_unused]] const DiscordUser* request)
	{
		ZeroMemory(&discord_presence, sizeof(discord_presence));

		discord_presence.instance = 1;

		Discord_UpdatePresence(&discord_presence);
	}

	static void errored([[maybe_unused]] const int error_code, [[maybe_unused]] const char* message)
	{
		//shared::common::log("Discord", std::format("({}) {}", error_code, message), shared::common::LOG_TYPE::LOG_TYPE_ERROR);
	}

	void discord::init()
	{
		if (comp_settings::get()->discord_rpc._bool() && !discord_presence_initialized)
		{
			DiscordEventHandlers handlers;
			ZeroMemory(&handlers, sizeof(handlers));
			handlers.ready = ready;
			handlers.errored = errored;
			handlers.disconnected = errored;
			handlers.joinGame = nullptr;
			handlers.spectateGame = nullptr;
			handlers.joinRequest = nullptr;

			Discord_Initialize("1517281468550742147", &handlers, 1, nullptr);
			discord_presence_initialized = true;

			first_frame_setup();
		}
	}

	void discord::shutdown()
	{
		if (discord_presence_initialized)
		{
			Discord_Shutdown();
			discord_presence_initialized = false;
		}
	}

	discord::discord()
	{
		p_this = this;

		discord::init();

		// -----
		m_initialized = true;
		shared::common::log("Discord", "Module initialized.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	discord::~discord()
	{
		discord::shutdown();
	}
}
