#include "rocket_save.h"

#include <base/math.h>

#include <game/collision.h>
#include <game/mapitems.h>

#include <algorithm>

CRocketSaveTuning CRocketSave::ms_Tuning;

static bool DangerAt(CCollision *pCollision, float x, float y, const CRocketSaveCfg &Cfg)
{
	const int Tx = (int)(x / 32.0f);
	const int Ty = (int)(y / 32.0f);
	if(Tx < 0 || Ty < 0 || Tx >= pCollision->GetWidth() || Ty >= pCollision->GetHeight())
		return true; // off the map is as deadly as it gets
	const int Index = pCollision->GetPureMapIndex(x, y);
	for(const int T : {pCollision->GetTileIndex(Index), pCollision->GetFrontTileIndex(Index), pCollision->GetSwitchType(Index)})
		if((Cfg.m_Freeze && T == TILE_FREEZE) || (Cfg.m_DeepFreeze && T == TILE_DFREEZE) ||
			(Cfg.m_LiveFreeze && T == TILE_LFREEZE) || (Cfg.m_Death && T == TILE_DEATH))
			return true;
	return false;
}

// Same test for a map index the tee crossed. The game freezes on tile CROSSINGS (CCharacter::HandleTiles
// over Collision()->GetMapIndices(PrevPos, Pos)), so the simulation has to look at the same tiles.
static bool DangerIndex(CCollision *pCollision, int Index, const CRocketSaveCfg &Cfg)
{
	for(const int T : {pCollision->GetTileIndex(Index), pCollision->GetFrontTileIndex(Index), pCollision->GetSwitchType(Index)})
		if((Cfg.m_Freeze && T == TILE_FREEZE) || (Cfg.m_DeepFreeze && T == TILE_DFREEZE) ||
			(Cfg.m_LiveFreeze && T == TILE_LFREEZE) || (Cfg.m_Death && T == TILE_DEATH))
			return true;
	return false;
}

// How much open space around the tee still counts as "clear": beyond this, one escape is as good as
// another and the score should not split hairs over it.
static constexpr float ROCKET_CLEARANCE_MAX = 200.0f;

// Minimum push, in px/tick, for a shot to count as a save at all: below this the blast is too far or too
// weak to change anything, and firing would only waste the grenade.
static constexpr float ROCKET_MIN_KICK = 4.0f;

// The game tick is SERVER_TICK_SPEED (50) per second. The grenade's flight time and the tee simulation
// both run on game ticks, so the two are converted with this.
static constexpr float TICK_SECONDS = 1.0f / 50.0f;

// Fly the grenade the way the game does and return where it goes off. Trajectory is the projectile maths
// from gamecore (CalcPos); detonation is the first SOLID tile it touches. *pHitSolid says whether it hit
// anything at all — a grenade that burns out in mid-air, or sails through freeze (freeze is not solid, so a
// rocket aimed straight into it just passes through), gives no push worth having and is not a candidate.
// *pOutTime receives the seconds it took to get there: the blast cannot happen before the grenade has
// flown, and the caller has to wait that long before the explosion force exists.
static vec2 BlastPos(CCollision *pCollision, vec2 From, vec2 Dir, const CRocketSaveCfg &Cfg, bool *pHitSolid, float *pOutTime)
{
	const vec2 Start = From + Dir * 28.0f * 0.75f; // the game spawns the projectile just outside the tee
	vec2 Prev = Start;
	// ~7px of flight per sample at the default 1000px/s (and still well under the 32px tile size even
	// for the fastest tuned grenades), which is fine enough that a solid tile is never skipped and keeps
	// the 32-ray search cheap.
	const float Step = 1.0f / 150.0f;
	if(pHitSolid)
		*pHitSolid = false;
	if(pOutTime)
		*pOutTime = Cfg.m_Lifetime;
	for(float t = Step; t <= Cfg.m_Lifetime; t += Step)
	{
		const vec2 P = CalcPos(Start, Dir, Cfg.m_Curvature, Cfg.m_Speed, t);
		if(pCollision->CheckPoint(P.x, P.y))
		{
			if(pHitSolid)
				*pHitSolid = true;
			if(pOutTime)
				*pOutTime = t;
			return Prev; // detonates against the surface it just hit
		}
		Prev = P;
	}
	return Prev; // burned out in mid-air: no surface, no real kick
}

// The explosion force the game would apply to the tee, straight from CGameWorld::CreateExplosion.
static vec2 BlastForce(vec2 TeePos, vec2 Blast, const CRocketSaveCfg &Cfg)
{
	const float Radius = 135.0f;
	const float InnerRadius = 48.0f;
	const vec2 Diff = TeePos - Blast;
	const float l = length(Diff);
	const vec2 ForceDir = l > 0.0f ? Diff / l : vec2(0.0f, -1.0f);
	const float Falloff = 1.0f - std::clamp((l - InnerRadius) / (Radius - InnerRadius), 0.0f, 1.0f);
	const float Dmg = Cfg.m_ExplosionStrength * Falloff;
	if((int)Dmg == 0)
		return vec2(0.0f, 0.0f); // the game ignores explosions this weak entirely
	return ForceDir * Dmg * 2.0f;
}

// Run the tee forward and score how the situation ends. The blast does not exist yet when the shot is
// fired: the tee keeps moving WITHOUT the explosion for the ticks the grenade needs to fly to its surface,
// and the force is added on the tick it actually detonates, measured from where the tee is THEN (that is
// how CGameWorld::CreateExplosion works). Applying the force at tick 0 (the old behaviour) rated
// last-moment shots as saves even though the grenade landed after the freeze. Higher is better: a run
// that never touches freeze scores by how much room it keeps around it, a run that freezes scores by how
// long it lasted.
static float Outcome(CCollision *pCollision, CCharacterCore Core, const CNetObj_PlayerInput &Input, const CRocketSaveCfg &Cfg, int Ticks, vec2 Blast, float BlastTime, float *pOutKick = nullptr)
{
	Core.Init(nullptr, pCollision);
	Core.SetHookedPlayer(-1);
	const int BlastTick = std::clamp(round_to_int(BlastTime / TICK_SECONDS), 0, Ticks);
	float Worst = 1e9f;
	if(pOutKick)
		*pOutKick = 0.0f;
	for(int i = 0; i < Ticks; ++i)
	{
		if(i == BlastTick)
		{
			// The grenade goes off now. The game applies the explosion through CCharacter::TakeDamage,
			// which clamps the resulting velocity with the current move restrictions (tiles can forbid
			// moving up/down/left/right). Adding it raw here made the simulation believe in kicks the
			// game silently zeroes — that is how shots "saving for 40 ticks" ended in a freeze 4 ticks
			// later next to directional freeze tiles.
			const vec2 VelBefore = Core.m_Vel;
			Core.m_Vel = ClampVel(Core.MoveRestrictions(), Core.m_Vel + BlastForce(Core.m_Pos, Blast, Cfg));
			if(pOutKick)
				*pOutKick = length(Core.m_Vel - VelBefore);
		}
		Core.m_Input = Input;
		const vec2 Prev = Core.m_Pos;
		Core.Tick(true);
		Core.Move();
		Core.Quantize();
		// Look at the tiles the centre crossed this tick, exactly like the game's freeze trigger does.
		// Point sampling every 8px misses the freeze corner a slow diagonal grind clips, which made the
		// simulation promise saves the game never delivered.
		for(const int Index : pCollision->GetMapIndices(Prev, Core.m_Pos))
			if(DangerIndex(pCollision, Index, Cfg))
				return (float)i; // frozen at tick i: the earlier, the worse
		if(DangerAt(pCollision, Core.m_Pos.x, Core.m_Pos.y, Cfg))
			return (float)i; // off the map is as deadly as it gets
		// How much open space is there around the tee at this moment? Sampled in eight directions, the
		// nearest danger wins — that is the "how far did the rocket actually throw me clear" measure.
		// Only every other tick and in 32px steps: this score only ranks shots that all survived the
		// window anyway, so coarse sampling is plenty and keeps the search cheap enough to fire often.
		if((i & 1) == 0)
		{
			for(int d = 0; d < 8; ++d)
			{
				const vec2 Dir = direction((float)d / 8.0f * 2.0f * pi);
				for(float r = 16.0f; r <= ROCKET_CLEARANCE_MAX; r += 32.0f)
					if(DangerAt(pCollision, Core.m_Pos.x + Dir.x * r, Core.m_Pos.y + Dir.y * r, Cfg))
					{
						Worst = minimum(Worst, r);
						break;
					}
			}
		}
	}
	// Survived the whole window: score above every "frozen at tick i" result, ranked by the room it kept.
	return (float)Ticks + minimum(Worst, ROCKET_CLEARANCE_MAX);
}

CRocketSaveAim CRocketSave::BestAim(CCollision *pCollision, const CCharacterCore &Core, const CNetObj_PlayerInput &Input, const CRocketSaveCfg &Cfg, vec2 Fallback)
{
	CRocketSaveAim Out;
	CCharacterCore Sim = Core;
	Sim.Init(nullptr, pCollision);
	Sim.SetHookedPlayer(-1);

	// What doing NOTHING is worth in the same simulation: the tee run forward with no blast at all
	// (a blast time past the horizon never gets applied). A shot is only accepted when it beats this.
	// The old code fired any direction with a score above zero, which included shots whose own
	// simulation froze a few ticks later — occasionally earlier than doing nothing, i.e. a blast that
	// pushed the tee into freeze instead of out of it.
	Out.m_BaseScore = Outcome(pCollision, Sim, Input, Cfg, ms_Tuning.m_Horizon, Sim.m_Pos, Cfg.m_Lifetime);
	if(Out.m_BaseScore >= (float)ms_Tuning.m_Horizon)
		return Out; // nothing predicted to happen even without a rocket: no shot needed

	// A candidate is only worth anything when the grenade actually detonates against something solid close
	// enough to move us: a ceiling, a floor, a wall. That is the whole point of the rocket save — the blast
	// needs a surface to push off. The minimum kick is what the tee gains in one tick of air control, so a
	// shot that barely tickles it is not treated as a save.
	auto Try = [&](vec2 Dir, vec2 *pOutBlast, float *pOutKick, float *pOutTicks) -> float {
		bool HitSolid = false;
		float BlastTime = Cfg.m_Lifetime;
		const vec2 Blast = BlastPos(pCollision, Sim.m_Pos, Dir, Cfg, &HitSolid, &BlastTime);
		if(pOutBlast)
			*pOutBlast = Blast;
		if(pOutKick)
			*pOutKick = 0.0f;
		if(pOutTicks)
			*pOutTicks = 0.0f;
		if(!HitSolid)
			return 0.0f;
		if(pOutTicks)
			*pOutTicks = BlastTime / TICK_SECONDS;
		// Measure the kick where the tee actually is when the projectile arrives. Using the fire-time
		// position here rejected shots the tee was moving into and accepted shots it had already moved
		// away from. Outcome also applies the current move restrictions at that future tick.
		float Kick = 0.0f;
		const float Score = Outcome(pCollision, Sim, Input, Cfg, ms_Tuning.m_Horizon, Blast, BlastTime, &Kick);
		if(pOutKick)
			*pOutKick = Kick;
		if(Kick < ROCKET_MIN_KICK)
			return 0.0f;
		return Score;
	};

	// What the plain "shoot at the danger" aim would achieve, as the baseline to beat.
	if(length(Fallback) > 0.001f)
	{
		vec2 Blast;
		float Kick = 0.0f;
		Out.m_Dir = normalize(Fallback);
		Out.m_PlainScore = Try(Out.m_Dir, &Blast, &Kick, &Out.m_BlastTicks);
		Out.m_Score = Out.m_PlainScore;
		Out.m_Blast = Blast;
		Out.m_Kick = Kick;
		Out.m_Found = Out.m_PlainScore > 0.0f; // the plain aim only counts if it hits something solid
	}

	// Where are we actually going? The shot has to come from the direction of travel — that is what makes the
	// blast throw us BACK out of the danger we are flying into. Aiming behind us would only push us in
	// harder. The nearest solid surface within that arc is the one that hits hardest, and the tie-break
	// below picks it.
	const float Speed = length(Sim.m_Vel);
	const vec2 MoveDir = Speed > 0.001f ? Sim.m_Vel / Speed : vec2(0.0f, 0.0f);
	for(int i = 0; i < ms_Tuning.m_Rays; ++i)
	{
		const vec2 Dir = direction((float)i / (float)ms_Tuning.m_Rays * 2.0f * pi);
		if(Speed > 1.0f && dot(MoveDir, Dir) < ms_Tuning.m_InertiaDot)
			continue; // behind us: firing there would shove us further into what we are flying at
		vec2 Blast;
		float Kick = 0.0f;
		float BlastTicks = 0.0f;
		const float Score = Try(Dir, &Blast, &Kick, &BlastTicks);
		// Ties go to the harder kick, i.e. to the shot that detonates against the NEAREST solid surface:
		// blast force falls off with distance, so the closest wall, floor or ceiling throws us the furthest.
		if(Score > Out.m_Score || (Score > 0.0f && Score >= Out.m_Score - 0.5f && Kick > Out.m_Kick))
		{
			Out.m_Score = Score;
			Out.m_Dir = Dir;
			Out.m_Blast = Blast;
			Out.m_Kick = Kick;
			Out.m_BlastTicks = BlastTicks;
			Out.m_Found = true;
		}
	}
	// A shot that beats doing nothing is the normal case; the caller may still fire the least-bad valid
	// shot when avoid is out of options entirely, so the two are reported separately.
	Out.m_Improves = Out.m_Found && Out.m_Score > Out.m_BaseScore;
	return Out;
}
