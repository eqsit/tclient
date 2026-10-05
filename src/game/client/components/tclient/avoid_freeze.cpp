#include "avoid_freeze.h"

#include <base/log.h>
#include <base/time.h>

#include <engine/client.h>
#include <engine/console.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>

using EMode = CAvoidPlanner::EMode;

CAvoidPlanner::SConfig CAvoidFreeze::Config() const
{
	CAvoidPlanner::SConfig C;
	C.m_Ticks = g_Config.m_KxBafTicks;
	C.m_RocketTicks = g_Config.m_TcRocketAvoidTicks;
	C.m_RocketLead = g_Config.m_TcRocketAvoidLead;
	C.m_MaxFlight = g_Config.m_TcRocketAvoidFlight;
	C.m_Angles = g_Config.m_KxBafAngles;
	C.m_Fov = g_Config.m_KxBafFov;
	C.m_Direction = g_Config.m_KxBafDirection;
	C.m_Jump = g_Config.m_KxBafJump;
	C.m_Hook = g_Config.m_KxBafHook;
	C.m_Aim = g_Config.m_KxBafAim;
	C.m_Rocket = g_Config.m_TcRocketAvoid;
	C.m_Boost = g_Config.m_TcRocketBoost;
	C.m_AutoRehook = g_Config.m_TcAvoidAutoRehook;
	C.m_BoostVertical = m_aBoostDown[g_Config.m_ClDummy] - m_aBoostUp[g_Config.m_ClDummy];
	C.m_Freeze = g_Config.m_KxBafAvoidFreeze;
	C.m_Death = g_Config.m_KxBafAvoidDeath;
	C.m_Teleport = g_Config.m_KxBafAvoidTeleport;
	return C;
}

void CAvoidFreeze::ConToggleMenu(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CAvoidFreeze *>(pUserData);
	pSelf->GameClient()->m_Menus.ToggleAvoidMenu();
}

void CAvoidFreeze::ConToggleAvoid(IConsole::IResult *pResult, void *pUserData)
{
	g_Config.m_KxBasicAvoidFreeze ^= 1;
	log_info("avoid", "[CONFIG] enabled=%d", g_Config.m_KxBasicAvoidFreeze);
}

void CAvoidFreeze::ConStatus(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CAvoidFreeze *>(pUserData);
	const auto &S = pSelf->m_aState[g_Config.m_ClDummy];
	log_info("avoid", "[STATUS] version=kog-kinetix-12-hook-priority-local enabled=%d rocket=%d hook=%d horizon=%d rocket_horizon=%d lead=%d mode=%s tick=%d sims=%d sim_ticks=%d debug=%d boost=%d auto_rehook=%d",
		g_Config.m_KxBasicAvoidFreeze, g_Config.m_TcRocketAvoid, g_Config.m_KxBafHook, g_Config.m_KxBafTicks,
		g_Config.m_TcRocketAvoidTicks, g_Config.m_TcRocketAvoidLead, CAvoidPlanner::ModeName(S.m_Decision.m_Mode), S.m_Tick,
		S.m_Decision.m_Calls, S.m_Decision.m_SimTicks, g_Config.m_KxBafDebug, g_Config.m_TcRocketBoost, g_Config.m_TcAvoidAutoRehook);
}

void CAvoidFreeze::ConBoostUp(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CAvoidFreeze *>(pUserData)->m_aBoostUp[g_Config.m_ClDummy] = pResult->GetInteger(0) != 0;
}

void CAvoidFreeze::ConBoostDown(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CAvoidFreeze *>(pUserData)->m_aBoostDown[g_Config.m_ClDummy] = pResult->GetInteger(0) != 0;
}

void CAvoidFreeze::OnConsoleInit()
{
	Console()->Register("tc_toggle_menu", "", CFGFLAG_CLIENT, ConToggleMenu, this, "Open/close avoid menu (F12)");
	Console()->Register("tc_avoid_toggle", "", CFGFLAG_CLIENT, ConToggleAvoid, this, "Toggle Kinetix avoid and rocket rescue");
	Console()->Register("tc_avoid_status", "", CFGFLAG_CLIENT, ConStatus, this, "Print avoid configuration and last decision");
	Console()->Register("+tc_boost_up", "i", CFGFLAG_CLIENT, ConBoostUp, this, "Hold to prefer upward rocket saves");
	Console()->Register("+tc_boost_down", "i", CFGFLAG_CLIENT, ConBoostDown, this, "Hold to prefer downward rocket saves");
}

void CAvoidFreeze::OnReset()
{
	for(auto &State : m_aState)
		State = SState();
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
		m_aBoostUp[Dummy] = m_aBoostDown[Dummy] = 0;
}

void CAvoidFreeze::OnPlayerDeath()
{
	if(g_Config.m_KxBafDebug)
	{
		const auto &S = m_aState[g_Config.m_ClDummy];
		log_info("avoid", "[OUTCOME] death tick=%d mode=%s danger=%s danger_tick=%d last_shot=%d",
			S.m_Tick, CAvoidPlanner::ModeName(S.m_Decision.m_Mode), S.m_Decision.m_Base.m_Kind, S.m_Decision.m_Base.m_Danger, S.m_FireTick);
	}
	OnReset();
}

void CAvoidFreeze::ApplyOverride()
{
	auto *pGame = GameClient();
	const int Dummy = g_Config.m_ClDummy;
	auto &S = m_aState[Dummy];
	auto &Input = pGame->m_Controls.m_aInputData[Dummy];
	if(!g_Config.m_KxBasicAvoidFreeze || Client()->State() != IClient::STATE_ONLINE || pGame->m_Snap.m_SpecInfo.m_Active)
	{
		if(S.m_Feedback.m_OwnHook)
			Input.m_Hook = 0;
		if(S.m_WeaponReturn.m_PreviousWeapon >= 0 && S.m_WeaponReturn.m_SelectionSerial == pGame->m_Controls.m_aWeaponSelectionSerial[Dummy])
			Input.m_WantedWeapon = S.m_WeaponReturn.m_PreviousWeapon + 1;
		S = SState();
		return;
	}
	const int LocalId = pGame->m_Snap.m_LocalClientId;
	const int Tick = Client()->PredGameTick(Dummy);
	// SendInput runs before OnPredict. If rendering skipped game ticks, the
	// previous prediction can be several ticks behind the outgoing input.
	// Catch up with inputs already sent, so a rescue starts at Tick, not in
	// an outdated position. Repeated input within Tick uses its previous world.
	CGameWorld Aligned;
	CGameWorld *pWorld = &pGame->m_PredictedWorld;
	if(pWorld->GameTick() >= Tick && pGame->m_PrevRegularPredictedWorld.GameTick() < Tick)
		pWorld = &pGame->m_PrevRegularPredictedWorld;
	if(LocalId < 0 || !pWorld->GetCharacterById(LocalId))
		return;
	if(pWorld->GameTick() < Tick - 1)
	{
		Aligned.CopyWorldClean(pWorld, false);
		Aligned.m_LocalClientId = LocalId;
		Aligned.m_WorldConfig.m_PredictEvents = false;
		Aligned.m_WorldConfig.m_PredictTiles = true;
		Aligned.m_WorldConfig.m_PredictFreeze = 1;
		for(int t = Aligned.GameTick() + 1; t < Tick; t++)
		{
			for(int d = 0; d < NUM_DUMMIES; d++)
				if(auto *pChar = Aligned.GetCharacterById(pGame->m_aLocalIds[d]))
					if(auto *pSent = (CNetObj_PlayerInput *)Client()->GetInput(t, d ^ Dummy))
						pChar->OnDirectInput(pSent);
			Aligned.m_GameTick = t;
			for(int d = 0; d < NUM_DUMMIES; d++)
				if(auto *pChar = Aligned.GetCharacterById(pGame->m_aLocalIds[d]))
					if(auto *pSent = (CNetObj_PlayerInput *)Client()->GetInput(t, d ^ Dummy))
						pChar->OnPredictedInput(pSent);
			Aligned.Tick();
		}
		pWorld = &Aligned;
	}
	auto *pLocal = pWorld->GetCharacterById(LocalId);
	if(!pLocal)
		return;
	if(S.m_LocalId != LocalId || Tick < S.m_Tick)
	{
		S = SState();
		if(g_Config.m_KxBafDebug)
		{
			CServerInfo Info;
			Client()->GetServerInfo(&Info);
			const auto *pTune = pGame->m_PredictedWorld.GetTuning(pLocal->GetOverriddenTuneZone());
			log_info("avoid", "[SESSION] version=kog-kinetix-12-hook-priority-local dummy=%d local=%d server=%s map=%s type=%s zone=%d grenade_speed=%.3f curvature=%.3f explosion=%.3f hook_drag=%.3f",
				Dummy, LocalId, Info.m_aName, Info.m_aMap, Info.m_aGameType, pLocal->GetOverriddenTuneZone(), (float)pTune->m_GrenadeSpeed, (float)pTune->m_GrenadeCurvature, (float)pTune->m_ExplosionStrength, (float)pTune->m_HookDragAccel);
		}
	}
	S.m_LocalId = LocalId;
	if(pLocal->GetAttackTick() > S.m_LastAttackTick)
	{
		S.m_LastAttackTick = pLocal->GetAttackTick();
		const int FireDelay = pWorld->GetTuning(pLocal->GetOverriddenTuneZone())->GetWeaponFireDelay(pLocal->GetActiveWeapon()) * (int)SERVER_TICK_SPEED;
		S.m_ReloadUntil = std::max(S.m_ReloadUntil, pLocal->GetAttackTick() + FireDelay + 1);
	}
	const auto &ServerChar = pGame->m_aClients[LocalId];
	const bool ServerFrozen = ServerChar.m_FreezeEnd > Client()->GameTick(Dummy) || ServerChar.m_DeepFrozen || ServerChar.m_LiveFrozen;
	if(ServerFrozen != S.m_ServerFrozen && g_Config.m_KxBafDebug)
		log_info("avoid", "[SERVER-OUTCOME] tick=%d server_tick=%d frozen=%d freeze_end=%d mode=%s since_shot=%d", Tick, Client()->GameTick(Dummy), ServerFrozen, ServerChar.m_FreezeEnd, CAvoidPlanner::ModeName(S.m_Decision.m_Mode), Tick - S.m_FireTick);
	S.m_ServerFrozen = ServerFrozen;
	if(S.m_ShotPending)
	{
		if(pLocal->GetAttackTick() > S.m_AttackBeforeShot && pLocal->GetActiveWeapon() == WEAPON_GRENADE)
		{
			if(g_Config.m_KxBafDebug)
				log_info("avoid", "[SHOT-CONFIRMED] tick=%d requested=%d attack=%d weapon=%d", Tick, S.m_FireTick, pLocal->GetAttackTick(), pLocal->GetActiveWeapon());
			S.m_ShotPending = false;
		}
		else if(Tick - S.m_FireTick > 4)
		{
			if(g_Config.m_KxBafDebug)
				log_info("avoid", "[SHOT-MISSED] tick=%d requested=%d reload=%d reason=no-predicted-attack", Tick, S.m_FireTick, pLocal->GetReloadTimer());
			S.m_ShotPending = false;
			S.m_WeaponReturn.m_Confirmed = true;
			S.m_Feedback.m_HasPrevious = false;
		}
	}
	const auto &ServerCore = pGame->m_Snap.m_aCharacters[LocalId].m_Cur;
	const bool ServerShot = S.m_WeaponReturn.m_ShotSent && ServerCore.m_Weapon == WEAPON_GRENADE && ServerCore.m_AttackTick > S.m_ServerAttackBeforeShot;
	const bool ReturnTimedOut = S.m_WeaponReturn.m_ShotSent && Tick - S.m_FireTick > SERVER_TICK_SPEED;
	const bool WasReturning = S.m_WeaponReturn.m_Requested;
	const int PreviousWeapon = S.m_WeaponReturn.m_PreviousWeapon;
	const unsigned SelectionSerial = pGame->m_Controls.m_aWeaponSelectionSerial[Dummy];
	const bool ManualSelection = PreviousWeapon >= 0 && SelectionSerial != S.m_WeaponReturn.m_SelectionSerial;
	const int ReturnWeapon = CAvoidPlanner::WeaponReturnInput(S.m_WeaponReturn, pLocal->GetActiveWeapon(), pGame->m_Controls.m_aWeaponSelectionSerial[Dummy], ServerShot || ReturnTimedOut);
	if(PreviousWeapon >= 0 && S.m_WeaponReturn.m_PreviousWeapon < 0 && g_Config.m_KxBafDebug)
		log_info("avoid", "[WEAPON-RETURN-END] tick=%d previous=%d active=%d manual=%d", Tick, PreviousWeapon, pLocal->GetActiveWeapon(), ManualSelection);
	if(ReturnWeapon)
	{
		Input.m_WantedWeapon = ReturnWeapon;
		if(!WasReturning && g_Config.m_KxBafDebug)
			log_info("avoid", "[WEAPON-RETURN] tick=%d wanted=%d server_ack=%d timeout=%d", Tick, ReturnWeapon, ServerShot, ReturnTimedOut);
	}
	const bool Frozen = pLocal->m_FreezeTime > 0 || pLocal->Core()->m_DeepFrozen || pLocal->Core()->m_LiveFrozen;
	if(Frozen != S.m_Frozen && g_Config.m_KxBafDebug)
		log_info("avoid", "[OUTCOME] tick=%d dummy=%d frozen=%d mode=%s since_shot=%d pos=(%.2f,%.2f) vel=(%.2f,%.2f)",
			Tick, Dummy, Frozen, CAvoidPlanner::ModeName(S.m_Decision.m_Mode), Tick - S.m_FireTick, pLocal->m_Pos.x, pLocal->m_Pos.y, pLocal->Core()->m_Vel.x, pLocal->Core()->m_Vel.y);
	S.m_Frozen = Frozen;
	if(g_Config.m_TcAvoidAutoRehook || !Input.m_Hook)
		S.m_Feedback.m_ManualHookSuppressed = false;
	if(Frozen)
	{
		if(!g_Config.m_TcAvoidAutoRehook && Input.m_Hook && S.m_Feedback.m_OwnHook)
			S.m_Feedback.m_ManualHookSuppressed = true;
		if(S.m_Feedback.m_OwnHook || S.m_Feedback.m_ManualHookSuppressed)
			Input.m_Hook = 0;
		const bool Suppressed = S.m_Feedback.m_ManualHookSuppressed;
		S.m_Feedback = {};
		S.m_Feedback.m_ManualHookSuppressed = Suppressed;
		return;
	}
	const CNetObj_PlayerInput Raw = Input;
	// Input can be snapped several times per game tick. Do not repeatedly
	// launch the same rocket or spend another brute-force search that tick.
	if(Tick == S.m_Tick)
	{
		const int Flags = Input.m_PlayerFlags;
		// A weapon key/wheel event between snaps of this tick must survive
		// cached avoid output, including selecting the same grenade again.
		if(SelectionSerial != S.m_OutputSelectionSerial)
		{
			S.m_Output.m_WantedWeapon = Raw.m_WantedWeapon;
			S.m_Output.m_NextWeapon = Raw.m_NextWeapon;
			S.m_Output.m_PrevWeapon = Raw.m_PrevWeapon;
			S.m_OutputSelectionSerial = SelectionSerial;
		}
		Input = S.m_Output;
		Input.m_PlayerFlags = Flags;
		if(!g_Config.m_TcAvoidAutoRehook)
		{
			if(Raw.m_Hook && !Input.m_Hook)
				S.m_Feedback.m_ManualHookSuppressed = true;
			if(!S.m_Feedback.m_OwnHook && (!Raw.m_Hook || S.m_Feedback.m_ManualHookSuppressed))
				Input.m_Hook = 0;
		}
		return;
	}
	const int64_t Start = time_get_impl();
	const auto Cfg = Config();
	const bool ReloadReady = Tick >= S.m_ReloadUntil - (pLocal->GetActiveWeapon() == WEAPON_GRENADE ? 0 : 1);
	auto RecordShot = [&]() {
		S.m_AttackBeforeShot = pLocal->GetAttackTick();
		S.m_ServerAttackBeforeShot = ServerCore.m_AttackTick;
		if(S.m_WeaponReturn.m_PreviousWeapon >= 0)
		{
			S.m_WeaponReturn.m_ShotSent = true;
			S.m_WeaponReturn.m_Confirmed = S.m_WeaponReturn.m_Requested = false;
		}
		S.m_ShotPending = true;
		S.m_FireTick = Tick;
		if(Cfg.m_Boost)
		{
			S.m_BoostAimPending = true;
			S.m_BoostShot = Input;
		}
		const int FireDelay = pWorld->GetTuning(pLocal->GetOverriddenTuneZone())->GetWeaponFireDelay(WEAPON_GRENADE) * (int)SERVER_TICK_SPEED;
		// A late snapshot can temporarily restore the pre-shot attack tick
		// and a zero native reload. Keep the real tuned cooldown independently.
		S.m_ReloadUntil = Tick + std::max(1, FireDelay);
	};
	CAvoidPlanner Planner;
	auto SearchConfig = Cfg;
	SearchConfig.m_Rocket = SearchConfig.m_Rocket && !S.m_ShotPending && ReloadReady;
	const auto Step = Planner.Step(*pWorld, LocalId, Raw, SearchConfig, S.m_Feedback);
	const auto &Decision = Step.m_Decision;
	Input = Step.m_Input;
	Input.m_PlayerFlags = Raw.m_PlayerFlags;
	if(Decision.m_Mode == EMode::ROCKET || Decision.m_Mode == EMode::ROCKET_HOOK)
	{
		int Wanted = Raw.m_WantedWeapon;
		if(Wanted != WEAPON_GRENADE + 1 && (Wanted < 1 || Wanted > NUM_WEAPONS || !pLocal->GetWeaponGot(Wanted - 1)))
			Wanted = 0;
		if(S.m_WeaponReturn.m_PreviousWeapon < 0)
			S.m_WeaponSwitchTick = Tick;
		CAvoidPlanner::BeginWeaponReturn(S.m_WeaponReturn, pLocal->GetActiveWeapon(), Wanted, pGame->m_Controls.m_aWeaponSelectionSerial[Dummy]);
	}
	else if(S.m_WeaponReturn.m_PreviousWeapon >= 0 && !S.m_WeaponReturn.m_ShotSent && Tick > S.m_WeaponSwitchTick && !Decision.m_Plan.m_Rocket)
	{
		Input.m_WantedWeapon = CAvoidPlanner::WeaponReturnInput(S.m_WeaponReturn, pLocal->GetActiveWeapon(), pGame->m_Controls.m_aWeaponSelectionSerial[Dummy], true);
	}
	if((Decision.m_Mode == EMode::ROCKET || Decision.m_Mode == EMode::ROCKET_HOOK) && !Decision.m_Plan.m_SwitchTicks)
	{
		RecordShot();
		// Keep the real fire counter in sync with the one automatic press.
		pGame->m_Controls.m_aInputFire[Dummy] = (Input.m_Fire + (pGame->m_Controls.m_aInputFireHeld[Dummy] ? 0 : 1)) & INPUT_STATE_MASK;
	}

	if(S.m_BoostAimPending)
	{
		if(!Cfg.m_Boost || ServerCore.m_AttackTick > S.m_ServerAttackBeforeShot ||
			Tick - S.m_FireTick > SERVER_TICK_SPEED || SelectionSerial != S.m_OutputSelectionSerial)
			S.m_BoostAimPending = false;
		else if(Tick > S.m_FireTick)
			CAvoidPlanner::PreserveBoostReleaseAim(Input, S.m_BoostShot);
	}
	const double Ms = (time_get_impl() - Start) * 1000.0 / time_freq();
	if(g_Config.m_KxBafDebug && (Decision.m_Mode != S.m_LogMode || g_Config.m_KxBafDebug >= 2 || Decision.m_Mode == EMode::ROCKET || Decision.m_Mode == EMode::ROCKET_HOOK))
	{
		log_info("avoid", "[DECISION] tick=%d dummy=%d mode=%s danger=%s in=%d tile=%d front=%d wait=%d result=%d shot=%d pulse=%d pos=(%.2f,%.2f) vel=(%.2f,%.2f) weapon=%d grenade=%d ammo=%d raw=(%d,%d,%d,%d,%d,%d) out=(%d,%d,%d,%d,%d,%d) sims=%d sim_ticks=%d ms=%.3f world=%d previous_world=%d pending=%d reload=%d cooldown=%d limited=%d hold=%d plan_ticks=%d hook_state=%d hook_pos=(%.2f,%.2f) manual_defer=%d own_hook=%d feedback=1 raw_weapon=%d out_weapon=%d selection=%u",
			Tick, Dummy, CAvoidPlanner::ModeName(Decision.m_Mode), Decision.m_Base.m_Kind, Decision.m_Base.m_Danger, Decision.m_Base.m_Tile, Decision.m_Base.m_Front,
			Decision.m_CanWait, Decision.m_Result.m_Danger, Decision.m_Result.m_ShotTick, Decision.m_Plan.m_HookTicks,
			pLocal->m_Pos.x, pLocal->m_Pos.y, pLocal->Core()->m_Vel.x, pLocal->Core()->m_Vel.y,
			pLocal->GetActiveWeapon(), pLocal->GetWeaponGot(WEAPON_GRENADE), pLocal->GetWeaponAmmo(WEAPON_GRENADE),
			Raw.m_Direction, Raw.m_Jump, Raw.m_Hook, Raw.m_Fire, Raw.m_TargetX, Raw.m_TargetY,
			Input.m_Direction, Input.m_Jump, Input.m_Hook, Input.m_Fire, Input.m_TargetX, Input.m_TargetY,
			Decision.m_Calls, Decision.m_SimTicks, Ms, pWorld->GameTick(), pGame->m_PredictedWorld.GameTick(), S.m_ShotPending, pLocal->GetReloadTimer(), std::max(0, S.m_ReloadUntil - Tick), Decision.m_Limited, 0, CAvoidPlanner::Duration(Decision.m_Plan), pLocal->Core()->m_HookState, pLocal->Core()->m_HookPos.x, pLocal->Core()->m_HookPos.y, Decision.m_ManualHookDeferred, S.m_Feedback.m_OwnHook, Raw.m_WantedWeapon, Input.m_WantedWeapon, SelectionSerial);
		if(Decision.m_RocketCandidates || g_Config.m_KxBafDebug >= 2)
			log_info("avoid", "[ROCKET] tick=%d candidates=%d reject[shot=%d flight=%d danger=%d turn=%d] selected=%d sent=%d delay=%d shot_tick=%d blast_tick=%d blast=(%.2f,%.2f) reload=%d",
				Tick, Decision.m_RocketCandidates, Decision.m_RejectShot, Decision.m_RejectFlight, Decision.m_RejectDanger, Decision.m_RejectTurn,
				Decision.m_Plan.m_Rocket, (Decision.m_Mode == EMode::ROCKET || Decision.m_Mode == EMode::ROCKET_HOOK) && !Decision.m_Plan.m_SwitchTicks, Decision.m_Plan.m_Delay, Decision.m_Result.m_ShotTick, Decision.m_Result.m_BlastTick, Decision.m_Result.m_BlastPos.x, Decision.m_Result.m_BlastPos.y, pLocal->GetReloadTimer());
		if(Cfg.m_Boost && Decision.m_Plan.m_Rocket && !Decision.m_Plan.m_Delay)
			log_info("avoid", "[BOOST] tick=%d intent=(%.3f,%.3f) gain=%.3f long_safe=%d", Tick,
				Decision.m_BoostDirection.x, Decision.m_BoostDirection.y, Decision.m_BoostScore, Decision.m_BoostLongSafe);
		if(Decision.m_HookCandidates)
			log_info("avoid", "[HOOK-MOMENTUM] tick=%d candidates=%d speed_gain=%.3f", Tick, Decision.m_HookCandidates, Decision.m_HookSpeedGain);
	}
	if(g_Config.m_KxBafDebug && Ms > 5 && Tick - S.m_PerfLogTick >= SERVER_TICK_SPEED)
	{
		log_info("avoid", "[PERF] tick=%d ms=%.3f sims=%d sim_ticks=%d", Tick, Ms, Decision.m_Calls, Decision.m_SimTicks);
		S.m_PerfLogTick = Tick;
	}
	S.m_LogMode = Decision.m_Mode;
	S.m_Tick = Tick;
	S.m_Output = Input;
	S.m_OutputSelectionSerial = SelectionSerial;
	S.m_Decision = Decision;
	if(!g_Config.m_KxBafSilent && (Input.m_TargetX != Raw.m_TargetX || Input.m_TargetY != Raw.m_TargetY))
		pGame->m_Controls.m_aMousePos[Dummy] = vec2(Input.m_TargetX, Input.m_TargetY);
}
