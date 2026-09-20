// Ported from Kinetix "Basic Avoid Freeze".
// Algorithm: "Latest Safe Tick Search" by Claude + Survival Score
//
// Each frame:
//   Test A: "Can I wait 1 more tick?" — simulate 1 tick with current input,
//           then brute-force all combos on remaining ticks. If ANY combo
//           eliminates danger (danger=0) → wait, don't intervene.
//   Test B: "If I act NOW — will it work?" — brute-force all combos from
//           tick 0. Score by survival (how long before danger). Higher = better.
//           Among same survival, prefer minimal input diff.
//
//   If Test A passes → wait (don't intervene).
//   If Test A fails but Test B passes → LAST MOMENT, apply best combo.
//   If both fail → too late, danger inevitable.
//
// Combos include: direction(-1,0,1) × jump(0,1) × hook(0,1) × aim angles.
// Aim angles start at the current aim, then spread outward in nearest-angle-first order over the FOV.

#include <game/client/components/tclient/avoid_freeze.h>

#include <base/log.h>
#include <base/math.h>
#include <base/system.h>
#include <base/vmath.h>

#include <engine/shared/config.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/mapitems.h>
#include <generated/protocol.h>

void CAvoidFreeze::OnReset()
{
	m_WasOverriding = false;
	m_OverrideHook = 0;
	m_BlockHeldHookUntilRelease = false;
	m_LogAction = -1;
}

void CAvoidFreeze::OnUpdate()
{
}

int CAvoidFreeze::InputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B) const
{
	int diff = 0;
	if(A.m_Direction != B.m_Direction)
		diff++;
	if((A.m_Jump != 0) != (B.m_Jump != 0))
		diff++;
	if((A.m_Hook != 0) != (B.m_Hook != 0))
		diff++;
	if(A.m_TargetX != B.m_TargetX || A.m_TargetY != B.m_TargetY)
		diff++;
	return diff;
}

int CAvoidFreeze::SimulateDangerTick(int LocalId, const CNetObj_PlayerInput &Input, int Ticks) const
{
	return SimulateDangerTickDelayed(LocalId, Input, 0, Input, Ticks);
}

int CAvoidFreeze::SimulateDangerTickDelayed(int LocalId, const CNetObj_PlayerInput &DelayInput,
	int Delay, const CNetObj_PlayerInput &ComboInput, int Ticks) const
{
	CGameClient *pGame = GameClient();
	CGameWorld SimWorld;
	// A released hook cannot interact with players, so the cheap local-only copy is exact for that
	// branch. An active or candidate hook must see the other tees: dropping them made an existing
	// player-hook retract in the simulation and made auto-hook aim at places that were not the real
	// world. Only pay for the full character copy in hook branches.
	const bool SimulatesHook = DelayInput.m_Hook != 0 || ComboInput.m_Hook != 0;
	SimWorld.CopyWorld(&pGame->m_PredictedWorld, SimulatesHook ? -1 : LocalId);

	CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
	if(!pChar)
		return 0;

	const bool AvoidFreeze = g_Config.m_KxBafAvoidFreeze != 0;
	const bool AvoidTeleport = g_Config.m_KxBafAvoidTeleport != 0;
	const bool AvoidDeath = g_Config.m_KxBafAvoidDeath != 0;
	CCollision *pCol = pChar->Collision();

	const int StartTick = SimWorld.GameTick();
	for(int t = 0; t < Ticks; t++)
	{
		const CNetObj_PlayerInput &In = (t < Delay) ? DelayInput : ComboInput;
		pChar->OnDirectInput(&In);
		SimWorld.m_GameTick = StartTick + t + 1;
		pChar->OnPredictedInput(&In);
		SimWorld.Tick();

		if(!SimWorld.GetCharacterById(LocalId))
			return AvoidDeath ? (t + 1) : 0;

		if(AvoidFreeze && pChar->m_FreezeTime > 0)
			return t + 1;

		if(AvoidDeath)
		{
			const int Idx = pCol->GetPureMapIndex(pChar->m_Pos);
			if(Idx >= 0)
			{
				const int Tile = pCol->GetTileIndex(Idx);
				const int FTile = pCol->GetFrontTileIndex(Idx);
				if(Tile == TILE_DEATH || FTile == TILE_DEATH)
					return t + 1;
			}
		}

		if(AvoidTeleport)
		{
			const int Idx = pCol->GetPureMapIndex(pChar->m_Pos);
			if(Idx >= 0)
			{
				if(pCol->IsTeleport(Idx) || pCol->IsEvilTeleport(Idx))
					return t + 1;
			}
		}
	}
	return 0;
}

void CAvoidFreeze::ApplyOverride()
{
	m_SavedThisTick = false;
	m_NoSolutionThisTick = false;
	m_DangerTickThisTick = 0;
	if(!g_Config.m_KxBasicAvoidFreeze)
	{
		m_BlockHeldHookUntilRelease = false;
		return;
	}

	CGameClient *pGame = GameClient();
	const int LocalId = pGame->m_Snap.m_LocalClientId;
	if(LocalId < 0 || !pGame->m_Snap.m_aCharacters[LocalId].m_Active)
		return;

	CCharacter *pLocalChar = pGame->m_PredictedWorld.GetCharacterById(LocalId);
	if(!pLocalChar)
		return;

	// While frozen the server ignores the whole input, so don't simulate an override.
	if(pGame->m_aClients[LocalId].m_Predicted.m_FreezeEnd != 0)
		return;

	CNetObj_PlayerInput *pInput = &pGame->m_Controls.m_aInputData[g_Config.m_ClDummy];
	const bool HookKeyHeld = pGame->m_Controls.m_aInputHook[g_Config.m_ClDummy] != 0;
	if(!HookKeyHeld || g_Config.m_KxBafRehook)
		m_BlockHeldHookUntilRelease = false;
	else if(m_BlockHeldHookUntilRelease)
		pInput->m_Hook = 0;
	const CNetObj_PlayerInput Current = *pInput;

	int SimTicks = g_Config.m_KxBafTicks;
	if(SimTicks < 1)
		SimTicks = 1;
	if(SimTicks > 20)
		SimTicks = 20;

	// Check danger WITH the current input (including the user's hook state).
	// This detects when the hook itself is pulling the player into danger.
	const int DangerWithCurrent = SimulateDangerTick(LocalId, Current, SimTicks);

	// Check danger WITHOUT hook (m_Hook=0). This is the "natural" danger state —
	// if the player isn't holding hook, are they in danger? When hook is already released this is
	// exactly the simulation above, which is the common case; do not run the same physics twice.
	CNetObj_PlayerInput NoHook = Current;
	NoHook.m_Hook = 0;
	const int DangerWithoutHook = Current.m_Hook == 0 ? DangerWithCurrent : SimulateDangerTick(LocalId, NoHook, SimTicks);

	// Remember the earliest danger tick for the rocket counter (it may not see it on its own path).
	m_DangerTickThisTick = DangerWithoutHook > 0 ? DangerWithoutHook : DangerWithCurrent;

	// Debug log (kx_baf_debug), tag "avoid": level 1 prints decision changes, level 2 every tick.
	const int PredTick = Client()->PredGameTick(g_Config.m_ClDummy);
	auto LogDecision = [&](int Action, const char *pText, int DangerTick, const CNetObj_PlayerInput *pChosen, int Survival) {
		if(g_Config.m_KxBafDebug == 0)
			return;
		if(Action == m_LogAction && g_Config.m_KxBafDebug < 2)
			return;
		m_LogAction = Action;
		char aChosen[96] = "";
		if(pChosen)
			str_format(aChosen, sizeof(aChosen), " chosen[dir=%d jump=%d hook=%d aim=(%d,%d)] survival=%d/%d",
				pChosen->m_Direction, pChosen->m_Jump, pChosen->m_Hook, pChosen->m_TargetX, pChosen->m_TargetY, Survival, SimTicks);
		log_info("avoid", "tick=%d %s pos=(%.0f,%.0f) vel=(%.0f,%.0f) danger[with_hook=%d no_hook=%d first=%d]%s",
			PredTick, pText, pGame->m_PredictedChar.m_Pos.x, pGame->m_PredictedChar.m_Pos.y,
			pGame->m_PredictedChar.m_Vel.x, pGame->m_PredictedChar.m_Vel.y,
			DangerWithCurrent, DangerWithoutHook, DangerTick, aChosen);
	};

	// No danger at all (with or without hook) — safe. Release any override.
	if(DangerWithCurrent == 0 && DangerWithoutHook == 0)
	{
		LogDecision(0, "safe: no danger on the current path", 0, nullptr, 0);
		if(m_WasOverriding)
		{
			// Only retract an override hook if the player is not holding hook themselves
			// (tclient rebuilds the hook bit from the raw key, so a held key is genuine).
			if(m_OverrideHook != 0 && pGame->m_Controls.m_aInputHook[g_Config.m_ClDummy] == 0)
				pInput->m_Hook = 0;
			m_WasOverriding = false;
		}
		return;
	}

	// Danger WITH hook, but safe WITHOUT hook — the user's hook IS the problem. Releasing it is a
	// separate permission from throwing one (kx_baf_release_hook vs kx_baf_hook): hook-walk players
	// usually want the release protection but not the auto-throw.
	if(g_Config.m_KxBafReleaseHook && DangerWithCurrent > 0 && DangerWithoutHook == 0)
	{
		// Test A: Can we wait 1 more tick with hook still active?
		const int DelayedDanger = SimulateDangerTickDelayed(LocalId, Current, 1, NoHook, SimTicks);
		if(DelayedDanger == 0)
		{
			m_SavedThisTick = true;
			LogDecision(1, "wait: keeping the hook is safe for one more tick", DelayedDanger, nullptr, 0);
			return; // hook is safe for now, don't intervene yet
		}

		// Can't wait — release hook NOW.
		LogDecision(3, "RELEASE hook: it is dragging us into danger", DangerWithoutHook, nullptr, 0);
		pInput->m_Hook = 0;
		m_SavedThisTick = true;
		m_WasOverriding = true;
		m_OverrideHook = 0;
		m_BlockHeldHookUntilRelease = HookKeyHeld;
		return;
	}

	// Danger exists even without hook — use brute-force to find an alternative.
	const int CurrentDanger = DangerWithoutHook;

	const bool AllowDir = g_Config.m_KxBafDirection != 0;
	const bool AllowJump = g_Config.m_KxBafJump != 0;
	const bool AllowHook = g_Config.m_KxBafHook != 0;
	const bool AllowAim = g_Config.m_KxBafAim != 0;

	int aDirs[3] = {0, 0, 0};
	int DirCount = 1;
	if(AllowDir)
	{
		aDirs[0] = -1;
		aDirs[1] = 0;
		aDirs[2] = 1;
		DirCount = 3;
	}
	else
	{
		aDirs[0] = Current.m_Direction;
	}

	int aJumps[2] = {0, 0};
	int JumpCount = 1;
	if(AllowJump)
	{
		aJumps[0] = 0;
		aJumps[1] = 1;
		JumpCount = 2;
	}
	else
	{
		aJumps[0] = Current.m_Jump != 0 ? 1 : 0;
	}

	int aHooks[2] = {0, 0};
	int HookCount = 1;
	if(AllowHook)
	{
		aHooks[0] = 0;
		aHooks[1] = 1;
		HookCount = 2;
	}
	else
	{
		aHooks[0] = Current.m_Hook != 0 ? 1 : 0;
	}

	// Aim angles: current cursor first, then expand symmetrically out through the FOV. The previous
	// edge-to-edge order combined with the early exit below selected the far left edge as soon as it
	// survived, even when a direction next to the cursor was equally safe.
	vec2 aAimTargets[145];
	int AimCount = 1;
	aAimTargets[0] = vec2((float)Current.m_TargetX, (float)Current.m_TargetY);
	if(AllowAim)
	{
		int NumAngles = g_Config.m_KxBafAngles;
		if(NumAngles < 1)
			NumAngles = 1;
		if(NumAngles > 144)
			NumAngles = 144;

		const float FovRad = (float)g_Config.m_KxBafFov * (pi / 180.0f);
		const float CurAngle = atan2f((float)Current.m_TargetY, (float)Current.m_TargetX);
		float AimDist = sqrtf((float)Current.m_TargetX * Current.m_TargetX + (float)Current.m_TargetY * Current.m_TargetY);
		if(AimDist < 1.0f)
			AimDist = 100.0f;

		AimCount = 1;
		const int Levels = maximum(1, NumAngles / 2);
		for(int Level = 1; AimCount < NumAngles; Level++)
		{
			const float Offset = (float)Level / (float)Levels * FovRad * 0.5f;
			for(const float Sign : {1.0f, -1.0f})
			{
				if(AimCount >= NumAngles)
					break;
				const float Angle = CurAngle + Sign * Offset;
				aAimTargets[AimCount++] = vec2(cosf(Angle) * AimDist, sinf(Angle) * AimDist);
			}
		}
	}

	// Profiling: the brute force is the heaviest thing the safety features do. With kx_baf_debug on,
	// report a search phase that took long enough to be felt as a frame hitch.
	auto ProfLog = [&](const char *pPhase, int64_t Start) {
		if(g_Config.m_KxBafDebug == 0)
			return;
		const float Ms = (float)(time_get() - Start) * 1000.0f / (float)time_freq();
		if(Ms < 2.0f)
			return;
		log_info("avoid", "PROFILE: %s took %.1f ms", pPhase, Ms);
	};

	// =====================================================
	// Test A: "Can I wait 1 more tick?"
	// =====================================================
	const int64_t ProfA = time_get();
	bool CanWait = false;
	for(int di = 0; di < DirCount && !CanWait; di++)
	{
		for(int ji = 0; ji < JumpCount && !CanWait; ji++)
		{
			for(int hi = 0; hi < HookCount && !CanWait; hi++)
			{
				// With hook released the aim cannot affect movement. All aim candidates below are
				// physically identical, so simulate the first one only. Test A only needs a boolean.
				const int AnglesToTest = aHooks[hi] == 0 ? 1 : AimCount;
				for(int ai = 0; ai < AnglesToTest && !CanWait; ai++)
				{
					CNetObj_PlayerInput Test = Current;
					Test.m_Direction = aDirs[di];
					Test.m_Jump = aJumps[ji];
					Test.m_Hook = aHooks[hi];
					Test.m_TargetX = (int)aAimTargets[ai].x;
					Test.m_TargetY = (int)aAimTargets[ai].y;

					const int DelayDanger = SimulateDangerTickDelayed(LocalId, Current, 1, Test, SimTicks);
					if(DelayDanger == 0)
						CanWait = true;
				}
			}
		}
	}

	ProfLog("test A (wait check)", ProfA);

	if(CanWait)
	{
		m_SavedThisTick = true;
		LogDecision(1, "wait: an escape input survives after one more tick", DangerWithoutHook, nullptr, 0);
		return;
	}

	// =====================================================
	// Test B: "Act NOW" — brute-force all combos from tick 0.
	// =====================================================
	const int64_t ProfB = time_get();
	bool Found = false;
	bool Done = false; // a full-window survivor with the smallest possible diff: nothing can beat it
	CNetObj_PlayerInput BestInput = Current;
	int BestSurvival = -1;
	int BestDiff = 999;

	for(int di = 0; di < DirCount && !Done; di++)
	{
		for(int ji = 0; ji < JumpCount && !Done; ji++)
		{
			for(int hi = 0; hi < HookCount && !Done; hi++)
			{
				// Releasing hook makes aim irrelevant to physics. Keep iterating every aim below so
				// InputDiff and the original tie-breaking order stay bit-for-bit unchanged, but reuse
				// the one simulation result for all of them.
				int NoHookDanger = -1;
				for(int ai = 0; ai < AimCount && !Done; ai++)
				{
					CNetObj_PlayerInput Test = Current;
					Test.m_Direction = aDirs[di];
					Test.m_Jump = aJumps[ji];
					Test.m_Hook = aHooks[hi];
					Test.m_TargetX = (int)aAimTargets[ai].x;
					Test.m_TargetY = (int)aAimTargets[ai].y;

					// Skip the exact current input.
					if(Test.m_Direction == Current.m_Direction &&
						(Test.m_Jump != 0) == (Current.m_Jump != 0) &&
						(Test.m_Hook != 0) == (Current.m_Hook != 0) &&
						Test.m_TargetX == Current.m_TargetX &&
						Test.m_TargetY == Current.m_TargetY)
						continue;

					int Danger;
					if(aHooks[hi] == 0 && NoHookDanger >= 0)
						Danger = NoHookDanger;
					else
					{
						Danger = SimulateDangerTick(LocalId, Test, SimTicks);
						if(aHooks[hi] == 0)
							NoHookDanger = Danger;
					}
					const int Survival = (Danger == 0) ? SimTicks : Danger - 1;
					const int Diff = InputDiff(Current, Test);

					if(BestSurvival < 0 ||
						Survival > BestSurvival ||
						(Survival == BestSurvival && Diff < BestDiff))
					{
						BestSurvival = Survival;
						BestDiff = Diff;
						BestInput = Test;
						Found = true;
						// Every combo changes at least one field (the exact current input is skipped), so
						// a full survivor at diff 1 is optimal: later combos can at best match survival
						// with a larger diff. Stop the brute force right here.
						if(BestSurvival >= SimTicks && BestDiff <= 1)
							Done = true;
					}
				}
			}
		}
	}

	ProfLog("test B (brute force)", ProfB);

	// Apply if the best combo survives longer than the current input.
	const int CurrentSurvival = CurrentDanger - 1;
	if(Found && BestSurvival > CurrentSurvival)
	{
		LogDecision(2, "OVERRIDE input", DangerWithoutHook, &BestInput, BestSurvival);
		// A full-window survivor means avoid handles this on its own; tell the rocket counter.
		m_SavedThisTick = BestSurvival >= SimTicks;
		pInput->m_Direction = BestInput.m_Direction;
		pInput->m_Jump = BestInput.m_Jump;
		pInput->m_Hook = BestInput.m_Hook;

		m_WasOverriding = true;
		m_OverrideHook = BestInput.m_Hook;
		if(HookKeyHeld && Current.m_Hook != 0 && BestInput.m_Hook == 0)
			m_BlockHeldHookUntilRelease = true;

		// Apply aim: silent (only patched into the sent packet by CControls)
		// or visible (move the local mouse/input as well). While the player is holding the hook key the
		// aim is usually THEIR hook throw direction and moving it would make their hooks miss — but when
		// the escape combo throws a hook ITSELF, the aim has to be the simulated one: otherwise the game
		// throws at the player's cursor, the hook grabs a different spot (or nothing at all) and the
		// rescue the simulation promised never happens. That is why falling into freeze "just wasn't
		// saved" for hook-holding players: every simulated hook rescue was fiction.
		const bool ThrowOwnHook = BestInput.m_Hook != 0;
		if(AllowAim && (pGame->m_Controls.m_aInputHook[g_Config.m_ClDummy] == 0 || ThrowOwnHook) &&
			(BestInput.m_TargetX != Current.m_TargetX || BestInput.m_TargetY != Current.m_TargetY))
		{
			const vec2 AimTarget((float)BestInput.m_TargetX, (float)BestInput.m_TargetY);
			if(g_Config.m_KxBafSilent)
			{
				pGame->m_Controls.m_AvoidAimActive = true;
				pGame->m_Controls.m_AvoidAimTarget = AimTarget;
				// The hook avoid throws must aim the same way locally, or the client predicts a
				// different hook than the server gets and every later decision runs on a world that
				// never existed.
				if(ThrowOwnHook)
				{
					pInput->m_TargetX = BestInput.m_TargetX;
					pInput->m_TargetY = BestInput.m_TargetY;
				}
			}
			else
			{
				pGame->m_Controls.m_aMousePos[g_Config.m_ClDummy] = AimTarget;
				pInput->m_TargetX = BestInput.m_TargetX;
				pInput->m_TargetY = BestInput.m_TargetY;
				if(!pInput->m_TargetX && !pInput->m_TargetY)
					pInput->m_TargetX = 1;
			}
		}
	}
	else
	{
		m_NoSolutionThisTick = true;
		LogDecision(4, "no safe input found: leaving momentum alone", DangerWithoutHook, nullptr, 0);
	}
}
