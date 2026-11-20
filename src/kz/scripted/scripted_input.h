#pragma once

#include "sdk/usercmd.h"

#include <fstream>
#include <string>
#include <vector>

class KZPlayer;
class Jump;

// Scripted usercmd playback (offline CSV -> server-side override).
// CSV columns: tick,yaw,pitch,fm,sm,um,buttons
// Optional first comment line: "# start x y z yaw"
namespace scripted
{
	struct ScriptTick
	{
		f32 yaw {};
		f32 pitch {};
		f32 fm {};
		f32 sm {};
		f32 um {};
		u32 buttons {};
	};

	class ScriptedInput
	{
	public:
		bool Load(const char *path);
		bool Start(KZPlayer *player);
		void Stop();
		void PrintStatus(KZPlayer *player) const;
		// Returns true if this command was overwritten by the script.
		bool OnPlayerRunCommand(KZPlayer *player, CUserCmd *cmd);
		void Reset();

		bool IsLoaded() const
		{
			return this->loaded;
		}

		bool IsActive() const
		{
			return this->active;
		}

		size_t GetIndex() const
		{
			return this->index;
		}

		size_t GetCount() const
		{
			return this->ticks.size();
		}

		const std::string &GetPath() const
		{
			return this->path;
		}

	private:
		std::vector<ScriptTick> ticks;
		Vector startOrigin {};
		f32 startYaw {};
		std::string path;
		std::ofstream log;
		bool hasStart {};
		bool loaded {};
		bool active {};
		size_t index {};
		u32 startTick {};
		u32 lastTick {};
		bool wasAirborne {};
		int groundedTicks {};
	};

	// One scripted input per player slot.
	ScriptedInput *GetForPlayer(KZPlayer *player);
	// Returns true when the command was overwritten (caller must run the original).
	bool OnPlayerRunCommand(KZPlayer *player, CUserCmd *cmd);
	void ResetAll();

	// Y3 diagnostics: unconditional per-jump and per-usercmd CSV logs.
	void LogJump(KZPlayer *player, Jump *jump);
	void LogCommand(KZPlayer *player, CUserCmd *cmd);
} // namespace scripted
