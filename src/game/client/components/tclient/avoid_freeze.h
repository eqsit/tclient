#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_AVOID_FREEZE_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_AVOID_FREEZE_H

// Ported from Kinetix "Basic Avoid Freeze" (kx_basic_avoid_freeze).
//
// Each tick:
//   1. Simulate the prediction window with the current input. If danger
//      (freeze / teleport / death, depending on the kx_baf_avoid_* cvars)
//      happens, activate avoidance.
//   2. Brute-force all combinations of {direction, jump, hook, aim angles}
//      and keep the ones that survive the whole window.
//   3. Prefer waiting one more tick if any combination does; otherwise pick
//      the combination with the longest survival and the smallest input diff.
//   4. Override the player's input with the chosen combination. In silent
//      aim mode the aim is only patched into the packet that is sent to the
//      server (CControls::SnapInput), never into local prediction.

#include <game/client/component.h>
#include <generated/protocol.h>

class CAvoidFreeze : public CComponent
{
public:
	CAvoidFreeze() = default;
	~CAvoidFreeze() override = default;

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnUpdate() override;
	// Called from CControls::SnapInput before the other safety features so they can
	// see whether avoid already found a way out for this tick.
	void ApplyOverride();
	// True when this tick's evaluation found a way to survive on its own: either the
	// situation is safe, waiting one more tick works, or a fully-surviving input was
	// applied. The rocket counter stays silent while this is true.
	bool WouldSave() const { return m_SavedThisTick; }

private:
	// Returns the tick (1-based) at which danger first occurs, 0 if none.
	int SimulateDangerTick(int LocalId, const CNetObj_PlayerInput &Input, int Ticks) const;
	// First 'Delay' ticks with DelayInput, then ComboInput. Returns danger tick.
	int SimulateDangerTickDelayed(int LocalId, const CNetObj_PlayerInput &DelayInput,
		int Delay, const CNetObj_PlayerInput &ComboInput, int Ticks) const;
	int InputDiff(const CNetObj_PlayerInput &A, const CNetObj_PlayerInput &B) const;

	// Hook override tracking. Used to release a hook that avoid itself threw once
	// danger clears without cancelling a hook the player is physically holding.
	bool m_WasOverriding = false;
	int m_OverrideHook = 0;
	// Set when avoid force-releases a hook that the player is still holding. Unless
	// kx_baf_rehook is enabled, keep the hook suppressed until the key is released.
	bool m_BlockHeldHookUntilRelease = false;
	// Result of this tick's evaluation, read by the rocket counter (WouldSave).
	bool m_SavedThisTick = false;
	// True when the brute force found no input that avoids the danger at all this tick.
	bool m_NoSolutionThisTick = false;
	// True when avoid has the situation under control this tick: safe, can wait one more tick, released
	// the hook, or applied a fully-surviving input. The rocket counter is a last resort and stays out of
	// it while this holds — the logs showed the large majority of shots were fired while avoid was still
	// in its "wait" state and nothing ever came of them.
	bool m_HasPlanThisTick = false;
	// First tick danger appears in avoid's simulation (0 = safe). The rocket uses it when its own
	// path prediction sees nothing but avoid does.
	int m_DangerTickThisTick = 0;
	// Debug log (kx_baf_debug): last decision logged, so level 1 only prints on change.
	int m_LogAction = -1;

public:
	// Last logged decision (debug): 0 safe, 1 wait, 2 override, 3 hook release, 4 no solution.
	int LastAction() const { return m_LogAction; }
	// No input survives: the rocket counter uses this to fire as soon as it has a valid shot.
	bool NoSolution() const { return m_NoSolutionThisTick; }
	int DangerTick() const { return m_DangerTickThisTick; }
	// Avoid is on top of the situation: the rocket counter must not fire a speculative grenade.
	bool HasPlan() const { return m_HasPlanThisTick; }
};

#endif // GAME_CLIENT_COMPONENTS_TCLIENT_AVOID_FREEZE_H
