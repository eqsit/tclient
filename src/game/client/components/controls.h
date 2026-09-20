/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls : public CComponent
{
public:
	float GetMinMouseDistance() const;
	float GetMaxMouseDistance() const;

	enum class EMouseInputType
	{
		ABSOLUTE,
		RELATIVE,
		AUTOMATED,
	};

	vec2 m_aMousePos[NUM_DUMMIES];
	vec2 m_aMousePosOnAction[NUM_DUMMIES];
	vec2 m_aTargetPos[NUM_DUMMIES];

	EMouseInputType m_aMouseInputType[NUM_DUMMIES];

	int m_aAmmoCount[NUM_WEAPONS];

	int64_t m_LastSendTime;
	CNetObj_PlayerInput m_aInputData[NUM_DUMMIES];
	CNetObj_PlayerInput m_aLastData[NUM_DUMMIES];
	int m_aInputDirectionLeft[NUM_DUMMIES];
	int m_aInputDirectionRight[NUM_DUMMIES];
	// TClient: the raw held state of the hook key. The +hook bind writes HERE, not straight into
	// m_aInputData.m_Hook, and every tick m_aInputData.m_Hook is rebuilt from it (exactly like the
	// direction is rebuilt from the left/right keys). This is what lets avoid force the hook off for as
	// long as it is dangerous without the forced 0 sticking forever: the next tick starts from the real
	// held key again, so the instant hooking is safe the player's still-held hook simply resumes.
	int m_aInputHook[NUM_DUMMIES];
	bool m_aInputFirePressed[NUM_DUMMIES] = {false, false}; // physical +fire state, separate from the counter an automatic shot changes
	int m_aShowHookColl[NUM_DUMMIES];

	// TClient
	CNetObj_PlayerInput m_aFastInput[NUM_DUMMIES];
	bool m_FastInputHookAction = false;
	bool m_FastInputFireAction = false;

	// TClient safety: tile tests shared by the rocket and laser counters.
	int AvoidDangerClassPoint(float x, float y, bool ForceFreezeRecoverable = false) const; // classify one tile point: 0 = safe, 1 = recoverable freeze, 2 = lethal. ForceFreezeRecoverable: freeze counts as class 1 even with tc_avoid_unfreeze off
	bool AvoidHardDeathPoint(float x, float y) const; // is this point a kill tile or off-map (the hitbox-corner death test, freeze excluded)
	int AvoidDangerClass(float x, float y, bool ForceFreezeRecoverable = false) const; // same but for the whole tee body at (x,y): centre + 4 hitbox corners, worst wins
	void ApplyAntiVoidRocket(bool Suppressed = false); // rocket-first grenade counter. Suppressed: do upkeep (release fire, tick cooldown) but don't arm/fire
	void CancelAntiVoidRocket(int Dummy, bool ReleaseFire = true); // relinquish fire/weapon ownership when disabled, reset, or dead
	static constexpr int MAX_LASER_BOUNCES = 12;
	void ApplyAntiVoidLaser(bool Suppressed = false); // laser self-ricochet counter; runs independently of the rocket
	int TraceLaserPath(vec2 From, vec2 AimDir, int MaxBounces, vec2 *pSegStart, vec2 *pSegEnd) const;
	bool FindLaserSelfBounce(const vec2 *pTargetPos, const bool *pValid, int MaxBounces, int BounceDelayTicks, vec2 &OutAimDir, float &OutDistToWall, vec2 &OutBouncePos, vec2 &OutReflDir, float &OutTeeHitOffset, int &OutBounces, int &OutArrivalTicks) const;
	bool TeeFullyClearOfFreeze(vec2 Pos) const; // true if the full 28px tee body has zero intersection with freeze/death tiles
	// True if our own tee is still on the map (normal play, or paused/spectating). The client leaves
	// m_pLocalCharacter null in spec, so we also accept an active local character item in the snapshot.
	bool HaveLocalChar() const;
	// Position of our own tee for the safety features. Normally the smoothed render position, but while
	// paused/spectating (press Q to watch others) the client leaves that stale, so we fall back to the
	// predicted core position, which stays valid as long as our tee is still on the map.
	vec2 LocalCharPos() const;
	// Runs the automatic safety features (balancer, rocket counter, laser counter) on the local tee if
	// there is one. Shared by the normal-play path and the frozen-input (chat/menu) path so they work in
	// both, and in spec. With no local tee at all it does nothing.
	void ApplyAutoSafety();

	// TClient: silent aim channel for the ported Kinetix Basic Avoid Freeze. When set by
	// CAvoidFreeze::ApplyOverride it is patched into the packet sent to the server only
	// (CControls::SnapInput), so local prediction and the crosshair keep the real aim.
	bool m_AvoidAimActive = false;
	vec2 m_AvoidAimTarget = vec2(0.0f, 0.0f);

	// TClient balancer: while standing on a tee that is over the void, only steer us back when we start to
	// slide off its rounded head, so we don't fall in. Hands-off otherwise.
	bool BalancerTeeInVoid(vec2 TeePos) const; // true if the tee has no safe solid ground below it
	bool ApplyBalancer(); // modifies m_aInputData[g_Config.m_ClDummy].m_Direction; returns true if engaged on a tee

	// TClient hole assist: while the +tc_hole_assist bind is active (held or toggled, see
	// tc_hole_assist_hold), find the nearest narrow gap in the surrounding walls and steer so we come to
	// rest centered on it. FindNearestHoleX returns the world-x of the best gap center (nearest to us) or
	// false if none in range. ApplyHoleAssist sets m_Direction accordingly.
	bool FindNearestHoleX(float &OutX) const;
	void ApplyHoleAssist();
	bool HoleAssistActive() const; // resolves hold-vs-toggle mode into "is it engaged right now?"
	bool m_HoleAssistPressed = false; // the bound key is physically held right now
	bool m_HoleAssistToggled = false; // toggle-mode state, flipped on each key press
	bool m_aHoleSettled[NUM_DUMMIES] = {false, false}; // hysteresis latch: we are parked on the gap, hold still
	bool m_aAvoidWasFrozen[NUM_DUMMIES] = {false, false}; // debug outcome log: edge detection for "got frozen"
	int m_aAntiVoidRocketCooldown[NUM_DUMMIES] = {0, 0}; // ticks left before the rocket counter may fire again
	int m_aAntiVoidRocketLogState[NUM_DUMMIES] = {-1, -1}; // debug log state, see ApplyAntiVoidRocket
	// Debug diagnostics of the last rocket evaluation, printed in the OUTCOME lines.
	struct CAntiVoidRocketDiag
	{
		int m_DangerTick = -1;
		float m_DangerDist = 0.0f;
		bool m_Solid = false;
		bool m_AvoidSaves = false;
		bool m_AvoidNoSolution = false;
		bool m_NeedRocket = false;
	};
	CAntiVoidRocketDiag m_aAntiVoidRocketDiag[NUM_DUMMIES];
	int m_aAntiVoidRocketLastFireTick[NUM_DUMMIES] = {-1, -1}; // PredGameTick of the last fired rocket
	bool m_aAntiVoidRocketReleasePending[NUM_DUMMIES] = {false, false}; // we pressed fire last tick and must release it
	int m_aAntiVoidRocketFireValue[NUM_DUMMIES] = {0, 0}; // the m_Fire value we set, so we only release our own press
	int m_aAntiVoidRocketPrevWeapon[NUM_DUMMIES] = {-1, -1}; // weapon to switch back to once the rocket save is done (-1 = none)
	bool m_aAntiVoidRocketManualWeapon[NUM_DUMMIES] = {false, false}; // manual weapon input wins until the current danger has passed
	int m_aAntiVoidLaserCooldown[NUM_DUMMIES] = {0, 0}; // ticks left before the laser counter may fire again
	bool m_aAntiVoidLaserReleasePending[NUM_DUMMIES] = {false, false}; // we pressed fire last tick and must release it
	int m_aAntiVoidLaserFireValue[NUM_DUMMIES] = {0, 0}; // the m_Fire value we set, so we only release our own press
	int m_aAntiVoidLaserPrevWeapon[NUM_DUMMIES] = {-1, -1}; // weapon to switch back to once the laser save is done (-1 = none)

	struct CRescueLaserTracker
	{
		bool m_Active = false;
		int m_FireTick = -1;
		int m_ArrivalTick = -1;
		vec2 m_FirePos = vec2(0.0f, 0.0f);
		vec2 m_TargetPos = vec2(0.0f, 0.0f);
		vec2 m_WallPos = vec2(0.0f, 0.0f);
		vec2 m_ReflDir = vec2(0.0f, 0.0f);
		float m_DistToWall = 0.0f;
		float m_TeeHitOffset = 0.0f;
		int m_Bounces = 0;
	};
	CRescueLaserTracker m_aLaserTracker[NUM_DUMMIES];

	// TClient weapon spinner. Single source of truth for the spin angle so the local visual
	// (players.cpp) and the optional "real" sent aim (SnapInput) always agree.
	// RealAngle = the player's actual aim; some modes (pendulum/jitter) orbit around it.
	static constexpr int NUM_WEAPON_SPIN_MODES = 8;
	static float WeaponSpinAngle(float RealAngle, float Time);

	CControls();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	void OnConsoleInit() override;
	virtual void OnPlayerDeath();

	int SnapInput(int *pData);
	void ClampMousePos();
	void ResetInput(int Dummy);
	bool CheckNewInput();

private:
	static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyHoleAssist(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData);
};
#endif
