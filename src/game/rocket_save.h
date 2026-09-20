#ifndef GAME_ROCKET_SAVE_H
#define GAME_ROCKET_SAVE_H

#include <base/vmath.h>

#include <game/gamecore.h>

class CCollision;

// TClient anti-void rocket, simulated version.
//
// The old rocket save was a distance trigger: scan for the nearest danger ahead, and once it is within N
// pixels, fire at it. That fires at a fixed geometric moment, which is not the moment that saves you the
// most — a grenade thrown a few ticks earlier or at a slightly different angle can land against a wall and
// throw you much further out of the freeze than one fired point blank at the tile you are about to touch.
//
// WHEN to fire is still the old, predictable distance trigger — the moment the danger is within the
// configured distance. Trying to also pick the moment by simulation was worse in practice: it sat on the
// shot waiting for a better one and let the freeze arrive.
//
// What this does is pick WHERE to fire. A grenade aimed straight at the tile you are about to touch often
// flies through it (freeze is not solid) and goes off somewhere useless. So every direction is flown out
// with the real projectile maths, detonated where it would actually hit, the real explosion force applied,
// and the tee run forward from there — the direction that leaves it furthest from freeze wins, and between
// equally good ones the harder kick wins, which is the one detonating against the nearest solid surface.
struct CRocketSaveCfg
{
	float m_Curvature = 7.0f; // tuning: grenade_curvature
	float m_Speed = 1000.0f; // tuning: grenade_speed
	float m_Lifetime = 2.0f; // tuning: grenade_lifetime, seconds
	float m_ExplosionStrength = 6.0f; // tuning: explosion_strength
	bool m_Freeze = true;
	bool m_DeepFreeze = true;
	bool m_LiveFreeze = true;
	bool m_Death = true;
};

struct CRocketSaveTuning
{
	int m_Rays = 32; // directions tried, spread over the full circle
	int m_Horizon = 50; // ticks the outcome of a shot is followed for
	// How far off the direction of travel a shot may aim. -1 = no restriction: every direction is
	// flown and scored by the outcome simulation. The old -0.1 filter threw away shots that go
	// against the current motion, which is exactly the shot needed when hovering over a floor freeze
	// while the tee drifts upward (the blast must come from below to push it back up).
	float m_InertiaDot = -1.0f;
};

struct CRocketSaveAim
{
	bool m_Found = false;
	vec2 m_Dir = vec2(0.0f, 0.0f); // direction to fire in
	float m_Score = 0.0f; // how the tee ends up after that shot
	float m_PlainScore = 0.0f; // ...compared to firing straight at the danger, for the log
	vec2 m_Blast = vec2(0.0f, 0.0f); // where that grenade goes off
	float m_Kick = 0.0f; // px/tick the blast adds to our velocity
	float m_BlastTicks = 0.0f; // ticks the grenade needs to fly there and detonate (this is what the outcome sim now waits for)
	// What doing nothing is worth in the same simulation. A shot is only preferred when its score beats
	// this, so blasts that freeze the tee sooner than doing nothing are not chosen. In an emergency
	// (avoid has no solution at all) the least-bad valid shot is fired even without an improvement.
	float m_BaseScore = 0.0f;
	bool m_Improves = false; // the chosen shot beats doing nothing in the simulation
};

class CRocketSave
{
public:
	// Best direction to fire in RIGHT NOW. Fallback is the direction the caller already had in mind (the
	// danger it found), which is also what the result is scored against.
	static CRocketSaveAim BestAim(CCollision *pCollision, const CCharacterCore &Core, const CNetObj_PlayerInput &Input, const CRocketSaveCfg &Cfg, vec2 Fallback);

	static CRocketSaveTuning ms_Tuning;
};

#endif // GAME_ROCKET_SAVE_H
