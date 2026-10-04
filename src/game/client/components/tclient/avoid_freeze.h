#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_AVOID_FREEZE_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_AVOID_FREEZE_H

#include <engine/client/enums.h>
#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/prediction/avoid_planner.h>

class CAvoidFreeze : public CComponent
{
	struct SState
	{
		int m_Tick = -1;
		int m_LocalId = -1;
		int m_FireTick = -10000;
		int m_ReloadUntil = -10000;
		int m_LastAttackTick = -1;
		int m_AttackBeforeShot = -1;
		int m_PerfLogTick = -10000;
		bool m_ShotPending = false;
		bool m_Frozen = false;
		bool m_ServerFrozen = false;
		CNetObj_PlayerInput m_Output{};
		CAvoidPlanner::SFeedback m_Feedback;
		CAvoidPlanner::SWeaponReturn m_WeaponReturn;
		int m_WeaponSwitchTick = -1;
		int m_ServerAttackBeforeShot = -1;
		unsigned m_OutputSelectionSerial = 0;
		CAvoidPlanner::SDecision m_Decision;
		CAvoidPlanner::EMode m_LogMode = CAvoidPlanner::EMode::NO_SOLUTION;
	};
	SState m_aState[NUM_DUMMIES];
	static void ConToggleMenu(IConsole::IResult *pResult, void *pUserData);
	static void ConToggleAvoid(IConsole::IResult *pResult, void *pUserData);
	static void ConStatus(IConsole::IResult *pResult, void *pUserData);
	static CAvoidPlanner::SConfig Config();

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnConsoleInit() override;
	void OnReset() override;
	void OnPlayerDeath();
	void ApplyOverride();
};

#endif
