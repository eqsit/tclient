// Adapted from Kinetix's basic_avoid_freeze.cpp (local Kinetix source).
// Keep the latest-safe-tick rule and input-difference tie-break; prefer a
// verified rocket over an automatic hook, and the shortest safe hook pulse.
#include "avoid_planner.h"

#include "entities/character.h"
#include "entities/projectile.h"
#include "gameworld.h"

#include <base/math.h>
#include <base/time.h>

#include <game/collision.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <limits>

const char *CAvoidPlanner::ModeName(EMode Mode)
{
	switch(Mode)
	{
	case EMode::SAFE: return "safe";
	case EMode::WAIT: return "wait";
	case EMode::MOVE: return "move";
	case EMode::HOOK: return "hook-pulse";
	case EMode::ROCKET: return "rocket";
	case EMode::ROCKET_HOOK: return "rocket+hook";
	case EMode::BEST_EFFORT: return "best-effort";
	case EMode::NO_SOLUTION: return "no-solution";
	}
	return "unknown";
}

CNetObj_PlayerInput CAvoidPlanner::InputAt(const SPlan &Plan, const CNetObj_PlayerInput &Current, int Tick)
{
	if(Tick < Plan.m_Delay)
	{
		auto Input = Plan.m_FixedWaitInput ? Plan.m_WaitInput : Current;
		if(Plan.m_ReleaseDuringDelay && Tick == 0)
			Input.m_Hook = 0;
		return Input;
	}
	const int Step = Tick - Plan.m_Delay;
	CNetObj_PlayerInput Input = Plan.m_Input;
	if(Plan.m_MoveTicks >= 0 && Step >= Plan.m_MoveTicks)
	{
		Input.m_Direction = Current.m_Direction;
		Input.m_Jump = Current.m_Jump;
		Input.m_TargetX = Current.m_TargetX;
		Input.m_TargetY = Current.m_TargetY;
		if(Plan.m_HookTicks < 0)
			Input.m_Hook = Current.m_Hook;
	}
	if(Plan.m_HookTicks >= 0)
	{
		// A rocket and a newly launched hook share one aim field. Launch the
		// hook on the next tick with its own aim; validate that exact sequence.
		const int HookStart = Plan.m_Rocket ? Plan.m_SwitchTicks + 1 : Plan.m_HookDelay;
		const int End = HookStart + Plan.m_HookTicks;
		Input.m_Hook = Step > End ? Current.m_Hook : Step >= HookStart && Step < End;
		if(Input.m_Hook && Step <= End)
		{
			Input.m_TargetX = Plan.m_Input.m_TargetX;
			Input.m_TargetY = Plan.m_Input.m_TargetY;
		}
		if(Step > End)
		{
			Input.m_TargetX = Current.m_TargetX;
			Input.m_TargetY = Current.m_TargetY;
		}
	}
	if(Plan.m_Rocket)
	{
		Input.m_WantedWeapon = WEAPON_GRENADE + 1;
		Input.m_NextWeapon = Current.m_NextWeapon;
		Input.m_PrevWeapon = Current.m_PrevWeapon;
		// Exactly one press, then an even release counter, including wraparound.
		const int Press = (Current.m_Fire + ((Current.m_Fire & 1) ? 2 : 1)) & INPUT_STATE_MASK;
		Input.m_Fire = Step < Plan.m_SwitchTicks ? (Current.m_Fire + (Current.m_Fire & 1)) & INPUT_STATE_MASK : Step == Plan.m_SwitchTicks ? Press :
																		     (Press + 1) & INPUT_STATE_MASK;
		if(Step <= Plan.m_SwitchTicks)
		{
			Input.m_TargetX = round_to_int(Plan.m_RocketAim.x);
			Input.m_TargetY = round_to_int(Plan.m_RocketAim.y);
		}
	}
	return Input;
}

CAvoidPlanner::SProbe CAvoidPlanner::Simulate(CGameWorld &World, int LocalId, const CNetObj_PlayerInput &Current, const SPlan &Plan, int Ticks, const SConfig &Config)
{
	m_Calls++;
	CGameWorld Sim;
	// CopyWorldClean does not link speculative entities back into live prediction.
	Sim.CopyWorldClean(&World, false);
	Sim.m_LocalClientId = World.m_LocalClientId;
	Sim.m_WorldConfig.m_PredictEvents = false;
	Sim.m_WorldConfig.m_PredictTiles = true;
	Sim.m_WorldConfig.m_PredictFreeze = 1;
	Sim.m_WorldConfig.m_PredictWeapons = true;
	SProbe Result;
	CCharacter *pChar = Sim.GetCharacterById(LocalId);
	if(!pChar)
	{
		Result.m_Danger = 1;
		Result.m_Kind = "missing-character";
		return Result;
	}
	// Unlike the original Kinetix probe, keep other tees and projectiles:
	// collisions, existing hooks and grenade hits are part of the rescue.
	const int StartTick = Sim.GameTick();
	const int OldAttack = pChar->GetAttackTick();
	CProjectile *pShot = nullptr;
	for(int t = 0; t < Ticks; t++)
	{
		m_SimTicks++;
		const auto Input = InputAt(Plan, Current, t);
		Result.m_Pos = pChar->m_Pos;
		pChar->OnDirectInput(&Input);
		Sim.m_GameTick = StartTick + t + 1;
		pChar->OnPredictedInput(&Input);
		if(Plan.m_Rocket && !Result.m_ShotTick && pChar->GetActiveWeapon() == WEAPON_GRENADE && pChar->GetAttackTick() > OldAttack)
			Result.m_ShotTick = t + 1;
		if(Result.m_ShotTick && !pShot && !Result.m_BlastTick)
			for(CEntity *pEnt = Sim.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
			{
				auto *pProj = static_cast<CProjectile *>(pEnt);
				if(pProj->GetOwner() == LocalId && pProj->GetWeaponType() == WEAPON_GRENADE && pProj->GetStartTick() >= StartTick)
				{
					pShot = pProj;
					break;
				}
			}
		if(pShot)
		{
			const float PrevTime = (Sim.GameTick() - pShot->GetStartTick() - 1) / (float)Sim.GameTickSpeed();
			const float NowTime = (Sim.GameTick() - pShot->GetStartTick()) / (float)Sim.GameTickSpeed();
			// For logging; physics and the blast itself still come from Tick().
			Sim.Collision()->IntersectLine(pShot->GetPos(PrevTime), pShot->GetPos(NowTime), &Result.m_BlastPos, nullptr);
		}
		Sim.Tick();
		if(pShot)
		{
			bool Present = false;
			for(CEntity *pEnt = Sim.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEnt; pEnt = pEnt->TypeNext())
				Present |= pEnt == pShot;
			if(!Present)
			{
				Result.m_BlastTick = t + 1;
				pShot = nullptr;
			}
		}
		pChar = Sim.GetCharacterById(LocalId);
		const char *pDanger = nullptr;
		if(!pChar)
			pDanger = "removed";
		else
		{
			Result.m_Pos = pChar->m_Pos;
			const auto *pCore = pChar->Core();
			if(Config.m_Freeze && (pChar->m_FreezeTime > 0 || pCore->m_DeepFrozen || pCore->m_LiveFrozen || pCore->m_IsInFreeze))
				pDanger = "freeze";
			const int Index = Sim.Collision()->GetPureMapIndex(pChar->m_Pos);
			Result.m_Tile = Sim.Collision()->GetTileIndex(Index);
			Result.m_Front = Sim.Collision()->GetFrontTileIndex(Index);
			auto FreezeTile = [](int Tile) { return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE; };
			// Native tile traversal can omit indices outside map bounds, while
			// the server's center collision clamps to the edge freeze tile.
			if(Config.m_Freeze && !pCore->m_Super && !pCore->m_Invincible && (FreezeTile(Result.m_Tile) || FreezeTile(Result.m_Front)))
				pDanger = "freeze";
			if(Config.m_Death && (Result.m_Tile == TILE_DEATH || Result.m_Front == TILE_DEATH))
				pDanger = "death";
			if(Config.m_Teleport && (Sim.Collision()->IsTeleport(Index) || Sim.Collision()->IsEvilTeleport(Index)))
				pDanger = "teleport";
		}
		if(pDanger)
		{
			Result.m_Danger = t + 1;
			Result.m_Kind = pDanger;
			Result.m_Survival = t;
			return Result;
		}
	}
	Result.m_Survival = Ticks;
	return Result;
}

CAvoidPlanner::SPlan CAvoidPlanner::ReturnControlPlan(const CNetObj_PlayerInput &Current, bool OwnHook)
{
	SPlan Plan;
	Plan.m_Input = Current;
	if(OwnHook && Current.m_Hook)
	{
		// A held hook ignores a new aim until it has actually been released.
		// Returning the user's hook needs a release tick followed by relaunch.
		Plan.m_HookTicks = 0;
		Plan.m_MoveTicks = 1;
	}
	return Plan;
}

int CAvoidPlanner::Duration(const SPlan &Plan)
{
	const int HookStart = Plan.m_Rocket ? Plan.m_SwitchTicks + 1 : Plan.m_HookDelay;
	return std::max({0, Plan.m_MoveTicks, Plan.m_Rocket ? Plan.m_SwitchTicks + 1 : 0,
		Plan.m_HookTicks >= 0 ? HookStart + Plan.m_HookTicks + 1 : 0});
}

CAvoidPlanner::SPlan CAvoidPlanner::RemainingPlan(const SPlan &Plan, const CNetObj_PlayerInput &Current, int Elapsed)
{
	SPlan Remaining = Plan;
	Remaining.m_Input.m_Fire = Current.m_Fire;
	if(Plan.m_Rocket)
	{
		Remaining.m_Input.m_WantedWeapon = WEAPON_GRENADE + 1;
		Remaining.m_Input.m_NextWeapon = Current.m_NextWeapon;
		Remaining.m_Input.m_PrevWeapon = Current.m_PrevWeapon;
	}
	Remaining.m_MoveTicks = Plan.m_MoveTicks < 0 ? -1 : std::max(0, Plan.m_MoveTicks - Elapsed);
	const int HookStart = Plan.m_Rocket ? Plan.m_SwitchTicks + 1 : Plan.m_HookDelay;
	Remaining.m_HookDelay = std::max(0, HookStart - Elapsed);
	if(Plan.m_HookTicks >= 0)
	{
		if(Elapsed > HookStart + Plan.m_HookTicks)
		{
			Remaining.m_HookTicks = -1;
			Remaining.m_Input.m_Hook = Current.m_Hook;
		}
		else
			Remaining.m_HookTicks = std::max(0, HookStart + Plan.m_HookTicks - Elapsed - Remaining.m_HookDelay);
	}
	Remaining.m_Rocket = Plan.m_Rocket && Elapsed <= Plan.m_SwitchTicks;
	Remaining.m_SwitchTicks = std::max(0, Plan.m_SwitchTicks - Elapsed);
	Remaining.m_Delay = 0;
	return Remaining;
}

CAvoidPlanner::SDecision CAvoidPlanner::Decide(CGameWorld &World, int LocalId, const CNetObj_PlayerInput &Current, const SConfig &Config, const SPlan *pPrevious, bool OwnHook)
{
	m_Calls = m_SimTicks = 0;
	const int64_t Start = time_get_impl();
	SDecision Decision;
	Decision.m_Plan = ReturnControlPlan(Current, OwnHook);
	auto Finish = [&]() {
		Decision.m_Calls = m_Calls;
		Decision.m_SimTicks = m_SimTicks;
		return Decision;
	};
	auto Exhausted = [&]() {
		const bool Limited = m_Calls >= std::max(1, Config.m_MaxCalls - 2) ||
				     (Config.m_BudgetMs > 0 && (time_get_impl() - Start) * 1000.0 / time_freq() >= Config.m_BudgetMs * 0.80);
		Decision.m_Limited |= Limited;
		return Limited;
	};
	const int Ticks = std::clamp(Config.m_Ticks, 2, 20);
	Decision.m_Base = Simulate(World, LocalId, Current, Decision.m_Plan, Ticks, Config);
	Decision.m_Result = Decision.m_Base;
	if(!Decision.m_Base.m_Danger)
		return Finish();
	CCharacter *pLocal = World.GetCharacterById(LocalId);
	if(!pLocal)
	{
		Decision.m_Mode = EMode::NO_SOLUTION;
		return Finish();
	}
	// Resolve the current threat, then reconsider the player's next input.
	// Requiring a hook to make the entire distant horizon safe unnecessarily
	// brakes rocket boosts and pulls the tee away from the nearby freeze edge.
	const int RescueTicks = std::min(Ticks, std::max(6, Decision.m_Base.m_Danger + 4));
	auto Diff = [&](const SPlan &Plan) {
		return (Plan.m_Input.m_Direction != Current.m_Direction) +
		       ((Plan.m_Input.m_Jump != 0) != (Current.m_Jump != 0)) +
		       ((Plan.m_Input.m_Hook != 0) != (Current.m_Hook != 0)) +
		       (Plan.m_Input.m_TargetX != Current.m_TargetX || Plan.m_Input.m_TargetY != Current.m_TargetY);
	};
	std::vector<SPlan> Moves, Hooks;
	for(int Dir : {Current.m_Direction, 0, -1, 1})
		for(int Jump : {int(Current.m_Jump != 0), 0, 1})
			for(int Hook : {Current.m_Hook, 0})
			{
				if((!Config.m_Direction && Dir != Current.m_Direction) || (!Config.m_Jump && Jump != (Current.m_Jump != 0)))
					continue;
				SPlan Move;
				Move.m_Input = Current;
				Move.m_Input.m_Direction = Dir;
				Move.m_Input.m_Jump = Jump;
				Move.m_Input.m_Hook = Hook;
				if(OwnHook && Hook)
					Move.m_HookTicks = 0;
				Move.m_MoveTicks = Ticks;
				if(std::none_of(Moves.begin(), Moves.end(), [&](const SPlan &P) {
					   return P.m_Input.m_Direction == Dir && P.m_Input.m_Jump == Jump && P.m_Input.m_Hook == Hook;
				   }))
					Moves.push_back(Move);
			}
	std::stable_sort(Moves.begin(), Moves.end(), [&](const SPlan &A, const SPlan &B) { return Diff(A) < Diff(B); });
	const int Angles = Config.m_Aim ? std::clamp(Config.m_Angles, 1, 72) : 1;
	const float Angle = std::atan2((float)Current.m_TargetY, (float)Current.m_TargetX);
	const vec2 Velocity = pLocal->Core()->m_Vel;
	const bool RocketBoost = pLocal->GetActiveWeapon() == WEAPON_GRENADE && pLocal->GetReloadTimer() > 0 &&
				 std::abs(Velocity.x) >= 12 && std::abs(Velocity.x) > 2 * std::abs(Velocity.y);
	auto HookCost = [&](vec2 Aim, float Distance) {
		// Preserve a horizontal rocket boost when a forward ray can rescue.
		// Vertical falls still need the original priority for braking hooks.
		const float Pull = dot(Aim, Velocity);
		return (RocketBoost ? std::max(0.0f, -Pull) : Pull) + Distance / 80;
	};
	std::vector<vec2> Aims;
	Aims.emplace_back(Current.m_TargetX, Current.m_TargetY);
	if(Config.m_Aim)
	{
		if(OwnHook && (pLocal->Core()->m_HookState == HOOK_FLYING || pLocal->Core()->m_HookState == HOOK_GRABBED))
			Aims.push_back(pLocal->Core()->m_HookDir * 1000);
		if(pPrevious && pPrevious->m_HookTicks > 0)
			Aims.emplace_back(pPrevious->m_Input.m_TargetX, pPrevious->m_Input.m_TargetY);
		for(int a = 1; a < Angles; a++)
		{
			const float A = Angle + (float)a / Angles * Config.m_Fov * pi / 180;
			Aims.push_back(vec2(std::cos(A), std::sin(A)) * 1000);
		}
		// A uniform angle grid can miss a single hookable tile between rays.
		// Aim at exposed, actual wall tiles too, particularly nearby corners.
		const float Reach = World.GetTuning(pLocal->GetOverriddenTuneZone())->m_HookLength;
		struct STarget
		{
			vec2 m_Pos;
			float m_Cost;
		};
		std::vector<STarget> Targets;
		const int Radius = (int)Reach / 32 + 1;
		const int TileX = (int)pLocal->m_Pos.x / 32, TileY = (int)pLocal->m_Pos.y / 32;
		for(int y = TileY - Radius; y <= TileY + Radius; y++)
			for(int x = TileX - Radius; x <= TileX + Radius; x++)
			{
				const vec2 Center(x * 32 + 16, y * 32 + 16);
				if(distance(Center, pLocal->m_Pos) > Reach + 16 || World.Collision()->GetTile(Center.x, Center.y) != TILE_SOLID)
					continue;
				if(World.Collision()->GetTile(Center.x - 32, Center.y) == TILE_SOLID &&
					World.Collision()->GetTile(Center.x + 32, Center.y) == TILE_SOLID &&
					World.Collision()->GetTile(Center.x, Center.y - 32) == TILE_SOLID &&
					World.Collision()->GetTile(Center.x, Center.y + 32) == TILE_SOLID)
					continue;
				const vec2 Near(std::clamp(pLocal->m_Pos.x, Center.x - 14, Center.x + 14), std::clamp(pLocal->m_Pos.y, Center.y - 14, Center.y + 14));
				for(vec2 Target : {Near, Center})
				{
					const vec2 Delta = Target - pLocal->m_Pos;
					if(length(Delta) < 1)
						continue;
					const float Offset = std::remainder(std::atan2(Delta.y, Delta.x) - Angle, 2 * pi);
					if(std::abs(Offset) > Config.m_Fov * pi / 360)
						continue;
					Targets.push_back({Target, HookCost(normalize(Delta), length(Delta))});
				}
			}
		std::stable_sort(Targets.begin(), Targets.end(), [](const STarget &A, const STarget &B) { return A.m_Cost < B.m_Cost; });
		for(size_t i = 0; i < std::min<size_t>(Targets.size(), 32); i++)
			Aims.push_back(normalize(Targets[i].m_Pos - pLocal->m_Pos) * 1000);
	}
	struct SHookRay
	{
		vec2 m_Aim;
		int m_Delay;
		float m_Cost;
	};
	std::vector<SHookRay> Rays;
	if(Config.m_Hook)
		for(vec2 Target : Aims)
		{
			Target = vec2(round_to_int(Target.x), round_to_int(Target.y));
			if(std::any_of(Rays.begin(), Rays.end(), [&](const SHookRay &R) { return R.m_Aim == Target; }))
				continue;
			const bool Reaim = Target.x != Current.m_TargetX || Target.y != Current.m_TargetY;
			const vec2 Aim = normalize(Target);
			vec2 Hit, Before;
			const float Reach = World.GetTuning(pLocal->GetOverriddenTuneZone())->m_HookLength;
			const int Tile = World.Collision()->IntersectLineTeleHook(pLocal->m_Pos, pLocal->m_Pos + Aim * Reach, &Hit, &Before);
			const bool KeepingOwnedHook = OwnHook &&
						      (pLocal->Core()->m_HookState == HOOK_FLYING || pLocal->Core()->m_HookState == HOOK_GRABBED) &&
						      length(pLocal->Core()->m_HookDir) > 0.5f &&
						      dot(Aim, normalize(pLocal->Core()->m_HookDir)) > 0.9999f;
			const bool KeepingHook = KeepingOwnedHook || (!Reaim && pLocal->Core()->m_HookState == HOOK_GRABBED);
			if(!KeepingHook && Tile != TILE_SOLID && Tile != TILE_TELEINHOOK && !World.IntersectCharacter(pLocal->m_Pos, pLocal->m_Pos + Aim * Reach, 0, Hit, pLocal, LocalId))
				continue;
			const int Delay = !KeepingOwnedHook && (OwnHook || Reaim) && pLocal->Core()->m_HookState != HOOK_IDLE ? 1 : 0;
			Rays.push_back({Target, Delay, HookCost(Aim, distance(pLocal->m_Pos, Hit))});
		}
	for(const auto &Move : Moves)
	{
		if(Move.m_Input.m_Hook != Current.m_Hook)
			continue;
		for(const auto &Ray : Rays)
		{
			SPlan Hook = Move;
			Hook.m_Input.m_Hook = 1;
			Hook.m_Input.m_TargetX = round_to_int(Ray.m_Aim.x);
			Hook.m_Input.m_TargetY = round_to_int(Ray.m_Aim.y);
			Hook.m_HookDelay = Ray.m_Delay;
			Hook.m_SearchCost = Ray.m_Cost;
			Hook.m_HookTicks = Ticks;
			Hook.m_MoveTicks = Duration(Hook);
			Hooks.push_back(Hook);
		}
	}
	std::stable_sort(Hooks.begin(), Hooks.end(), [&](const SPlan &A, const SPlan &B) {
		return Diff(A) * 2 + A.m_SearchCost < Diff(B) * 2 + B.m_SearchCost;
	});
	if(OwnHook && length(pLocal->Core()->m_HookDir) > 0.5f)
	{
		const auto Keeping = std::find_if(Hooks.begin(), Hooks.end(), [&](const SPlan &Plan) {
			return Plan.m_HookDelay == 0 && Plan.m_Input.m_Direction == Current.m_Direction &&
			       dot(normalize(vec2(Plan.m_Input.m_TargetX, Plan.m_Input.m_TargetY)), normalize(pLocal->Core()->m_HookDir)) > 0.9999f;
		});
		if(Keeping != Hooks.end())
			std::rotate(Hooks.begin(), Keeping, Keeping + 1);
	}
	bool Found = false;
	int BestCost = std::numeric_limits<int>::max();
	auto RefreshPrevious = [&]() {
		auto Previous = *pPrevious;
		Previous.m_Input.m_Direction = Current.m_Direction;
		Previous.m_Input.m_Jump = Current.m_Jump;
		Previous.m_Input.m_Fire = Current.m_Fire;
		Previous.m_Input.m_WantedWeapon = Current.m_WantedWeapon;
		Previous.m_Input.m_NextWeapon = Current.m_NextWeapon;
		Previous.m_Input.m_PrevWeapon = Current.m_PrevWeapon;
		return Previous;
	};
	auto Consider = [&](const SPlan &Plan) {
		const auto Probe = Simulate(World, LocalId, Current, Plan, RescueTicks, Config);
		const int Cost = Diff(Plan) * 100 + Duration(Plan);
		if(Probe.m_Survival > Decision.m_Result.m_Survival ||
			(Found && Probe.m_Survival == Decision.m_Result.m_Survival && Cost < BestCost))
		{
			Found = true;
			Decision.m_Plan = Plan;
			Decision.m_Result = Probe;
			BestCost = Cost;
		}
		return !Probe.m_Danger;
	};
	auto ShortenHook = [&]() {
		if(Decision.m_Result.m_Danger || Decision.m_Plan.m_HookTicks <= 1)
			return;
		const auto Long = Decision.m_Plan;
		for(int Pulse = 1; Pulse < Long.m_HookTicks && !Exhausted(); Pulse++)
		{
			auto Short = Long;
			Short.m_HookTicks = Pulse;
			Short.m_MoveTicks = Short.m_HookDelay + Pulse + 1;
			if(Consider(Short))
				break;
		}
	};
	if(pPrevious && !pPrevious->m_Rocket && !Exhausted())
		Consider(RefreshPrevious());
	const bool CanRocket = Config.m_Rocket && pLocal->GetWeaponGot(WEAPON_GRENADE) && pLocal->GetWeaponAmmo(WEAPON_GRENADE) != 0 &&
			       !pLocal->GetWeaponGot(WEAPON_NINJA) && pLocal->GetReloadTimer() <= (pLocal->GetActiveWeapon() == WEAPON_GRENADE ? 0 : 1);
	const int RocketTicks = std::clamp(Config.m_RocketTicks, Ticks, 60);
	int SwitchTicks = 1;
	if(CanRocket && !pLocal->GetReloadTimer() && !Exhausted())
	{
		// Active weapon alone is insufficient: returning the previous weapon
		// can leave a queued switch, and a grenade warmup may already have
		// prepared the next direct input while the snapshot still shows a gun.
		// Ask native weapon handling whether firing this tick actually works.
		SPlan Ready;
		Ready.m_Input = Current;
		Ready.m_Rocket = true;
		Ready.m_RocketAim = vec2(Current.m_TargetX, Current.m_TargetY);
		SwitchTicks = Simulate(World, LocalId, Current, Ready, 1, Config).m_ShotTick == 1 ? 0 : 1;
	}
	const int CloseWindow = 4 + SwitchTicks;
	auto RocketSearch = [&](const std::vector<SPlan> &Bases, bool Combined, int PhaseCalls) {
		const int FirstCall = m_Calls;
		const int Samples = std::clamp(Config.m_RocketAngles, 8, 96);
		auto TurnsSafe = [&](const SPlan &Plan, const SProbe &Probe) {
			// Only uncertainty before/around this blast matters to this rescue.
			// Later danger is handled by the next tick's avoid decision.
			const int Horizon = std::min(RocketTicks, Probe.m_BlastTick + 3);
			for(int Dir : {-1, 0, 1})
				for(int Jump : {int(Current.m_Jump != 0)})
				{
					if(Exhausted())
						return false;
					auto Turn = Current;
					Turn.m_Direction = Dir;
					Turn.m_Jump = Jump;
					auto TurnPlan = Plan;
					TurnPlan.m_FixedWaitInput = true;
					TurnPlan.m_WaitInput = Current;
					if(Simulate(World, LocalId, Turn, TurnPlan, Horizon, Config).m_Danger)
						return false;
				}
			return true;
		};
		// Cover all four quadrants early, rather than spending the budget on
		// downward shots when the tee is flying into upper freeze.
		for(size_t b = 0; b < std::min<size_t>(Bases.size(), Combined ? 4 : 6); b++)
		{
			for(int a = -1; a < Samples; a++)
			{
				if(a < 0 && (!pPrevious || !pPrevious->m_Rocket))
					continue;
				if(Exhausted())
					return false;
				if(m_Calls - FirstCall >= PhaseCalls ||
					(Config.m_BudgetMs > 0 && (time_get_impl() - Start) * 1000.0 / time_freq() >= Config.m_BudgetMs * (Combined ? 0.60 : 0.30)))
				{
					Decision.m_Limited = true;
					return false;
				}
				SPlan Shot = Bases[b];
				Shot.m_Rocket = true;
				// Forecast a later close shot; never fire it from far away. Rebuild
				// that forecast from the player's latest input on the next tick.
				// A held grenade fire button already requests a shot now. Do not
				// forecast waiting through that real shot and reject every rescue
				// as a shot at the wrong tick; test redirecting the requested shot.
				const bool RequestedShot = (Current.m_Fire & 1) && !SwitchTicks;
				Shot.m_Delay = RequestedShot ? 0 : std::max(0, Decision.m_Base.m_Danger - CloseWindow);
				Shot.m_ReleaseDuringDelay = OwnHook;
				if(OwnHook && !Combined)
					Shot.m_HookTicks = 0; // Replace our hook; do not count its pull as rocket-only rescue.
				Shot.m_SwitchTicks = SwitchTicks;
				Shot.m_HookDelay = 0;
				Shot.m_MoveTicks = Shot.m_SwitchTicks + 1;
				const int Index = a < 0 ? 0 : (a % 4) * (Samples / 4) + a / 4;
				const float A = pi / 2 + 2 * pi * Index / Samples;
				Shot.m_RocketAim = a < 0 ? pPrevious->m_RocketAim : vec2(std::cos(A), std::sin(A)) * 1000;
				Decision.m_RocketCandidates++;
				const auto Flight = Simulate(World, LocalId, Current, Shot, Shot.m_Delay + Shot.m_SwitchTicks + Config.m_MaxFlight, Config);
				if(Flight.m_ShotTick != Shot.m_Delay + Shot.m_SwitchTicks + 1)
				{
					Decision.m_RejectShot++;
					continue;
				}
				if(Flight.m_Danger)
				{
					Decision.m_RejectDanger++;
					continue;
				}
				if(!Flight.m_BlastTick)
				{
					Decision.m_RejectFlight++;
					continue;
				}
				if(Exhausted())
					return false;
				const int RescueTicks = std::min(RocketTicks, std::max(Decision.m_Base.m_Danger + 4, Flight.m_BlastTick + 6));
				SProbe Probe = Simulate(World, LocalId, Current, Shot, RescueTicks, Config);
				if(Probe.m_Danger)
				{
					Decision.m_RejectDanger++;
					continue;
				}
				if(!TurnsSafe(Shot, Probe))
				{
					Decision.m_RejectTurn++;
					continue;
				}
				if(Combined)
					for(int Pulse = 1; Pulse < Shot.m_HookTicks && !Exhausted(); Pulse++)
					{
						auto Short = Shot;
						Short.m_HookTicks = Pulse;
						Short.m_MoveTicks = Shot.m_SwitchTicks + 1;
						const auto ShortProbe = Simulate(World, LocalId, Current, Short, RescueTicks, Config);
						if(!ShortProbe.m_Danger && TurnsSafe(Short, ShortProbe))
						{
							Shot = Short;
							Probe = ShortProbe;
							break;
						}
					}
				Decision.m_Plan = Shot;
				Decision.m_Result = Probe;
				Decision.m_Mode = Shot.m_Delay ? EMode::WAIT : Combined ? EMode::ROCKET_HOOK :
											  EMode::ROCKET;
				Decision.m_CanWait = Shot.m_Delay > 0;

				return true;
			}
		}
		return false;
	};
	// Reserve two native probes for the selected plan's deadline. Search
	// exhaustion is not evidence that the physical input must be overridden.
	auto TryWait = [&](SPlan Plan) {
		if(m_Calls + 2 > std::max(1, Config.m_MaxCalls))
			return false;
		const SPlan Now = Plan;
		Plan.m_ReleaseDuringDelay = OwnHook;
		const bool Reaim = Plan.m_Input.m_TargetX != Current.m_TargetX || Plan.m_Input.m_TargetY != Current.m_TargetY;
		SProbe Probe;
		for(int Delay : {1, Plan.m_Rocket ? std::clamp(Config.m_RocketLead, 1, 4) + 1 : 2 + (Plan.m_HookTicks > 0 && (Plan.m_HookDelay > 0 || Current.m_Hook))})
		{
			Plan.m_Delay = Delay;
			Plan.m_HookDelay = Now.m_HookDelay;
			if(Plan.m_HookTicks > 0 && !Plan.m_Rocket)
			{
				if(OwnHook && Delay == 1)
					Plan.m_HookDelay = 0; // The waiting tick really released the hook.
				else if(Current.m_Hook && (Reaim || OwnHook))
					Plan.m_HookDelay = std::max(1, Plan.m_HookDelay);
			}
			Probe = Simulate(World, LocalId, Current, Plan, RescueTicks + Delay, Config);
			if(Probe.m_Danger || (Plan.m_Rocket && (Probe.m_ShotTick != Delay + Plan.m_SwitchTicks + 1 || !Probe.m_BlastTick)))
				return false;
		}
		Decision.m_Plan = Plan;
		Decision.m_Plan.m_Delay = 0;
		Decision.m_Result = Probe;
		Decision.m_Mode = EMode::WAIT;
		Decision.m_CanWait = true;
		Decision.m_ManualHookDeferred = Current.m_Hook != 0;
		return true;
	};
	if(CanRocket &&
		(RocketSearch(Moves, false, 192) || (Decision.m_Base.m_Danger <= CloseWindow && RocketSearch(Hooks, true, 144))))
	{
		if(Decision.m_Mode != EMode::WAIT)
			TryWait(Decision.m_Plan);
		return Finish();
	}
	if(pPrevious && !pPrevious->m_Rocket)
	{
		const auto Previous = RefreshPrevious();
		if(Config.m_Hook && Previous.m_CompleteHookLaunch && Previous.m_HookTicks > 0 && Found && !Decision.m_Result.m_Danger)
		{
			Decision.m_Mode = EMode::HOOK;
			ShortenHook();
			return Finish();
		}
		if(TryWait(Previous))
			return Finish();
	}
	// Keep most of the remaining budget for an executable rescue now.
	// Failure to prove waiting must not consume the whole fallback search.
	const int WaitCalls = m_Calls;
	for(size_t h = 0; h < Hooks.size() && !Exhausted() && m_Calls - WaitCalls < 8; h++)
	{
		if(Config.m_BudgetMs > 0 && (time_get_impl() - Start) * 1000.0 / time_freq() >= Config.m_BudgetMs * 0.45)
			break;
		if(TryWait(Hooks[h]))
			return Finish();
	}

	for(auto Move : Moves)
	{
		if(Exhausted())
			break;
		if(Consider(Move))
		{
			if(TryWait(Move))
				return Finish();
			for(int Pulse = 1; Pulse < Ticks && !Exhausted(); Pulse++)
			{
				Move.m_MoveTicks = Pulse;
				if(Consider(Move))
					break;
			}
			break;
		}
	}

	// Cover every hook ray before spending the remaining budget on pulses.
	// Nearby floor targets must not crowd ceiling rescues out of the search.
	if(Decision.m_Result.m_Danger)
		for(const auto &Candidate : Hooks)
		{
			if(Exhausted())
				break;
			auto Hook = Candidate;
			if(!Consider(Hook))
				continue;
			if(TryWait(Hook))
				return Finish();
			for(int Pulse = 1; Pulse < Ticks && !Exhausted(); Pulse++)
			{
				Hook.m_HookTicks = Pulse;
				Hook.m_MoveTicks = Hook.m_HookDelay + Pulse + 1;
				if(Consider(Hook))
					break;
			}
			break;
		}
	if(Decision.m_Result.m_Danger)
		for(size_t h = 0; h < Hooks.size() && !Exhausted(); h++)
			for(int Pulse = 1; Pulse <= (OwnHook && h == 0 ? Ticks : 8) && !Exhausted(); Pulse++)
			{
				auto Hook = Hooks[h];
				Hook.m_HookTicks = Pulse;
				Hook.m_MoveTicks = Hook.m_HookDelay + Pulse + 1;
				if(Consider(Hook))
					break;
			}

	// A plan that merely postpones freeze is not a verified rescue. Holding
	// it for the entire horizon blocks later input and the next ready rocket.
	Decision.m_Mode = !Found || Decision.m_Result.m_Danger ?
				  (Found && Decision.m_Base.m_Danger <= 4 && Decision.m_Result.m_Survival >= Decision.m_Base.m_Survival + 2 ? EMode::BEST_EFFORT : EMode::NO_SOLUTION) :
			  Decision.m_Plan.m_HookTicks > 0 ? EMode::HOOK :
							    EMode::MOVE;
	if(Decision.m_Mode == EMode::HOOK)
		ShortenHook();

	if(Decision.m_Mode == EMode::MOVE || Decision.m_Mode == EMode::HOOK)
		TryWait(Decision.m_Plan);
	return Finish();
}

CAvoidPlanner::SStep CAvoidPlanner::Step(CGameWorld &World, int LocalId, const CNetObj_PlayerInput &Current, const SConfig &Config, SFeedback &Feedback)
{
	SStep Result;
	const bool Owned = Feedback.m_OwnHook;
	Result.m_Decision = Decide(World, LocalId, Current, Config, Feedback.m_HasPrevious ? &Feedback.m_Previous : nullptr, Owned);
	const auto Mode = Result.m_Decision.m_Mode;
	const bool Apply = Mode == EMode::MOVE || Mode == EMode::HOOK || Mode == EMode::ROCKET || Mode == EMode::ROCKET_HOOK || Mode == EMode::BEST_EFFORT;
	// Execute one tick, then reconsider the latest physical input and world.
	// A forecast's duration is never a commitment to block later movement.
	Result.m_Input = InputAt(Apply ? Result.m_Decision.m_Plan : ReturnControlPlan(Current, Owned), Current, 0);
	const bool ForcedHook = Apply && Result.m_Decision.m_Plan.m_HookTicks > 0;
	Feedback.m_OwnHook = Result.m_Input.m_Hook && (Owned || (ForcedHook && (Result.m_Input.m_Hook != Current.m_Hook ||
										       Result.m_Input.m_TargetX != Current.m_TargetX || Result.m_Input.m_TargetY != Current.m_TargetY)));
	Feedback.m_Previous = Apply ? RemainingPlan(Result.m_Decision.m_Plan, Current, 1) : Result.m_Decision.m_Plan;
	Feedback.m_Previous.m_CompleteHookLaunch = Apply && Result.m_Decision.m_Plan.m_HookTicks > 0 && !Result.m_Input.m_Hook;
	Feedback.m_HasPrevious = Apply || Mode == EMode::WAIT;
	return Result;
}

void CAvoidPlanner::BeginWeaponReturn(SWeaponReturn &State, int ActiveWeapon, int UserWantedWeapon, unsigned SelectionSerial)
{
	if(State.m_PreviousWeapon >= 0 || UserWantedWeapon == WEAPON_GRENADE + 1 || (!UserWantedWeapon && ActiveWeapon == WEAPON_GRENADE))
		return;
	State.m_PreviousWeapon = UserWantedWeapon > 0 ? UserWantedWeapon - 1 : ActiveWeapon;
	State.m_SelectionSerial = SelectionSerial;
}

int CAvoidPlanner::WeaponReturnInput(SWeaponReturn &State, int ActiveWeapon, unsigned SelectionSerial, bool AttackConfirmed)
{
	if(State.m_PreviousWeapon < 0)
		return 0;
	if(SelectionSerial != State.m_SelectionSerial)
	{
		State = {};
		return 0;
	}
	State.m_Confirmed |= AttackConfirmed;
	if(!State.m_Confirmed)
		return 0;
	if(State.m_Requested && ActiveWeapon == State.m_PreviousWeapon)
	{
		State = {};
		return 0;
	}
	State.m_Requested = true;
	return State.m_PreviousWeapon + 1;
}
