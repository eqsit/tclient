/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"


#include <base/log.h>
#include <base/math.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>
#include <game/mapitems.h>
#include <game/rocket_save.h>

CControls::CControls()
{
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
}

void CControls::OnReset()
{
	ResetInput(0);
	ResetInput(1);
	CancelAntiVoidRocket(0, false);
	CancelAntiVoidRocket(1, false);

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;

	m_LastSendTime = 0;
}

void CControls::ResetInput(int Dummy)
{
	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing the fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
	m_aInputHook[Dummy] = 0;
	m_aInputFirePressed[Dummy] = false;
}

void CControls::OnPlayerDeath()
{
	// Outcome log: the tee died, report the last avoid/rocket state so the logs show what happened.
	if(g_Config.m_KxBafDebug != 0 || g_Config.m_TcAntiVoidRocketDebug != 0)
		log_info("avoid", "OUTCOME: DIED pos=(%.0f,%.0f) vel=(%.0f,%.0f) lastAvoid=%d rocketState=%d",
			GameClient()->m_PredictedChar.m_Pos.x, GameClient()->m_PredictedChar.m_Pos.y,
			GameClient()->m_PredictedChar.m_Vel.x, GameClient()->m_PredictedChar.m_Vel.y,
			GameClient()->m_AvoidFreeze.LastAction(), m_aAntiVoidRocketLogState[g_Config.m_ClDummy]);

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;
	// Drop all rocket ownership so neither a held auto-fire press nor a weapon restore crosses respawn.
	CancelAntiVoidRocket(0);
	CancelAntiVoidRocket(1);
	for(int &PrevWeapon : m_aAntiVoidLaserPrevWeapon)
		PrevWeapon = -1;
	for(bool &Pending : m_aAntiVoidLaserReleasePending)
		Pending = false;
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	*pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(pVariable == &pState->m_pControls->m_aInputData[g_Config.m_ClDummy].m_Fire)
		pState->m_pControls->m_aInputFirePressed[g_Config.m_ClDummy] = pResult->GetInteger(0) != 0;
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		// A real key press always owns the weapon choice. In particular, do not let the rocket
		// counter overwrite this choice later in the same SnapInput call or restore an older weapon.
		const int Dummy = g_Config.m_ClDummy;
		pSet->m_pControls->m_aAntiVoidRocketManualWeapon[Dummy] = true;
		pSet->m_pControls->m_aAntiVoidRocketPrevWeapon[Dummy] = -1;
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

// TClient hole assist bind: track the raw held state, and in toggle mode flip the latch on each press edge.
// Both are kept up to date regardless of mode, so switching tc_hole_assist_hold mid-game just works.
void CControls::ConKeyHoleAssist(IConsole::IResult *pResult, void *pUserData)
{
	CControls *pControls = (CControls *)pUserData;
	const bool Pressed = pResult->GetInteger(0) != 0;
	const bool PressEdge = Pressed && !pControls->m_HoleAssistPressed;
	const bool ReleaseEdge = !Pressed && pControls->m_HoleAssistPressed;
	const bool WasActive = pControls->HoleAssistActive();
	if(PressEdge)
		pControls->m_HoleAssistToggled = !pControls->m_HoleAssistToggled;
	pControls->m_HoleAssistPressed = Pressed;
	// Fresh engage/disengage: forget any parked state so the next run re-approaches the gap cleanly.
	if(PressEdge || ReleaseEdge)
		for(bool &Settled : pControls->m_aHoleSettled)
			Settled = false;
	// Announce the on/off status in chat whenever the effective activation flips (both hold and toggle modes).
	const bool NowActive = pControls->HoleAssistActive();
	if(g_Config.m_TcHoleAssist && NowActive != WasActive && pControls->Client()->State() == IClient::STATE_ONLINE)
		pControls->GameClient()->Echo(NowActive ? "Hole assist: ON" : "Hole assist: OFF");
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		// Next/previous uses counters and clears WantedWeapon below, so inspecting WantedWeapon in
		// the rocket code cannot distinguish this manual request. Latch it at the input edge instead.
		const int Dummy = g_Config.m_ClDummy;
		pSet->m_pControls->m_aAntiVoidRocketManualWeapon[Dummy] = true;
		pSet->m_pControls->m_aAntiVoidRocketPrevWeapon[Dummy] = -1;
	}
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump}};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		// TClient: the hook key writes to the shadow m_aInputHook, from which m_aInputData.m_Hook is
		// rebuilt every tick (like +left/+right feed the direction). Avoid can therefore distinguish a
		// physically held key from the hook bit it overrides in the outgoing input.
		static CInputState s_State = {this, {&m_aInputHook[0], &m_aInputHook[1]}};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}

	// TClient hole assist activation key (any bindable key; hold or toggle per tc_hole_assist_hold)
	Console()->Register("+tc_hole_assist", "", CFGFLAG_CLIENT, ConKeyHoleAssist, this, "Activate the hole assist (hold or toggle depending on tc_hole_assist_hold)");
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

int CControls::SnapInput(int *pData)
{
	// TClient: silent-aim channel of the Kinetix avoid is only valid for this tick.
	m_AvoidAimActive = false;

	// update player state
	if(GameClient()->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	switch(m_aMouseInputType[g_Config.m_ClDummy])
	{
	case CControls::EMouseInputType::AUTOMATED:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
		break;
	case CControls::EMouseInputType::ABSOLUTE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
		break;
	case CControls::EMouseInputType::RELATIVE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
		break;
	}

	// TClient
	if(g_Config.m_TcHideChatBubbles && Client()->RconAuthed())
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags &= ~PLAYERFLAG_CHATTING;

	if(g_Config.m_TcNameplatePingCircle)
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
			ResetInput(g_Config.m_ClDummy);

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		vec2 Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// TClient: keep the automatic safety features (anti-void, balancer, rocket counter) alive even while
		// chat or a menu is open. First rebuild the movement direction from the held keys, so a brake set on a
		// previous tick is released once the danger is gone instead of sticking, then let the features steer.
		if(g_Config.m_TcBalancer || g_Config.m_TcAntiVoidRocket || g_Config.m_TcAntiVoidLaser)
		{
			m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
			if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
				m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
			if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
				m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

			// Snapshot the (idle) input before the safety features run, so we can detect whether any of
			// them actually steered us on this tick.
			const int PreDirection = m_aInputData[g_Config.m_ClDummy].m_Direction;
			const int PreJump = m_aInputData[g_Config.m_ClDummy].m_Jump;
			const int PreFire = m_aInputData[g_Config.m_ClDummy].m_Fire;
			const int PreHook = m_aInputData[g_Config.m_ClDummy].m_Hook;
			const int PreWantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

			// TClient: Kinetix Basic Avoid Freeze runs first so the rocket counter below can see
			// whether avoid already saves us on its own (and skip a pointless shot).
			GameClient()->m_AvoidFreeze.ApplyOverride();
			ApplyAutoSafety();

			// The server silently drops BOTH movement and fire from any input that still carries
			// PLAYERFLAG_CHATTING (see CPlayer::OnPredictedInput / OnPredictedEarlyInput in
			// src/game/server/player.cpp). So while chat is open we present the input as PLAYING instead
			// of CHATTING — but ONLY on the ticks where a safety feature actually changed our input.
			// Stripping the flag every tick would flip it CHATTING<->PLAYING constantly, which forces the
			// input to be resent ~50x/s while you type: that floods the netchannel and was making chat
			// messages drop and the rocket counter misfire. Keeping the normal CHATTING flag on idle ticks
			// also leaves your "typing" bubble visible except during an actual save. Opt-in via
			// tc_safety_in_chat (on by default).
			const bool SafetyActed =
				m_aInputData[g_Config.m_ClDummy].m_Direction != PreDirection ||
				m_aInputData[g_Config.m_ClDummy].m_Jump != PreJump ||
				m_aInputData[g_Config.m_ClDummy].m_Fire != PreFire ||
				m_aInputData[g_Config.m_ClDummy].m_Hook != PreHook ||
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != PreWantedWeapon;
			if(g_Config.m_TcSafetyInChat && SafetyActed && HaveLocalChar() && (m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_CHATTING))
				m_aInputData[g_Config.m_ClDummy].m_PlayerFlags =
					(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & ~PLAYERFLAG_CHATTING) | PLAYERFLAG_PLAYING;

			// Send the moment a feature changed our input (or we flipped the chatting flag), so
			// braking/rockets react without waiting on the once-a-second heartbeat below.
			Send = Send || m_aInputData[g_Config.m_ClDummy].m_PlayerFlags != m_aLastData[g_Config.m_ClDummy].m_PlayerFlags;
			Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
			Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
			Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
			Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
			Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		}

		// send once a second just to be sure
		Send = Send || time_get() > m_LastSendTime + time_freq();
	}
	else
	{
		// TClient
		vec2 Pos;
		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			Pos = GameClient()->m_Controls.m_aMousePosOnAction[g_Config.m_ClDummy];
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}
		else
			Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];

		m_FastInputHookAction = false;
		m_FastInputFireAction = false;

		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

		// TClient: rebuild the hook bit from the raw held key every tick (see m_aInputHook), then let avoid
		// decide whether a previously force-released hook may return. By default avoid keeps it suppressed
		// until the physical key is released; kx_baf_rehook opts into restoring it automatically when safe.
		m_aInputData[g_Config.m_ClDummy].m_Hook = m_aInputHook[g_Config.m_ClDummy];

		// TClient: Kinetix Basic Avoid Freeze runs first so the rocket counter below can see
		// whether avoid already saves us on its own (and skip a pointless shot).
		GameClient()->m_AvoidFreeze.ApplyOverride();

		// Anti-void braking, balancer, and the rocket counter. Factored into ApplyAutoSafety so the exact same
		// logic also runs while chat/menu is open (the frozen-input branch above) and while spectating.
		ApplyAutoSafety();
		// TClient: hook aim assist — while holding hook and the hook has not grabbed anything yet,
		// nudge the aim toward the best *reachable* player so it's easier to save someone.
		// A player only counts when ALL of these hold:
		//   - aim is within tc_hook_aim_angle degrees of them (the cone)
		//   - they are within hook range (the hook physically cannot reach further)
		//   - line of sight is clear (a solid block between us would just catch the hook first)
		// Among the candidates we pick the one closest to where you are already aiming.
		if(g_Config.m_TcHookAim && m_aInputData[g_Config.m_ClDummy].m_Hook != 0 &&
			!GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_pLocalCharacter)
		{
			const int HookState = GameClient()->m_PredictedChar.m_HookState;
			if(HookState == HOOK_IDLE || HookState == HOOK_FLYING)
			{
				const vec2 LocalPos = GameClient()->m_LocalCharacterPos;
				const vec2 AimVec((float)m_aInputData[g_Config.m_ClDummy].m_TargetX,
					(float)m_aInputData[g_Config.m_ClDummy].m_TargetY);
				if(length(AimVec) > 0.001f)
				{
					const vec2 AimDir = normalize(AimVec);
					const float MaxAngle = (float)g_Config.m_TcHookAimAngle * (pi / 180.0f);
					// The hook can never reach further than its tuned length, so anyone beyond it is ignored.
					const float MaxDist = GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookLength;

					int BestId = -1;
					float BestAngle = MaxAngle;

					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(!GameClient()->m_Snap.m_aCharacters[i].m_Active)
							continue;
						if(i == GameClient()->m_Snap.m_LocalClientId)
							continue;

						const vec2 PlayerPos = GameClient()->m_aClients[i].m_RenderPos;
						const vec2 ToPlayer = PlayerPos - LocalPos;
						const float Dist = length(ToPlayer);
						// Too close to derive a direction, or simply out of hook range.
						if(Dist < 1.0f || Dist > MaxDist)
							continue;

						// Must sit inside the cone and beat the current best candidate.
						const float Ang = acosf(std::clamp(dot(AimDir, ToPlayer / Dist), -1.0f, 1.0f));
						if(Ang >= BestAngle)
							continue;

						// Line of sight: if a solid tile sits between us and the player, the hook would
						// stick to that wall instead, so don't aim at this player.
						vec2 ColPos;
						if(Collision()->IntersectLine(LocalPos, PlayerPos, &ColPos, nullptr) && distance(LocalPos, ColPos) < Dist - 2.0f)
							continue;

						BestAngle = Ang;
						BestId = i;
					}

					if(BestId >= 0)
					{
						const vec2 ToPlayer = GameClient()->m_aClients[BestId].m_RenderPos - LocalPos;
						m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)ToPlayer.x;
						m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)ToPlayer.y;
						if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
							m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;
					}
				}
			}
		}

		// TClient: real weapon spin — rotate the SENT aim so other players also see the weapon spinning.
		// Keep the real aim on the exact ticks we hook or fire, so those actions still go where we point.
		if(g_Config.m_TcWeaponSpin && g_Config.m_TcWeaponSpinReal && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const bool Firing = (m_aInputData[g_Config.m_ClDummy].m_Fire & 1) != 0;
			const bool Hooking = m_aInputData[g_Config.m_ClDummy].m_Hook != 0;
			if(!Firing && !Hooking)
			{
				const vec2 Target((float)m_aInputData[g_Config.m_ClDummy].m_TargetX, (float)m_aInputData[g_Config.m_ClDummy].m_TargetY);
				float Mag = length(Target);
				if(Mag < 30.0f)
					Mag = 100.0f;
				const float Spin = WeaponSpinAngle(Mag > 0.001f ? angle(Target) : 0.0f, Client()->LocalTime());
				const vec2 Spun = direction(Spin) * Mag;
				m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Spun.x;
				m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Spun.y;
				if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
					m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;
			}
		}

		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;

			// Don't copy any input to dummy when spectating others
			if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
			{
				pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
				pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
				pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
				pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
				pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
				pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
				pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

				if(!g_Config.m_ClDummyControl)
					pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

				pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
				pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
			}

			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		// stress testing
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}

		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

	if(!Send)
		return 0;

	m_LastSendTime = time_get();
	mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

	// TClient: Kinetix Basic Avoid Freeze silent aim — patch the packet that is sent to the
	// server only, so the local prediction and the visible crosshair keep the real aim.
	if(m_AvoidAimActive)
	{
		CNetObj_PlayerInput *pSend = (CNetObj_PlayerInput *)pData;
		pSend->m_TargetX = (int)m_AvoidAimTarget.x;
		pSend->m_TargetY = (int)m_AvoidAimTarget.y;
		if(!pSend->m_TargetX && !pSend->m_TargetY)
			pSend->m_TargetX = 1;
		m_AvoidAimActive = false;
	}
	return sizeof(m_aInputData[0]);
}

// TClient: do we have a local tee on the map to act on? Normally that is m_pLocalCharacter, but while
// paused/spectating (press Q to watch others) the client leaves m_pLocalCharacter null even though our
// character is still in the snapshot — so also accept an active local character item. With no character at
// all (true spectator on the spectators team) this is false and the safety features stay idle.
bool CControls::HaveLocalChar() const
{
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	return GameClient()->m_Snap.m_pLocalCharacter ||
	       (LocalId >= 0 && GameClient()->m_Snap.m_aCharacters[LocalId].m_Active);
}

// TClient: position of our own tee for the safety features. In normal play this is the smoothed render
// position (m_LocalCharacterPos). While paused/spectating (press Q to watch others) the client deliberately
// leaves that stale and m_pLocalCharacter null even though our tee is still in the snapshot, so we fall back
// to the predicted core position, which keeps updating as long as the local character is on the map.
vec2 CControls::LocalCharPos() const
{
	if(GameClient()->m_Snap.m_pLocalCharacter)
		return GameClient()->m_LocalCharacterPos;
	return GameClient()->m_PredictedChar.m_Pos;
}

// TClient: run the automatic safety features on the local tee. Factored out of SnapInput so the exact same
// logic runs both during normal play and while input is frozen by an open chat or menu, and so it also runs
// while paused/spectating — as long as our tee is still on the map. With no local tee it does nothing.
void CControls::ApplyAutoSafety()
{
	const int Dummy = g_Config.m_ClDummy;
	// Disabling the feature must still release a press it generated and discard its saved weapon.
	// Previously the entire function stopped being called, leaving full-auto fire held indefinitely.
	if(!g_Config.m_TcAntiVoidRocket)
		CancelAntiVoidRocket(Dummy);

	// We need a local tee to act on. With no character at all (true spectator) the features simply no-op.
	if(!HaveLocalChar())
		return;

	// While frozen the server throws the whole input away, so the safety features stand down.
	// The laser counter still gets its upkeep call so a held fire press is released.
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	const bool Frozen = LocalId >= 0 && GameClient()->m_aClients[LocalId].m_Predicted.m_FreezeEnd != 0;

	// Outcome log: edge-triggered "got frozen" report so the logs show whether avoid/rocket saved us.
	if((g_Config.m_KxBafDebug != 0 || g_Config.m_TcAntiVoidRocketDebug != 0) && Frozen != m_aAvoidWasFrozen[g_Config.m_ClDummy])
	{
		const int SinceFire = m_aAntiVoidRocketLastFireTick[g_Config.m_ClDummy] >= 0 ? Client()->PredGameTick(g_Config.m_ClDummy) - m_aAntiVoidRocketLastFireTick[g_Config.m_ClDummy] : -1;
		if(Frozen)
		{
			const CAntiVoidRocketDiag &D = m_aAntiVoidRocketDiag[g_Config.m_ClDummy];
			log_info("avoid", "OUTCOME: GOT FROZEN pos=(%.0f,%.0f) vel=(%.0f,%.0f) lastAvoid=%d rocketState=%d sinceFire=%d rocket[pathTick=%d dist=%.1f solid=%d avoidSaves=%d nosol=%d need=%d]",
				GameClient()->m_PredictedChar.m_Pos.x, GameClient()->m_PredictedChar.m_Pos.y,
				GameClient()->m_PredictedChar.m_Vel.x, GameClient()->m_PredictedChar.m_Vel.y,
				GameClient()->m_AvoidFreeze.LastAction(), m_aAntiVoidRocketLogState[g_Config.m_ClDummy], SinceFire,
				D.m_DangerTick, D.m_DangerDist, D.m_Solid ? 1 : 0, D.m_AvoidSaves ? 1 : 0, D.m_AvoidNoSolution ? 1 : 0, D.m_NeedRocket ? 1 : 0);
		}
		else
			log_info("avoid", "OUTCOME: unfrozen pos=(%.0f,%.0f) lastAvoid=%d rocketState=%d sinceFire=%d",
				GameClient()->m_PredictedChar.m_Pos.x, GameClient()->m_PredictedChar.m_Pos.y,
				GameClient()->m_AvoidFreeze.LastAction(), m_aAntiVoidRocketLogState[g_Config.m_ClDummy], SinceFire);
	}
	m_aAvoidWasFrozen[g_Config.m_ClDummy] = Frozen;

	if(Frozen)
	{
		if(g_Config.m_TcAntiVoidRocket)
			ApplyAntiVoidRocket(true);
		ApplyAntiVoidLaser(true);
		return;
	}

	// Balancer: we remember whether it is engaged on a tee this tick so the rocket counter below can stand down.
	bool BalancerActive = false;
	if(g_Config.m_TcBalancer)
		BalancerActive = ApplyBalancer();

	// Rocket counter runs independently of every other safety feature. But while the balancer
	// is holding us on someone's head over the void, an auto-fired rocket would blow us off it, so optionally
	// suppress the FIRING for as long as the balancer is engaged. We still CALL it (Suppressed) so it can
	// release a previously-held fire press and tick its cooldown — skipping the call entirely would leave the
	// fire bit stuck "held" and permanently break the counter once the balancer engaged right after a shot.
	if(g_Config.m_TcAntiVoidRocket)
		ApplyAntiVoidRocket(g_Config.m_TcBalancerDisableRocket && BalancerActive);

	// Laser self-ricochet counter: auto-fires laser into nearest wall to bounce back into ourselves on danger.
	if(g_Config.m_TcAntiVoidLaser)
		ApplyAntiVoidLaser(g_Config.m_TcBalancerDisableRocket && BalancerActive);

	// Hole assist runs last so that, while you deliberately engage it, it wins the horizontal direction.
	if(g_Config.m_TcHoleAssist && HoleAssistActive())
		ApplyHoleAssist();
}

void CControls::CancelAntiVoidRocket(int Dummy, bool ReleaseFire)
{
	if(ReleaseFire && !m_aInputFirePressed[Dummy] && m_aAntiVoidRocketReleasePending[Dummy] &&
		(m_aInputData[Dummy].m_Fire & 1) != 0 &&
		m_aInputData[Dummy].m_Fire == m_aAntiVoidRocketFireValue[Dummy])
		m_aInputData[Dummy].m_Fire++;
	m_aAntiVoidRocketReleasePending[Dummy] = false;
	m_aAntiVoidRocketFireValue[Dummy] = 0;
	m_aAntiVoidRocketCooldown[Dummy] = 0;
	m_aAntiVoidRocketPrevWeapon[Dummy] = -1;
	m_aAntiVoidRocketManualWeapon[Dummy] = false;
}

// TClient hole assist: is it engaged right now? Hold mode follows the key; toggle mode follows the latch.
bool CControls::HoleAssistActive() const
{
	return g_Config.m_TcHoleAssistHold ? m_HoleAssistPressed : m_HoleAssistToggled;
}

// TClient avoid: classify a single tile point. 0 = safe, 1 = freeze (you lose control and slide, but
// freeze itself is NOT death — you get hooked out or unfrozen), 2 = real death with no recovery:
// a kill tile, deep freeze, the map edge, or a teleporter. This split is the whole point: in gores
// you dive into freeze constantly and hook back out, so treating plain freeze as death made the bot
// fight every jump. Death is only when the frozen slide actually reaches a class-2 spot. Both game
// and front layers are read raw (GetCollisionAt never reports freeze tiles). With tc_avoid_unfreeze
// off, freeze is treated strictly (class 2) for players who really want to never touch it.
int CControls::AvoidDangerClassPoint(float x, float y, bool ForceFreezeRecoverable) const
{
	const int Tx = (int)(x / 32.0f);
	const int Ty = (int)(y / 32.0f);
	if(Tx < 0 || Ty < 0 || Tx >= Collision()->GetWidth() || Ty >= Collision()->GetHeight())
		return 2; // running off the map kills
	const int Index = Collision()->GetPureMapIndex(x, y);
	if(g_Config.m_TcAntiVoidTele && (Collision()->IsTeleport(Index) || Collision()->IsEvilTeleport(Index)))
		return 2;
	const bool FreezeRecoverable = g_Config.m_TcAvoidUnfreeze || ForceFreezeRecoverable;
	int Worst = 0;
	const int aTiles[] = {
		Collision()->GetTileIndex(Index),
		Collision()->GetFrontTileIndex(Index),
		Collision()->GetSwitchType(Index)
	};
	for(const int T : aTiles)
	{
		if((g_Config.m_TcAntiVoidDeath && T == TILE_DEATH) || (g_Config.m_TcAntiVoidDeepFreeze && T == TILE_DFREEZE))
			return 2;
		if((g_Config.m_TcAntiVoidFreeze && T == TILE_FREEZE) || (g_Config.m_TcAntiVoidLiveFreeze && T == TILE_LFREEZE))
			Worst = FreezeRecoverable ? 1 : 2;
	}
	return Worst;
}

// TClient avoid: is this exact point a hard, no-recovery death — a kill tile or off the map edge?
// Freeze is deliberately excluded here; it has its own, smaller corner margin below.
bool CControls::AvoidHardDeathPoint(float x, float y) const
{
	const int Tx = (int)(x / 32.0f);
	const int Ty = (int)(y / 32.0f);
	if(Tx < 0 || Ty < 0 || Tx >= Collision()->GetWidth() || Ty >= Collision()->GetHeight())
		return true; // off the map
	const int Index = Collision()->GetPureMapIndex(x, y);
	return (g_Config.m_TcAntiVoidDeath && (
		Collision()->GetTileIndex(Index) == TILE_DEATH ||
		Collision()->GetFrontTileIndex(Index) == TILE_DEATH ||
		Collision()->GetSwitchType(Index) == TILE_DEATH));
}

// TClient avoid: danger for the whole tee body centered at (x, y). Three shells, small to big:
//  - centre: the true tile you stand on (freeze or death), exactly like the game.
//  - ±6px freeze margin: a freeze tile this close means your body is a pixel from freezing — the
//    game freezes on the centre only, but the lightweight sim can drift ~1px, so this small buffer
//    catches the "clipped the freeze corner by a pixel" case. Kept small (6px « 16px half-tile) so a
//    tight freeze channel you thread down the middle does NOT false-trigger.
//  - ±14px death corners: a kill tile / off-map anywhere under your full hitbox, so a death void that
//    only juts into the edge of your body is caught with a margin.
int CControls::AvoidDangerClass(float x, float y, bool ForceFreezeRecoverable) const
{
	int Worst = AvoidDangerClassPoint(x, y, ForceFreezeRecoverable);
	if(Worst == 2)
		return 2;
	// tc_avoid_freeze_margin: 0 reproduces the game exactly (freeze is decided by the centre tile alone,
	// so grazing freeze with the edge of the hitbox is legal and the bot stays out of it); anything larger
	// makes the bot treat freeze that close to your centre as already touched.
	const float FreezeMargin = (float)g_Config.m_TcAvoidFreezeMargin;
	if(FreezeMargin > 0.0f)
	{
		for(const float Ox : {-FreezeMargin, FreezeMargin})
			for(const float Oy : {-FreezeMargin, FreezeMargin})
				Worst = maximum(Worst, AvoidDangerClassPoint(x + Ox, y + Oy, ForceFreezeRecoverable));
		if(Worst == 2)
			return 2;
	}
	const float R = CCharacterCore::PhysicalSize() / 2.0f; // full half-hitbox = 14px
	for(const float Ox : {-R, R})
		for(const float Oy : {-R, R})
			if(AvoidHardDeathPoint(x + Ox, y + Oy))
				return 2;
	return Worst; // 0 = safe, or 1 = recoverable freeze within the body
}

bool CControls::BalancerTeeInVoid(vec2 TeePos) const
{
	const float R = 28.0f; // tee half-size
	auto Bad = [](int T) { return T == TILE_DEATH || T == TILE_FREEZE || T == TILE_DFREEZE || T == TILE_LFREEZE; };
	const float MaxY = TeePos.y + (float)g_Config.m_TcBalancerVoidDepth * 32.0f;
	const float aColX[] = {TeePos.x - R * 0.7f, TeePos.x, TeePos.x + R * 0.7f};
	for(const float ColX : aColX)
	{
		for(float y = TeePos.y; y <= MaxY; y += 16.0f)
		{
			const int Tx = (int)(ColX / 32.0f);
			const int Ty = (int)(y / 32.0f);
			if(Tx < 0 || Ty < 0 || Tx >= Collision()->GetWidth() || Ty >= Collision()->GetHeight())
				break; // off the map edge => this column is deadly, try the next one
			const int Index = Collision()->GetPureMapIndex(ColX, y);
			if(Bad(Collision()->GetTileIndex(Index)) || Bad(Collision()->GetFrontTileIndex(Index)))
				break; // death/freeze before any ground => this column is deadly, try the next one
			if(Collision()->CheckPoint(ColX, y))
				return false; // safe solid ground under the body => the tee has a foothold, not the void
		}
	}
	return true; // no column had safe footing => the tee is in the void
}

// TClient balancer: only steps in when we are about to slide off the head of a tee that is over the void.
// It does NOT take over our movement: while we are comfortably within the head, our input is left alone so
// we can still walk/jump on the model. Only once we drift past the edge threshold (predicting one tick
// ahead so it reacts as we *start* to slide) does it counter-steer back toward the center to save us.
bool CControls::ApplyBalancer()
{
	const int Dummy = g_Config.m_ClDummy;
	const vec2 CharPos = LocalCharPos();
	const float MaxDist = (float)g_Config.m_TcBalancerDistance;
	const float Edge = (float)g_Config.m_TcBalancerEdge;

	// Pick the nearest in-void tee we are standing on. Smaller Y is higher up, so "below us" is TeePos.y > CharPos.y.
	int BestId = -1;
	float BestDist = MaxDist;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(i == GameClient()->m_Snap.m_LocalClientId)
			continue;
		if(!GameClient()->m_Snap.m_aCharacters[i].m_Active)
			continue;

		const vec2 TeePos = GameClient()->m_aClients[i].m_RenderPos;
		if(g_Config.m_TcBalancerOnlyAbove && TeePos.y <= CharPos.y)
			continue;
		const float Dist = distance(CharPos, TeePos);
		if(Dist > BestDist)
			continue;
		// Requirement: only ever balance while the tee is actually in the void.
		if(!BalancerTeeInVoid(TeePos))
			continue;

		BestDist = Dist;
		BestId = i;
	}

	static bool s_aRescuing[NUM_DUMMIES] = {false, false};
	if(BestId < 0)
	{
		if(g_Config.m_TcBalancerDebug && s_aRescuing[Dummy])
			log_info("balancer", "idle (no in-void tee under you)");
		s_aRescuing[Dummy] = false;
		return false; // not engaged on any tee
	}

	// How far off the head center are we (+ = right of center), looked one tick ahead so we react as we
	// start to slide rather than after we already have.
	const float OffsetX = CharPos.x - GameClient()->m_aClients[BestId].m_RenderPos.x;
	const float PredOffsetX = OffsetX + GameClient()->m_PredictedChar.m_Vel.x;

	int &Dir = m_aInputData[Dummy].m_Direction;
	bool Rescue = false;
	if(PredOffsetX > Edge)
	{
		Dir = -1; // sliding off to the right -> push back left
		Rescue = true;
	}
	else if(PredOffsetX < -Edge)
	{
		Dir = 1; // sliding off to the left -> push back right
		Rescue = true;
	}
	// else: within the safe zone -> leave m_Direction exactly as the player set it (no paralysis).

	if(g_Config.m_TcBalancerDebug && Rescue != s_aRescuing[Dummy])
		log_info("balancer", "%s (offset %.0fpx, edge %.0f)", Rescue ? "rescuing" : "released (within safe zone)", OffsetX, Edge);
	s_aRescuing[Dummy] = Rescue;
	return true; // engaged on an in-void tee (whether actively rescuing or holding within the safe zone)
}

// TClient hole assist: scan the tiles in a box around the tee for the nearest narrow gap. A "gap" is a run
// of open (non-solid) tiles in a single row that is bounded by a solid tile on BOTH ends and is at most
// 2 tiles wide — i.e. a slot in a wall you could fall/fly through, not the open corridor you are standing
// in. Returns the world-x of the center of the gap whose center is horizontally closest to us. Off-map
// tiles count as solid so a gap right at the map edge is still bounded correctly.
bool CControls::FindNearestHoleX(float &OutX) const
{
	// Use the predicted core position (tick-quantized), not the render-smoothed LocalCharPos, so the choice
	// of gap and all the steering below are frame-rate independent.
	const vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	const int Tx0 = (int)(Pos.x / 32.0f);
	const int Ty0 = (int)(Pos.y / 32.0f);
	const int Rx = 10; // search range sideways, in tiles
	const int Up = 8, Down = 8; // search range above/below, in tiles
	const int MaxW = 2; // widest opening that still counts as a "hole"
	const int W = Collision()->GetWidth();
	const int H = Collision()->GetHeight();

	auto Solid = [&](int tx, int ty) -> bool {
		if(tx < 0 || ty < 0 || tx >= W || ty >= H)
			return true; // off the map = wall boundary, so edge gaps are still bounded
		return Collision()->CheckPoint(tx * 32.0f + 16.0f, ty * 32.0f + 16.0f);
	};

	bool Found = false;
	float BestDist = 1e30f;
	for(int ty = Ty0 - Up; ty <= Ty0 + Down; ++ty)
	{
		int col = Tx0 - Rx;
		while(col <= Tx0 + Rx)
		{
			if(Solid(col, ty)) // walls are not gaps; step past
			{
				++col;
				continue;
			}
			// Start of an open run at [Start .. End]. Extend it while still open and within the window.
			const int Start = col;
			int End = Start;
			while(End + 1 <= Tx0 + Rx && !Solid(End + 1, ty))
				++End;
			const int Len = End - Start + 1;
			// Must be pinched by a wall on both sides and no wider than the configured max to count as a hole.
			if(Len <= MaxW && Solid(Start - 1, ty) && Solid(End + 1, ty))
			{
				const float Cx = (float)(Start + End + 1) * 16.0f; // world-x of the run's center
				const float Dist = absolute(Cx - Pos.x);
				if(Dist < BestDist)
				{
					BestDist = Dist;
					OutX = Cx;
					Found = true;
				}
			}
			col = End + 1;
		}
	}
	return Found;
}

// TClient hole assist: steer left/right/stop so the tee comes to rest centered on the nearest gap.
// Counter-strafe controller: while we'd still overshoot the gap at the current speed, brake against our
// motion; otherwise drive toward it. Everything runs on the predicted core position/velocity (tick-quantized)
// so it behaves identically at any frame rate. A small dead zone plus a settle latch stop it from jittering
// by a pixel once parked, without the passive-coast prediction that misjudged airborne moves.
// Only ever touches m_Direction; jump/hook stay yours.
void CControls::ApplyHoleAssist()
{
	const int Dummy = g_Config.m_ClDummy;

	float TargetX;
	if(!FindNearestHoleX(TargetX))
	{
		m_aHoleSettled[Dummy] = false;
		return;
	}

	const CCharacterCore &Core = GameClient()->m_PredictedChar;
	const float Px = Core.m_Pos.x;
	const float Vx = Core.m_Vel.x;
	const float Err = TargetX - Px; // + = gap is to our right
	const float Lock = 3.0f; // dead zone half-width around the gap center, in pixels
	const float Reengage = 10.0f; // must drift this far off-center before we start correcting again
	const float VelEps = 0.4f;
	// Braking distance grows LINEARLY with speed (ground friction decays velocity geometrically, so the real
	// stop distance is ~proportional to speed, not speed^2). tc_hole_assist_brake (tenths) is the proportion:
	// 10 => brake when the gap is closer than 1.0x our current speed. Lower brakes later / carries more speed.
	const float StopDist = ((float)g_Config.m_TcHoleAssistBrake / 10.0f) * absolute(Vx);

	int &Dir = m_aInputData[Dummy].m_Direction;

	// Parked on the gap: hold still until we drift well off center, so we don't micro-correct every tick.
	if(m_aHoleSettled[Dummy])
	{
		if(absolute(Err) > Reengage)
			m_aHoleSettled[Dummy] = false;
		else
		{
			Dir = 0;
			return;
		}
	}

	// Centered and nearly stopped -> lock in and stop steering.
	if(absolute(Err) <= Lock && absolute(Vx) <= VelEps)
	{
		Dir = 0;
		m_aHoleSettled[Dummy] = true;
	}
	else if(Err > 0.0f)
	{
		// Gap to the right: brake if we're heading right fast enough to overshoot it, else drive right.
		Dir = (Vx > VelEps && StopDist >= Err) ? -1 : 1;
	}
	else
	{
		// Gap to the left: brake if we're heading left fast enough to overshoot it, else drive left.
		Dir = (Vx < -VelEps && StopDist >= -Err) ? 1 : -1;
	}
}

// TClient: rocket (grenade) counter. Independent of the braking anti-void (tc_anti_void): if we carry
// the grenade launcher and are flying into the void, auto-fire a rocket along our movement direction so
// the explosion sits between us and the void and knocks us straight back. Works even when the basic
// anti-void is turned off. Each shot is a clean single press, rate-limited by a cooldown.
void CControls::ApplyAntiVoidRocket(bool Suppressed)
{
	const int Dummy = g_Config.m_ClDummy;
	const vec2 CharPos = LocalCharPos();
	const float R = 28.0f; // tee half-size

	// First, release the fire press we made on the previous tick. Without this the fire bit stays "held",
	// which on a full-auto-grenade server keeps firing forever even after we have left the void. We only
	// release our OWN press (the value is unchanged), so a manual click in between is left untouched.
	if(m_aAntiVoidRocketReleasePending[Dummy])
	{
		m_aAntiVoidRocketReleasePending[Dummy] = false;
		if(!m_aInputFirePressed[Dummy] && (m_aInputData[Dummy].m_Fire & 1) != 0 && m_aInputData[Dummy].m_Fire == m_aAntiVoidRocketFireValue[Dummy])
			m_aInputData[Dummy].m_Fire++;
	}

	if(m_aAntiVoidRocketCooldown[Dummy] > 0)
		m_aAntiVoidRocketCooldown[Dummy]--;

	// Suppressed by the balancer: the essential upkeep above (releasing our held fire press and ticking the
	// cooldown) is done; don't scan for void, arm the grenade or fire while the balancer is holding us.
	if(Suppressed)
		return;

	const bool HaveGrenade = GameClient()->m_PredictedChar.m_aWeapons[WEAPON_GRENADE].m_Got &&
		GameClient()->m_PredictedChar.m_aWeapons[WEAPON_GRENADE].m_Ammo != 0;

	const float FireDist = (float)g_Config.m_TcAntiVoidRocketDistance / 100.0f; // stored in hundredths of a pixel, so the timing can be set right down to the edge
	// Keep both plans. Which one rocket evaluates depends on where the danger is: below uses the raw
	// player path (rocket-first), while side/ceiling danger uses avoid's prepared path (avoid-first).
	const CNetObj_PlayerInput OriginalInput = GameClient()->m_AvoidFreeze.HasInputBeforeOverride() ?
		GameClient()->m_AvoidFreeze.InputBeforeOverride() : m_aInputData[Dummy];

	// Where is the tee actually heading? Predict the real trajectory with the current input and find the
	// first place it would touch danger. This is what fixes the inertia case: even when we fly fast
	// sideways, the hook curves the path up into a ceiling freeze, and that is the danger we must answer.
	vec2 DangerPos(0.0f, 0.0f);
	int DangerTick = -1;
	float DangerDist = 1e9f;
	if(HaveGrenade)
	{
		const int PathTicks = 20;
		CCharacterCore SimCore = GameClient()->m_PredictedChar;
		SimCore.Init(nullptr, Collision());
		SimCore.SetHookedPlayer(-1);
		for(int t = 1; t <= PathTicks && DangerTick < 0; ++t)
		{
			SimCore.m_Input = OriginalInput;
			const vec2 Prev = SimCore.m_Pos;
			SimCore.Tick(true);
			SimCore.Move();
			SimCore.Quantize();
			const int Steps = maximum(1, (int)(distance(Prev, SimCore.m_Pos) / 4.0f));
			for(int s = 1; s <= Steps; ++s)
			{
				const vec2 Pt = mix(Prev, SimCore.m_Pos, (float)s / (float)Steps);
				if(AvoidDangerClass(Pt.x, Pt.y) >= 1)
				{
					DangerTick = t;
					DangerPos = Pt;
					DangerDist = distance(CharPos, Pt);
					break;
				}
			}
		}
	}

	// How early the grenade may be taken depends on how fast we are moving: at high speed a fixed
	// 5-tick window is not enough to get the grenade in hand and land the blast before the danger
	// arrives. The lead grows with the distance actually covered in the next few ticks, capped so a
	// slow player still gets the grenade only at the last moment (no early weapon interference).
	const vec2 Vel = GameClient()->m_PredictedChar.m_Vel;
	const float Speed = length(Vel);
	const float LeadDist = std::clamp(Speed * 8.0f, 64.0f, 320.0f);

	// Take the grenade at the last moment by default, earlier only when the current speed demands it.
	// Fire a bit earlier at high speed: the grenade needs time to fly and the blast to land before the
	// danger arrives, so the distance trigger grows with the distance covered in the next ticks. The
	// minimum lead keeps the last moment from being too late even at low speed (the logs showed the
	// rocket firing at the same tick the freeze already happened).
	const float FireLead = std::clamp(Speed * 8.0f, 64.0f, 300.0f);

	// Avoid still runs before us so its movement/hook correction can be included in the rocket simulation,
	// but it no longer vetoes a shot. Rocket is the primary save: avoid remains active as a simultaneous
	// movement fallback and is the sole fallback when no useful rocket exists.
	const bool AvoidSaves = GameClient()->m_AvoidFreeze.WouldSave();
	// ...but when avoid reports that NOTHING survives, the rocket is the last resort: fire as soon as
	// the grenade is ready and BestAim has a valid detonation, even if the rocket's own path prediction
	// does not see the danger (avoid's model and the core simulation sometimes disagree).
	const bool AvoidNoSolution = GameClient()->m_AvoidFreeze.NoSolution();
	// avoid may see the danger while our own core prediction does not (different models). Merge its
	// tick into the time gates; the distance gates still use our own path.
	const int BafDangerTick = GameClient()->m_AvoidFreeze.DangerTick();
	const int EffectiveDangerTick = DangerTick >= 0 ? (BafDangerTick > 0 ? minimum(DangerTick, BafDangerTick) : DangerTick) : BafDangerTick;

	// When only avoid sees the danger, our own path has no distance to gate on (DangerDist stays 1e9),
	// so take the grenade early for the weapon switch. The actual fire moment is decided by the chosen
	// shot's own flight time further down, not by a fixed lead.
	const float AvoidLeadTicks = std::clamp((R + FireDist + FireLead) / maximum(Speed, 4.0f), 6.0f, 14.0f);
	const bool AvoidOnlyDanger = DangerTick < 0 && BafDangerTick > 0;
	const bool AvoidDangerInArm = AvoidOnlyDanger && (float)EffectiveDangerTick <= AvoidLeadTicks + 3.0f;
	const bool AvoidDangerInFire = AvoidOnlyDanger && (float)EffectiveDangerTick <= AvoidLeadTicks;
	// Teeworlds Y grows downward. Only a predominantly downward impact is "freeze below" and grants
	// rocket maximum priority. A diagonal whose horizontal component dominates is a side impact and
	// belongs to avoid. If only avoid's full-world model sees danger, use velocity as a conservative
	// fallback; ambiguous cases deliberately stay avoid-first.
	const vec2 DangerDelta = DangerPos - CharPos;
	const bool DangerBelow = DangerTick >= 0 ?
		(DangerDelta.y > 0.0f && DangerDelta.y >= absolute(DangerDelta.x)) :
		(AvoidOnlyDanger && Vel.y > 0.0f && Vel.y >= absolute(Vel.x));
	const bool SmartDirectionalPriority = g_Config.m_TcAntiVoidRocketSmartPriority != 0;
	const CNetObj_PlayerInput RocketPlanInput = !SmartDirectionalPriority || DangerBelow ? OriginalInput : m_aInputData[Dummy];

	const bool DangerInArm = (EffectiveDangerTick > 0 && (EffectiveDangerTick <= 12 || DangerDist <= R + FireDist + LeadDist)) || AvoidNoSolution || AvoidDangerInArm;
	const bool DangerInFire = (EffectiveDangerTick > 0 && (DangerDist <= R + FireDist + FireLead || EffectiveDangerTick <= 2)) || AvoidNoSolution || AvoidDangerInFire;
	// Genuinely about to be hit and nothing else saves us: ignore the flight cooldown and fire whatever
	// we can. The extra blast can only add velocity; waiting for the previous one is pointless here.
	const bool Emergency = AvoidNoSolution && EffectiveDangerTick > 0 && EffectiveDangerTick <= 2;

	// A rocket is only worth taking when there is a solid surface to detonate against within blast
	// reach. 106px is where the explosion still gives a kick worth having (force falls off with
	// distance; beyond that BestAim rejects the shot, which used to leave us holding a useless
	// grenade). A freeze with nothing solid around it would just swallow the grenade, so the weapon
	// is not even taken in that case. BestAim does the precise check at fire time.
	const bool RocketPolicyAllows = !SmartDirectionalPriority || DangerBelow || !AvoidSaves;
	bool SolidWithinBlast = false;
	for(int i = 0; DangerInArm && RocketPolicyAllows && i < 16 && !SolidWithinBlast; i++)
	{
		const vec2 Dir = direction((float)i / 16.0f * 2.0f * pi);
		vec2 Hit;
		if(Collision()->IntersectLine(CharPos + Dir * R, CharPos + Dir * 106.0f, &Hit, nullptr))
			SolidWithinBlast = true;
	}

	// In optional smart-priority mode, floor freeze keeps rocket primary while side/ceiling freeze
	// lets avoid go first. With the option off, preserve the current aggressive rocket-first behavior.
	const bool NeedRocket = DangerInArm && SolidWithinBlast && RocketPolicyAllows;

	// Active weapon alone does not mean it can fire. The old counter emitted a new "shot" every other
	// tick while the server was still rejecting them for reload, then suppressed avoid as if those
	// phantom grenades existed. Read the same predicted reload timer FireWeapon uses.
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	const CCharacter *pPredictedCharacter = LocalId >= 0 ? GameClient()->m_PredictedWorld.GetCharacterById(LocalId) : nullptr;
	const int GrenadeReload = pPredictedCharacter ? pPredictedCharacter->GetReloadTimer() : 0;
	const bool GrenadeReady = GameClient()->m_PredictedChar.m_ActiveWeapon == WEAPON_GRENADE && GrenadeReload == 0;

	// Debug log (tc_anti_void_rocket_debug), tag "rocket": level 1 prints state changes, level 2 every tick.
	const int RocketTick = Client()->PredGameTick(Dummy);

	// Remember this tick's evaluation for the OUTCOME lines.
	CAntiVoidRocketDiag &Diag = m_aAntiVoidRocketDiag[Dummy];
	Diag.m_DangerTick = DangerTick;
	Diag.m_DangerDist = DangerDist;
	Diag.m_Solid = SolidWithinBlast;
	Diag.m_AvoidSaves = AvoidSaves;
	Diag.m_AvoidNoSolution = AvoidNoSolution;
	Diag.m_NeedRocket = NeedRocket;

	auto LogRocket = [&](int State, const char *pText) {
		if(g_Config.m_TcAntiVoidRocketDebug == 0)
			return;
		if(State == m_aAntiVoidRocketLogState[Dummy] && g_Config.m_TcAntiVoidRocketDebug < 2)
			return;
		m_aAntiVoidRocketLogState[Dummy] = State;
		log_info("rocket", "tick=%d state=%d %s pos=(%.0f,%.0f) vel=(%.0f,%.0f) danger[tick=%d dist=%.1f bafTick=%d below=%d] speed=%.1f armLead=%.0f fireLead=%.0f solid=%d avoidSaves=%d avoidNoSolution=%d grenade=%d ready=%d reload=%d cooldown=%d",
			RocketTick, State, pText, CharPos.x, CharPos.y, Vel.x, Vel.y, DangerTick, DangerDist, BafDangerTick, DangerBelow ? 1 : 0,
			Speed, LeadDist, FireLead, SolidWithinBlast ? 1 : 0, AvoidSaves ? 1 : 0, AvoidNoSolution ? 1 : 0,
			HaveGrenade ? 1 : 0, GrenadeReady ? 1 : 0, GrenadeReload, m_aAntiVoidRocketCooldown[Dummy]);
	};

	// Weapon binds are authoritative. Once the player changes weapon during a rocket-save window,
	// leave both their direct slot request and next/previous counters untouched until that danger
	// has passed. Without this latch the counter wrote grenade here on every tick, making switching
	// weapons impossible; merely dropping PrevWeapon was insufficient because it immediately re-armed.
	if(m_aAntiVoidRocketManualWeapon[Dummy])
	{
		m_aAntiVoidRocketPrevWeapon[Dummy] = -1;
		if(DangerInArm)
		{
			LogRocket(8, "manual weapon input: rocket weapon override suspended until danger clears");
			return;
		}
		m_aAntiVoidRocketManualWeapon[Dummy] = false;
	}

	if(DangerInArm)
	{
		if(SmartDirectionalPriority && !DangerBelow && AvoidSaves)
			LogRocket(1, "avoid-first: side/ceiling danger is already handled");
		else if(!SolidWithinBlast)
			LogRocket(2, "skip: no solid surface within blast reach");
	}
	else if(m_aAntiVoidRocketLogState[Dummy] > 0)
	{
		LogRocket(0, "idle: no danger on the predicted path");
	}

	if(NeedRocket && (m_aAntiVoidRocketCooldown[Dummy] == 0 || Emergency))
	{
		// Remember the weapon we had before the save (recorded once, kept across multiple rockets).
		const bool FirstArm = m_aAntiVoidRocketPrevWeapon[Dummy] < 0;
		if(FirstArm)
		{
			m_aAntiVoidRocketPrevWeapon[Dummy] = GameClient()->m_PredictedChar.m_ActiveWeapon;
			// Log the arm only once per save: re-arming every tick while holding for the fire moment
			// would otherwise alternate arm/hold lines and flood the log.
			LogRocket(3, Emergency ? "arm: taking the grenade (emergency, cooldown ignored)" : "arm: taking the grenade");
		}

		// Arm: switch to the grenade now so it is ready by the fire moment.
		m_aInputData[Dummy].m_WantedWeapon = WEAPON_GRENADE + 1;
		if(GameClient()->m_PredictedChar.m_ActiveWeapon == WEAPON_GRENADE && GrenadeReload > 0)
			LogRocket(11, "hold: grenade is reloading, avoid remains authoritative");

		// Fire once the grenade is in hand and either the usual gate is met or avoid has no solution at
		// all (last resort). One clean press (released next tick).
		if(GrenadeReady && (DangerInFire || (AvoidNoSolution && DangerInArm)) && (m_aInputData[Dummy].m_Fire & 1) == 0)
		{
			// WHERE to fire: fly the grenade in every direction with the real projectile maths, detonate it
			// on the real surface, apply the real explosion force and run the tee forward. The direction that
			// keeps us out of freeze the longest wins, so the blast throws us straight away from the danger
			// (a ceiling freeze gets an upward shot that pushes down) instead of along our sideways inertia.
			vec2 FireDir = DangerPos - CharPos;
			if(length(FireDir) > 0.001f)
				FireDir = normalize(FireDir);
			else if(Speed > 0.001f)
				FireDir = Vel / Speed; // avoid says "no solution" but our path saw no danger: fall back to motion
			else
				FireDir = vec2(1.0f, 0.0f);

			const int TuneZone = Collision()->IsTune(Collision()->GetMapIndex(CharPos));
			const CTuningParams *pTuning = GameClient()->GetTuning(TuneZone);
			CRocketSaveCfg Cfg;
			if(pTuning)
			{
				Cfg.m_Curvature = pTuning->m_GrenadeCurvature;
				Cfg.m_Speed = pTuning->m_GrenadeSpeed;
				Cfg.m_Lifetime = pTuning->m_GrenadeLifetime;
				Cfg.m_ExplosionStrength = pTuning->m_ExplosionStrength;
			}
			Cfg.m_Freeze = g_Config.m_TcAntiVoidFreeze != 0;
			Cfg.m_DeepFreeze = g_Config.m_TcAntiVoidDeepFreeze != 0;
			Cfg.m_LiveFreeze = g_Config.m_TcAntiVoidLiveFreeze != 0;
			Cfg.m_Death = g_Config.m_TcAntiVoidDeath != 0;

			// Profiling: the aim search flies 32 grenades and runs the tee forward for the good ones;
			// report it when it is slow enough to be felt.
			const int64_t ProfAim = time_get();
			const CRocketSaveAim RS = CRocketSave::BestAim(Collision(), GameClient()->m_PredictedChar, RocketPlanInput, Cfg, FireDir);
			if(g_Config.m_TcAntiVoidRocketDebug >= 1)
			{
				const float AimMs = (float)(time_get() - ProfAim) * 1000.0f / (float)time_freq();
				if(AimMs >= 2.0f)
					log_info("rocket", "PROFILE: BestAim took %.1f ms", AimMs);
			}
			// Fire early enough that the explosion exists before the predicted freeze. The previous gate
			// did the reverse: it waited until dangerTick <= flight+2, which routinely sent a one-tick
			// grenade one tick before freeze. Start the burst up to six ticks before the latest safe
			// moment, then keep firing at the configured cadence while danger persists.
			const int FlightTicks = (int)std::ceil(RS.m_BlastTicks);
			const bool FireWindow = EffectiveDangerTick > 0 && EffectiveDangerTick <= FlightTicks + 6;
			const bool CanLandSafely = EffectiveDangerTick > FlightTicks + 1;
			const bool LastChance = EffectiveDangerTick > 0 && !CanLandSafely;
			const bool PositiveEscapeKick = RS.m_EscapeKick > 1.0f;
			const bool ShouldFire = RS.m_Found && PositiveEscapeKick &&
				((RS.m_Improves && FireWindow && (CanLandSafely || LastChance)) ||
					Emergency);
			if(ShouldFire)
			{
				LogRocket(5, "FIRE rocket");
				const bool PlayerHookUncertain = RocketPlanInput.m_Hook != 0 &&
					(GameClient()->m_PredictedChar.HookedPlayer() >= 0 || GameClient()->m_PredictedChar.m_HookState == HOOK_FLYING);
				const bool RocketAloneSaves = (!SmartDirectionalPriority || DangerBelow) && RS.m_Improves &&
					RS.m_Score >= (float)CRocketSave::ms_Tuning.m_Horizon && !PlayerHookUncertain;
				if(g_Config.m_TcAntiVoidRocketDebug >= 1)
					log_info("rocket", "  mode=%s aim=(%.2f,%.2f) blast=(%.0f,%.0f) kick=%.1f escapeKick=%.1f flight=%.0f score=%.1f base=%.1f plain=%.1f",
						RocketAloneSaves ? "rocket-first" : "rocket+avoid", RS.m_Dir.x, RS.m_Dir.y, RS.m_Blast.x, RS.m_Blast.y,
						RS.m_Kick, RS.m_EscapeKick, RS.m_BlastTicks, RS.m_Score, RS.m_BaseScore, RS.m_PlainScore);

				// A full-window rocket plan is authoritative: discard avoid's movement/hook change for
				// this packet. A partial plan deliberately keeps them, producing rocket+avoid. Every next
				// tick is evaluated afresh, so avoid immediately takes over if the explosion was not enough.
				if(RocketAloneSaves)
				{
					m_aInputData[Dummy].m_Direction = OriginalInput.m_Direction;
					m_aInputData[Dummy].m_Jump = OriginalInput.m_Jump;
					m_aInputData[Dummy].m_Hook = OriginalInput.m_Hook;
					GameClient()->m_AvoidFreeze.DiscardOverrideForRocket();
				}
				// Never launch a new hook along the temporary rocket aim. Existing attached/flying hooks
				// are left alone and can combine with the blast.
				if(m_aInputData[Dummy].m_Hook != 0 && GameClient()->m_PredictedChar.m_HookState == HOOK_IDLE)
					m_aInputData[Dummy].m_Hook = 0;
				m_AvoidAimActive = false;

				const vec2 Aim = normalize(RS.m_Dir) * 100.0f;
				m_aInputData[Dummy].m_TargetX = (int)Aim.x;
				m_aInputData[Dummy].m_TargetY = (int)Aim.y;
				if(!m_aInputData[Dummy].m_TargetX && !m_aInputData[Dummy].m_TargetY)
					m_aInputData[Dummy].m_TargetX = 1;
				m_aInputData[Dummy].m_Fire++; // even -> odd = one press
				m_aAntiVoidRocketFireValue[Dummy] = m_aInputData[Dummy].m_Fire;
				m_aAntiVoidRocketReleasePending[Dummy] = true;
				m_aAntiVoidRocketLastFireTick[Dummy] = RocketTick;

				// Honour the user's cadence. Multiple grenades in flight are intentional here: the next
				// shot is re-simulated from the newest predicted state and gives the requested aggressive
				// rocket-first behaviour instead of silently stretching a 1-tick cooldown to flight time.
				m_aAntiVoidRocketCooldown[Dummy] = g_Config.m_TcAntiVoidRocketCooldown;
			}
			else if(!RS.m_Found)
			{
				LogRocket(4, "skip: no usable detonation before danger");
			}
			else if(!PositiveEscapeKick)
			{
				LogRocket(10, "skip: blast impulse points toward danger");
			}
			else if(!RS.m_Improves)
			{
				LogRocket(7, "skip: no shot beats doing nothing");
			}
			else
			{
				// The shot is good but the danger is still too far away for its flight time: hold the
				// grenade and wait for the moment instead of firing at a spot the player may not even
				// fall from.
				LogRocket(6, "hold: shot ready, waiting for the fire moment");
			}
		}
	}
	else if(m_aAntiVoidRocketPrevWeapon[Dummy] >= 0 && !NeedRocket && !m_aAntiVoidRocketReleasePending[Dummy])
	{
		LogRocket(0, "restore: switching back to the previous weapon");
		// Save is over (or never needed): take the grenade back out and switch to the weapon we had
		// before the rocket(s), so the grenade doesn't linger in our hands. Keep requesting it until
		// the switch is confirmed.
		const int Prev = m_aAntiVoidRocketPrevWeapon[Dummy];
		m_aInputData[Dummy].m_WantedWeapon = Prev + 1;
		if(GameClient()->m_PredictedChar.m_ActiveWeapon == Prev)
			m_aAntiVoidRocketPrevWeapon[Dummy] = -1; // restored
	}
}

// TClient laser self-ricochet: trace laser bounces through the world matching CLaser::DoBounce on server.
int CControls::TraceLaserPath(vec2 From, vec2 AimDir, int MaxBounces, vec2 *pSegStart, vec2 *pSegEnd) const
{
	const int TuneZone = Collision()->IsTune(Collision()->GetMapIndex(From));
	const CTuningParams *pTuning = GameClient()->GetTuning(TuneZone);
	const float LaserReach = pTuning ? (float)pTuning->m_LaserReach : 800.0f;
	const float LaserBounceCost = pTuning ? (float)pTuning->m_LaserBounceCost : 0.0f;

	vec2 Pos = From;
	vec2 Dir = normalize(AimDir);
	float Energy = LaserReach;
	int Segments = 0;

	for(int k = 0; k <= MaxBounces; ++k)
	{
		vec2 To = Pos + Dir * Energy;
		vec2 Coltile;
		int TeleNr = 0;
		const int Res = Collision()->IntersectLineTeleWeapon(Pos, To, &Coltile, &To, &TeleNr);

		pSegStart[Segments] = Pos;
		pSegEnd[Segments] = To;
		Segments++;

		if(!Res)
			break;

		vec2 TempPos = To;
		vec2 TempDir = Dir * 4.0f;
		int DoorTile = 0;
		if(Res == -1)
		{
			DoorTile = Collision()->GetTile(round_to_int(Coltile.x), round_to_int(Coltile.y));
			Collision()->SetCollisionAt(round_to_int(Coltile.x), round_to_int(Coltile.y), TILE_SOLID);
		}
		Collision()->MovePoint(&TempPos, &TempDir, 1.0f, nullptr);
		if(Res == -1)
			Collision()->SetCollisionAt(round_to_int(Coltile.x), round_to_int(Coltile.y), DoorTile);

		if(length_squared(TempDir) < 0.001f)
			break;

		Energy -= distance(Pos, TempPos) + LaserBounceCost;
		if(Energy <= 0.0f)
			break;

		Dir = normalize(TempDir);
		Pos = TempPos;
	}
	return Segments;
}

// TClient laser self-ricochet: find the best wall bounce path to unfreeze the tee in safe air.
bool CControls::FindLaserSelfBounce(const vec2 *pTargetPos, const bool *pValid, int MaxBounces, int BounceDelayTicks,
	vec2 &OutAimDir, float &OutDistToWall, vec2 &OutBouncePos, vec2 &OutReflDir, float &OutTeeHitOffset, int &OutBounces, int &OutArrivalTicks) const
{
	const vec2 CharPos = LocalCharPos();
	const float R = 28.0f; // Tee radius

	struct CLaserCandidate
	{
		float m_Angle;
		vec2 m_AimDir;
		float m_DistToWall;
		vec2 m_BouncePos;
		vec2 m_ReflDir;
		float m_TeeHitOffset;
		int m_Bounces;
		int m_ArrivalTicks;
		vec2 m_TargetPos;
	};

	std::vector<CLaserCandidate> vCandidates;
	std::vector<vec2> vSegStart(MaxBounces + 2);
	std::vector<vec2> vSegEnd(MaxBounces + 2);

	const int NumRays = 720; // 0.5 degree angular resolution
	for(int i = 0; i < NumRays; ++i)
	{
		const float Angle = (float)i / (float)NumRays * 2.0f * pi;
		const vec2 Dir0 = vec2(cos(Angle), sin(Angle));

		const int Segments = TraceLaserPath(CharPos, Dir0, MaxBounces, vSegStart.data(), vSegEnd.data());
		if(Segments < 2)
			continue;

		const float Dist0 = distance(CharPos, vSegEnd[0]);

		for(int k = 1; k < Segments && k <= MaxBounces; ++k)
		{
			if(!pValid[k])
				continue;

			const vec2 Target = pTargetPos[k];
			vec2 ClosestPt(0.0f, 0.0f);
			if(!closest_point_on_line(vSegStart[k], vSegEnd[k], Target, ClosestPt))
				continue;

			const float Offset = distance(Target, ClosestPt);
			if(Offset >= R)
				continue;

			// Check if any earlier leg j < k would intercept the tee before safe air
			bool EarlyIntercept = false;
			for(int j = 1; j < k; ++j)
			{
				vec2 EarlyPt(0.0f, 0.0f);
				if(closest_point_on_line(vSegStart[j], vSegEnd[j], pTargetPos[j], EarlyPt))
				{
					if(distance(pTargetPos[j], EarlyPt) < R)
					{
						EarlyIntercept = true;
						break;
					}
				}
			}
			if(EarlyIntercept)
				continue;

			vCandidates.push_back({
				Angle,
				Dir0,
				Dist0,
				vSegStart[k],
				normalize(vSegEnd[k] - vSegStart[k]),
				Offset,
				k,
				k * BounceDelayTicks,
				Target
			});
		}
	}

	if(vCandidates.empty())
	{
		if(g_Config.m_TcAntiVoidLaserDebug >= 2)
			log_info("laser_ricochet", "scan %d rays: no bounce path reaches safe-air targets (MaxBounces=%d)", NumRays, MaxBounces);
		return false;
	}

	// Select best candidate: prefer fewer bounces, then smallest offset to center, then shortest distance to wall
	std::sort(vCandidates.begin(), vCandidates.end(), [](const CLaserCandidate &a, const CLaserCandidate &b) {
		if(a.m_Bounces != b.m_Bounces)
			return a.m_Bounces < b.m_Bounces;
		if(std::abs(a.m_TeeHitOffset - b.m_TeeHitOffset) > 1.0f)
			return a.m_TeeHitOffset < b.m_TeeHitOffset;
		return a.m_DistToWall < b.m_DistToWall;
	});

	CLaserCandidate Best = vCandidates[0];

	// Sub-degree refinement (±0.5° with 0.02° steps)
	const float DegToRad = pi / 180.0f;
	float BestFineOffset = Best.m_TeeHitOffset;
	vec2 BestFineDir = Best.m_AimDir;
	vec2 BestFineBounce = Best.m_BouncePos;
	vec2 BestFineRefl = Best.m_ReflDir;
	float BestFineDist = Best.m_DistToWall;

	for(int step = -25; step <= 25; ++step)
	{
		const float FineAngle = Best.m_Angle + (float)step * 0.02f * DegToRad;
		const vec2 FineDir0 = vec2(cos(FineAngle), sin(FineAngle));

		const int Segs = TraceLaserPath(CharPos, FineDir0, Best.m_Bounces, vSegStart.data(), vSegEnd.data());
		if(Segs <= Best.m_Bounces)
			continue;

		const int k = Best.m_Bounces;
		vec2 ClosestPt(0.0f, 0.0f);
		if(closest_point_on_line(vSegStart[k], vSegEnd[k], Best.m_TargetPos, ClosestPt))
		{
			const float Offset = distance(Best.m_TargetPos, ClosestPt);
			if(Offset < BestFineOffset)
			{
				BestFineOffset = Offset;
				BestFineDir = FineDir0;
				BestFineBounce = vSegStart[k];
				BestFineRefl = normalize(vSegEnd[k] - vSegStart[k]);
				BestFineDist = distance(CharPos, vSegEnd[0]);
			}
		}
	}

	Best.m_AimDir = BestFineDir;
	Best.m_BouncePos = BestFineBounce;
	Best.m_ReflDir = BestFineRefl;
	Best.m_DistToWall = BestFineDist;
	Best.m_TeeHitOffset = BestFineOffset;

	OutAimDir = Best.m_AimDir;
	OutDistToWall = Best.m_DistToWall;
	OutBouncePos = Best.m_BouncePos;
	OutReflDir = Best.m_ReflDir;
	OutTeeHitOffset = Best.m_TeeHitOffset;
	OutBounces = Best.m_Bounces;
	OutArrivalTicks = Best.m_ArrivalTicks;

	if(g_Config.m_TcAntiVoidLaserDebug >= 1)
	{
		const float AngleDeg = std::atan2(Best.m_AimDir.y, Best.m_AimDir.x) * 180.0f / pi;
		log_info("laser_ricochet", "found %d candidates | best: wall=(%.0f, %.0f) dist=%.1fpx angle=%.1f° [bounces=%d] -> target_pos=(%.0f, %.0f) in %d ticks, hit_offset=%.2fpx",
			(int)vCandidates.size(), Best.m_BouncePos.x, Best.m_BouncePos.y, Best.m_DistToWall, AngleDeg, Best.m_Bounces, Best.m_TargetPos.x, Best.m_TargetPos.y, Best.m_ArrivalTicks, Best.m_TeeHitOffset);
	}
	return true;
}

// Check if the full tee hitbox has zero intersection with ANY freeze/death tiles.
bool CControls::TeeFullyClearOfFreeze(vec2 Pos) const
{
	const float R = 22.0f; // Tee radius buffer
	auto IsDangerousTile = [&](float x, float y) -> bool {
		const int Tx = (int)std::floor(x / 32.0f);
		const int Ty = (int)std::floor(y / 32.0f);
		if(Tx < 0 || Ty < 0 || Tx >= Collision()->GetWidth() || Ty >= Collision()->GetHeight())
			return true; // off map
		const int Index = Ty * Collision()->GetWidth() + Tx;
		if(Index < 0 || Index >= Collision()->GetWidth() * Collision()->GetHeight())
			return true;
		const int aTiles[] = {
			Collision()->GetTileIndex(Index),
			Collision()->GetFrontTileIndex(Index),
			Collision()->GetSwitchType(Index)
		};
		for(const int T : aTiles)
		{
			if(T == TILE_DEATH || T == TILE_FREEZE || T == TILE_DFREEZE || T == TILE_LFREEZE)
				return true;
		}
		return false;
	};

	if(IsDangerousTile(Pos.x, Pos.y))
		return false;
	for(int i = 0; i < 12; ++i)
	{
		const float A = (float)i / 12.0f * 2.0f * pi;
		const float Px = Pos.x + cos(A) * R;
		const float Py = Pos.y + sin(A) * R;
		if(IsDangerousTile(Px, Py))
			return false;
	}
	return true;
}

// Laser self-ricochet counter: fires laser at the nearest wall so it ricochets back into the player as they emerge from freeze.
void CControls::ApplyAntiVoidLaser(bool Suppressed)
{
	const int Dummy = g_Config.m_ClDummy;
	const vec2 CharPos = LocalCharPos();
	const int CurrentTick = Client()->PredGameTick(Dummy);

	// First, release the fire press we made on the previous tick.
	if(m_aAntiVoidLaserReleasePending[Dummy])
	{
		m_aAntiVoidLaserReleasePending[Dummy] = false;
		if((m_aInputData[Dummy].m_Fire & 1) != 0 && m_aInputData[Dummy].m_Fire == m_aAntiVoidLaserFireValue[Dummy])
		{
			m_aInputData[Dummy].m_Fire++;
			if(g_Config.m_TcAntiVoidLaserDebug >= 2)
				log_info("laser_ricochet", "fire released on tick %d", CurrentTick);
		}
	}

	if(m_aAntiVoidLaserCooldown[Dummy] > 0)
		m_aAntiVoidLaserCooldown[Dummy]--;

	// In-flight rescue laser tracking and verification
	if(m_aLaserTracker[Dummy].m_Active)
	{
		if(CurrentTick >= m_aLaserTracker[Dummy].m_ArrivalTick)
		{
			const vec2 RealPos = LocalCharPos();
			const vec2 RayStart = m_aLaserTracker[Dummy].m_WallPos;
			const vec2 RayEnd = RayStart + m_aLaserTracker[Dummy].m_ReflDir * 800.0f;
			vec2 ClosestOnRay(0.0f, 0.0f);
			closest_point_on_line(RayStart, RayEnd, RealPos, ClosestOnRay);
			const float RealRayDist = distance(RealPos, ClosestOnRay);
			const float PredDrift = distance(RealPos, m_aLaserTracker[Dummy].m_TargetPos);
			const bool Hit = (RealRayDist < 28.0f);
			const int LocalId = GameClient()->m_Snap.m_LocalClientId;
			const bool FrozenNow = (LocalId >= 0 && GameClient()->m_aClients[LocalId].m_Predicted.m_FreezeEnd != 0);

			if(g_Config.m_TcAntiVoidLaserDebug >= 1)
			{
				log_info("laser_ricochet", "==>> ARRIVAL TICK %d: real_pos=(%.1f, %.1f) pred_pos=(%.1f, %.1f) drift=%.1fpx | ray_dist=%.2fpx (%s, R=28px) | frozen=%d",
					CurrentTick, RealPos.x, RealPos.y, m_aLaserTracker[Dummy].m_TargetPos.x, m_aLaserTracker[Dummy].m_TargetPos.y, PredDrift,
					RealRayDist, Hit ? "HIT" : "MISS", FrozenNow ? 1 : 0);

				if(!Hit)
				{
					log_info("laser_ricochet", "     MISS DETAILS: ray passed at (%.1f, %.1f), player is at (%.1f, %.1f) (drift=%.1fpx)",
						ClosestOnRay.x, ClosestOnRay.y, RealPos.x, RealPos.y, PredDrift);
				}
			}

			m_aLaserTracker[Dummy].m_Active = false;
		}
		else if(CurrentTick > m_aLaserTracker[Dummy].m_ArrivalTick + 15)
		{
			m_aLaserTracker[Dummy].m_Active = false;
		}
	}

	if(Suppressed)
		return;

	const bool HaveLaser = GameClient()->m_PredictedChar.m_aWeapons[WEAPON_LASER].m_Got &&
		GameClient()->m_PredictedChar.m_aWeapons[WEAPON_LASER].m_Ammo != 0;

	if(!HaveLaser)
	{
		if(m_aAntiVoidLaserPrevWeapon[Dummy] >= 0 && !m_aAntiVoidLaserReleasePending[Dummy])
		{
			const int Prev = m_aAntiVoidLaserPrevWeapon[Dummy];
			if(Prev >= 0 && Prev != WEAPON_LASER)
			{
				m_aInputData[Dummy].m_WantedWeapon = Prev + 1;
				if(GameClient()->m_PredictedChar.m_ActiveWeapon == Prev)
					m_aAntiVoidLaserPrevWeapon[Dummy] = -1;
			}
			else
			{
				m_aAntiVoidLaserPrevWeapon[Dummy] = -1;
			}
		}
		return;
	}

	// If player manually selected a weapon, never fight their choice
	const int UserWanted = m_aInputData[Dummy].m_WantedWeapon;
	if(UserWanted != 0 && UserWanted != WEAPON_LASER + 1)
	{
		m_aAntiVoidLaserPrevWeapon[Dummy] = -1;
	}

	// Get laser bounce delay to calculate ideal rescue timing
	const int TuneZone = Collision()->IsTune(Collision()->GetMapIndex(CharPos));
	const CTuningParams *pTuning = GameClient()->GetTuning(TuneZone);
	const float LaserBounceDelay = pTuning ? (float)pTuning->m_LaserBounceDelay : 150.0f;
	const int BounceDelayTicks = maximum(1, (int)(50.0f * LaserBounceDelay / 1000.0f) + 1);
	const int LaserBounceNum = pTuning ? (int)pTuning->m_LaserBounceNum : 1000;
	const int MaxBounces = maximum(1, minimum(LaserBounceNum, (int)MAX_LASER_BOUNCES));
	const int SafeTicks = 4;

	// 1. Simulate forward trajectory
	const int PathTicks = MaxBounces * BounceDelayTicks + SafeTicks + 15;
	std::vector<vec2> vPath(PathTicks + 1);
	std::vector<char> vClear(PathTicks + 1, 0);
	std::vector<char> vTouchedFreeze(PathTicks + 1, 0);

	CCharacterCore SimCore = GameClient()->m_PredictedChar;
	SimCore.Init(nullptr, Collision());
	SimCore.SetHookedPlayer(-1);

	int EnterFreezeTick = -1;
	bool InFreeze = (AvoidDangerClass(CharPos.x, CharPos.y) >= 1);
	if(InFreeze)
		EnterFreezeTick = 0;

	vPath[0] = CharPos;
	vClear[0] = (!InFreeze && TeeFullyClearOfFreeze(CharPos)) ? 1 : 0;
	vTouchedFreeze[0] = InFreeze ? 1 : 0;

	for(int t = 1; t <= PathTicks; ++t)
	{
		if(InFreeze)
		{
			SimCore.m_Input.m_Direction = 0;
			SimCore.m_Input.m_Hook = 0;
			SimCore.m_Input.m_Jump = 0;
		}
		else
		{
			SimCore.m_Input = m_aInputData[Dummy];
		}

		vec2 Prev = SimCore.m_Pos;
		SimCore.Tick(true);
		SimCore.Move();
		SimCore.Quantize();

		bool HitFreezeThisTick = false;
		const int Steps = maximum(1, (int)(distance(Prev, SimCore.m_Pos) / 4.0f));
		for(int s = 1; s <= Steps; ++s)
		{
			vec2 Pt = mix(Prev, SimCore.m_Pos, (float)s / (float)Steps);
			if(AvoidDangerClass(Pt.x, Pt.y) >= 1)
			{
				HitFreezeThisTick = true;
				break;
			}
		}

		if(HitFreezeThisTick && EnterFreezeTick < 0)
			EnterFreezeTick = t;

		if(HitFreezeThisTick || AvoidDangerClass(SimCore.m_Pos.x, SimCore.m_Pos.y) >= 1)
			InFreeze = true;

		vPath[t] = SimCore.m_Pos;
		vClear[t] = TeeFullyClearOfFreeze(SimCore.m_Pos) ? 1 : 0;
		vTouchedFreeze[t] = (vTouchedFreeze[t - 1] || HitFreezeThisTick || InFreeze) ? 1 : 0;
	}

	// 2. Strict Close-To-Freeze Triggering:
	// Only fire when we are AT THE VERY EDGE of freeze (0 to 2 ticks away), never far in advance!
	const bool ReadyToEdgeFire = (EnterFreezeTick >= 0 && EnterFreezeTick <= 2);

	// 3. Evaluate target positions for each bounce count k
	vec2 aTargetPos[MAX_LASER_BOUNCES + 1] = {};
	bool aValid[MAX_LASER_BOUNCES + 1] = {};
	bool AnyValid = false;

	if(ReadyToEdgeFire)
	{
		for(int k = 1; k <= MaxBounces; ++k)
		{
			const int ArrivalTick = k * BounceDelayTicks;
			if(ArrivalTick > PathTicks)
				break;

			aTargetPos[k] = vPath[ArrivalTick];

			// Arrival must be after entering freeze, and at arrival tee must be fully clear of freeze
			if(!vTouchedFreeze[ArrivalTick] || !vClear[ArrivalTick])
				continue;

			// Check window around arrival (±1 tick) to tolerate minor timing variance
			bool WindowClear = true;
			for(int w = maximum(0, ArrivalTick - 1); w <= minimum(PathTicks, ArrivalTick + 1); ++w)
			{
				if(!vClear[w])
				{
					WindowClear = false;
					break;
				}
			}
			if(!WindowClear)
				continue;

			// Check that tee remains in safe air for SafeTicks after arrival
			bool SafeAfter = true;
			for(int s = 1; s <= SafeTicks; ++s)
			{
				if(ArrivalTick + s <= PathTicks && !vClear[ArrivalTick + s])
				{
					SafeAfter = false;
					break;
				}
			}
			if(!SafeAfter)
				continue;

			aValid[k] = true;
			AnyValid = true;
		}
	}

	// 4. Arm weapon and fire on edge
	const bool DangerInArm = (EnterFreezeTick >= 0 && EnterFreezeTick <= 10) || ReadyToEdgeFire;

	if(DangerInArm)
	{
		const int ActiveWeapon = GameClient()->m_PredictedChar.m_ActiveWeapon;
		if(m_aAntiVoidLaserPrevWeapon[Dummy] < 0 && ActiveWeapon != WEAPON_LASER)
			m_aAntiVoidLaserPrevWeapon[Dummy] = ActiveWeapon;

		// Swap weapon to laser
		m_aInputData[Dummy].m_WantedWeapon = WEAPON_LASER + 1;

		const int LocalId = GameClient()->m_Snap.m_LocalClientId;
		const bool FrozenNow = (LocalId >= 0 && GameClient()->m_aClients[LocalId].m_Predicted.m_FreezeEnd != 0);

		// Fire immediately in the same packet without waiting for weapon switch roundtrip
		if(ReadyToEdgeFire && AnyValid && m_aAntiVoidLaserCooldown[Dummy] == 0 &&
			(m_aInputData[Dummy].m_Fire & 1) == 0 && !FrozenNow)
		{
			vec2 AimDir;
			float DistToWall = 0.0f;
			vec2 BouncePos(0.0f, 0.0f);
			vec2 ReflDir(0.0f, 0.0f);
			float TeeHitOffset = 0.0f;
			int Bounces = 0;
			int ArrivalTicks = 0;

			bool Found = FindLaserSelfBounce(aTargetPos, aValid, MaxBounces, BounceDelayTicks,
				AimDir, DistToWall, BouncePos, ReflDir, TeeHitOffset, Bounces, ArrivalTicks);

			if(Found)
			{
				const vec2 Aim = normalize(AimDir) * 1000.0f; // 1000px length for extreme angular precision
				m_aInputData[Dummy].m_TargetX = (int)Aim.x;
				m_aInputData[Dummy].m_TargetY = (int)Aim.y;
				if(!m_aInputData[Dummy].m_TargetX && !m_aInputData[Dummy].m_TargetY)
					m_aInputData[Dummy].m_TargetX = 1;

				m_aInputData[Dummy].m_Fire++;
				m_aAntiVoidLaserFireValue[Dummy] = m_aInputData[Dummy].m_Fire;
				m_aAntiVoidLaserReleasePending[Dummy] = true;
				m_aAntiVoidLaserCooldown[Dummy] = maximum(35, ArrivalTicks + 15);

				// Start in-flight rescue tracker
				m_aLaserTracker[Dummy].m_Active = true;
				m_aLaserTracker[Dummy].m_FireTick = CurrentTick;
				m_aLaserTracker[Dummy].m_ArrivalTick = CurrentTick + ArrivalTicks;
				m_aLaserTracker[Dummy].m_FirePos = CharPos;
				m_aLaserTracker[Dummy].m_TargetPos = aTargetPos[Bounces];
				m_aLaserTracker[Dummy].m_WallPos = BouncePos;
				m_aLaserTracker[Dummy].m_ReflDir = ReflDir;
				m_aLaserTracker[Dummy].m_DistToWall = DistToWall;
				m_aLaserTracker[Dummy].m_TeeHitOffset = TeeHitOffset;
				m_aLaserTracker[Dummy].m_Bounces = Bounces;

				if(g_Config.m_TcAntiVoidLaserDebug >= 1)
				{
					const float AngleDeg = std::atan2(AimDir.y, AimDir.x) * 180.0f / pi;
					log_info("laser_ricochet", "!! PERFECT EDGE RESCUE SHOT at wall (%.0f, %.0f) [bounces=%d, dist=%.1fpx, angle=%.1f°] -> will strike tee in SAFE AIR at (%.0f, %.0f) in %d ticks (offset=%.2fpx)",
						BouncePos.x, BouncePos.y, Bounces, DistToWall, AngleDeg, m_aLaserTracker[Dummy].m_TargetPos.x, m_aLaserTracker[Dummy].m_TargetPos.y, ArrivalTicks, TeeHitOffset);
				}
			}
			else if(g_Config.m_TcAntiVoidLaserDebug >= 2)
			{
				log_info("laser_ricochet", "ready to fire at edge but no unblocked bounce path found");
			}
		}
		else if(g_Config.m_TcAntiVoidLaserDebug >= 2)
		{
			log_info("laser_ricochet", "arming laser: ready_edge=%d (any_valid=%d, enter_tick=%d, frozen=%d)", ReadyToEdgeFire, AnyValid ? 1 : 0, EnterFreezeTick, FrozenNow ? 1 : 0);
		}
	}
	else if(m_aAntiVoidLaserPrevWeapon[Dummy] >= 0 && !DangerInArm && !m_aAntiVoidLaserReleasePending[Dummy])
	{
		const int Prev = m_aAntiVoidLaserPrevWeapon[Dummy];
		if(Prev >= 0 && Prev != WEAPON_LASER)
		{
			m_aInputData[Dummy].m_WantedWeapon = Prev + 1;
			if(GameClient()->m_PredictedChar.m_ActiveWeapon == Prev)
			{
				if(g_Config.m_TcAntiVoidLaserDebug >= 1)
					log_info("laser_ricochet", "safe again: restored weapon %d", Prev);
				m_aAntiVoidLaserPrevWeapon[Dummy] = -1;
			}
		}
		else
		{
			m_aAntiVoidLaserPrevWeapon[Dummy] = -1;
		}
	}
}

// Deterministic pseudo-random in [0, 1) from an integer seed. Used so the "snap and hold" spin
// modes stay put within a time bucket (instead of jittering every frame from rand()), and so the
// rendered weapon and the sent ("real") aim compute the exact same angle for a given moment.
static float SpinHash01(int Seed)
{
	uint32_t x = (uint32_t)Seed * 747796405u + 2891336453u;
	x = ((x >> ((x >> 28) + 4u)) ^ x) * 277803737u;
	x = (x >> 22) ^ x;
	return (float)(x & 0x00ffffffu) / (float)0x01000000;
}

// Single source of truth for the weapon spinner angle (cosmetic). RealAngle is the player's actual
// aim; pendulum/jitter modes orbit around it, the rest ignore it. Time is Client()->LocalTime().
float CControls::WeaponSpinAngle(float RealAngle, float Time)
{
	const float Speed = g_Config.m_TcWeaponSpinSpeed / 10.0f; // base rate (rad/s for spin modes)
	const float Rand = g_Config.m_TcWeaponSpinRandom / 100.0f; // 0..1 extra chaos overlay
	const float Tau = 2.0f * pi;
	float Angle = RealAngle;

	switch(g_Config.m_TcWeaponSpinMode)
	{
	default:
	case 0: // spin clockwise
		Angle = Time * Speed;
		break;
	case 1: // spin counter-clockwise
		Angle = -Time * Speed;
		break;
	case 2: // pendulum: sweep back and forth around the real aim
		Angle = RealAngle + std::sin(Time * Speed * 0.5f) * (pi * 0.75f);
		break;
	case 3: // random flicks: snap to a random direction, hold, then flick to a new one
	{
		const float Rate = maximum(0.5f, Speed * 0.25f); // flicks per second
		Angle = SpinHash01((int)(Time * Rate)) * Tau;
		break;
	}
	case 4: // jitter: small fast shake around the real aim
	{
		const float Rate = maximum(1.0f, Speed); // shakes per second
		Angle = RealAngle + (SpinHash01((int)(Time * Rate)) - 0.5f) * (pi * (0.25f + Rand));
		break;
	}
	case 5: // snap through the 8 cardinal/diagonal directions in order
	{
		const float Rate = maximum(0.5f, Speed * 0.25f);
		Angle = (float)(((int)(Time * Rate)) & 7) * (Tau / 8.0f);
		break;
	}
	case 6: // random drift: smoothly wander between random directions
	{
		const float Rate = maximum(0.25f, Speed * 0.15f);
		const float T = Time * Rate;
		const int B = (int)T;
		const float Frac = T - (float)B;
		const float A0 = SpinHash01(B) * Tau;
		float Delta = SpinHash01(B + 1) * Tau - A0;
		while(Delta > pi)
			Delta -= Tau;
		while(Delta < -pi)
			Delta += Tau;
		Angle = A0 + Delta * (Frac * Frac * (3.0f - 2.0f * Frac)); // smoothstep
		break;
	}
	case 7: // chaos: variable-speed spin combined with random flicks
	{
		const float SpinPart = Time * Speed * (0.5f + SpinHash01((int)(Time * 2.0f)));
		const float FlickPart = (SpinHash01((int)(Time * maximum(1.0f, Speed * 0.5f))) - 0.5f) * Tau;
		Angle = SpinPart + FlickPart;
		break;
	}
	}

	// Universal randomness overlay on top of any mode (modes 3/4 are already random enough).
	if(Rand > 0.0f && g_Config.m_TcWeaponSpinMode != 3 && g_Config.m_TcWeaponSpinMode != 4)
		Angle += (SpinHash01((int)(Time * 40.0f)) - 0.5f) * Tau * Rand * 0.5f;

	return Angle;
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[maximum(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = GameClient()->m_Camera.m_Zoom;
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
	}
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
		{
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
			GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
		}
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const float MouseMax = GetMaxMouseDistance();

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;

		if(g_Config.m_TcLimitMouseToScreen)
		{
			float Width, Height;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), 1.0f, &Width, &Height);
			Height /= 2.0f;
			Width /= 2.0f;
			if(g_Config.m_TcLimitMouseToScreen == 2)
				Width = Height;
			m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -Height, Height);
			m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -Width, Width);
		}
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}

bool CControls::CheckNewInput()
{
	bool NewInput[2] = {};
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Dummy];
		if(Dummy == g_Config.m_ClDummy)
		{
			TestInput.m_Direction = 0;
			if(m_aInputDirectionLeft[Dummy] && !m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = -1;
			if(!m_aInputDirectionLeft[Dummy] && m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = 1;
			// TClient: mirror the SnapInput rebuild so fast-input detects a press/release of the hook key
			// straight away, even though the key now feeds the shadow m_aInputHook instead of m_aInputData.
			TestInput.m_Hook = m_aInputHook[Dummy];
		}

		if(m_aFastInput[Dummy].m_Direction != TestInput.m_Direction)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Hook != TestInput.m_Hook)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Jump != TestInput.m_Jump)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_NextWeapon != TestInput.m_NextWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_PrevWeapon != TestInput.m_PrevWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_WantedWeapon != TestInput.m_WantedWeapon)
			NewInput[Dummy] = true;

		bool SetMousePos = false;
		// We need to be careful about how we manage the mouse position to avoid mispredicted hooks and fires
		// on the first tick that they activate before we know what mouse position we actually sent to the server
		if(Dummy == g_Config.m_ClDummy)
		{
			if(m_aFastInput[Dummy].m_Hook == 0 && TestInput.m_Hook == 1)
			{
				m_FastInputHookAction = true;
				SetMousePos = true;
			}
			if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire && TestInput.m_Fire % 2 == 1)
			{
				m_FastInputFireAction = true;
				SetMousePos = true;
			}
			if(!m_FastInputHookAction && !m_FastInputFireAction)
			{
				SetMousePos = true;
			}
		}

		if(SetMousePos)
		{
			TestInput.m_TargetX = (int)m_aMousePos[Dummy].x;
			TestInput.m_TargetY = (int)m_aMousePos[Dummy].y;
		}
		else
		{
			TestInput.m_TargetX = m_aFastInput[Dummy].m_TargetX;
			TestInput.m_TargetY = m_aFastInput[Dummy].m_TargetY;
		}

		m_aFastInput[Dummy] = TestInput;
	}

	if(NewInput[0] || NewInput[1])
		return true;
	else
		return false;
}
