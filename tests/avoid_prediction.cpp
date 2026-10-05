#include <engine/map.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/client/prediction/avoid_planner.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/layers.h>
#include <game/mapbugs.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <sstream>

// A native in-memory map. Tests run the actual collision, character, hook,
// projectile and explosion implementations without graphics or a server.
class CTestMap : public IMap
{
public:
	static constexpr int WIDTH = 32, HEIGHT = 32;
	std::array<CTile, WIDTH * HEIGHT> m_Tiles{};
	std::array<CTile, WIDTH * HEIGHT> m_FrontTiles{};
	CMapItemGroup m_Group{};
	CMapItemLayerTilemap m_Game{}, m_Front{};
	CTestMap()
	{
		m_Group.m_Version = 3;
		m_Group.m_NumLayers = 2;
		m_Game.m_Layer.m_Type = LAYERTYPE_TILES;
		m_Game.m_Version = 3;
		m_Game.m_Width = WIDTH;
		m_Game.m_Height = HEIGHT;
		m_Game.m_Flags = TILESLAYERFLAG_GAME;
		m_Game.m_Data = 0;
		m_Front = m_Game;
		m_Front.m_Flags = TILESLAYERFLAG_FRONT;
		m_Front.m_Data = m_Front.m_Front = 1;
	}
	void *GetData(int Index) override { return Index == 0 ? m_Tiles.data() : m_FrontTiles.data(); }
	int GetDataSize(int Index) const override { return sizeof(m_Tiles); }
	void *GetDataSwapped(int Index) override { return GetData(Index); }
	const char *GetDataString(int Index) override { return ""; }
	void UnloadData(int Index) override {}
	int NumData() const override { return 2; }
	int GetItemSize(int Index) override { return Index == 0 ? sizeof(m_Group) : sizeof(m_Game); }
	void *GetItem(int Index, int *pType = nullptr, int *pId = nullptr) override { return Index == 0 ? (void *)&m_Group : Index == 1 ? (void *)&m_Game :
																	  (void *)&m_Front; }
	void GetType(int Type, int *pStart, int *pNum) override
	{
		*pStart = Type == MAPITEMTYPE_GROUP ? 0 : 1;
		*pNum = Type == MAPITEMTYPE_GROUP ? 1 : Type == MAPITEMTYPE_LAYER ? 2 :
										    0;
	}
	int FindItemIndex(int Type, int Id) override { return -1; }
	void *FindItem(int Type, int Id) override { return nullptr; }
	int NumItems() const override { return 3; }
	bool Load(const char *, IStorage *, const char *, int) override { return false; }
	bool Load(IStorage *, const char *, int) override { return false; }
	void Unload() override {}
	bool IsLoaded() const override { return true; }
	IOHANDLE File() const override { return nullptr; }
	const char *FullName() const override { return "avoid-test"; }
	const char *BaseName() const override { return "avoid-test"; }
	const char *Path() const override { return "avoid-test"; }
	SHA256_DIGEST Sha256() const override { return {}; }
	unsigned Crc() const override { return 0; }
	int Size() const override { return 0; }
};

class CAvoidPrediction : public testing::Test
{
protected:
	CTestMap m_Map;
	CLayers m_Layers;
	CCollision m_Collision;
	std::array<CTuningParams, 256> m_Tuning;
	CMapBugs m_Bugs;
	CGameWorld m_World;
	CAvoidPlanner::SConfig m_Cfg;
	CNetObj_PlayerInput m_Input{};
	void SetUp() override
	{
		g_Config = {};
		g_Config.m_SvFreezeDelay = 3;
		g_Config.m_SvHit = 1;
		for(int x = 0; x < CTestMap::WIDTH; x++)
		{
			m_Map.m_Tiles[17 * CTestMap::WIDTH + x].m_Index = TILE_SOLID;
			m_Map.m_Tiles[16 * CTestMap::WIDTH + x].m_Index = TILE_FREEZE;
			m_Map.m_Tiles[9 * CTestMap::WIDTH + x].m_Index = TILE_SOLID;
		}
		m_Layers.Init(&m_Map, false);
		m_Collision.Init(&m_Layers);
		m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
		m_World.m_GameTick = 100;
		m_World.m_LocalClientId = 0;
		m_World.m_WorldConfig = {};
		m_World.m_WorldConfig.m_IsDDRace = true;
		m_World.m_WorldConfig.m_PredictDDRace = true;
		m_World.m_WorldConfig.m_PredictTiles = true;
		m_World.m_WorldConfig.m_PredictFreeze = 1;
		m_World.m_WorldConfig.m_PredictWeapons = true;
		m_World.m_WorldConfig.m_InfiniteAmmo = true;
		m_Input.m_TargetY = -1000;
		m_Cfg.m_Direction = m_Cfg.m_Jump = false;
		m_Cfg.m_Angles = 12;
		m_Cfg.m_BudgetMs = 0; // deterministic call bound, independent of host speed
		m_Cfg.m_Boost = std::getenv("TCLIENT_AVOID_TEST_BOOST") != nullptr;
		// Optional native map export for the isolated client/server smoke test.
		if(const char *pPath = std::getenv("TCLIENT_AVOID_EXPORT_MAP"))
		{
			auto pStorage = CreateLocalStorage();
			CDataFileWriter Writer;
			ASSERT_TRUE(Writer.Open(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
			// Runtime checks need a real continuous floor/ceiling even after
			// strong boosts travel beyond this small unit-test map's edge.
			constexpr int ExportWidth = 2048;
			std::vector<CTile> Tiles(ExportWidth * CTestMap::HEIGHT), FrontTiles(Tiles.size());
			for(int y = 0; y < CTestMap::HEIGHT; y++)
				for(int x = 0; x < ExportWidth; x++)
					Tiles[y * ExportWidth + x] = m_Map.m_Tiles[y * CTestMap::WIDTH + std::min(x, CTestMap::WIDTH - 1)];
			for(int x = 0; x <= 10; x++)
				Tiles[16 * ExportWidth + x].m_Index = TILE_SOLID;
			Tiles[15 * ExportWidth + 8].m_Index = ENTITY_OFFSET + ENTITY_SPAWN;
			Tiles[15 * ExportWidth + 9].m_Index = ENTITY_OFFSET + ENTITY_WEAPON_GRENADE;
			CMapItemVersion Version{};
			Version.m_Version = 1;
			Writer.AddItem(MAPITEMTYPE_VERSION, 0, sizeof(Version), &Version);
			Writer.AddItem(MAPITEMTYPE_GROUP, 0, sizeof(m_Map.m_Group), &m_Map.m_Group);
			auto Game = m_Map.m_Game;
			Game.m_Width = ExportWidth;
			Game.m_Image = -1;
			Game.m_ColorEnv = -1;
			Game.m_Data = Writer.AddData(Tiles.size() * sizeof(CTile), Tiles.data());
			Writer.AddItem(MAPITEMTYPE_LAYER, 0, sizeof(Game), &Game);
			auto Front = m_Map.m_Front;
			Front.m_Width = ExportWidth;
			Front.m_Image = -1;
			Front.m_ColorEnv = -1;
			Front.m_Data = Front.m_Front = Writer.AddData(FrontTiles.size() * sizeof(CTile), FrontTiles.data());
			Writer.AddItem(MAPITEMTYPE_LAYER, 1, sizeof(Front), &Front);
			Writer.Finish();
		}
	}
	CCharacter *Spawn(int Y, float Vy, bool Grenade = true, int X = 16 * 32)
	{
		m_World.Clear();
		CNetObj_Character Net{};
		Net.m_X = X;
		Net.m_Y = Y;
		Net.m_VelY = round_to_int(Vy * 256);
		Net.m_Weapon = Grenade ? WEAPON_GRENADE : WEAPON_GUN;
		Net.m_AmmoCount = -1;
		Net.m_HookedPlayer = -1;
		CNetObj_DDNetCharacter Extended{};
		Extended.m_Jumps = 0;
		Extended.m_TuneZoneOverride = -1;
		Extended.m_Flags = CHARACTERFLAG_WEAPON_GUN | (Grenade ? CHARACTERFLAG_WEAPON_GRENADE : 0);
		auto *pChar = new CCharacter(&m_World, 0, &Net, &Extended);
		m_World.InsertEntity(pChar);
		pChar->m_GameTeam = 0;
		pChar->SetActiveWeapon(Net.m_Weapon);
		pChar->SetWeaponGot(WEAPON_GRENADE, Grenade);
		for(int i = 0; i < 3; i++)
		{
			pChar->OnDirectInput(&m_Input);
			pChar->OnPredictedInput(&m_Input);
		}
		return pChar;
	}
};

TEST_F(CAvoidPrediction, SafeInputIsUntouched)
{
	Spawn(400, -5);
	CAvoidPlanner P;
	const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
	EXPECT_EQ(D.m_Mode, CAvoidPlanner::EMode::SAFE);
	EXPECT_EQ(D.m_Calls, 1);
}

TEST_F(CAvoidPrediction, NativeFreezeAndFrontFreezeAreDetected)
{
	Spawn(505, 8);
	CAvoidPlanner P;
	CAvoidPlanner::SPlan Natural;
	Natural.m_Input = m_Input;
	EXPECT_GT(P.Simulate(m_World, 0, m_Input, Natural, 12, m_Cfg).m_Danger, 0);
	for(int x = 0; x < CTestMap::WIDTH; x++)
	{
		m_Map.m_Tiles[16 * CTestMap::WIDTH + x].m_Index = 0;
		m_Map.m_FrontTiles[16 * CTestMap::WIDTH + x].m_Index = TILE_FREEZE;
	}
	Spawn(505, 8);
	EXPECT_GT(P.Simulate(m_World, 0, m_Input, Natural, 12, m_Cfg).m_Danger, 0);
}

TEST_F(CAvoidPrediction, FreezeAtTheMapBoundaryIsNotLost)
{
	auto *pChar = Spawn(511, 8, false);
	auto Core = pChar->GetCore();
	Core.m_Pos = pChar->m_Pos = vec2(CTestMap::WIDTH * 32 + 24, 511);
	pChar->SetCore(Core);
	CAvoidPlanner::SPlan Raw;
	Raw.m_Input = m_Input;
	CAvoidPlanner P;
	const auto Probe = P.Simulate(m_World, 0, m_Input, Raw, 3, m_Cfg);
	EXPECT_GT(Probe.m_Danger, 0);
	EXPECT_STREQ(Probe.m_Kind, "freeze");
	EXPECT_EQ(Probe.m_Tile, TILE_FREEZE);
}

TEST_F(CAvoidPrediction, RocketUsesNativeBlastAndBeatsForcedHook)
{
	bool Found = false;
	std::ostringstream Trace;
	for(int Y = 452; Y <= 511; Y++)
	{
		Spawn(Y, 2);
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
		CAvoidPlanner::SPlan Shot;
		Shot.m_Input = m_Input;
		Shot.m_Rocket = true;
		Shot.m_RocketAim = vec2(0, 1000);
		const auto Probe = P.Simulate(m_World, 0, m_Input, Shot, 30, m_Cfg);
		Trace << "Y=" << Y << " mode=" << P.ModeName(D.m_Mode) << " danger=" << D.m_Base.m_Danger << " result=" << D.m_Result.m_Danger << " shot=" << Probe.m_ShotTick << " blast=" << Probe.m_BlastTick << " shot-danger=" << Probe.m_Danger << '\n';
		if(D.m_Mode != CAvoidPlanner::EMode::ROCKET)
			continue;
		Found = true;
		EXPECT_EQ(D.m_Plan.m_HookTicks, -1);
		EXPECT_EQ(D.m_Plan.m_Input.m_Hook, m_Input.m_Hook);
		EXPECT_EQ(D.m_Result.m_Danger, 0);
		EXPECT_GT(D.m_Result.m_BlastTick, 0);
		EXPECT_LE(D.m_Result.m_BlastTick - D.m_Result.m_ShotTick + 1, m_Cfg.m_MaxFlight);
		EXPECT_GT(D.m_Base.m_Danger, 0);
		break;
	}
	EXPECT_TRUE(Found) << Trace.str();
}

TEST_F(CAvoidPrediction, MissingGrenadeNeverProducesRocketDecision)
{
	for(int Y : {460, 480, 500})
	{
		Spawn(Y, 5, false);
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
		EXPECT_NE(D.m_Mode, CAvoidPlanner::EMode::ROCKET);
		EXPECT_NE(D.m_Mode, CAvoidPlanner::EMode::ROCKET_HOOK);
		EXPECT_FALSE(D.m_Plan.m_Rocket);
	}
}

TEST_F(CAvoidPrediction, ReadyRocketCanReplaceAutomaticHookWithAnActualRelease)
{
	m_Input.m_Hook = 1;
	m_Input.m_TargetY = 1000;
	bool Found = false;
	std::ostringstream Trace;
	for(int Y = 470; Y <= 510 && !Found; Y++)
	{
		auto *pChar = Spawn(Y, 2);
		auto Core = pChar->GetCore();
		Core.m_HookState = HOOK_GRABBED;
		Core.m_HookPos = vec2(512, 300);
		pChar->SetCore(Core);
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg, nullptr, true);
		Trace << "Y=" << Y << " mode=" << P.ModeName(D.m_Mode) << " base=" << D.m_Base.m_Danger << " result=" << D.m_Result.m_Danger << " shot=" << D.m_Result.m_ShotTick << " hook=" << D.m_Plan.m_HookTicks << '\n';
		if(D.m_Mode != CAvoidPlanner::EMode::ROCKET)
			continue;
		Found = true;
		ASSERT_EQ(D.m_Result.m_Danger, 0);
		EXPECT_EQ(D.m_Plan.m_HookTicks, 0);
		EXPECT_EQ(P.Simulate(m_World, 0, m_Input, D.m_Plan, m_Cfg.m_Ticks, m_Cfg).m_Danger, 0);
		const auto Shot = CAvoidPlanner::InputAt(D.m_Plan, m_Input, 0);
		EXPECT_EQ(Shot.m_Hook, 0);
		pChar->OnDirectInput(&Shot);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Shot);
		m_World.Tick();
		EXPECT_EQ(pChar->Core()->m_HookState, HOOK_IDLE);
		EXPECT_GT(pChar->GetReloadTimer(), 0);
	}
	EXPECT_TRUE(Found) << Trace.str();
}

TEST_F(CAvoidPrediction, HookOnlyUsesBoundedPulseAndWaitsUntilNecessary)
{
	m_Cfg.m_Rocket = false;
	bool Hook = false, Wait = false;
	std::ostringstream Trace;
	for(int Y = 435; Y <= 510; Y += 3)
	{
		Spawn(Y, 4, false);
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
		Wait |= D.m_Mode == CAvoidPlanner::EMode::WAIT;
		Trace << "Y=" << Y << " mode=" << P.ModeName(D.m_Mode) << " base=" << D.m_Base.m_Danger << " result=" << D.m_Result.m_Danger << " pulse=" << D.m_Plan.m_HookTicks << " sims=" << D.m_Calls << "\n";
		if(D.m_Mode != CAvoidPlanner::EMode::HOOK)
			continue;
		Hook = true;
		EXPECT_GT(D.m_Plan.m_HookTicks, 0);
		EXPECT_EQ(D.m_Result.m_Danger, 0);
		EXPECT_FALSE(D.m_CanWait);
		EXPECT_EQ(CAvoidPlanner::InputAt(D.m_Plan, m_Input, D.m_Plan.m_HookTicks).m_Hook, 0);
		if(D.m_Plan.m_HookTicks > 1)
		{
			auto Shorter = D.m_Plan;
			Shorter.m_HookTicks--;
			EXPECT_GT(P.Simulate(m_World, 0, m_Input, Shorter, m_Cfg.m_Ticks, m_Cfg).m_Danger, 0);
		}
	}
	EXPECT_TRUE(Hook) << Trace.str();
	EXPECT_TRUE(Wait);
}

TEST_F(CAvoidPrediction, SpeculationDoesNotChangeLiveWorld)
{
	auto *pChar = Spawn(480, 8);
	const auto Core = pChar->GetCore();
	const int Attack = pChar->GetAttackTick();
	CAvoidPlanner P;
	P.Decide(m_World, 0, m_Input, m_Cfg);
	EXPECT_EQ(m_World.GameTick(), 100);
	EXPECT_EQ(pChar->Core()->m_Pos, Core.m_Pos);
	EXPECT_EQ(pChar->Core()->m_Vel, Core.m_Vel);
	EXPECT_EQ(pChar->GetAttackTick(), Attack);
	EXPECT_EQ(m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE), nullptr);
}

TEST_F(CAvoidPrediction, CombinedAimSequenceAndCounterWrap)
{
	CAvoidPlanner::SPlan Plan;
	Plan.m_Input = m_Input;
	Plan.m_Rocket = true;
	Plan.m_RocketAim = vec2(0, 1000);
	Plan.m_HookTicks = 3;
	m_Input.m_Fire = INPUT_STATE_MASK - 1;
	const auto Shot = CAvoidPlanner::InputAt(Plan, m_Input, 0);
	const auto Hook = CAvoidPlanner::InputAt(Plan, m_Input, 1);
	EXPECT_EQ(Shot.m_Hook, 0);
	EXPECT_EQ(Shot.m_TargetY, 1000);
	EXPECT_EQ(Shot.m_Fire, INPUT_STATE_MASK);
	EXPECT_EQ(Hook.m_Hook, 1);
	EXPECT_EQ(Hook.m_TargetY, -1000);
	EXPECT_EQ(Hook.m_Fire, 0);
	EXPECT_EQ(CAvoidPlanner::InputAt(Plan, m_Input, 4).m_Hook, 0);
}

TEST_F(CAvoidPrediction, RocketDoesNotForceHookForDistantFutureDanger)
{
	Spawn(504, 8);
	CAvoidPlanner P;
	const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
	ASSERT_EQ(D.m_Mode, CAvoidPlanner::EMode::ROCKET);
	EXPECT_EQ(D.m_Result.m_Danger, 0);
	EXPECT_EQ(D.m_Plan.m_HookTicks, -1);
}

TEST_F(CAvoidPrediction, RealCombinedRescueWhenNeitherAloneSurvives)
{
	bool Found = false;
	for(int Strength : {2, 3, 4, 5, 6})
	{
		m_Tuning[0].m_ExplosionStrength = Strength;
		for(int Y = 492; Y <= 506 && !Found; Y += 2)
			for(int Vy : {4, 6, 8, 10, 12})
			{
				Spawn(Y, Vy);
				m_Cfg.m_Rocket = m_Cfg.m_Hook = true;
				CAvoidPlanner P;
				const auto Both = P.Decide(m_World, 0, m_Input, m_Cfg);
				if(Both.m_Mode != CAvoidPlanner::EMode::ROCKET_HOOK)
					continue;
				m_Cfg.m_Rocket = false;
				const auto Hook = P.Decide(m_World, 0, m_Input, m_Cfg);
				m_Cfg.m_Rocket = true;
				m_Cfg.m_Hook = false;
				const auto Rocket = P.Decide(m_World, 0, m_Input, m_Cfg);
				if(!Hook.m_Result.m_Danger || Rocket.m_Plan.m_Rocket)
					continue;
				Found = true;
				EXPECT_EQ(Both.m_Result.m_Danger, 0);
				EXPECT_GT(Both.m_Result.m_ShotTick, 0);
				EXPECT_GT(Both.m_Result.m_BlastTick, 0);
				EXPECT_GT(Both.m_Plan.m_HookTicks, 0);
				break;
			}
		if(Found)
			break;
	}
	EXPECT_TRUE(Found);
}

TEST_F(CAvoidPrediction, EmptyAmmoAndLongFlightAreRejected)
{
	auto *pChar = Spawn(490, 2);
	pChar->SetWeaponAmmo(WEAPON_GRENADE, 0);
	CAvoidPlanner P;
	EXPECT_FALSE(P.Decide(m_World, 0, m_Input, m_Cfg).m_Plan.m_Rocket);
	Spawn(480, 10);
	m_Cfg.m_MaxFlight = 1;
	const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
	EXPECT_FALSE(D.m_Plan.m_Rocket);
	EXPECT_GT(D.m_RejectFlight, 0);
}

TEST_F(CAvoidPrediction, SwitchToGrenadeBeforePressingFire)
{
	auto *pChar = Spawn(504, 2);
	pChar->SetActiveWeapon(WEAPON_GUN);
	CAvoidPlanner P;
	const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
	ASSERT_TRUE(D.m_Plan.m_Rocket);
	EXPECT_EQ(D.m_Plan.m_SwitchTicks, 1);
	EXPECT_EQ(D.m_Result.m_ShotTick, 2);
	EXPECT_EQ(CAvoidPlanner::InputAt(D.m_Plan, m_Input, 0).m_Fire & 1, 0);
	EXPECT_EQ(CAvoidPlanner::InputAt(D.m_Plan, m_Input, 1).m_Fire & 1, 1);
}

TEST_F(CAvoidPrediction, RocketReturnsMovementToPlayerAfterShot)
{
	CAvoidPlanner::SPlan Plan;
	Plan.m_Input = m_Input;
	Plan.m_Input.m_Direction = 1;
	Plan.m_Rocket = true;
	Plan.m_MoveTicks = 1;
	m_Input.m_Direction = -1;
	EXPECT_EQ(CAvoidPlanner::InputAt(Plan, m_Input, 0).m_Direction, 1);
	EXPECT_EQ(CAvoidPlanner::InputAt(Plan, m_Input, 1).m_Direction, -1);
}

TEST_F(CAvoidPrediction, AutomaticWeaponReturnWaitsForShotAndNativeReload)
{
	auto *pChar = Spawn(480, 0);
	pChar->SetActiveWeapon(WEAPON_GUN);
	CAvoidPlanner::SWeaponReturn Return;
	CAvoidPlanner::BeginWeaponReturn(Return, pChar->GetActiveWeapon(), 0, 12);
	CAvoidPlanner::SPlan Shot;
	Shot.m_Input = m_Input;
	Shot.m_Rocket = true;
	Shot.m_RocketAim = vec2(0, 1000);
	Shot.m_SwitchTicks = 1;
	const int OldAttack = pChar->GetAttackTick();
	for(int t = 0; t < 2; t++)
	{
		const auto Input = CAvoidPlanner::InputAt(Shot, m_Input, t);
		pChar->OnDirectInput(&Input);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Input);
		m_World.Tick();
	}
	ASSERT_EQ(pChar->GetActiveWeapon(), WEAPON_GRENADE);
	ASSERT_GT(pChar->GetAttackTick(), OldAttack);
	ASSERT_GT(pChar->GetReloadTimer(), 1);
	// A predicted attack alone must not replace the weapon in an unreceived
	// fire packet. Queue the return only after authoritative acknowledgement.
	EXPECT_EQ(CAvoidPlanner::WeaponReturnInput(Return, WEAPON_GRENADE, 12, false), 0);
	m_Input.m_Fire = 2;
	m_Input.m_WantedWeapon = CAvoidPlanner::WeaponReturnInput(Return, WEAPON_GRENADE, 12, true);
	ASSERT_EQ(m_Input.m_WantedWeapon, WEAPON_GUN + 1);
	const int ShotAttack = pChar->GetAttackTick();
	bool Returned = false;
	for(int t = 0; t < 35; t++)
	{
		const int Reload = pChar->GetReloadTimer();
		pChar->OnDirectInput(&m_Input);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&m_Input);
		m_World.Tick();
		EXPECT_EQ(pChar->GetAttackTick(), ShotAttack);
		if(Reload > 1)
			EXPECT_EQ(pChar->GetActiveWeapon(), WEAPON_GRENADE);
		if(pChar->GetActiveWeapon() == WEAPON_GUN)
		{
			Returned = true;
			EXPECT_EQ(CAvoidPlanner::WeaponReturnInput(Return, WEAPON_GUN, 12, false), 0);
			EXPECT_EQ(Return.m_PreviousWeapon, -1);
			break;
		}
	}
	EXPECT_TRUE(Returned);
}

TEST_F(CAvoidPrediction, ManualGrenadeAndNewWeaponChoiceCancelAutomaticReturn)
{
	CAvoidPlanner::SWeaponReturn Return;
	CAvoidPlanner::BeginWeaponReturn(Return, WEAPON_GRENADE, 0, 12);
	EXPECT_EQ(CAvoidPlanner::WeaponReturnInput(Return, WEAPON_GRENADE, 12, true), 0);
	CAvoidPlanner::BeginWeaponReturn(Return, WEAPON_GUN, WEAPON_GRENADE + 1, 12);
	EXPECT_EQ(CAvoidPlanner::WeaponReturnInput(Return, WEAPON_GRENADE, 12, true), 0);
	// Includes explicitly choosing the grenade again while it is already
	// selected by avoid, and wheel input whose wanted weapon is zero.
	for(int ManualChoice : {WEAPON_GRENADE, WEAPON_HAMMER, WEAPON_GUN})
	{
		CAvoidPlanner::BeginWeaponReturn(Return, WEAPON_GUN, 0, 12);
		EXPECT_EQ(CAvoidPlanner::WeaponReturnInput(Return, ManualChoice, 13, true), 0);
		EXPECT_EQ(Return.m_PreviousWeapon, -1);
	}
}

TEST_F(CAvoidPrediction, ReplanningCompletesGrenadeWarmupAfterReturningToGun)
{
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Direction = true;
	m_Input.m_Direction = 1;
	m_Input.m_WantedWeapon = WEAPON_GUN + 1;
	auto *pChar = Spawn(504, 2);
	pChar->SetActiveWeapon(WEAPON_GUN);
	// Native direct weapon switching reads the preceding predicted input.
	// Reproduce the persistent gun selection left by automatic weapon return.
	pChar->SetInput(&m_Input);
	CAvoidPlanner P;
	CAvoidPlanner::SFeedback Feedback;
	const auto Warmup = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
	ASSERT_EQ(Warmup.m_Decision.m_Mode, CAvoidPlanner::EMode::ROCKET);
	ASSERT_EQ(Warmup.m_Decision.m_Plan.m_SwitchTicks, 1);
	ASSERT_EQ(Warmup.m_Input.m_Fire & 1, 0);
	pChar->OnDirectInput(&Warmup.m_Input);
	m_World.m_GameTick++;
	pChar->OnPredictedInput(&Warmup.m_Input);
	m_World.Tick();
	ASSERT_EQ(pChar->GetActiveWeapon(), WEAPON_GUN);
	m_Input.m_WantedWeapon = Warmup.m_Input.m_WantedWeapon;
	const auto Fire = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
	ASSERT_EQ(Fire.m_Decision.m_Mode, CAvoidPlanner::EMode::ROCKET);
	EXPECT_EQ(Fire.m_Decision.m_Plan.m_SwitchTicks, 0);
	EXPECT_EQ(Fire.m_Input.m_Fire & 1, 1);
	pChar->OnDirectInput(&Fire.m_Input);
	m_World.m_GameTick++;
	pChar->OnPredictedInput(&Fire.m_Input);
	m_World.Tick();
	EXPECT_EQ(pChar->GetActiveWeapon(), WEAPON_GRENADE);
	EXPECT_GT(pChar->GetReloadTimer(), 0);
	EXPECT_EQ(pChar->m_FreezeTime, 0);
}

TEST_F(CAvoidPrediction, NativeReloadBlocksSecondRescueShot)
{
	auto *pChar = Spawn(490, 2);
	CAvoidPlanner::SPlan Shot;
	Shot.m_Input = m_Input;
	Shot.m_Rocket = true;
	Shot.m_RocketAim = vec2(0, 1000);
	const auto Fire = CAvoidPlanner::InputAt(Shot, m_Input, 0);
	pChar->OnDirectInput(&Fire);
	m_World.m_GameTick++;
	pChar->OnPredictedInput(&Fire);
	m_World.Tick();
	ASSERT_GT(pChar->GetReloadTimer(), 1);
	// A new fall during the same reload must use a non-rocket fallback.
	while(auto *pEnt = m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE))
		delete pEnt;
	auto Core = pChar->GetCore();
	Core.m_Pos = pChar->m_Pos = vec2(512, 500);
	Core.m_Vel = vec2(0, 8);
	pChar->SetCore(Core);
	m_Input.m_Fire = 2;
	CAvoidPlanner P;
	const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
	EXPECT_GT(D.m_Base.m_Danger, 0);
	EXPECT_FALSE(D.m_Plan.m_Rocket);
	EXPECT_EQ(D.m_RocketCandidates, 0);
	EXPECT_EQ(P.Simulate(m_World, 0, m_Input, Shot, 12, m_Cfg).m_ShotTick, 0);
}

TEST_F(CAvoidPrediction, FiniteHookReleaseReturnsPhysicalInputInSimulation)
{
	m_Input.m_Hook = 1;
	m_Input.m_Direction = -1;
	CAvoidPlanner::SPlan Move;
	Move.m_Input = m_Input;
	Move.m_Input.m_Hook = 0;
	Move.m_Input.m_Direction = 1;
	Move.m_MoveTicks = 3;
	EXPECT_EQ(CAvoidPlanner::Duration(Move), 3);
	for(int t = 0; t < 3; t++)
	{
		EXPECT_EQ(CAvoidPlanner::InputAt(Move, m_Input, t).m_Hook, 0);
		EXPECT_EQ(CAvoidPlanner::InputAt(Move, m_Input, t).m_Direction, 1);
	}
	EXPECT_EQ(CAvoidPlanner::InputAt(Move, m_Input, 3).m_Hook, 1);
	EXPECT_EQ(CAvoidPlanner::InputAt(Move, m_Input, 3).m_Direction, -1);
}

TEST_F(CAvoidPrediction, AttachedHookIsReleasedBeforeChangingAim)
{
	auto *pChar = Spawn(440, 3, false);
	auto Core = pChar->GetCore();
	Core.m_HookState = HOOK_GRABBED;
	Core.m_HookPos = vec2(512, 550);
	pChar->SetCore(Core);
	m_Input.m_Hook = 1;
	m_Input.m_TargetY = 1000;
	CAvoidPlanner::SPlan Plan;
	Plan.m_Input = m_Input;
	Plan.m_Input.m_TargetY = -1000;
	Plan.m_HookDelay = 1;
	Plan.m_HookTicks = 3;
	Plan.m_MoveTicks = 5;
	const auto Release = CAvoidPlanner::InputAt(Plan, m_Input, 0);
	pChar->OnDirectInput(&Release);
	m_World.m_GameTick++;
	pChar->OnPredictedInput(&Release);
	m_World.Tick();
	EXPECT_EQ(pChar->Core()->m_HookState, HOOK_IDLE);
	const auto Launch = CAvoidPlanner::InputAt(Plan, m_Input, 1);
	pChar->OnDirectInput(&Launch);
	m_World.m_GameTick++;
	pChar->OnPredictedInput(&Launch);
	m_World.Tick();
	EXPECT_EQ(pChar->Core()->m_HookState, HOOK_FLYING);
	EXPECT_LT(pChar->Core()->m_HookDir.y, 0);
	EXPECT_EQ(CAvoidPlanner::InputAt(Plan, m_Input, 4).m_Hook, 0);
	EXPECT_EQ(CAvoidPlanner::InputAt(Plan, m_Input, 5).m_Hook, 1);
}

TEST_F(CAvoidPrediction, RemainingHookAndRocketSequenceMatchesTheExecutedPlan)
{
	CAvoidPlanner::SPlan Plan;
	Plan.m_Input = m_Input;
	Plan.m_Input.m_Hook = 1;
	Plan.m_Input.m_TargetX = 800;
	Plan.m_Input.m_TargetY = -600;
	Plan.m_HookDelay = 1;
	Plan.m_HookTicks = 7;
	Plan.m_MoveTicks = 9;
	for(bool Rocket : {false, true})
	{
		Plan.m_Rocket = Rocket;
		for(int Elapsed = Rocket ? 1 : 0; Elapsed < CAvoidPlanner::Duration(Plan); Elapsed++)
		{
			const auto Remaining = CAvoidPlanner::RemainingPlan(Plan, m_Input, Elapsed);
			EXPECT_FALSE(Remaining.m_Rocket);
			for(int t = 0; t < 12; t++)
			{
				const auto Whole = CAvoidPlanner::InputAt(Plan, m_Input, Elapsed + t);
				const auto Rest = CAvoidPlanner::InputAt(Remaining, m_Input, t);
				EXPECT_EQ(Rest.m_Hook, Whole.m_Hook) << "rocket=" << Rocket << " elapsed=" << Elapsed << " t=" << t;
				EXPECT_EQ(Rest.m_Direction, Whole.m_Direction);
				EXPECT_EQ(Rest.m_TargetX, Whole.m_TargetX);
				EXPECT_EQ(Rest.m_TargetY, Whole.m_TargetY);
				EXPECT_EQ(Rest.m_Fire, m_Input.m_Fire);
			}
		}
	}
}

TEST_F(CAvoidPrediction, ReplanningDoesNotCancelTheRescueHookInFlight)
{
	m_Cfg.m_Rocket = false;
	m_Input.m_Hook = 1;
	m_Input.m_TargetY = 1000;
	auto *pChar = Spawn(500, 3, false);
	auto Core = pChar->GetCore();
	Core.m_HookState = HOOK_FLYING;
	Core.m_HookDir = vec2(0, -255.0f / 256); // Network quantization does not preserve unit length.
	Core.m_HookPos = vec2(512, 400);
	pChar->SetCore(Core);
	CAvoidPlanner P;
	const auto D = P.Decide(m_World, 0, m_Input, m_Cfg, nullptr, true);
	ASSERT_EQ(D.m_Mode, CAvoidPlanner::EMode::HOOK);
	EXPECT_EQ(D.m_Plan.m_HookDelay, 0);
	EXPECT_EQ(D.m_Result.m_Danger, 0);
	const auto Continue = CAvoidPlanner::InputAt(D.m_Plan, m_Input, 0);
	EXPECT_EQ(Continue.m_Hook, 1);
	pChar->OnDirectInput(&Continue);
	m_World.m_GameTick++;
	pChar->OnPredictedInput(&Continue);
	m_World.Tick();
	EXPECT_NE(pChar->Core()->m_HookState, HOOK_IDLE);
	EXPECT_LT(pChar->Core()->m_HookPos.y, 400);
	EXPECT_EQ(pChar->m_FreezeTime, 0);
}

TEST_F(CAvoidPrediction, PredictionCorrectionDoesNotReplayGrenadeImpact)
{
	g_Config.m_ClPredictEvents = 1;
	g_Config.m_SndEnable = 1;
	m_World.m_WorldConfig.m_PredictEvents = true;
	m_World.CreatePredictedExplosionEvent(vec2(512, 540), 90, 0);
	ASSERT_EQ(m_World.m_PredictedEvents.size(), 1u);
	m_World.m_PredictedEvents.front().m_Handled = true;
	m_World.m_GameTick++;
	m_World.CreatePredictedExplosionEvent(vec2(514, 540), 90, 0);
	EXPECT_EQ(m_World.m_PredictedEvents.size(), 1u);
	EXPECT_TRUE(m_World.CheckPredictedEventHandled(CGameWorld::CPredictedEvent(NETEVENTTYPE_EXPLOSION, vec2(512, 540), -1, 110)));
	m_World.CreatePredictedExplosionEvent(vec2(512, 540), 90, 0);
	EXPECT_EQ(m_World.m_PredictedEvents.size(), 1u);
	EXPECT_TRUE(m_World.m_PredictedEvents.front().m_Handled);
	// Two players may fire at the same tick; their impacts remain distinct.
	m_World.CreatePredictedExplosionEvent(vec2(512, 540), 90, 1);
	EXPECT_EQ(m_World.m_PredictedEvents.size(), 2u);
	m_World.CreatePredictedSound(vec2(512, 540), SOUND_GRENADE_EXPLODE, 90, 0);
	m_World.m_GameTick++;
	m_World.CreatePredictedSound(vec2(513, 540), SOUND_GRENADE_EXPLODE, 90, 0);
	EXPECT_EQ(m_World.m_PredictedEvents.size(), 3u);
}

TEST_F(CAvoidPrediction, SpeculativeRocketNeverEmitsOrCopiesLiveEffects)
{
	Spawn(490, 2);
	g_Config.m_ClPredictEvents = 1;
	m_World.m_WorldConfig.m_PredictEvents = true;
	m_World.CreatePredictedExplosionEvent(vec2(50, 50), 80, 1);
	CGameWorld Copy;
	Copy.CopyWorldClean(&m_World, false);
	EXPECT_TRUE(Copy.m_PredictedEvents.empty());
	CAvoidPlanner::SPlan Shot;
	Shot.m_Input = m_Input;
	Shot.m_Rocket = true;
	Shot.m_RocketAim = vec2(0, 1000);
	CAvoidPlanner P;
	const auto Probe = P.Simulate(m_World, 0, m_Input, Shot, 30, m_Cfg);
	EXPECT_GT(Probe.m_BlastTick, 0);
	ASSERT_EQ(m_World.m_PredictedEvents.size(), 1u);
	EXPECT_EQ(m_World.m_PredictedEvents.front().m_Id, 80);
	EXPECT_EQ(m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE), nullptr);
}

TEST_F(CAvoidPrediction, DifficultSearchRespectsCallBudget)
{
	Spawn(510, 30);
	m_Input.m_Hook = 1;
	m_Cfg.m_Direction = m_Cfg.m_Jump = true;
	m_Cfg.m_MaxCalls = 1;
	CAvoidPlanner P;
	const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
	EXPECT_LE(D.m_Calls, m_Cfg.m_MaxCalls);
	EXPECT_TRUE(D.m_Limited);
}

TEST_F(CAvoidPrediction, WaitingRequiresAnExecutableDelayedRescue)
{
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Ticks = 20;
	// A higher wall needs several flight ticks, so acting only inside a
	// universal four-tick window cannot rescue this approach.
	for(int x = 0; x < CTestMap::WIDTH; x++)
	{
		m_Map.m_Tiles[9 * CTestMap::WIDTH + x].m_Index = TILE_AIR;
		m_Map.m_Tiles[5 * CTestMap::WIDTH + x].m_Index = TILE_SOLID;
	}
	m_Collision.Init(&m_Layers);
	m_Input.m_TargetY = 1000;
	bool CheckedHook = false, CheckedWalking = false, LongHook = false;
	for(int Y = 425; Y < 500; Y++)
	{
		for(int Hook : {0, 1})
		{
			Spawn(Y, 4, false);
			m_Input.m_Hook = Hook;
			m_Input.m_Direction = 1;
			CAvoidPlanner P;
			const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
			if(D.m_Mode == CAvoidPlanner::EMode::WAIT)
			{
				auto Delayed = D.m_Plan;
				Delayed.m_Delay = 1;
				EXPECT_EQ(P.Simulate(m_World, 0, m_Input, Delayed, m_Cfg.m_Ticks + 1, m_Cfg).m_Danger, 0);
				Delayed.m_Delay = 2;
				EXPECT_EQ(P.Simulate(m_World, 0, m_Input, Delayed, m_Cfg.m_Ticks + 2, m_Cfg).m_Danger, 0);
				EXPECT_EQ(CAvoidPlanner::InputAt(Delayed, m_Input, 0).m_Hook, Hook);
				EXPECT_EQ(CAvoidPlanner::InputAt(Delayed, m_Input, 0).m_Direction, 1);
				(Hook ? CheckedHook : CheckedWalking) = true;
			}
			else if(D.m_Mode == CAvoidPlanner::EMode::HOOK)
			{
				EXPECT_EQ(D.m_Result.m_Danger, 0);
				LongHook |= D.m_Base.m_Danger > 4;
			}
		}
	}
	EXPECT_TRUE(CheckedHook);
	EXPECT_TRUE(CheckedWalking);
	EXPECT_TRUE(LongHook); // A distant wall may require more than four flight ticks.
}

TEST_F(CAvoidPrediction, ReturningManualHookChecksActualReleaseAndRelaunch)
{
	m_Cfg.m_Rocket = false;
	m_Input.m_Hook = 1;
	m_Input.m_TargetY = 1000;
	bool Found = false;
	for(int Y = 420; Y <= 490 && !Found; Y += 2)
	{
		auto *pChar = Spawn(Y, 4, false);
		CAvoidPlanner::SPlan Hook;
		Hook.m_Input = m_Input;
		Hook.m_Input.m_TargetY = -1000;
		Hook.m_HookTicks = 12;
		Hook.m_MoveTicks = 13;
		const auto Launch = CAvoidPlanner::InputAt(Hook, m_Input, 0);
		pChar->OnDirectInput(&Launch);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Launch);
		m_World.Tick();
		ASSERT_TRUE(pChar->Core()->m_HookState == HOOK_FLYING || pChar->Core()->m_HookState == HOOK_GRABBED);
		CAvoidPlanner P;
		CAvoidPlanner::SPlan OldProbe;
		OldProbe.m_Input = m_Input;
		const auto ContinuingAuto = P.Simulate(m_World, 0, m_Input, OldProbe, 12, m_Cfg);
		const auto Return = CAvoidPlanner::ReturnControlPlan(m_Input, true);
		const auto Released = P.Simulate(m_World, 0, m_Input, Return, 12, m_Cfg);
		if(ContinuingAuto.m_Danger || !Released.m_Danger)
			continue;
		Found = true;
		EXPECT_EQ(CAvoidPlanner::InputAt(Return, m_Input, 0).m_Hook, 0);
		EXPECT_EQ(CAvoidPlanner::InputAt(Return, m_Input, 1).m_Hook, 1);
		const auto Release = CAvoidPlanner::InputAt(Return, m_Input, 0);
		pChar->OnDirectInput(&Release);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Release);
		m_World.Tick();
		EXPECT_EQ(pChar->Core()->m_HookState, HOOK_IDLE);
		const auto Manual = CAvoidPlanner::InputAt(Return, m_Input, 1);
		pChar->OnDirectInput(&Manual);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Manual);
		m_World.Tick();
		EXPECT_GT(pChar->Core()->m_HookDir.y, 0);
	}
	EXPECT_TRUE(Found) << "Must expose the false-safe forecast caused by keeping the automatic hook";
}

TEST_F(CAvoidPrediction, PartialRescuesAreExplicitAndOnlyUsedForImmediateDanger)
{
	m_Cfg.m_Rocket = false;
	bool Improved = false;
	for(int Y = 485; Y <= 510; Y++)
	{
		Spawn(Y, 8, false);
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
		if(D.m_Result.m_Danger)
		{
			if(D.m_Mode == CAvoidPlanner::EMode::BEST_EFFORT)
			{
				EXPECT_LE(D.m_Base.m_Danger, 4);
				EXPECT_GE(D.m_Result.m_Survival, D.m_Base.m_Survival + 2);
			}
			else
				EXPECT_EQ(D.m_Mode, CAvoidPlanner::EMode::NO_SOLUTION);
			Improved |= D.m_Result.m_Survival > D.m_Base.m_Survival;
		}
	}
	EXPECT_TRUE(Improved);
}

TEST_F(CAvoidPrediction, AiPManualCeilingHookApproachFromUserLog)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP to the downloaded AiP-Gores.map";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Direction = m_Cfg.m_Jump = true;
	m_Input.m_Direction = -1;
	m_Input.m_Hook = 1;
	m_Input.m_TargetX = -46;
	m_Input.m_TargetY = -175;
	auto *pChar = Spawn(256, 2.27f, false, 1041);
	auto Core = pChar->GetCore();
	Core.m_Pos = pChar->m_Pos = vec2(1041, 256);
	Core.m_Vel = vec2(-14.56f, 2.27f);
	Core.m_HookPos = vec2(1024, 159);
	Core.m_HookState = HOOK_GRABBED;
	pChar->SetCore(Core);
	CAvoidPlanner Planner;
	const auto First = Planner.Decide(m_World, 0, m_Input, m_Cfg);
	ASSERT_TRUE(!First.m_Base.m_Danger || First.m_Base.m_Danger > 4);
	EXPECT_EQ(First.m_Mode, First.m_Base.m_Danger ? CAvoidPlanner::EMode::WAIT : CAvoidPlanner::EMode::SAFE);
	if(First.m_Mode == CAvoidPlanner::EMode::WAIT)
	{
		auto Delayed = First.m_Plan;
		Delayed.m_Delay = 1;
		EXPECT_EQ(CAvoidPlanner::InputAt(Delayed, m_Input, 0).m_Hook, m_Input.m_Hook);
		EXPECT_EQ(Planner.Simulate(m_World, 0, m_Input, Delayed, m_Cfg.m_Ticks + 1, m_Cfg).m_Danger, 0);
	}
	CAvoidPlanner::SPlan Held;
	int HeldStep = 0;
	bool Intervened = false;
	std::ostringstream Trace;
	for(int t = 0; t < 25; t++)
	{
		auto Input = m_Input;
		if(HeldStep < CAvoidPlanner::Duration(Held))
			Input = CAvoidPlanner::InputAt(Held, m_Input, HeldStep++);
		else
		{
			const auto D = Planner.Decide(m_World, 0, m_Input, m_Cfg);
			Trace << "t=" << t << " pos=" << pChar->m_Pos.x << ',' << pChar->m_Pos.y << " mode=" << Planner.ModeName(D.m_Mode) << " danger=" << D.m_Base.m_Danger << " result=" << D.m_Result.m_Danger << '\n';
			if(D.m_Mode != CAvoidPlanner::EMode::SAFE && D.m_Mode != CAvoidPlanner::EMode::WAIT && D.m_Mode != CAvoidPlanner::EMode::NO_SOLUTION)
			{
				Intervened = true;
				EXPECT_EQ(D.m_Result.m_Danger, 0);
				Held = D.m_Plan;
				HeldStep = 1;
				Input = CAvoidPlanner::InputAt(Held, m_Input, 0);
			}
		}
		pChar->OnDirectInput(&Input);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Input);
		m_World.Tick();
		ASSERT_EQ(pChar->m_FreezeTime, 0) << Trace.str();
	}
	EXPECT_TRUE(Intervened || !First.m_Base.m_Danger) << Trace.str();
}

TEST_F(CAvoidPrediction, AiPAutomaticHookMustNotBeReleasedAfterTwoTicks)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP to the downloaded AiP-Gores.map";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Direction = true;
	m_Input.m_Direction = 1;
	m_Input.m_Hook = 1;
	m_Input.m_TargetX = -52;
	m_Input.m_TargetY = -80;
	struct SApproach
	{
		vec2 m_Pos, m_Vel, m_OldHook, m_RawAim, m_RescueAim;
		int m_Direction, m_Pulse;
	};
	// Logged failing approaches: 1767580 (floor), 1768272 / 1767974
	// (ceiling). A validated hook was cut short at step two in each case.
	for(const auto &Case : {
		    SApproach{{983, 429}, {5.05f, 6.79f}, {635, 384}, {-52, -80}, {-185, -983}, 1, 7},
		    SApproach{{1034, 254}, {10.91f, -9.79f}, {936, 159}, {-130, -73}, {-325, 946}, 1, 10},
		    SApproach{{1117, 264}, {3.54f, -12.04f}, {889, 63}, {18, -214}, {-244, 970}, -1, 12}})
	{
		SCOPED_TRACE(::testing::Message() << "pos=" << Case.m_Pos.x << ',' << Case.m_Pos.y);
		m_Input.m_Direction = Case.m_Direction;
		m_Input.m_TargetX = round_to_int(Case.m_RawAim.x);
		m_Input.m_TargetY = round_to_int(Case.m_RawAim.y);
		auto *pChar = Spawn(round_to_int(Case.m_Pos.y), Case.m_Vel.y, false, round_to_int(Case.m_Pos.x));
		auto Core = pChar->GetCore();
		Core.m_Pos = pChar->m_Pos = Case.m_Pos;
		Core.m_Vel = Case.m_Vel;
		Core.m_HookState = HOOK_GRABBED;
		Core.m_HookPos = Case.m_OldHook;
		Core.SetHookedPlayer(-1);
		pChar->SetCore(Core);
		CAvoidPlanner::SPlan Hook;
		Hook.m_Input = m_Input;
		Hook.m_Input.m_TargetX = round_to_int(Case.m_RescueAim.x);
		Hook.m_Input.m_TargetY = round_to_int(Case.m_RescueAim.y);
		Hook.m_HookDelay = 1;
		Hook.m_HookTicks = Case.m_Pulse;
		Hook.m_MoveTicks = Case.m_Pulse + 2;
		CAvoidPlanner P;
		ASSERT_EQ(P.Simulate(m_World, 0, m_Input, Hook, 20, m_Cfg).m_Danger, 0);
		for(int t = 0; t < 2; t++)
		{
			const auto Input = CAvoidPlanner::InputAt(Hook, m_Input, t);
			pChar->OnDirectInput(&Input);
			m_World.m_GameTick++;
			pChar->OnPredictedInput(&Input);
			m_World.Tick();
		}
		ASSERT_EQ(pChar->Core()->m_HookState, HOOK_FLYING);
		CAvoidPlanner::SPlan OldProbe;
		OldProbe.m_Input = m_Input;
		EXPECT_EQ(P.Simulate(m_World, 0, m_Input, OldProbe, 20, m_Cfg).m_Danger, 0);
		const auto Return = CAvoidPlanner::ReturnControlPlan(m_Input, true);
		EXPECT_GT(P.Simulate(m_World, 0, m_Input, Return, 20, m_Cfg).m_Danger, 0);
		for(int t = 2; t < 20; t++)
		{
			const auto Input = CAvoidPlanner::InputAt(Hook, m_Input, t);
			pChar->OnDirectInput(&Input);
			m_World.m_GameTick++;
			pChar->OnPredictedInput(&Input);
			m_World.Tick();
			ASSERT_EQ(pChar->m_FreezeTime, 0) << "step=" << t;
		}
	}
}

TEST_F(CAvoidPrediction, FeedbackDoesNotCacheOldMovementOrKeepAnUnneededHook)
{
	auto *pChar = Spawn(400, -1, false);
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Direction = true;
	m_Input.m_Direction = -1;
	CAvoidPlanner::SFeedback Feedback;
	Feedback.m_HasPrevious = Feedback.m_OwnHook = true;
	Feedback.m_Previous.m_Input = m_Input;
	Feedback.m_Previous.m_Input.m_Direction = 1;
	Feedback.m_Previous.m_HookTicks = 20;
	Feedback.m_Previous.m_MoveTicks = 20;
	auto Core = pChar->GetCore();
	Core.m_HookState = HOOK_GRABBED;
	Core.m_HookPos = vec2(512, 300);
	Core.m_HookDir = vec2(0, -1);
	pChar->SetCore(Core);
	CAvoidPlanner P;
	const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
	EXPECT_EQ(Step.m_Input.m_Direction, -1);
	EXPECT_EQ(Step.m_Input.m_Hook, 0);
	EXPECT_FALSE(Feedback.m_OwnHook);
}

TEST_F(CAvoidPrediction, FeedbackRescuesWithoutGrenadeWhileReconsideringEveryTick)
{
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Ticks = 20;
	m_Input.m_TargetY = 1000;
	m_Input.m_Hook = 1;
	auto *pChar = Spawn(435, 4, false);
	CAvoidPlanner P;
	CAvoidPlanner::SFeedback Feedback;
	std::ostringstream Trace;
	int Hooks = 0;
	for(int t = 0; t < 50; t++)
	{
		const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		Trace << "t=" << t << " y=" << pChar->m_Pos.y << " vy=" << pChar->Core()->m_Vel.y << " mode=" << P.ModeName(Step.m_Decision.m_Mode) << " base=" << Step.m_Decision.m_Base.m_Danger << " out_hook=" << Step.m_Input.m_Hook << " owned=" << Feedback.m_OwnHook << '\n';
		Hooks += Feedback.m_OwnHook;
		pChar->OnDirectInput(&Step.m_Input);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Step.m_Input);
		m_World.Tick();
		ASSERT_EQ(pChar->m_FreezeTime, 0) << Trace.str();
	}
	EXPECT_GT(Hooks, 0) << Trace.str();
}

TEST_F(CAvoidPrediction, AiPFeedbackPreservesRescueFlightAndLatestPhysicalDirection)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Direction = true;
	m_Input.m_Hook = 1;
	struct SCase
	{
		vec2 m_Pos, m_Vel, m_Anchor, m_Aim;
		int m_Dir;
	};
	for(const auto &Case : {
		    SCase{{983, 429}, {5.05f, 6.79f}, {635, 384}, {-52, -80}, 1},
		    SCase{{1034, 254}, {10.91f, -9.79f}, {936, 159}, {-130, -73}, 1},
		    SCase{{1117, 264}, {3.54f, -12.04f}, {889, 63}, {18, -214}, -1}})
	{
		SCOPED_TRACE(::testing::Message() << "start=" << Case.m_Pos.x << ',' << Case.m_Pos.y);
		m_Input.m_Direction = Case.m_Dir;
		m_Input.m_TargetX = round_to_int(Case.m_Aim.x);
		m_Input.m_TargetY = round_to_int(Case.m_Aim.y);
		auto *pChar = Spawn(round_to_int(Case.m_Pos.y), Case.m_Vel.y, false, round_to_int(Case.m_Pos.x));
		auto Core = pChar->GetCore();
		Core.m_Pos = pChar->m_Pos = Case.m_Pos;
		Core.m_Vel = Case.m_Vel;
		Core.m_HookState = HOOK_GRABBED;
		Core.m_HookPos = Case.m_Anchor;
		Core.m_HookDir = normalize(Case.m_Anchor - Case.m_Pos);
		pChar->SetCore(Core);
		CAvoidPlanner P;
		CAvoidPlanner::SFeedback Feedback;
		std::ostringstream Trace;
		for(int t = 0; t < 30; t++)
		{
			const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
			Trace << "t=" << t << " pos=" << pChar->m_Pos.x << ',' << pChar->m_Pos.y << " mode=" << P.ModeName(Step.m_Decision.m_Mode) << " base=" << Step.m_Decision.m_Base.m_Danger << " result=" << Step.m_Decision.m_Result.m_Danger << " hook=" << Step.m_Input.m_Hook << " owned=" << Feedback.m_OwnHook << " dir=" << Step.m_Input.m_Direction << '\n';
			if(Step.m_Decision.m_Mode == CAvoidPlanner::EMode::WAIT || Step.m_Decision.m_Mode == CAvoidPlanner::EMode::SAFE)
			{
				EXPECT_EQ(Step.m_Input.m_Direction, m_Input.m_Direction) << Trace.str();
			}
			pChar->OnDirectInput(&Step.m_Input);
			m_World.m_GameTick++;
			pChar->OnPredictedInput(&Step.m_Input);
			m_World.Tick();
			ASSERT_EQ(pChar->m_FreezeTime, 0) << Trace.str();
		}
	}
}

TEST_F(CAvoidPrediction, AiPFeedbackAvoidsTheLoggedFallAfterTheRocket)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Direction = true;
	m_Input.m_Direction = 1;
	m_Input.m_TargetX = 183;
	m_Input.m_TargetY = -212;
	auto *pChar = Spawn(528, 6.68f, true, 6171);
	auto Core = pChar->GetCore();
	Core.m_Pos = pChar->m_Pos = vec2(6171, 528);
	Core.m_Vel = vec2(14.84f, 6.68f);
	SCOPED_TRACE(::testing::Message() << "map=" << pPath << " size=" << m_Collision.GetWidth() << "," << m_Collision.GetHeight() << " start_tile=" << m_Collision.GetTile(6171, 528));
	pChar->SetCore(Core);
	CAvoidPlanner P;
	CAvoidPlanner::SFeedback Feedback;
	std::ostringstream Trace;
	int Shots = 0;
	for(int t = 0; t < 60; t++)
	{
		const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		const auto &D = Step.m_Decision;
		Trace << "t=" << t << " pos=" << pChar->m_Pos.x << ',' << pChar->m_Pos.y << " vel=" << pChar->Core()->m_Vel.x << ',' << pChar->Core()->m_Vel.y << " mode=" << P.ModeName(D.m_Mode) << " base=" << D.m_Base.m_Danger << " result=" << D.m_Result.m_Danger << " hook=" << Step.m_Input.m_Hook << " reload=" << pChar->GetReloadTimer() << '\n';
		if(D.m_Mode == CAvoidPlanner::EMode::ROCKET || D.m_Mode == CAvoidPlanner::EMode::ROCKET_HOOK)
		{
			EXPECT_LE(D.m_Base.m_Danger, 4);
			if(!D.m_Plan.m_SwitchTicks)
			{
				Shots++;
				m_Input.m_Fire = (Step.m_Input.m_Fire + 1) & INPUT_STATE_MASK;
			}
		}
		pChar->OnDirectInput(&Step.m_Input);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Step.m_Input);
		m_World.Tick();
		ASSERT_EQ(pChar->m_FreezeTime, 0) << Trace.str();
	}
	EXPECT_GT(Shots, 0) << Trace.str();
}

TEST_F(CAvoidPrediction, AiPDoesNotOverrideTheLoggedDistantDanger)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Direction = true;
	struct SCase
	{
		vec2 m_Pos, m_Vel, m_Anchor, m_Aim;
		int m_Dir, m_Hook;
		bool m_Own, m_Rocket;
	};
	for(const auto &Case : {
		    SCase{{777, 489}, {10.21f, -9.80f}, {896, 563}, {293, 188}, 1, 1, false, true},
		    SCase{{1377, 493}, {1.31f, -5.79f}, {1237, 159}, {-85, -78}, -1, 0, true, false},
		    SCase{{1382, 469}, {3.70f, 6.71f}, {1311, 557}, {-246, 249}, 1, 1, false, true}})
	{
		SCOPED_TRACE(::testing::Message() << "start=" << Case.m_Pos.x << ',' << Case.m_Pos.y);
		m_Input.m_Direction = Case.m_Dir;
		m_Input.m_Hook = Case.m_Hook;
		m_Input.m_TargetX = round_to_int(Case.m_Aim.x);
		m_Input.m_TargetY = round_to_int(Case.m_Aim.y);
		m_Cfg.m_Rocket = Case.m_Rocket;
		auto *pChar = Spawn(round_to_int(Case.m_Pos.y), Case.m_Vel.y, true, round_to_int(Case.m_Pos.x));
		auto Core = pChar->GetCore();
		Core.m_Pos = pChar->m_Pos = Case.m_Pos;
		Core.m_Vel = Case.m_Vel;
		Core.m_HookState = HOOK_GRABBED;
		Core.m_HookPos = Case.m_Anchor;
		Core.m_HookDir = normalize(Case.m_Anchor - Case.m_Pos);
		pChar->SetCore(Core);
		CAvoidPlanner P;
		CAvoidPlanner::SFeedback Feedback;
		Feedback.m_OwnHook = Case.m_Own;
		const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		EXPECT_TRUE(Step.m_Decision.m_Mode == CAvoidPlanner::EMode::SAFE || Step.m_Decision.m_Mode == CAvoidPlanner::EMode::WAIT)
			<< P.ModeName(Step.m_Decision.m_Mode) << " base=" << Step.m_Decision.m_Base.m_Danger << " result=" << Step.m_Decision.m_Result.m_Danger;
		EXPECT_EQ(Step.m_Input.m_Direction, m_Input.m_Direction);
		EXPECT_EQ(Step.m_Input.m_Hook, m_Input.m_Hook);
		EXPECT_EQ(Step.m_Input.m_Fire, m_Input.m_Fire);
	}
}

TEST_F(CAvoidPrediction, HookBackupSurvivesInputsDeliveredOnlyEveryOtherTick)
{
	for(int x = 0; x <= 10; x++)
		m_Map.m_Tiles[16 * CTestMap::WIDTH + x].m_Index = TILE_SOLID;
	m_Collision.Init(&m_Layers);
	m_Cfg.m_Rocket = m_Cfg.m_Direction = false;
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Angles = 24;
	m_Input.m_Direction = 1;
	m_Input.m_Hook = 1;
	m_Input.m_TargetX = 274;
	m_Input.m_TargetY = 291;
	auto *pChar = Spawn(497, 0, false, 272);
	CAvoidPlanner P;
	CAvoidPlanner::SFeedback Feedback;
	auto Delivered = m_Input;
	std::ostringstream Trace;
	int Hooks = 0;
	for(int t = 0; t < 120; t++)
	{
		if(t == 70)
			m_Input.m_Direction = -1;
		if(t == 80)
			m_Input.m_Direction = 1;
		if(t % 2 == 0)
		{
			const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
			Delivered = Step.m_Input;
			Hooks += Feedback.m_OwnHook;
			Trace << "t=" << t << " pos=" << pChar->m_Pos.x << ',' << pChar->m_Pos.y << " mode=" << P.ModeName(Step.m_Decision.m_Mode) << " base=" << Step.m_Decision.m_Base.m_Danger << " result=" << Step.m_Decision.m_Result.m_Danger << " hook=" << Delivered.m_Hook << '\n';
			EXPECT_EQ(Delivered.m_Direction, m_Input.m_Direction);
			pChar->OnDirectInput(&Delivered);
		}
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Delivered);
		m_World.Tick();
		ASSERT_EQ(pChar->m_FreezeTime, 0) << Trace.str();
	}
	EXPECT_GT(Hooks, 0);
}

TEST_F(CAvoidPrediction, ReleaseHandshakeCompletesTheLaunchWithLatestPhysicalDirection)
{
	m_Cfg.m_Rocket = m_Cfg.m_Direction = false;
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Angles = 24;
	m_Input.m_Direction = 1;
	m_Input.m_Hook = 1;
	m_Input.m_TargetX = 274;
	m_Input.m_TargetY = 291;
	auto *pChar = Spawn(492, 0.45f, false, 527);
	auto Core = pChar->GetCore();
	Core.m_Vel.x = 14.29f;
	Core.m_HookState = HOOK_GRABBED;
	Core.m_HookPos = vec2(534, 544);
	Core.m_HookDir = normalize(Core.m_HookPos - Core.m_Pos);
	pChar->SetCore(Core);
	CAvoidPlanner P;
	CAvoidPlanner::SFeedback Feedback;
	const auto Release = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
	ASSERT_EQ(Release.m_Decision.m_Mode, CAvoidPlanner::EMode::HOOK);
	ASSERT_EQ(Release.m_Input.m_Hook, 0);
	ASSERT_TRUE(Feedback.m_Previous.m_CompleteHookLaunch);
	pChar->OnDirectInput(&Release.m_Input);
	m_World.m_GameTick++;
	pChar->OnPredictedInput(&Release.m_Input);
	m_World.Tick();
	ASSERT_EQ(pChar->Core()->m_HookState, HOOK_IDLE);
	m_Input.m_Direction = -1;
	const auto Launch = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
	EXPECT_EQ(Launch.m_Input.m_Direction, -1);
	EXPECT_EQ(Launch.m_Input.m_Hook, 1);
	EXPECT_LT(Launch.m_Input.m_TargetY, 0);
	EXPECT_FALSE(Feedback.m_Previous.m_CompleteHookLaunch);
	pChar->OnDirectInput(&Launch.m_Input);
	m_World.m_GameTick++;
	pChar->OnPredictedInput(&Launch.m_Input);
	m_World.Tick();
	EXPECT_LT(pChar->Core()->m_HookDir.y, 0);
}

TEST_F(CAvoidPrediction, AiPLoggedRescueFlightSurvivesAnExhaustedSearchBudget)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Rocket = false; // Both logged shots were still reloading.
	m_Cfg.m_Direction = true;
	struct SCase
	{
		vec2 m_Pos, m_Vel, m_HookPos, m_Aim, m_RawAim;
		int m_Dir, m_Pulse;
	};
	for(const auto &Case : {
		    SCase{{1695, 392}, {2.56f, 21.75f}, {1677, 249}, {-128, -992}, {-110, -25}, -1, 19},
		    SCase{{1842, 406}, {26.30f, 16.63f}, {1745, 293}, {-615, -788}, {312, -249}, 1, 10}})
	{
		SCOPED_TRACE(::testing::Message() << "start=" << Case.m_Pos.x << ',' << Case.m_Pos.y);
		m_Input.m_Direction = Case.m_Dir;
		m_Input.m_Hook = 1;
		m_Input.m_TargetX = round_to_int(Case.m_RawAim.x);
		m_Input.m_TargetY = round_to_int(Case.m_RawAim.y);
		auto *pChar = Spawn(round_to_int(Case.m_Pos.y), Case.m_Vel.y, false, round_to_int(Case.m_Pos.x));
		auto Core = pChar->GetCore();
		Core.m_Pos = pChar->m_Pos = Case.m_Pos;
		Core.m_Vel = Case.m_Vel;
		Core.m_HookState = HOOK_FLYING;
		Core.m_HookPos = Case.m_HookPos;
		Core.m_HookDir = normalize(Case.m_Aim);
		pChar->SetCore(Core);
		// Preserve a populated native world, as in the crowded user session.
		// These distant tees add prediction work without touching this rescue.
		for(int Id = 1; Id <= 24; Id++)
		{
			CNetObj_Character Net{};
			Net.m_X = 256;
			Net.m_Y = 480;
			Net.m_Weapon = WEAPON_GUN;
			Net.m_HookedPlayer = -1;
			CNetObj_DDNetCharacter Extended{};
			Extended.m_TuneZoneOverride = -1;
			Extended.m_Flags = CHARACTERFLAG_WEAPON_GUN;
			auto *pOther = new CCharacter(&m_World, Id, &Net, &Extended);
			m_World.InsertEntity(pOther);
		}
		CAvoidPlanner::SFeedback Feedback;
		Feedback.m_OwnHook = Feedback.m_HasPrevious = true;
		Feedback.m_Previous.m_Input = m_Input;
		Feedback.m_Previous.m_Input.m_TargetX = round_to_int(Case.m_Aim.x);
		Feedback.m_Previous.m_Input.m_TargetY = round_to_int(Case.m_Aim.y);
		Feedback.m_Previous.m_HookTicks = Case.m_Pulse;
		Feedback.m_Previous.m_MoveTicks = Case.m_Pulse + 1;
		CAvoidPlanner P;
		ASSERT_EQ(P.Simulate(m_World, 0, m_Input, Feedback.m_Previous, 20, m_Cfg).m_Danger, 0);
		// A deterministic call cap exposes budget starvation without depending
		// on CPU speed or changing the physics by removing nearby entities.
		m_Cfg.m_MaxCalls = 8;
		const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		ASSERT_GT(Step.m_Decision.m_Base.m_Danger, 0);
		EXPECT_LE(Step.m_Decision.m_Calls, m_Cfg.m_MaxCalls);
		EXPECT_EQ(Step.m_Input.m_Hook, 1);
		EXPECT_EQ(Step.m_Input.m_Direction, m_Input.m_Direction);
		EXPECT_EQ(Step.m_Decision.m_Result.m_Danger, 0);
		EXPECT_NE(Step.m_Decision.m_Mode, CAvoidPlanner::EMode::NO_SOLUTION);
	}
}

TEST_F(CAvoidPrediction, AiPWaitingLeavesBudgetForTheLoggedReloadFallback)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Direction = true;
	m_Cfg.m_Angles = 24;
	m_Cfg.m_MaxCalls = 48;
	m_Input.m_Direction = -1;
	m_Input.m_Hook = 1;
	m_Input.m_TargetX = -153;
	m_Input.m_TargetY = -19;
	auto *pChar = Spawn(349, 20.75f, false, 1689);
	auto Core = pChar->GetCore();
	Core.m_Vel.x = 5.56f;
	Core.m_HookState = HOOK_GRABBED;
	Core.m_HookPos = vec2(1459, 480);
	Core.m_HookDir = normalize(Core.m_HookPos - Core.m_Pos);
	pChar->SetCore(Core);
	CAvoidPlanner P;
	CAvoidPlanner::SFeedback Feedback;
	const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
	EXPECT_GT(Step.m_Decision.m_Base.m_Danger, 0);
	EXPECT_EQ(Step.m_Decision.m_Result.m_Danger, 0);
	EXPECT_NE(Step.m_Decision.m_Mode, CAvoidPlanner::EMode::NO_SOLUTION);
	EXPECT_LE(Step.m_Decision.m_Calls, m_Cfg.m_MaxCalls);
}

TEST_F(CAvoidPrediction, ContinuingARescueHookUsesTheShortestVerifiedPulse)
{
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Ticks = 20;
	m_Input.m_TargetX = 1000;
	m_Input.m_TargetY = 0;
	bool Exercised = false;
	for(int Y = 460; Y <= 505 && !Exercised; Y++)
	{
		auto *pChar = Spawn(Y, 8, false);
		auto Core = pChar->GetCore();
		Core.m_HookState = HOOK_GRABBED;
		Core.m_HookPos = vec2(512, 320);
		Core.m_HookDir = vec2(0, -1);
		pChar->SetCore(Core);
		CAvoidPlanner::SFeedback Feedback;
		Feedback.m_OwnHook = Feedback.m_HasPrevious = true;
		Feedback.m_Previous.m_Input = m_Input;
		Feedback.m_Previous.m_Input.m_Hook = 1;
		Feedback.m_Previous.m_Input.m_TargetX = 0;
		Feedback.m_Previous.m_Input.m_TargetY = -1000;
		Feedback.m_Previous.m_HookTicks = 20;
		Feedback.m_Previous.m_MoveTicks = 21;
		CAvoidPlanner P;
		const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		if(Step.m_Decision.m_Mode != CAvoidPlanner::EMode::HOOK)
			continue;
		const auto &Plan = Step.m_Decision.m_Plan;
		ASSERT_EQ(Step.m_Decision.m_Result.m_Danger, 0);
		EXPECT_GT(Plan.m_HookTicks, 0);
		EXPECT_LT(Plan.m_HookTicks, 20);
		if(Plan.m_HookTicks > 1)
		{
			auto Shorter = Plan;
			Shorter.m_HookTicks--;
			Shorter.m_MoveTicks = Shorter.m_HookDelay + Shorter.m_HookTicks + 1;
			const int RescueTicks = std::min(m_Cfg.m_Ticks, std::max(6, Step.m_Decision.m_Base.m_Danger + 4));
			EXPECT_GT(P.Simulate(m_World, 0, m_Input, Shorter, RescueTicks, m_Cfg).m_Danger, 0);
		}
		Exercised = true;
	}
	EXPECT_TRUE(Exercised);
}

TEST_F(CAvoidPrediction, AiPRequestedRocketDoesNotCauseAnUnnecessaryReverseHook)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Angles = 24;
	m_Cfg.m_Direction = true;
	m_Input.m_Direction = 1;
	m_Input.m_TargetX = -326;
	m_Input.m_TargetY = -231;
	m_Input.m_WantedWeapon = WEAPON_GRENADE + 1;
	m_Input.m_Fire = 2;
	auto *pChar = Spawn(1632, -4.96f, true, 6600);
	auto Core = pChar->GetCore();
	Core.m_Vel.x = 17.27f;
	pChar->SetCore(Core);
	m_Input.m_Fire = 3;
	CAvoidPlanner P;
	CAvoidPlanner::SFeedback Feedback;
	const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
	EXPECT_EQ(Step.m_Decision.m_Mode, CAvoidPlanner::EMode::ROCKET)
		<< P.ModeName(Step.m_Decision.m_Mode) << " base=" << Step.m_Decision.m_Base.m_Danger
		<< " rejects=" << Step.m_Decision.m_RejectShot << ',' << Step.m_Decision.m_RejectDanger << ',' << Step.m_Decision.m_RejectTurn;
	EXPECT_EQ(Step.m_Input.m_Direction, m_Input.m_Direction);
	EXPECT_EQ(Step.m_Input.m_Hook, 0);
	EXPECT_EQ(Step.m_Decision.m_Result.m_Danger, 0);
	EXPECT_EQ(Step.m_Decision.m_Result.m_ShotTick, 1);
	pChar->OnDirectInput(&Step.m_Input);
	m_World.m_GameTick++;
	pChar->OnPredictedInput(&Step.m_Input);
	m_World.Tick();
	m_Input.m_Fire = (Step.m_Input.m_Fire + 1) & INPUT_STATE_MASK;
	std::ostringstream Trace;
	for(int t = 0; t < 40; t++)
	{
		const auto Next = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		Trace << "t=" << t << " pos=" << pChar->m_Pos.x << ',' << pChar->m_Pos.y
		      << " vel=" << pChar->Core()->m_Vel.x << ',' << pChar->Core()->m_Vel.y
		      << " mode=" << P.ModeName(Next.m_Decision.m_Mode) << " base=" << Next.m_Decision.m_Base.m_Danger << '\n';
		if(Next.m_Decision.m_Plan.m_Rocket && !Next.m_Decision.m_Plan.m_SwitchTicks && Next.m_Decision.m_Mode != CAvoidPlanner::EMode::WAIT)
			m_Input.m_Fire = (Next.m_Input.m_Fire + 1) & INPUT_STATE_MASK;
		pChar->OnDirectInput(&Next.m_Input);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Next.m_Input);
		m_World.Tick();
		ASSERT_EQ(pChar->m_FreezeTime, 0) << Trace.str();
	}
}

TEST_F(CAvoidPrediction, HookBackupAvoidsReversePullWhenAVerticalRayCanSave)
{
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Angles = 24;
	m_Input.m_Direction = 1;
	m_Input.m_TargetX = -1000;
	m_Input.m_TargetY = 0;
	bool Exercised = false;
	for(int Y = 480; Y <= 504 && !Exercised; Y++)
	{
		m_Input.m_Fire = 0;
		auto *pChar = Spawn(Y, 2);
		auto Fire = m_Input;
		Fire.m_Fire = 1;
		pChar->OnDirectInput(&Fire);
		ASSERT_GT(pChar->GetReloadTimer(), 0);
		while(auto *pProjectile = m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE))
			delete pProjectile;
		auto Core = pChar->GetCore();
		Core.m_Vel.x = 20;
		pChar->SetCore(Core);
		m_Input.m_Fire = 2;
		CAvoidPlanner P;
		CAvoidPlanner::SFeedback Feedback;
		const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		if(Step.m_Decision.m_Mode != CAvoidPlanner::EMode::HOOK)
			continue;
		EXPECT_GE(Step.m_Decision.m_Plan.m_Input.m_TargetX, 0);
		EXPECT_EQ(Step.m_Input.m_Direction, 1);
		EXPECT_EQ(Step.m_Decision.m_Result.m_Danger, 0);
		const int Horizon = std::min(m_Cfg.m_Ticks, std::max(6, Step.m_Decision.m_Base.m_Danger + 4));
		EXPECT_EQ(P.Simulate(m_World, 0, m_Input, Step.m_Decision.m_Plan, Horizon, m_Cfg).m_Danger, 0);
		Exercised = true;
	}
	EXPECT_TRUE(Exercised);
}

TEST_F(CAvoidPrediction, BoostNeverFiresOnSafeGround)
{
	for(int x = 0; x < CTestMap::WIDTH; x++)
		m_Map.m_Tiles[16 * CTestMap::WIDTH + x].m_Index = TILE_AIR;
	m_Collision.Init(&m_Layers);
	m_Cfg.m_Boost = true;
	for(int Direction : {-1, 0, 1})
		for(int Vertical : {-1, 0, 1})
		{
			m_Input.m_Direction = Direction;
			m_Cfg.m_BoostVertical = Vertical;
			Spawn(530, 0);
			CAvoidPlanner P;
			const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
			EXPECT_EQ(D.m_Base.m_Danger, 0);
			EXPECT_EQ(D.m_Mode, CAvoidPlanner::EMode::SAFE);
			EXPECT_EQ(D.m_Calls, 1);
			EXPECT_FALSE(D.m_Plan.m_Rocket);
			EXPECT_EQ(D.m_Plan.m_Input.m_Direction, Direction);
		}
}

TEST_F(CAvoidPrediction, BoostLeavesEmergencyHookAndAvoidExactlyAsBefore)
{
	for(int Y : {480, 492, 504})
		for(int Vy : {2, 8})
		{
			Spawn(Y, Vy, false);
			CAvoidPlanner P;
			m_Cfg.m_Boost = false;
			const auto Old = P.Decide(m_World, 0, m_Input, m_Cfg);
			m_Cfg.m_Boost = true;
			const auto New = P.Decide(m_World, 0, m_Input, m_Cfg);
			EXPECT_EQ(New.m_Mode, Old.m_Mode);
			EXPECT_EQ(New.m_Calls, Old.m_Calls);
			EXPECT_EQ(New.m_Result.m_Danger, Old.m_Result.m_Danger);
			EXPECT_EQ(New.m_Plan.m_Delay, Old.m_Plan.m_Delay);
			EXPECT_EQ(New.m_Plan.m_HookTicks, Old.m_Plan.m_HookTicks);
			EXPECT_EQ(New.m_Plan.m_MoveTicks, Old.m_Plan.m_MoveTicks);
			EXPECT_EQ(New.m_Plan.m_Input.m_Direction, Old.m_Plan.m_Input.m_Direction);
			EXPECT_EQ(New.m_Plan.m_Input.m_Hook, Old.m_Plan.m_Input.m_Hook);
			EXPECT_EQ(New.m_Plan.m_Input.m_TargetX, Old.m_Plan.m_Input.m_TargetX);
			EXPECT_EQ(New.m_Plan.m_Input.m_TargetY, Old.m_Plan.m_Input.m_TargetY);
		}
}

TEST_F(CAvoidPrediction, BoostRescueUsesThePressedDirectionAgainstInertia)
{
	m_Cfg.m_Boost = true;
	for(int Direction : {-1, 1})
	{
		m_Input.m_Direction = Direction;
		auto *pChar = Spawn(504, 8);
		auto Core = pChar->GetCore();
		Core.m_Vel.x = -Direction * 8;
		pChar->SetCore(Core);
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
		ASSERT_EQ(D.m_Mode, CAvoidPlanner::EMode::ROCKET);
		EXPECT_GT(D.m_Base.m_Danger, 0);
		EXPECT_GT(D.m_BoostDirection.x * Direction, 0);
		EXPECT_EQ(D.m_Plan.m_Input.m_Direction, Direction);
		EXPECT_EQ(D.m_Result.m_Danger, 0);
		EXPECT_LE(D.m_Plan.m_HookTicks, 0);
	}
}

TEST_F(CAvoidPrediction, BoostAddsHorizontalSpeedToTheOrdinaryRocketSave)
{
	for(int Direction : {-1, 1})
	{
		m_Input.m_Direction = Direction;
		auto *pChar = Spawn(504, 2);
		auto Core = pChar->GetCore();
		Core.m_Vel.x = Direction * 12;
		pChar->SetCore(Core);
		CAvoidPlanner P;
		m_Cfg.m_Boost = false;
		const auto Old = P.Decide(m_World, 0, m_Input, m_Cfg);
		m_Cfg.m_Boost = true;
		const auto New = P.Decide(m_World, 0, m_Input, m_Cfg);
		ASSERT_EQ(Old.m_Mode, CAvoidPlanner::EMode::ROCKET);
		ASSERT_EQ(New.m_Mode, CAvoidPlanner::EMode::ROCKET);
		EXPECT_EQ(New.m_Result.m_Danger, 0);
		EXPECT_EQ(New.m_Plan.m_Input.m_Direction, Old.m_Plan.m_Input.m_Direction);
		EXPECT_EQ(New.m_Plan.m_HookTicks, Old.m_Plan.m_HookTicks);
		const int Horizon = std::max(Old.m_Result.m_BlastTick, New.m_Result.m_BlastTick) + 1;
		const auto Before = P.Simulate(m_World, 0, m_Input, Old.m_Plan, Horizon, m_Cfg);
		const auto After = P.Simulate(m_World, 0, m_Input, New.m_Plan, Horizon, m_Cfg);
		EXPECT_GT(After.m_Vel.x * Direction, Before.m_Vel.x * Direction + 5.0f);
		EXPECT_GT(New.m_BoostScore, 5.0f);
		EXPECT_LE(New.m_Result.m_BlastTick - New.m_Result.m_ShotTick + 1, m_Cfg.m_MaxFlight);
		auto Late = New.m_Plan;
		Late.m_Delay = 1;
		EXPECT_EQ(P.Simulate(m_World, 0, m_Input, Late, New.m_Result.m_Survival + 1, m_Cfg).m_Danger, 0);
		for(int TurnDirection : {-1, 0, 1})
		{
			auto Turn = m_Input;
			Turn.m_Direction = TurnDirection;
			EXPECT_EQ(P.Simulate(m_World, 0, Turn, New.m_Plan, m_Cfg.m_RocketTicks, m_Cfg).m_Danger, 0);
		}
	}
}

TEST_F(CAvoidPrediction, BoostReleaseStillRescuesWhenTheServerMissesThePress)
{
	for(int Fire : {0, 62})
	{
		m_Input.m_Fire = Fire;
		m_Input.m_Direction = 1;
		auto *pChar = Spawn(504, 2);
		auto Core = pChar->GetCore();
		Core.m_Vel.x = 12;
		pChar->SetCore(Core);
		m_Cfg.m_Boost = true;
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
		ASSERT_EQ(D.m_Mode, CAvoidPlanner::EMode::ROCKET);
		const auto Shot = CAvoidPlanner::InputAt(D.m_Plan, m_Input, 0);
		const int Before = pChar->GetAttackTick();
		// Server advances without receiving the press. Its next input has the
		// even release counter, which still contains exactly that one press.
		pChar->OnDirectInput(&m_Input);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&m_Input);
		m_World.Tick();
		auto Release = m_Input;
		Release.m_Fire = (Shot.m_Fire + 1) & INPUT_STATE_MASK;
		Release.m_TargetX = 1000;
		Release.m_TargetY = 0;
		CAvoidPlanner::PreserveBoostReleaseAim(Release, Shot);
		int Attack = -1;
		for(int t = 0; t < 35; t++)
		{
			pChar->OnDirectInput(&Release);
			m_World.m_GameTick++;
			pChar->OnPredictedInput(&Release);
			m_World.Tick();
			if(t == 0)
			{
				Attack = pChar->GetAttackTick();
				ASSERT_GT(Attack, Before);
			}
			EXPECT_EQ(pChar->GetAttackTick(), Attack); // No second shot after reload.
			if(t < 6)
			{
				EXPECT_EQ(pChar->m_FreezeTime, 0);
			}
		}
	}
}

TEST_F(CAvoidPrediction, BoostAimDeliveryPreservesHookAndSubsequentManualInputs)
{
	CNetObj_PlayerInput Shot = m_Input;
	Shot.m_Fire = 1;
	Shot.m_TargetX = -300;
	Shot.m_TargetY = 950;
	for(int Hook : {0, 1})
		for(int Fire : {2, 3, 4})
		{
			auto Input = m_Input;
			Input.m_Hook = Hook;
			Input.m_Fire = Fire;
			Input.m_Direction = -1;
			Input.m_Jump = 1;
			Input.m_WantedWeapon = WEAPON_GUN + 1;
			const auto Before = Input;
			CAvoidPlanner::PreserveBoostReleaseAim(Input, Shot);
			EXPECT_EQ(Input.m_Hook, Before.m_Hook);
			EXPECT_EQ(Input.m_Fire, Before.m_Fire);
			EXPECT_EQ(Input.m_Direction, Before.m_Direction);
			EXPECT_EQ(Input.m_Jump, Before.m_Jump);
			EXPECT_EQ(Input.m_WantedWeapon, Before.m_WantedWeapon);
			if(Hook || Fire != 2)
			{
				EXPECT_EQ(Input.m_TargetX, Before.m_TargetX);
				EXPECT_EQ(Input.m_TargetY, Before.m_TargetY);
			}
		}
}

TEST_F(CAvoidPrediction, VerticalRocketSavesBoostAwayFromFloorAndCeilingFreeze)
{
	for(int x = 0; x < CTestMap::WIDTH; x++)
	{
		m_Map.m_Tiles[16 * CTestMap::WIDTH + x].m_Index = TILE_AIR;
		m_Map.m_Tiles[17 * CTestMap::WIDTH + x].m_Index = TILE_AIR;
		m_Map.m_Tiles[10 * CTestMap::WIDTH + x].m_Index = TILE_FREEZE;
		m_Map.m_Tiles[27 * CTestMap::WIDTH + x].m_Index = TILE_FREEZE;
		m_Map.m_Tiles[28 * CTestMap::WIDTH + x].m_Index = TILE_SOLID;
	}
	m_Collision.Init(&m_Layers);
	m_Cfg.m_Boost = true;
	for(int Vertical : {-1, 1})
	{
		m_Cfg.m_BoostVertical = Vertical;
		Spawn(Vertical < 0 ? 850 : 370, Vertical < 0 ? 4 : -8);
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
		ASSERT_EQ(D.m_Mode, CAvoidPlanner::EMode::ROCKET) << Vertical;
		EXPECT_GT(D.m_Base.m_Danger, 0);
		EXPECT_EQ(D.m_BoostDirection, vec2(0, Vertical));
		EXPECT_LT(D.m_Plan.m_RocketAim.y * Vertical, 0);
		EXPECT_EQ(D.m_Result.m_Danger, 0);
		const auto After = P.Simulate(m_World, 0, m_Input, D.m_Plan, D.m_Result.m_BlastTick + 1, m_Cfg);
		EXPECT_GT(After.m_Vel.y * Vertical, 0);
	}
}

TEST_F(CAvoidPrediction, DiagonalBoostSaveCombinesHorizontalAndVerticalIntent)
{
	m_Cfg.m_Boost = true;
	m_Cfg.m_BoostVertical = -1;
	for(int Direction : {-1, 1})
	{
		m_Input.m_Direction = Direction;
		auto *pChar = Spawn(504, 2);
		auto Core = pChar->GetCore();
		Core.m_Vel.x = Direction * 8;
		pChar->SetCore(Core);
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
		ASSERT_EQ(D.m_Mode, CAvoidPlanner::EMode::ROCKET);
		EXPECT_GT(D.m_BoostDirection.x * Direction, 0);
		EXPECT_LT(D.m_BoostDirection.y, 0);
		EXPECT_EQ(D.m_Plan.m_Input.m_Jump, 0);
		EXPECT_EQ(D.m_Result.m_Danger, 0);
		EXPECT_LE(D.m_Plan.m_HookTicks, 0);
	}
}

TEST_F(CAvoidPrediction, BoostSavePreservesTheVerifiedRescueOnATightBudget)
{
	for(int Calls : {12, 24, 48, 64, 96, 128, 256})
	{
		m_Cfg.m_MaxCalls = Calls;
		m_Input.m_Direction = 1;
		auto *pChar = Spawn(504, 2);
		auto Core = pChar->GetCore();
		Core.m_Vel.x = 12;
		pChar->SetCore(Core);
		CAvoidPlanner P;
		m_Cfg.m_Boost = false;
		const auto Old = P.Decide(m_World, 0, m_Input, m_Cfg);
		m_Cfg.m_Boost = true;
		const auto New = P.Decide(m_World, 0, m_Input, m_Cfg);
		EXPECT_LE(New.m_Calls, Calls);
		EXPECT_EQ(New.m_Mode, Old.m_Mode);
		EXPECT_EQ(New.m_Result.m_Danger, Old.m_Result.m_Danger);
	}
}

TEST_F(CAvoidPrediction, BoostLeavesWeaponSwitchPreparationExactlyAsBefore)
{
	auto *pChar = Spawn(504, 2);
	pChar->SetActiveWeapon(WEAPON_GUN);
	m_Input.m_WantedWeapon = WEAPON_GUN + 1;
	m_Input.m_Direction = 1;
	CAvoidPlanner P;
	m_Cfg.m_Boost = false;
	const auto Old = P.Decide(m_World, 0, m_Input, m_Cfg);
	m_Cfg.m_Boost = true;
	const auto New = P.Decide(m_World, 0, m_Input, m_Cfg);
	ASSERT_TRUE(Old.m_Plan.m_Rocket);
	ASSERT_GT(Old.m_Plan.m_SwitchTicks, 0);
	EXPECT_EQ(New.m_Mode, Old.m_Mode);
	EXPECT_EQ(New.m_Calls, Old.m_Calls);
	EXPECT_EQ(New.m_Plan.m_RocketAim, Old.m_Plan.m_RocketAim);
	EXPECT_EQ(New.m_Plan.m_SwitchTicks, Old.m_Plan.m_SwitchTicks);
	EXPECT_EQ(New.m_Plan.m_Delay, Old.m_Plan.m_Delay);
	EXPECT_EQ(New.m_BoostScore, 0);
}

TEST_F(CAvoidPrediction, StrongerBoostDoesNotBringForwardTheNextFreeze)
{
	int Improved = 0;
	for(int Y : {492, 500, 504, 507})
		for(int Vy : {2, 6})
			for(int Vx : {8, 12, 20})
			{
				SCOPED_TRACE(::testing::Message() << Y << ',' << Vy << ',' << Vx);
				m_Input.m_Direction = 1;
				auto *pChar = Spawn(Y, Vy);
				auto Core = pChar->GetCore();
				Core.m_Vel.x = Vx;
				pChar->SetCore(Core);
				CAvoidPlanner P;
				m_Cfg.m_Boost = false;
				const auto Old = P.Decide(m_World, 0, m_Input, m_Cfg);
				m_Cfg.m_Boost = true;
				const auto New = P.Decide(m_World, 0, m_Input, m_Cfg);
				if(New.m_BoostScore <= 0)
					continue;
				Improved++;
				EXPECT_LE(New.m_Calls, m_Cfg.m_MaxCalls);
				EXPECT_EQ(New.m_Result.m_Danger, 0);
				for(int Dir : {-1, 0, 1})
				{
					auto Turn = m_Input;
					Turn.m_Direction = Dir;
					const auto Before = P.Simulate(m_World, 0, Turn, Old.m_Plan, m_Cfg.m_RocketTicks, m_Cfg);
					const auto After = P.Simulate(m_World, 0, Turn, New.m_Plan, m_Cfg.m_RocketTicks, m_Cfg);
					if(After.m_Danger)
					{
						EXPECT_GT(Before.m_Danger, 0);
						EXPECT_GE(After.m_Danger, Before.m_Danger);
					}
				}
				auto Late = New.m_Plan;
				Late.m_Delay = 1;
				EXPECT_EQ(P.Simulate(m_World, 0, m_Input, Late, New.m_Result.m_Survival + 1, m_Cfg).m_Danger, 0);
			}
	EXPECT_GE(Improved, 8);
}

TEST_F(CAvoidPrediction, AiPBoostPreservesDirectionDuringTheLoggedEmergencyHook)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Direction = true;
	m_Cfg.m_Angles = 24;
	m_Input.m_Direction = 1;
	m_Input.m_TargetX = 285;
	m_Input.m_TargetY = -280;
	auto *pChar = Spawn(505, 8.84f, true, 2329);
	auto Core = pChar->GetCore();
	Core.m_Vel.x = 19.91f;
	pChar->SetCore(Core);
	CAvoidPlanner P;
	m_Cfg.m_Boost = false;
	const auto Old = P.Decide(m_World, 0, m_Input, m_Cfg);
	m_Cfg.m_Boost = true;
	const auto New = P.Decide(m_World, 0, m_Input, m_Cfg);
	ASSERT_EQ(Old.m_Mode, CAvoidPlanner::EMode::HOOK);
	ASSERT_EQ(New.m_Mode, CAvoidPlanner::EMode::HOOK);
	ASSERT_EQ(Old.m_Plan.m_Input.m_Direction, -1);
	EXPECT_EQ(New.m_Plan.m_Input.m_Direction, 1);
	EXPECT_GT(New.m_Plan.m_Input.m_TargetX, 0);
	EXPECT_FALSE(New.m_Plan.m_Rocket);
	EXPECT_EQ(New.m_Result.m_Danger, 0);
	EXPECT_GT(New.m_Result.m_Vel.x, Old.m_Result.m_Vel.x + 5);
	const auto Before = P.Simulate(m_World, 0, m_Input, Old.m_Plan, m_Cfg.m_RocketTicks, m_Cfg);
	const auto After = P.Simulate(m_World, 0, m_Input, New.m_Plan, m_Cfg.m_RocketTicks, m_Cfg);
	EXPECT_TRUE(!After.m_Danger || (Before.m_Danger && After.m_Danger >= Before.m_Danger));
}

TEST_F(CAvoidPrediction, AiPHookPrefersTheSafeForwardAttachmentFromTheUserLog)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Direction = true;
	m_Cfg.m_Angles = 24;
	struct SCase
	{
		vec2 m_Pos, m_Vel, m_Aim;
		int m_Dir;
	};
	for(const auto &Case : {SCase{{8812, 1742}, {17.75f, 5.97f}, {192, -306}, 1}, SCase{{6680, 2502}, {14.66f, 5.63f}, {350, -183}, 1}, SCase{{8480, 2184}, {-14.76f, 4.63f}, {210, 72}, -1}})
	{
		SCOPED_TRACE(::testing::Message() << Case.m_Pos.x << ',' << Case.m_Pos.y);
		m_Input = {};
		m_Input.m_Direction = Case.m_Dir;
		m_Input.m_TargetX = round_to_int(Case.m_Aim.x);
		m_Input.m_TargetY = round_to_int(Case.m_Aim.y);
		auto *pChar = Spawn(round_to_int(Case.m_Pos.y), Case.m_Vel.y, true, round_to_int(Case.m_Pos.x));
		auto Core = pChar->GetCore();
		Core.m_Vel = Case.m_Vel;
		pChar->SetCore(Core);
		CAvoidPlanner P;
		m_Cfg.m_Boost = false;
		const auto Old = P.Decide(m_World, 0, m_Input, m_Cfg);
		m_Cfg.m_Boost = true;
		const auto New = P.Decide(m_World, 0, m_Input, m_Cfg);
		ASSERT_EQ(Old.m_Mode, CAvoidPlanner::EMode::HOOK);
		ASSERT_EQ(New.m_Mode, CAvoidPlanner::EMode::HOOK);
		EXPECT_LT(Old.m_Plan.m_Input.m_TargetX * Case.m_Dir, 0);
		EXPECT_GT(New.m_Plan.m_Input.m_TargetX * Case.m_Dir, 0);
		EXPECT_EQ(New.m_Plan.m_Input.m_Direction, Case.m_Dir);
		EXPECT_EQ(New.m_Result.m_Danger, 0);
		EXPECT_GT(New.m_Result.m_Vel.x * Case.m_Dir, Old.m_Result.m_Vel.x * Case.m_Dir + 1);
		EXPECT_LE(New.m_Calls, m_Cfg.m_MaxCalls);
		const auto Before = P.Simulate(m_World, 0, m_Input, Old.m_Plan, 30, m_Cfg);
		const auto After = P.Simulate(m_World, 0, m_Input, New.m_Plan, 30, m_Cfg);
		EXPECT_TRUE(!After.m_Danger || (Before.m_Danger && After.m_Danger >= Before.m_Danger));
		auto OldLate = Old.m_Plan;
		OldLate.m_Delay = 1;
		auto NewLate = New.m_Plan;
		NewLate.m_Delay = 1;
		const int Horizon = New.m_Result.m_Survival + 1;
		const auto BL = P.Simulate(m_World, 0, m_Input, OldLate, Horizon, m_Cfg);
		const auto AL = P.Simulate(m_World, 0, m_Input, NewLate, Horizon, m_Cfg);
		EXPECT_TRUE(!AL.m_Danger || (BL.m_Danger && AL.m_Danger >= BL.m_Danger));
	}
}

TEST_F(CAvoidPrediction, ABackwardHookStillRescuesWhenItIsTheOnlyAttachment)
{
	for(int x = 0; x < CTestMap::WIDTH; x++)
		m_Map.m_Tiles[9 * CTestMap::WIDTH + x].m_Index = TILE_AIR;
	for(int x = 12; x <= 14; x++)
		m_Map.m_Tiles[9 * CTestMap::WIDTH + x].m_Index = TILE_SOLID;
	m_Collision.Init(&m_Layers);
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Boost = true;
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Angles = 36;
	m_Input.m_Direction = 1;
	m_Input.m_TargetX = 1000;
	m_Input.m_TargetY = 0;
	bool Exercised = false;
	for(int Y = 465; Y <= 505 && !Exercised; Y++)
	{
		auto *pChar = Spawn(Y, 6, false);
		auto Core = pChar->GetCore();
		Core.m_Vel.x = 12;
		pChar->SetCore(Core);
		CAvoidPlanner P;
		const auto D = P.Decide(m_World, 0, m_Input, m_Cfg);
		if(D.m_Mode != CAvoidPlanner::EMode::HOOK)
			continue;
		EXPECT_LT(D.m_Plan.m_Input.m_TargetX, 0);
		EXPECT_EQ(D.m_Result.m_Danger, 0);
		EXPECT_LE(D.m_Calls, m_Cfg.m_MaxCalls);
		EXPECT_EQ(P.Simulate(m_World, 0, m_Input, D.m_Plan, D.m_Result.m_Survival, m_Cfg).m_Danger, 0);
		Exercised = true;
	}
	EXPECT_TRUE(Exercised);
}

TEST_F(CAvoidPrediction, AutoRehookCanWaitForAFreshPressAfterAnAvoidInterruption)
{
	for(bool Enabled : {true, false})
	{
		SCOPED_TRACE(Enabled);
		m_Cfg.m_AutoRehook = Enabled;
		m_Input.m_Hook = 1;
		m_Input.m_TargetX = 0;
		m_Input.m_TargetY = -1000;
		auto *pChar = Spawn(400, 0, false);
		auto Core = pChar->GetCore();
		Core.m_HookState = HOOK_GRABBED;
		Core.m_HookPos = vec2(512, 320);
		Core.m_HookDir = vec2(0, -1);
		pChar->SetCore(Core);
		CAvoidPlanner P;
		CAvoidPlanner::SFeedback Feedback;
		Feedback.m_OwnHook = true;
		const auto Interrupted = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		ASSERT_EQ(Interrupted.m_Decision.m_Mode, CAvoidPlanner::EMode::SAFE);
		ASSERT_EQ(Interrupted.m_Input.m_Hook, 0);
		pChar->OnDirectInput(&Interrupted.m_Input);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Interrupted.m_Input);
		m_World.Tick();
		const auto Held = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		EXPECT_EQ(Held.m_Input.m_Hook, int(Enabled));
		EXPECT_EQ(Feedback.m_ManualHookSuppressed, !Enabled);
		m_Input.m_Hook = 0;
		P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		EXPECT_FALSE(Feedback.m_ManualHookSuppressed);
		m_Input.m_Hook = 1;
		const auto Pressed = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		EXPECT_EQ(Pressed.m_Input.m_Hook, 1);
	}
}

TEST_F(CAvoidPrediction, DisablingAutoRehookStillAllowsEmergencyHooks)
{
	m_Cfg.m_AutoRehook = false;
	m_Cfg.m_Rocket = false;
	m_Cfg.m_Ticks = 20;
	m_Input.m_Hook = 1;
	m_Input.m_TargetX = 1000;
	m_Input.m_TargetY = 0;
	bool Exercised = false;
	for(int Y = 475; Y <= 505 && !Exercised; Y++)
	{
		Spawn(Y, 8, false);
		CAvoidPlanner P;
		CAvoidPlanner::SFeedback Feedback;
		Feedback.m_ManualHookSuppressed = true;
		const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		if(Step.m_Decision.m_Mode != CAvoidPlanner::EMode::HOOK || !Step.m_Input.m_Hook)
			continue;
		EXPECT_TRUE(Feedback.m_ManualHookSuppressed);
		EXPECT_TRUE(Feedback.m_OwnHook);
		EXPECT_EQ(Step.m_Decision.m_Result.m_Danger, 0);
		Exercised = true;
	}
	EXPECT_TRUE(Exercised);
	// Suppression belongs to one tee's feedback; another tee keeps its input.
	Spawn(400, 0, false);
	CAvoidPlanner P;
	CAvoidPlanner::SFeedback First, Second;
	First.m_ManualHookSuppressed = true;
	EXPECT_EQ(P.Step(m_World, 0, m_Input, m_Cfg, First).m_Input.m_Hook, 0);
	EXPECT_EQ(P.Step(m_World, 0, m_Input, m_Cfg, Second).m_Input.m_Hook, 1);
	m_Cfg.m_AutoRehook = true;
	EXPECT_EQ(P.Step(m_World, 0, m_Input, m_Cfg, First).m_Input.m_Hook, 1);
	EXPECT_FALSE(First.m_ManualHookSuppressed);
}

TEST_F(CAvoidPrediction, SuppressedManualRehookDoesNotCutShortTheCloseEmergencySave)
{
	for(bool Boost : {false, true})
	{
		SCOPED_TRACE(Boost);
		m_Cfg.m_AutoRehook = false;
		m_Cfg.m_Boost = Boost;
		m_Cfg.m_Rocket = false;
		m_Cfg.m_Ticks = 20;
		m_Cfg.m_Angles = 24;
		m_Input.m_Direction = 1;
		m_Input.m_Hook = 1;
		m_Input.m_TargetX = 274;
		m_Input.m_TargetY = 291;
		auto *pChar = Spawn(501, 2.74f, false);
		auto Core = pChar->GetCore();
		Core.m_Vel.x = 8.55f;
		Core.m_HookState = HOOK_GRABBED;
		Core.m_HookPos = vec2(556, 320);
		Core.m_HookDir = normalize(vec2(289, -957));
		pChar->SetCore(Core);
		CAvoidPlanner P;
		CAvoidPlanner::SFeedback Feedback;
		Feedback.m_OwnHook = Feedback.m_HasPrevious = Feedback.m_ManualHookSuppressed = true;
		Feedback.m_Previous.m_Input = m_Input;
		Feedback.m_Previous.m_Input.m_TargetX = 289;
		Feedback.m_Previous.m_Input.m_TargetY = -957;
		Feedback.m_Previous.m_HookTicks = 7;
		Feedback.m_Previous.m_MoveTicks = 8;
		const auto Save = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
		ASSERT_LE(Save.m_Decision.m_Base.m_Danger, 6);
		ASSERT_GT(Save.m_Decision.m_Base.m_Danger, 0);
		EXPECT_EQ(Save.m_Decision.m_Mode, CAvoidPlanner::EMode::HOOK);
		EXPECT_EQ(Save.m_Input.m_Hook, 1);
		EXPECT_TRUE(Feedback.m_OwnHook);
		EXPECT_TRUE(Feedback.m_ManualHookSuppressed);
		pChar->OnDirectInput(&Save.m_Input);
		m_World.m_GameTick++;
		pChar->OnPredictedInput(&Save.m_Input);
		m_World.Tick();
		for(int t = 0; t < 25; t++)
		{
			const auto Next = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
			EXPECT_TRUE(Feedback.m_ManualHookSuppressed);
			if(Next.m_Input.m_Hook)
				EXPECT_TRUE(Feedback.m_OwnHook);
			pChar->OnDirectInput(&Next.m_Input);
			m_World.m_GameTick++;
			pChar->OnPredictedInput(&Next.m_Input);
			m_World.Tick();
			ASSERT_EQ(pChar->m_FreezeTime, 0) << t;
		}
	}
}

TEST_F(CAvoidPrediction, AiPFeedbackFinishesTheLoggedFastFallEmergencyHook)
{
	const char *pPath = std::getenv("TCLIENT_AVOID_AIP_MAP");
	if(!pPath)
		GTEST_SKIP() << "Set TCLIENT_AVOID_AIP_MAP";
	auto pStorage = CreateLocalStorage();
	auto pMap = CreateMap();
	ASSERT_TRUE(pMap->Load(pStorage.get(), pPath, IStorage::TYPE_ABSOLUTE));
	m_World.Clear();
	m_Collision.Unload();
	m_Layers.Init(pMap.get(), false);
	m_Collision.Init(&m_Layers);
	m_World.Init(&m_Collision, m_Tuning.data(), &m_Bugs);
	m_Cfg.m_Ticks = 20;
	m_Cfg.m_Direction = true;
	m_Cfg.m_Jump = false;
	m_Cfg.m_Angles = 24;
	for(int Calls : {128, 166, 512})
		for(bool Boost : {false, true})
			for(bool AutoRehook : {false, true})
			{
				SCOPED_TRACE(::testing::Message() << "boost=" << Boost << " calls=" << Calls << " rehook=" << AutoRehook);
				m_Cfg.m_MaxCalls = Calls;
				m_Cfg.m_Boost = Boost;
				m_Cfg.m_AutoRehook = AutoRehook;
				m_Input = {};
				m_Input.m_Direction = -1;
				m_Input.m_TargetX = -39;
				m_Input.m_TargetY = 114;
				auto *pChar = Spawn(3263, 30.09f, true, 3230);
				auto Core = pChar->GetCore();
				Core.m_Vel.x = -5;
				pChar->SetCore(Core);
				m_World.m_GameTick = 17292;
				CAvoidPlanner P;
				CAvoidPlanner::SPlan Logged;
				Logged.m_Input = m_Input;
				Logged.m_Input.m_Hook = 1;
				Logged.m_Input.m_TargetX = -830;
				Logged.m_Input.m_TargetY = -558;
				Logged.m_HookTicks = 8;
				Logged.m_MoveTicks = 9;
				// The packet chosen at tick 17293 passes its immediate rescue
				// window, but needs further braking before the next freeze.
				ASSERT_EQ(P.Simulate(m_World, 0, m_Input, Logged, 11, m_Cfg).m_Danger, 0);
				ASSERT_GT(P.Simulate(m_World, 0, m_Input, Logged, 30, m_Cfg).m_Danger, 0);
				auto Extended = Logged;
				Extended.m_HookTicks = 20;
				Extended.m_MoveTicks = 21;
				ASSERT_EQ(P.Simulate(m_World, 0, m_Input, Extended, 30, m_Cfg).m_Danger, 0);
				const auto First = CAvoidPlanner::InputAt(Logged, m_Input, 0);
				pChar->OnDirectInput(&First);
				m_World.m_GameTick++;
				pChar->OnPredictedInput(&First);
				m_World.Tick();
				CAvoidPlanner::SFeedback Feedback;
				Feedback.m_OwnHook = Feedback.m_HasPrevious = true;
				Feedback.m_Previous = CAvoidPlanner::RemainingPlan(Logged, m_Input, 1);
				std::ostringstream Trace;
				for(int t = 1; t < 35; t++)
				{
					if(t >= 5)
					{
						m_Input.m_TargetX = -30;
						m_Input.m_TargetY = 1;
					}
					const auto Step = P.Step(m_World, 0, m_Input, m_Cfg, Feedback);
					Trace << "t=" << t << " pos=" << pChar->m_Pos.x << ',' << pChar->m_Pos.y
					      << " mode=" << P.ModeName(Step.m_Decision.m_Mode) << " base=" << Step.m_Decision.m_Base.m_Danger
					      << " result=" << Step.m_Decision.m_Result.m_Danger << " hook=" << Step.m_Input.m_Hook
					      << " pulse=" << Step.m_Decision.m_Plan.m_HookTicks << " owned=" << Feedback.m_OwnHook << '\n';
					EXPECT_LE(Step.m_Decision.m_Calls, Calls) << Trace.str();
					pChar->OnDirectInput(&Step.m_Input);
					m_World.m_GameTick++;
					pChar->OnPredictedInput(&Step.m_Input);
					m_World.Tick();
					ASSERT_EQ(pChar->m_FreezeTime, 0) << Trace.str();
				}
			}
}
