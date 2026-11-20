#include "scripted_input.h"

#include "kz/kz.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "sdk/navphysicsinterface.h"
#include "sdk/tracefilter.h"
#include "utils/simplecmds.h"
#include "utils/utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

using scripted::ScriptedInput;
using scripted::ScriptTick;

namespace
{
	constexpr int DEFAULT_START_DELAY_TICKS = 64;
	constexpr int GROUND_STOP_TICKS = 16;
	// A2: tunable from the server console (kz_script_delay).
	int g_startDelayTicks = DEFAULT_START_DELAY_TICKS;

	ScriptedInput inputs[64];

	static std::ofstream g_jumpLog;
	static std::ofstream g_cmdLog;
	static bool g_logsInit = false;

	static void InitLogs()
	{
		if (g_logsInit)
		{
			return;
		}
		g_logsInit = true;
		char path[512];
		g_SMAPI->PathFormat(path, sizeof(path), "%s/addons/cs2kz/logs/lj_jumps.csv", g_SMAPI->GetBaseDir());
		g_jumpLog.open(path, std::ios::out | std::ios::app);
		if (g_jumpLog.is_open())
		{
			g_jumpLog << "server_tick,player_slot,jump_type,valid,invalid_reason,official_distance,raw_distance,"
						 "takeoff_speed,max_speed,landing_speed,strafes,sync,takeoff_x,takeoff_y,takeoff_z,"
						 "landing_x,landing_y,landing_z,air_ticks,ground_z_takeoff,"
						 "landing_origin_actual_x,landing_origin_actual_y,landing_origin_actual_z\n";
		}
		g_SMAPI->PathFormat(path, sizeof(path), "%s/addons/cs2kz/logs/lj_cmds.csv", g_SMAPI->GetBaseDir());
		g_cmdLog.open(path, std::ios::out | std::ios::app);
		if (g_cmdLog.is_open())
		{
			g_cmdLog << "tick_count,server_tick,fm,sm,um,buttons,view_yaw,view_pitch,subtick,"
						"origin_x,origin_y,origin_z,vel_x,vel_y,vel_z,on_ground\n";
		}
	}

	inline int SlotOf(KZPlayer *player)
	{
		if (!player)
		{
			return -1;
		}
		int slot = player->GetPlayerSlot().Get();
		return (slot >= 0 && slot < 64) ? slot : -1;
	}

	inline void Split(const std::string &line, std::vector<std::string> &out)
	{
		out.clear();
		std::stringstream ss(line);
		std::string cell;
		while (std::getline(ss, cell, ','))
		{
			out.push_back(cell);
		}
	}

	// ---- H1: raw usercmd logging for diagnosing real input ----
	struct CommandLog
	{
		bool active {};
		int slot {-1};
		std::string tag;
		std::ofstream file;
	};

	CommandLog commandLogs[64];

	CommandLog *GetCommandLog(KZPlayer *player)
	{
		int slot = SlotOf(player);
		if (slot < 0 || !commandLogs[slot].active)
		{
			return nullptr;
		}
		return &commandLogs[slot];
	}

	void LogFullCommand(KZPlayer *player, CUserCmd *cmd)
	{
		CommandLog *log = GetCommandLog(player);
		if (!log || !log->file.is_open() || !cmd || !player->GetPlayerPawn())
		{
			return;
		}
		auto *base = cmd->mutable_base();
		std::string subtick;
		char buf[220];
		for (int i = 0; i < base->subtick_moves_size(); i++)
		{
			const auto &m = base->subtick_moves(i);
			if (i)
			{
				subtick += '|';
			}
			V_snprintf(buf, sizeof(buf), "%.5f;%u;%d;%.4f;%.4f;%.4f;%.4f", m.when(), (unsigned int)m.button(), m.pressed() ? 1 : 0,
					   m.analog_forward_delta(), m.analog_left_delta(), m.pitch_delta(), m.yaw_delta());
			subtick += buf;
		}
		Vector origin;
		player->GetOrigin(&origin);
		Vector vel;
		player->GetVelocity(&vel);
		log->file << log->tag << ',' << player->GetController()->m_nTickBase() << ',' << g_pKZUtils->GetServerGlobals()->tickcount << ','
				  << base->forwardmove() << ',' << base->leftmove() << ',' << base->upmove() << ',' << base->buttons_pb().buttonstate1() << ','
				  << base->buttons_pb().buttonstate2() << ',' << base->buttons_pb().buttonstate3() << ',' << cmd->buttonstates.m_pButtonStates[0]
				  << ',' << cmd->buttonstates.m_pButtonStates[1] << ',' << cmd->buttonstates.m_pButtonStates[2] << ',' << base->viewangles().x()
				  << ',' << base->viewangles().y() << ',' << base->viewangles().z() << ',' << base->subtick_moves_size() << ',' << subtick << ','
				  << (cmd->IsLate() ? 1 : 0) << ',' << cmd->flags << ',' << origin.x << ',' << origin.y << ',' << origin.z << ',' << vel.x << ','
				  << vel.y << ',' << vel.z << ',' << ((player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND) ? 1 : 0) << '\n';
		log->file.flush();
	}

	// H2/H3: how scripted input is injected into the usercmd.
	enum class ScriptMode
	{
		// original: only base fm/sm/um + CSV buttons
		Fields,
		// H2: derive IN_FORWARD/IN_BACK/IN_MOVELEFT/IN_MOVERIGHT from fm/sm, OR with CSV buttons
		Buttons,
		// H3: Buttons + one subtick move carrying the analog deltas
		Subtick,
		// R3: replay a recorded usercmd CSV exactly (base fields + every subtick field)
		Verbatim,
	};

	ScriptMode g_scriptMode = ScriptMode::Buttons;

	inline const char *ModeName(ScriptMode mode)
	{
		switch (mode)
		{
			case ScriptMode::Fields:
				return "fields";
			case ScriptMode::Buttons:
				return "buttons";
			case ScriptMode::Subtick:
				return "subtick";
			case ScriptMode::Verbatim:
				return "verbatim";
		}
		return "unknown";
	}
} // namespace

namespace scripted
{
	ScriptedInput *GetForPlayer(KZPlayer *player)
	{
		int slot = SlotOf(player);
		return slot < 0 ? nullptr : &inputs[slot];
	}

	bool OnPlayerRunCommand(KZPlayer *player, CUserCmd *cmd)
	{
		// H1 diagnostics: log every command (script or not) for a tagged slot.
		LogFullCommand(player, cmd);
		ScriptedInput *input = GetForPlayer(player);
		if (!input || !input->IsActive())
		{
			return false;
		}
		return input->OnPlayerRunCommand(player, cmd);
	}

	void ResetAll()
	{
		for (ScriptedInput &input : inputs)
		{
			input.Reset();
		}
	}

	void LogJump(KZPlayer *player, Jump *jump)
	{
		if (!player || !jump)
		{
			return;
		}
		InitLogs();
		if (!g_jumpLog.is_open())
		{
			return;
		}
		Vector vel;
		player->GetVelocity(&vel);
		const Vector &t = jump->adjustedTakeoffOrigin;
		const Vector &l = jump->adjustedLandingOrigin;
		f32 raw = 32.0f + (jump->landingOrigin - jump->takeoffOrigin).Length2D();
		g_jumpLog << jump->serverTick << ',' << player->GetPlayerSlot().Get() << ',' << (int)jump->GetJumpType() << ',' << (jump->IsValid() ? 1 : 0)
				  << ",\"" << jump->invalidateReason << "\"," << jump->GetDistance(true, false) << ',' << raw << ',' << jump->GetTakeoffSpeed() << ','
				  << jump->GetMaxSpeed() << ',' << vel.Length2D() << ',' << jump->strafes.size() << ',' << jump->GetSync() << ',' << t.x << ',' << t.y
				  << ',' << t.z << ',' << l.x << ',' << l.y << ',' << l.z << ',' << (int)std::lround(jump->airtime * 64.0f) << ','
				  << player->takeoffGroundOrigin.z << ',' << l.x << ',' << l.y << ',' << l.z << '\n';
		g_jumpLog.flush();
	}

	void LogCommand(KZPlayer *player, CUserCmd *cmd)
	{
		if (!player || !cmd)
		{
			return;
		}
		InitLogs();
		if (!g_cmdLog.is_open())
		{
			return;
		}
		auto *base = cmd->mutable_base();
		std::string subtick;
		for (int i = 0; i < base->subtick_moves_size(); i++)
		{
			const auto &m = base->subtick_moves(i);
			if (i)
			{
				subtick += '|';
			}
			char buf[160];
			V_snprintf(buf, sizeof(buf), "%.4f;%u;%.4f;%.4f;%.4f;%.4f", m.when(), (unsigned int)m.button(), m.analog_forward_delta(),
					   m.analog_left_delta(), m.pitch_delta(), m.yaw_delta());
			subtick += buf;
		}
		Vector origin;
		player->GetOrigin(&origin);
		Vector vel;
		player->GetVelocity(&vel);
		u32 serverTick = g_pKZUtils->GetServerGlobals()->tickcount;
		g_cmdLog << player->GetController()->m_nTickBase() << ',' << serverTick << ',' << base->forwardmove() << ',' << base->leftmove() << ','
				 << base->upmove() << ',' << base->buttons_pb().buttonstate1() << ',' << base->viewangles().y() << ',' << base->viewangles().x()
				 << ',' << subtick << ',' << origin.x << ',' << origin.y << ',' << origin.z << ',' << vel.x << ',' << vel.y << ',' << vel.z << ','
				 << ((player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND) ? 1 : 0) << '\n';
		g_cmdLog.flush();
	}
} // namespace scripted

bool ScriptedInput::Load(const char *filePath)
{
	this->Reset();
	if (!filePath || !filePath[0])
	{
		return false;
	}
	std::ifstream file(filePath);
	if (!file.is_open())
	{
		META_CONPRINTF("[KZScript] cannot open %s\n", filePath);
		return false;
	}
	this->path = filePath;
	this->hasStart = false;
	std::string line;
	std::vector<std::string> cells;
	while (std::getline(file, line))
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
		{
			line.pop_back();
		}
		// P1: tolerate a UTF-8 BOM written by PowerShell/Excel
		if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
		{
			line.erase(0, 3);
		}
		if (line.empty())
		{
			continue;
		}
		if (line[0] == '#')
		{
			f32 x, y, z, yaw;
			if (std::sscanf(line.c_str(), "# start %f %f %f %f", &x, &y, &z, &yaw) == 4)
			{
				this->startOrigin = Vector(x, y, z);
				this->startYaw = yaw;
				this->hasStart = true;
			}
			continue;
		}
		if (line.rfind("vtick", 0) == 0 || line.rfind("tick", 0) == 0)
		{
			continue; // header
		}
		Split(line, cells);
		if (cells.size() >= 14)
		{
			// R3 verbatim: vtick,fm,sm,um,pb1,pb2,pb3,bs0,bs1,bs2,pitch,yaw,roll,ns,subticks
			ScriptTick tick;
			tick.fm = std::clamp((f32)atof(cells[1].c_str()), -1.0f, 1.0f);
			tick.sm = std::clamp((f32)atof(cells[2].c_str()), -1.0f, 1.0f);
			tick.um = std::clamp((f32)atof(cells[3].c_str()), -1.0f, 1.0f);
			tick.buttons = (u32)strtoull(cells[4].c_str(), nullptr, 10);
			tick.button2 = (u32)strtoull(cells[5].c_str(), nullptr, 10);
			tick.button3 = (u32)strtoull(cells[6].c_str(), nullptr, 10);
			tick.bs0 = (u32)strtoull(cells[7].c_str(), nullptr, 10);
			tick.bs1 = (u32)strtoull(cells[8].c_str(), nullptr, 10);
			tick.bs2 = (u32)strtoull(cells[9].c_str(), nullptr, 10);
			tick.pitch = (f32)atof(cells[10].c_str());
			tick.yaw = (f32)atof(cells[11].c_str());
			tick.roll = (f32)atof(cells[12].c_str());
			if (cells.size() >= 15 && !cells[14].empty())
			{
				std::stringstream ss(cells[14]);
				std::string move;
				while (std::getline(ss, move, '|'))
				{
					std::vector<std::string> f;
					std::stringstream ms(move);
					std::string cell;
					while (std::getline(ms, cell, ';'))
					{
						f.push_back(cell);
					}
					if (f.size() < 7)
					{
						continue;
					}
					ScriptSubtick sub;
					sub.when = (f32)atof(f[0].c_str());
					sub.button = (u32)strtoull(f[1].c_str(), nullptr, 10);
					sub.pressed = atoi(f[2].c_str()) != 0;
					sub.afd = (f32)atof(f[3].c_str());
					sub.ald = (f32)atof(f[4].c_str());
					sub.pd = (f32)atof(f[5].c_str());
					sub.yd = (f32)atof(f[6].c_str());
					tick.subticks.push_back(sub);
				}
			}
			this->ticks.push_back(tick);
			continue;
		}
		if (cells.size() < 7)
		{
			continue;
		}
		ScriptTick tick;
		tick.yaw = (f32)atof(cells[1].c_str());
		tick.pitch = (f32)atof(cells[2].c_str());
		tick.fm = std::clamp((f32)atof(cells[3].c_str()), -1.0f, 1.0f);
		tick.sm = std::clamp((f32)atof(cells[4].c_str()), -1.0f, 1.0f);
		tick.um = std::clamp((f32)atof(cells[5].c_str()), -1.0f, 1.0f);
		tick.buttons = (u32)strtoull(cells[6].c_str(), nullptr, 10);
		this->ticks.push_back(tick);
	}
	this->loaded = !this->ticks.empty();
	META_CONPRINTF("[KZScript] loaded %s: %zu ticks, start=%s (%.2f %.2f %.2f yaw %.2f)\n", filePath, this->ticks.size(),
				   this->hasStart ? "yes" : "no", this->startOrigin.x, this->startOrigin.y, this->startOrigin.z, this->startYaw);
	return this->loaded;
}

bool ScriptedInput::Start(KZPlayer *player)
{
	if (!this->loaded || !player || !player->GetPlayerPawn())
	{
		return false;
	}
	Vector origin = this->startOrigin;
	if (!this->hasStart)
	{
		player->GetOrigin(&origin);
	}
	Vector velocity(0, 0, 0);
	QAngle angles;
	angles.x = 0;
	angles.y = this->startYaw;
	angles.z = 0;
	player->Teleport(&origin, &angles, &velocity);

	this->active = true;
	this->index = 0;
	this->startTick = player->GetController()->m_nTickBase() + g_startDelayTicks;
	this->lastTick = 0;
	this->wasAirborne = false;
	this->groundedTicks = 0;

	if (this->log.is_open())
	{
		this->log.close();
	}
	char logPath[512];
	g_SMAPI->PathFormat(logPath, sizeof(logPath), "%s/addons/cs2kz/logs/scripted_%d.csv", g_SMAPI->GetBaseDir(), player->GetPlayerSlot().Get());
	this->log.open(logPath, std::ios::out | std::ios::trunc);
	if (this->log.is_open())
	{
		this->log << "tick_count,script_idx,origin_x,origin_y,origin_z,vel_x,vel_y,vel_z,on_ground,yaw,eye_yaw,consumed,svac\n";
	}
	META_CONPRINTF("[KZScript] start slot=%d path=%s ticks=%zu start_tick=%u\n", player->GetPlayerSlot().Get(), this->path.c_str(),
				   this->ticks.size(), this->startTick);
	return true;
}

void ScriptedInput::Stop()
{
	if (!this->active)
	{
		return;
	}
	this->active = false;
	if (this->log.is_open())
	{
		this->log.flush();
		this->log.close();
	}
	META_CONPRINTF("[KZScript] stop at idx=%zu\n", this->index);
}

void ScriptedInput::Reset()
{
	this->Stop();
	this->ticks.clear();
	this->loaded = false;
	this->hasStart = false;
	this->index = 0;
	this->path.clear();
}

void ScriptedInput::PrintStatus(KZPlayer *player) const
{
	META_CONPRINTF("[KZScript] loaded=%d active=%d idx=%zu/%zu mode=%s path=%s\n", this->loaded ? 1 : 0, this->active ? 1 : 0, this->index,
				   this->ticks.size(), ModeName(g_scriptMode), this->path.c_str());
}

bool ScriptedInput::OnPlayerRunCommand(KZPlayer *player, CUserCmd *cmd)
{
	if (!this->active || !this->loaded || !player || !player->GetPlayerPawn())
	{
		return false;
	}
	if (cmd->IsLate() || (cmd->flags & USERCMD_SUBSTITUTE))
	{
		return false;
	}
	u32 tick = player->GetController()->m_nTickBase();
	if (tick < this->startTick || tick == this->lastTick)
	{
		return false;
	}
	if (this->index >= this->ticks.size())
	{
		this->Stop();
		return false;
	}

	const ScriptTick &t = this->ticks[this->index];
	LogCommand(player, cmd);
	cmd->mutable_base()->clear_subtick_moves();
	cmd->mutable_base()->set_forwardmove(t.fm);
	cmd->mutable_base()->set_leftmove(t.sm);
	cmd->mutable_base()->set_upmove(t.um);

	cmd->mutable_base()->mutable_viewangles()->Clear();
	cmd->mutable_base()->mutable_viewangles()->set_x(t.pitch);
	cmd->mutable_base()->mutable_viewangles()->set_y(t.yaw);
	cmd->mutable_base()->mutable_viewangles()->set_z(t.roll);

	if (g_scriptMode == ScriptMode::Verbatim)
	{
		// R3: reproduce the recorded usercmd exactly (buttons + every subtick field).
		cmd->mutable_base()->mutable_buttons_pb()->Clear();
		cmd->mutable_base()->mutable_buttons_pb()->set_buttonstate1(t.buttons);
		cmd->mutable_base()->mutable_buttons_pb()->set_buttonstate2(t.button2);
		cmd->mutable_base()->mutable_buttons_pb()->set_buttonstate3(t.button3);
		cmd->buttonstates.m_pButtonStates[0] = t.bs0;
		cmd->buttonstates.m_pButtonStates[1] = t.bs1;
		cmd->buttonstates.m_pButtonStates[2] = t.bs2;
		for (const ScriptSubtick &sub : t.subticks)
		{
			auto *move = cmd->mutable_base()->add_subtick_moves();
			move->set_when(sub.when);
			move->set_button(sub.button);
			move->set_pressed(sub.pressed);
			move->set_analog_forward_delta(sub.afd);
			move->set_analog_left_delta(sub.ald);
			move->set_pitch_delta(sub.pd);
			move->set_yaw_delta(sub.yd);
		}
	}
	else
	{
		// H2: CS2 movement keys may need to be present in the button state, not just as analog fields.
		u32 buttons = t.buttons;
		if (g_scriptMode != ScriptMode::Fields)
		{
			if (t.fm > 0.0f)
			{
				buttons |= IN_FORWARD;
			}
			else if (t.fm < 0.0f)
			{
				buttons |= IN_BACK;
			}
			if (t.sm > 0.0f)
			{
				buttons |= IN_MOVELEFT;
			}
			else if (t.sm < 0.0f)
			{
				buttons |= IN_MOVERIGHT;
			}
		}

		cmd->mutable_base()->mutable_buttons_pb()->Clear();
		cmd->mutable_base()->mutable_buttons_pb()->set_buttonstate1(buttons);
		cmd->mutable_base()->mutable_buttons_pb()->set_buttonstate2(0);
		cmd->mutable_base()->mutable_buttons_pb()->set_buttonstate3(0);
		cmd->buttonstates.m_pButtonStates[0] = buttons;
		cmd->buttonstates.m_pButtonStates[1] = 0;
		cmd->buttonstates.m_pButtonStates[2] = 0;

		// H3: carry the analog input as a subtick move as well.
		if (g_scriptMode == ScriptMode::Subtick)
		{
			auto *move = cmd->mutable_base()->add_subtick_moves();
			move->set_when(0.0f);
			move->set_analog_forward_delta(t.fm);
			move->set_analog_left_delta(t.sm);
		}
	}

	CCSPlayerPawn *pawn = player->GetPlayerPawn();
	bool onGround = (pawn->m_fFlags() & FL_ONGROUND) != 0;
	if (!onGround)
	{
		this->wasAirborne = true;
	}
	else if (this->wasAirborne)
	{
		this->groundedTicks++;
	}

	Vector origin;
	player->GetOrigin(&origin);
	Vector vel;
	player->GetVelocity(&vel);
	if (this->log.is_open())
	{
		QAngle eyeAngles = pawn->m_angEyeAngles();
		i32 angleChanges = pawn->m_ServerViewAngleChanges().Count();
		this->log << tick << ',' << this->index << ',' << origin.x << ',' << origin.y << ',' << origin.z << ',' << vel.x << ',' << vel.y << ','
				  << vel.z << ',' << (onGround ? 1 : 0) << ',' << t.yaw << ',' << eyeAngles.y << ',' << cmd->base().consumed_server_angle_changes()
				  << ',' << angleChanges << '\n';
		this->log.flush();
	}

	this->index++;
	this->lastTick = tick;

	if (this->index >= this->ticks.size() || (this->wasAirborne && this->groundedTicks > GROUND_STOP_TICKS))
	{
		this->Stop();
	}
	return true;
}

// ============================================================================
// Commands
// ============================================================================

SCMD(kz_script_load, SCFL_MISC)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return true;
	}
	if (args->ArgC() < 2)
	{
		META_CONPRINTF("[KZScript] usage: kz_script_load <path>\n");
		return true;
	}
	scripted::GetForPlayer(player)->Load(args->Arg(1));
	return true;
}

SCMD(kz_script_start, SCFL_MISC)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return true;
	}
	scripted::GetForPlayer(player)->Start(player);
	return true;
}

SCMD(kz_script_stop, SCFL_MISC)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return true;
	}
	scripted::GetForPlayer(player)->Stop();
	return true;
}

SCMD(kz_script_status, SCFL_MISC)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return true;
	}
	scripted::GetForPlayer(player)->PrintStatus(player);
	return true;
}

// Server-console variants (work from RCON / server console, no client needed).
CON_COMMAND_F(kz_script_start_bot, "Load and start a script on a player slot from the server console: kz_script_start_bot <slot> <csv path>",
			  FCVAR_NONE)
{
	if (args.ArgC() < 3)
	{
		META_CONPRINTF("[KZScript] usage: kz_script_start_bot <slot> <path>\n");
		return;
	}
	int slot = atoi(args.Arg(1));
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!player)
	{
		META_CONPRINTF("[KZScript] no player in slot %d\n", slot);
		return;
	}
	ScriptedInput *input = scripted::GetForPlayer(player);
	if (!input)
	{
		META_CONPRINTF("[KZScript] invalid slot %d\n", slot);
		return;
	}
	META_CONPRINTF("[KZScript] bot start slot=%d fake=%d path=%s\n", slot, player->IsFakeClient() ? 1 : 0, args.Arg(2));
	if (input->Load(args.Arg(2)))
	{
		input->Start(player);
	}
}

CON_COMMAND_F(kz_script_bot_stop, "Stop scripted playback for a player slot: kz_script_bot_stop <slot>", FCVAR_NONE)
{
	if (args.ArgC() < 2)
	{
		META_CONPRINTF("[KZScript] usage: kz_script_bot_stop <slot>\n");
		return;
	}
	int slot = atoi(args.Arg(1));
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	ScriptedInput *input = player ? scripted::GetForPlayer(player) : nullptr;
	if (input)
	{
		input->Stop();
		META_CONPRINTF("[KZScript] stopped slot=%d\n", slot);
	}
}

CON_COMMAND_F(kz_script_bot_status, "Print scripted playback status for a player slot: kz_script_bot_status <slot>", FCVAR_NONE)
{
	if (args.ArgC() < 2)
	{
		return;
	}
	int slot = atoi(args.Arg(1));
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	ScriptedInput *input = player ? scripted::GetForPlayer(player) : nullptr;
	if (input)
	{
		input->PrintStatus(player);
	}
}

// H1: raw usercmd logging for a tagged slot (works with or without a script running).
CON_COMMAND_F(kz_cmdlog_start, "Log every usercmd for a slot: kz_cmdlog_start <slot> <tag>", FCVAR_NONE)
{
	if (args.ArgC() < 3)
	{
		META_CONPRINTF("[KZScript] usage: kz_cmdlog_start <slot> <tag>\n");
		return;
	}
	int slot = atoi(args.Arg(1));
	if (slot < 0 || slot >= 64)
	{
		META_CONPRINTF("[KZScript] invalid slot %d\n", slot);
		return;
	}
	CommandLog &log = commandLogs[slot];
	if (log.file.is_open())
	{
		log.file.flush();
		log.file.close();
	}
	char path[512];
	g_SMAPI->PathFormat(path, sizeof(path), "%s/addons/cs2kz/logs/cmdlog_%d.csv", g_SMAPI->GetBaseDir(), slot);
	log.file.open(path, std::ios::out | std::ios::trunc);
	if (!log.file.is_open())
	{
		META_CONPRINTF("[KZScript] cannot open %s\n", path);
		return;
	}
	log.file << "tag,tick_count,server_tick,fm,sm,um,btn_pb1,btn_pb2,btn_pb3,bs0,bs1,bs2,view_x,view_y,view_z,subtick_count,subtick,late,flags,"
				"origin_x,origin_y,origin_z,vel_x,vel_y,vel_z,on_ground\n";
	log.file.flush();
	log.active = true;
	log.slot = slot;
	log.tag = args.Arg(2);
	META_CONPRINTF("[KZScript] cmdlog start slot=%d tag=%s path=%s\n", slot, args.Arg(2), path);
}

CON_COMMAND_F(kz_cmdlog_stop, "Stop usercmd logging for a slot: kz_cmdlog_stop <slot>", FCVAR_NONE)
{
	if (args.ArgC() < 2)
	{
		META_CONPRINTF("[KZScript] usage: kz_cmdlog_stop <slot>\n");
		return;
	}
	int slot = atoi(args.Arg(1));
	if (slot < 0 || slot >= 64)
	{
		return;
	}
	CommandLog &log = commandLogs[slot];
	log.active = false;
	if (log.file.is_open())
	{
		log.file.flush();
		log.file.close();
	}
	META_CONPRINTF("[KZScript] cmdlog stop slot=%d tag=%s\n", slot, log.tag.c_str());
}

CON_COMMAND_F(kz_probe_z, "Trace a player hull straight down and print the ground height: kz_probe_z <x> <y> [slot] [start_z]", FCVAR_NONE)
{
	if (args.ArgC() < 3)
	{
		META_CONPRINTF("[KZProbe] usage: kz_probe_z <x> <y> [slot] [start_z]\n");
		return;
	}
	f32 x = (f32)atof(args.Arg(1));
	f32 y = (f32)atof(args.Arg(2));
	int slot = args.ArgC() >= 4 ? atoi(args.Arg(3)) : 0;
	f32 startZ = args.ArgC() >= 5 ? (f32)atof(args.Arg(4)) : 2000.0f;
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	CCSPlayerPawn *pawn = player ? player->GetPlayerPawn() : nullptr;
	if (!pawn)
	{
		META_CONPRINTF("[KZProbe] no pawn in slot %d\n", slot);
		return;
	}
	CTraceFilterPlayerMovementCS filter(pawn);
	bbox_t bounds;
	bounds.mins = {-16.0f, -16.0f, 0.0f};
	bounds.maxs = {16.0f, 16.0f, 72.0f};
	trace_t trace;
	INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), Vector(x, y, startZ), Vector(x, y, -2000.0f), &filter, &trace);
	META_CONPRINTF("[KZProbe] x=%.3f y=%.3f z=%.4f start_solid=%d frac=%.4f nz=%.3f\n", x, y, trace.m_vEndPos.z, trace.m_bStartInSolid ? 1 : 0,
				   trace.m_flFraction, trace.m_vHitNormal.z);
}

CON_COMMAND_F(kz_script_delay, "Set scripted playback start delay in ticks: kz_script_delay <ticks> (default 64)", FCVAR_NONE)
{
	if (args.ArgC() < 2)
	{
		META_CONPRINTF("[KZScript] delay=%d ticks\n", g_startDelayTicks);
		return;
	}
	int ticks = atoi(args.Arg(1));
	g_startDelayTicks = std::clamp(ticks, 0, 512);
	META_CONPRINTF("[KZScript] delay=%d ticks\n", g_startDelayTicks);
}

CON_COMMAND_F(kz_script_mode, "Set scripted input injection mode: fields | buttons | subtick | verbatim", FCVAR_NONE)
{
	if (args.ArgC() < 2)
	{
		META_CONPRINTF("[KZScript] mode=%s\n", ModeName(g_scriptMode));
		return;
	}
	if (!V_stricmp(args.Arg(1), "fields"))
	{
		g_scriptMode = ScriptMode::Fields;
	}
	else if (!V_stricmp(args.Arg(1), "buttons"))
	{
		g_scriptMode = ScriptMode::Buttons;
	}
	else if (!V_stricmp(args.Arg(1), "subtick"))
	{
		g_scriptMode = ScriptMode::Subtick;
	}
	else if (!V_stricmp(args.Arg(1), "verbatim"))
	{
		g_scriptMode = ScriptMode::Verbatim;
	}
	else
	{
		META_CONPRINTF("[KZScript] usage: kz_script_mode <fields|buttons|subtick|verbatim>\n");
		return;
	}
	META_CONPRINTF("[KZScript] mode=%s\n", ModeName(g_scriptMode));
}
