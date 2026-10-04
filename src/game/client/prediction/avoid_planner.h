#ifndef GAME_CLIENT_PREDICTION_AVOID_PLANNER_H
#define GAME_CLIENT_PREDICTION_AVOID_PLANNER_H

#include <base/vmath.h>

#include <generated/protocol.h>

#include <vector>

class CGameWorld;

// Kinetix Basic Avoid Freeze's latest-safe-tick search. Rocket candidates use
// the real predicted character, weapon, projectile and explosion code.
class CAvoidPlanner
{
public:
	struct SConfig
	{
		int m_Ticks = 12;
		int m_RocketTicks = 30;
		int m_RocketLead = 2;
		int m_MaxFlight = 4;
		int m_Angles = 24;
		int m_RocketAngles = 48;
		int m_Fov = 360;
		int m_MaxCalls = 512;
		double m_BudgetMs = 3.0;
		bool m_Direction = true;
		bool m_Jump = true;
		bool m_Hook = true;
		bool m_Aim = true;
		bool m_Rocket = true;
		bool m_Freeze = true;
		bool m_Death = true;
		bool m_Teleport = false;
	};
	enum class EMode
	{
		SAFE,
		WAIT,
		MOVE,
		HOOK,
		ROCKET,
		ROCKET_HOOK,
		BEST_EFFORT,
		NO_SOLUTION
	};
	struct SPlan
	{
		CNetObj_PlayerInput m_Input{};
		vec2 m_RocketAim{0, 1};
		bool m_Rocket = false;
		int m_SwitchTicks = 0;
		int m_MoveTicks = -1;
		// -1 preserves the user's hook. Otherwise a bounded automatic pulse.
		int m_HookTicks = -1;
		int m_HookDelay = 0;
		int m_Delay = 0;
		bool m_ReleaseDuringDelay = false;
		bool m_FixedWaitInput = false;
		bool m_CompleteHookLaunch = false;
		CNetObj_PlayerInput m_WaitInput{};
		float m_SearchCost = 0;
	};
	struct SProbe
	{
		int m_Danger = 0;
		const char *m_Kind = "none";
		vec2 m_Pos{0, 0};
		int m_Tile = 0;
		int m_Front = 0;
		int m_ShotTick = 0;
		int m_BlastTick = 0;
		vec2 m_BlastPos{0, 0};
		int m_Survival = 0;
	};
	struct SDecision
	{
		EMode m_Mode = EMode::SAFE;
		SPlan m_Plan;
		SProbe m_Base;
		SProbe m_Result;
		int m_Calls = 0;
		int m_SimTicks = 0;
		bool m_CanWait = false;
		bool m_Limited = false;
		bool m_ManualHookDeferred = false;
		int m_RocketCandidates = 0;
		int m_RejectShot = 0;
		int m_RejectFlight = 0;
		int m_RejectDanger = 0;
		int m_RejectTurn = 0;
	};
	struct SWeaponReturn
	{
		int m_PreviousWeapon = -1;
		unsigned m_SelectionSerial = 0;
		bool m_ShotSent = false;
		bool m_Confirmed = false;
		bool m_Requested = false;
	};
	static void BeginWeaponReturn(SWeaponReturn &State, int ActiveWeapon, int UserWantedWeapon, unsigned SelectionSerial);
	static int WeaponReturnInput(SWeaponReturn &State, int ActiveWeapon, unsigned SelectionSerial, bool AttackConfirmed);
	struct SFeedback
	{
		bool m_OwnHook = false;
		bool m_HasPrevious = false;
		SPlan m_Previous;
	};
	struct SStep
	{
		SDecision m_Decision;
		CNetObj_PlayerInput m_Input{};
	};
	SStep Step(CGameWorld &World, int LocalId, const CNetObj_PlayerInput &Current, const SConfig &Config, SFeedback &Feedback);
	static const char *ModeName(EMode Mode);
	static CNetObj_PlayerInput InputAt(const SPlan &Plan, const CNetObj_PlayerInput &Current, int Tick);
	static SPlan ReturnControlPlan(const CNetObj_PlayerInput &Current, bool OwnHook);
	static SPlan RemainingPlan(const SPlan &Plan, const CNetObj_PlayerInput &Current, int Elapsed);
	SProbe Simulate(CGameWorld &World, int LocalId, const CNetObj_PlayerInput &Current, const SPlan &Plan, int Ticks, const SConfig &Config);
	SDecision Decide(CGameWorld &World, int LocalId, const CNetObj_PlayerInput &Current, const SConfig &Config, const SPlan *pPrevious = nullptr, bool OwnHook = false);
	static int Duration(const SPlan &Plan);

private:
	int m_Calls = 0;
	int m_SimTicks = 0;
};

#endif
