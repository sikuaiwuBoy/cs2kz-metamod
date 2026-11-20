#include "scripted_input.h"

#include "kz/kz.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "utils/simplecmds.h"
#include "utils/utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using scripted::ScriptTick;
using scripted::ScriptedInput;

namespace
{
	constexpr int START_DELAY_TICKS = 64;
	constexpr int GROUND_STOP_TICKS  = 16;

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
		int slot = player->GetPlayerSlot();
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
		f32 raw        = 32.0f + (jump->landingOrigin - jump->takeoffOrigin).Length2D();
		g_jumpLog << jump->serverTick << ',' << (int)player->GetPlayerSlot() << ',' << (int)jump->GetJumpType() << ','
				  << (jump->IsValid() ? 1 : 0) << ",\"" << jump->invalidateReason << "\","
				  << jump->GetDistance(true, false) << ',' << raw << ',' << jump->GetTakeoffSpeed() << ','
				  << jump->GetMaxSpeed() << ',' << vel.Length2D() << ',' << jump->strafes.size() << ',' << jump->GetSync()
				  << ',' << t.x << ',' << t.y << ',' << t.z << ',' << l.x << ',' << l.y << ',' << l.z << ','
				  << (int)std::lround(jump->airtime * 64.0f) << ',' << player->takeoffGroundOrigin.z << ',' << l.x << ','
				  << l.y << ',' << l.z << '\n';
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
			V_snprintf(buf, sizeof(buf), "%.4f;%u;%.4f;%.4f;%.4f;%.4f", m.when(), m.button(), m.analog_forward_delta(),
					   m.analog_left_delta(), m.pitch_delta(), m.yaw_delta());
			subtick += buf;
		}
		Vector origin = player->GetPlayerPawn()->m_vecAbsOrigin();
		Vector vel;
		player->GetVelocity(&vel);
		u32 serverTick = g_pKZUtils->GetServerGlobals()->tickcount;
		g_cmdLog << player->GetController()->m_nTickBase() << ',' << serverTick << ',' << base->forwardmove() << ','
				 << base->leftmove() << ',' << base->upmove() << ',' << base->buttons_pb().buttonstate1() << ','
				 << base->viewangles().y() << ',' << base->viewangles().x() << ',' << subtick << ',' << origin.x << ','
				 << origin.y << ',' << origin.z << ',' << vel.x << ',' << vel.y << ',' << vel.z << ','
				 << ((player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND) ? 1 : 0) << '\n';
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
	this->path   = filePath;
	this->hasStart = false;
	std::string line;
	std::vector<std::string> cells;
	while (std::getline(file, line))
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
		{
			line.pop_back();
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
				this->startYaw    = yaw;
				this->hasStart    = true;
			}
			continue;
		}
		if (line.rfind("tick", 0) == 0)
		{
			continue; // header
		}
		Split(line, cells);
		if (cells.size() < 7)
		{
			continue;
		}
		try
		{
			ScriptTick tick;
			tick.yaw     = std::stof(cells[1]);
			tick.pitch   = std::stof(cells[2]);
			tick.fm      = std::clamp(std::stof(cells[3]), -1.0f, 1.0f);
			tick.sm      = std::clamp(std::stof(cells[4]), -1.0f, 1.0f);
			tick.um      = std::clamp(std::stof(cells[5]), -1.0f, 1.0f);
			tick.buttons = static_cast<u32>(std::stoull(cells[6]));
			this->ticks.push_back(tick);
		}
		catch (...)
		{
			continue;
		}
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
		origin = player->GetPlayerPawn()->m_vecAbsOrigin();
	}
	Vector velocity(0, 0, 0);
	QAngle angles;
	angles.x = 0;
	angles.y = this->startYaw;
	angles.z = 0;
	player->Teleport(&origin, &angles, &velocity);

	this->active         = true;
	this->index          = 0;
	this->startTick      = player->GetController()->m_nTickBase() + START_DELAY_TICKS;
	this->lastTick       = 0;
	this->wasAirborne    = false;
	this->groundedTicks  = 0;

	if (this->log.is_open())
	{
		this->log.close();
	}
	char logPath[512];
	g_SMAPI->PathFormat(logPath, sizeof(logPath), "%s/addons/cs2kz/logs/scripted_%d.csv", g_SMAPI->GetBaseDir(),
						(int)player->GetPlayerSlot());
	this->log.open(logPath, std::ios::out | std::ios::trunc);
	if (this->log.is_open())
	{
		this->log << "tick_count,script_idx,origin_x,origin_y,origin_z,vel_x,vel_y,vel_z,on_ground,yaw\n";
	}
	META_CONPRINTF("[KZScript] start slot=%d path=%s ticks=%zu start_tick=%u\n", (int)player->GetPlayerSlot(), this->path.c_str(),
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
	this->loaded    = false;
	this->hasStart  = false;
	this->index     = 0;
	this->path.clear();
}

void ScriptedInput::PrintStatus(KZPlayer *player) const
{
	META_CONPRINTF("[KZScript] loaded=%d active=%d idx=%zu/%zu path=%s\n", this->loaded ? 1 : 0, this->active ? 1 : 0,
				   this->index, this->ticks.size(), this->path.c_str());
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
	cmd->mutable_base()->mutable_viewangles()->set_z(0.0f);

	cmd->mutable_base()->mutable_buttons_pb()->Clear();
	u32 b1 = t.buttons;
	u32 b2 = 0;
	if (t.buttons)
	{
		cmd->mutable_base()->mutable_buttons_pb()->set_buttonstate1(b1);
		cmd->mutable_base()->mutable_buttons_pb()->set_buttonstate2(b2);
		cmd->mutable_base()->mutable_buttons_pb()->set_buttonstate3(0);
	}
	cmd->buttonstates.m_pButtonStates[0] = b1;
	cmd->buttonstates.m_pButtonStates[1] = b2;
	cmd->buttonstates.m_pButtonStates[2] = 0;

	CCSPlayerPawn *pawn = player->GetPlayerPawn();
	bool onGround       = (pawn->m_fFlags() & FL_ONGROUND) != 0;
	if (!onGround)
	{
		this->wasAirborne = true;
	}
	else if (this->wasAirborne)
	{
		this->groundedTicks++;
	}

	Vector origin = pawn->m_vecAbsOrigin();
	Vector vel;
	player->GetVelocity(&vel);
	if (this->log.is_open())
	{
		this->log << tick << ',' << this->index << ',' << origin.x << ',' << origin.y << ',' << origin.z << ',' << vel.x << ','
				  << vel.y << ',' << vel.z << ',' << (onGround ? 1 : 0) << ',' << t.yaw << '\n';
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
	if (args->ArgCount() < 2)
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