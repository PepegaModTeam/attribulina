// World's GameCore types, from the 2010 beta's EASharp bindings (exes/pc/world-2010-beta/gameplay.dll and
// the accessors its nfsw.exe registers). Written by scripts/world/easharp.py;
// rerun it rather than editing by hand. scripts/gen_schema.py --game world reads
// this before Carbon's headers; a struct is used only where World's type table
// has its size. Member offsets are the native setters' (in comments).

enum Career::FirstSkill {
	kFirstSkill_Unknown = 0,
	kFirstSkill_Blocker = 1,
	kFirstSkill_Pathfinder = 2,
	kFirstSkill_Racer = 3,
	kFirstSkill_NumSkills = 4,
};

enum Career::SecondSkill {
	kSecondSkill_Unknown = 0,
	kSecondSkill_Mechanic = 1,
	kSecondSkill_Fabricator = 2,
	kSecondSkill_HeatMagnet = 3,
	kSecondSkill_NumSkills = 4,
};

enum EA::WorldMap::POIType {
	POIType_Unknown = -1,
	POIType_Cooldown = 0,
	POIType_PursuitBreaker = 1,
};

enum EA::WorldMap::eWMFeatureType {
	WORLDMAP_FEATURE_TYPE_BASICPOINTFEATURE = 0,
	WORLDMAP_FEATURE_TYPE_BASICAREAFEATURE = 1,
	WORLDMAP_FEATURE_TYPE_RACE_EVENT = 2,
	WORLDMAP_FEATURE_TYPE_COP_CARS = 3,
	WORLDMAP_FEATURE_TYPE_LOCAL_PLAYER = 4,
	WORLDMAP_FEATURE_TYPE_REMOTE_PLAYER = 5,
	WORLDMAP_FEATURE_TYPE_POINT_OF_INTEREST = 6,
	WORLDMAP_FEATURE_TYPE_MAX = 7,
};

enum GMetaGame::ProgressionTier {
	kProgression_Race = 0,
	kProgression_Explore = 1,
	kProgression_Pursuit = 2,
};

enum GRace::Boss {
	kRaceBoss_None = 0,
	kRaceBoss_Angie = 1,
	kRaceBoss_Darius = 2,
	kRaceBoss_Wolf = 3,
	kRaceBoss_Kenji = 4,
	kRaceBoss_Neville = 5,
	kRaceBoss_NumBosses = 6,
};

enum GRace::Context {
	kRaceContext_QuickRace = 0,
	kRaceContext_TimeTrial = 1,
	kRaceContext_Online = 2,
	kRaceContext_Career = 3,
	kRaceContext_Count = 4,
};

enum GRace::CopDensity {
	kRaceCops_Off = 0,
	kRaceCops_Light = 1,
	kRaceCops_Medium = 2,
	kRaceCops_Heavy = 3,
	kRaceCops_NumDensities = 4,
};

enum GRace::CornerMarker {
	kCornerMarker_MediumLeft = 0,
	kCornerMarker_HardLeft = 1,
	kCornerMarker_MediumRight = 2,
	kCornerMarker_HardRight = 3,
	kCornerMarker_ZigZag = 4,
};

enum GRace::Difficulty {
	kRaceDifficulty_Easy = 0,
	kRaceDifficulty_Medium = 1,
	kRaceDifficulty_Hard = 2,
	kRaceDifficulty_NumDifficulties = 3,
};

enum GRace::PrimaryRole {
	kPrimaryRole_None = 0,
	kPrimaryRole_Leader = 1,
	kPrimaryRole_Racer = 2,
	kPrimaryRole_Blocker = 3,
	kPrimaryRole_Pathfinder = 4,
};

enum GRace::RaceCarClass {
	kRaceCar_ClassNone = 0,
	kRaceCar_ClassExotic = 1,
	kRaceCar_ClassMuscle = 2,
	kRaceCar_ClassTuner = 3,
	kRaceCar_NumClasses = 4,
};

enum GRace::RaceLigthingMode {
	kLighting_Normal = 0,
	kLightingDrift = 1,
	kLightingCanyon = 2,
};

enum GRace::Region {
	kRaceRegion_None = -1,
	kRaceRegion_SantaFe = 0,
	kRaceRegion_Fortuna = 1,
	kRaceRegion_Kempton = 2,
	kRaceRegion_Palmont = 3,
	kRaceRegion_Silverton = 4,
	kRaceRegion_Canyon = 5,
	kRaceRegion_Camden = 6,
	kRaceRegion_Rockport = 7,
	kRaceRegion_Rosewood = 8,
	kRaceRegion_NumRegions = 9,
};

enum GRace::SecondaryRole {
	kSecondaryRole_None = 0,
	kSecondaryRole_Fabricator = 1,
	kSecondaryRole_Mechanic = 2,
	kSecondaryRole_HeatMagnet = 3,
};

enum GRace::ShortcutType {
	kShortcutType_None = 0,
	kShortcutType_ParkRight = 1,
	kShortcutType_AlleyRight = 2,
	kShortcutType_SideStreetRight = 3,
	kShortcutType_TunnelRight = 4,
	kShortcutType_GenericRight = 5,
	kShortcutType_ParkLeft = 6,
	kShortcutType_AlleyLeft = 7,
	kShortcutType_SideStreetLeft = 8,
	kShortcutType_TunnelLeft = 9,
	kShortcutType_GenericLeft = 10,
};

enum GRace::Tier {
	kRaceTier_None = 0,
	kRaceTier_1 = 1,
	kRaceTier_2 = 2,
	kRaceTier_3 = 3,
	kRaceTier_NumTiers = 4,
};

enum GRace::TrafficLevel {
	kRaceTraffic_Off = 0,
	kRaceTraffic_Light = 1,
	kRaceTraffic_Medium = 2,
	kRaceTraffic_Heavy = 3,
	kRaceTraffic_NumLevels = 4,
};

enum GRace::Type {
	kRaceType_None = -1,
	kRaceType_P2P = 0,
	kRaceType_Circuit = 1,
	kRaceType_Knockout = 2,
	kRaceType_Tollbooth = 3,
	kRaceType_SpeedTrap = 4,
	kRaceType_Checkpoint = 5,
	kRaceType_CashGrab = 6,
	kRaceType_Challenge = 7,
	kRaceType_JumpToSpeedTrap = 8,
	kRaceType_JumpToMilestone = 9,
	kRaceType_Drift = 10,
	kRaceType_PursuitTag = 11,
	kRaceType_PursuitKnockout = 12,
	kRaceType_Encounter = 13,
	kRaceType_NumTypes = 14,
};

enum GRace::WingmanShortcutTriggerHint {
	kShortcutTrigger_None = 0,
	kShortcutTrigger_Init = 1,
	kShortcutTrigger_Entry = 2,
	kShortcutTrigger_Pos = 3,
	kShortcutTrigger_Exit = 4,
	kShortcutTrigger_All = 5,
};

enum GameCore::AbortReason {
	kAbort_Disconnect = 0,
	kAbort_Exit = 1,
};

enum GameCore::Activities::Components::OverrideVehicleControl::Control {
	Coast = 0,
	Stop = 1,
	Release = 2,
};

enum GameCore::Activities::Entrant::Components::EnableGadget::GadgetToggle {
	Enable = 1,
	Disable = 2,
	Dispose = 6,
};

enum GameCore::Activities::Entrant::Components::EnableHud::HudToggle {
	Enable = 1,
	Disable = 2,
	Dispose = 6,
};

enum GameCore::BehaviorFlag {
	kBehavior_Activatable = 1,
	kBehavior_AutoActive = 2,
};

enum GameCore::BlackBoardChannel {
	kBlackBoard_Audio = 0,
	kBlackBoard_Frontend = 1,
	kBlackBoard_AI = 2,
	kBlackBoard_WorldMap = 3,
	kBlackBoard_Count = 4,
};

enum GameCore::BlackBoardFlag {
	kBlackBoardFlag_Loading = 1,
	kBlackBoardFlag_Running = 2,
};

enum GameCore::Collision::TriggerStimulus {
	kNone = 0,
	kEntry = 1,
	kExit = 2,
};

enum GameCore::Collision::TriggerType {
	Unknown = 0,
	Cylinder = 1,
	Box = 2,
	Plane = 3,
};

enum GameCore::ConnectionSupport {
	kConnectionNone = 0,
	kConnectionOffline = 1,
	kConnectionOnline = 2,
	kConnectionPrivate = 4,
};

enum GameCore::DriverSkill {
	kDriverSkill_None = 0,
	kDriverSkill_CollisionMass = 1,
	kDriverSkill_PerfectStart = 2,
	kDriverSkill_FastCooldown = 3,
	kDriverSkill_ExtendedNOS = 4,
	kDriverSkill_CopRadar = 5,
	kDriverSkill_CatchUp = 6,
	kDriverSkill_Dragster = 7,
	kDriverSkill_NeedForSpeed = 8,
	kDriverSkill_NervesOfSteel = 9,
	kDriverSkill_RapidFire = 10,
	kDriverSkill_StrongFinisher = 11,
	kDriverSkill_SuperShot = 12,
	kDriverSkill_TrafficCop = 13,
	kDriverSkill_MostWanted = 14,
	kDriverSkill_Socialite = 15,
	kDriverSkill_Tycoon = 16,
	kDriverSkill_Terminator = 17,
	kDriverSkill_DemolitionMan = 18,
	kDriverSkill_Evasion = 19,
	kDriverSkill_Untouchable = 20,
	kDriverSkill_Rampage = 21,
	kDriverSkill_JumpStart = 22,
	kDriverSkill_RollingFortress = 23,
	kDriverSkill_Vampire = 24,
	kDriverSkill_LightningReflex = 25,
	kDriverSkill_Scout = 26,
	kDriverSkill_Count = 27,
};

enum GameCore::EffectType {
	ENTER_ENGAGE_TRIGGER = 0,
	ENTER_COP_PURSUIT = 1,
	COP_DESTROYED = 2,
	TRAFFIC_DESTROYED = 3,
};

enum GameCore::EventMode {
	kEventMode_Unknown = 0,
	kEventMode_TollBooth = 1,
	kEventMode_Canyon = 2,
	kEventMode_Challenge = 3,
	kEventMode_Circuit = 4,
	kEventMode_Drift = 5,
	kEventMode_PursuitKnockOut = 6,
	kEventMode_PursuitTag = 7,
	kEventMode_SpeedTrap = 8,
	kEventMode_Sprint = 9,
	kEventMode_CanyonDrift = 10,
	kEventMode_Pursuit = 12,
	kEventMode_LapKnockOut = 15,
	kEventMode_Drag = 19,
	kEventMode_CoopPursuit = 21,
	kEventMode_MeetingPlace = 22,
};

enum GameCore::EventType {
	kEventType_P2P = 0,
	kEventType_Circuit = 1,
	kEventType_Drag = 2,
	kEventType_Knockout = 3,
	kEventType_Tollbooth = 4,
	kEventType_SpeedTrap = 5,
	kEventType_Checkpoint = 6,
	kEventType_CashGrab = 7,
	kEventType_Challenge = 8,
	kEventType_JumpToSpeedTrap = 9,
	kEventType_Drift = 10,
	kEventType_CanyonDrift = 11,
	kEventType_Canyon = 12,
	kEventType_PursuitTag = 13,
	kEventType_PursuitKnockout = 14,
	kEventType_Encounter = 15,
	kEventType_CopsAndRobbers = 16,
	kEventType_SmugglersRun = 17,
	kEventType_MilestoneJump = 18,
	kEventType_Blah1 = 19,
	kEventType_Blah2 = 20,
	kEventType_Blah3 = 21,
	kEventType_MeetingPlace = 22,
	kEventType_UnknownType = 23,
	kEventType_Count = 24,
};

enum GameCore::FLASHER_TYPE {
	COUNTDOWN_3_2_1_GO = 0,
	SPLIT_TIME = 1,
	LAP_TIME = 2,
	WRONG_WAY = 3,
	START_RACE = 4,
	PERFECT_LAUNCH = 5,
};

enum GameCore::FinishReason {
	kReason_Unknown = 0,
	kReason_Completed = 2,
	kReason_CrossedFinish = 6,
	kReason_DidNotFinish = 10,
	kReason_KnockedOut = 26,
	kReason_Totalled = 42,
	kReason_EngineBlown = 74,
	kReason_Busted = 138,
	kReason_Evaded = 258,
	kReason_ChallengeCompleted = 514,
	kReason_Disconnected = 1034,
	kReason_FalseStart = 2058,
	kReason_Aborted = 4106,
	kReason_TimedOut = 8202,
};

enum GameCore::InRacePlayerStatusEnum {
	INRACEPLAYERSTATUSENUM_IN_RACE = 0,
	INRACEPLAYERSTATUSENUM_FINISHED = 1,
	INRACEPLAYERSTATUSENUM_DISCONNECTED = 2,
	INRACEPLAYERSTATUSENUM_KNOCKED_OUT = 3,
	INRACEPLAYERSTATUSENUM_ITEM_COUNT = 4,
};

enum GameCore::JuiceGC::eOpponentPerformance {
	eOpponentPerformance_MIN = 0,
	DISABLED = 1,
	MIN_PERFORMANCE = 2,
	MAX_PERFORMANCE = 3,
	eOpponentPerformance_MAX = 4,
};

enum GameCore::MilestoneType {
	kMilestone_None = 0,
	kMilestone_General_AirTime = 1,
	kMilestone_General_TopSpeed = 2,
	kMilestone_General_FastestTopSpeed = 3,
	kMilestone_General_NoPowerupsUsed = 4,
	kMilestone_General_PowerupsUsed = 5,
	kMilestone_General_PerfectStart = 6,
	kMilestone_Circuit_BestLapTime = 7,
	kMilestone_Pursuit_CopsDeployed = 8,
	kMilestone_Pursuit_CopsEvaded = 9,
	kMilestone_Pursuit_CopsRammed = 10,
	kMilestone_Pursuit_CopsDestroyed = 11,
	kMilestone_Pursuit_SpikestripsEvaded = 12,
	kMilestone_Pursuit_RoadblocksEvaded = 13,
	kMilestone_Pursuit_CostToState = 14,
	kMilestone_Pursuit_Infractions = 15,
	kMilestone_Pursuit_Bounty = 16,
	kMilestone_Pursuit_Heat = 17,
	kMilestone_Pursuit_Time = 18,
	kMilestone_Infraction_Speeding = 19,
	kMilestone_Infraction_Racing = 20,
	kMilestone_Infraction_RecklessDriving = 21,
	kMilestone_Infraction_AssaultPolice = 22,
	kMilestone_Infraction_HitAndRun = 23,
	kMilestone_Infraction_DamageToProperty = 24,
	kMilestone_Infraction_ResistingArrest = 25,
	kMilestone_Infraction_DrivingOffRoad = 26,
	kMilestone_Count = 27,
};

enum GameCore::ObjFlag {
	kFlag_None = 0,
	kFlag_Engagable = 64,
	kFlag_Safehouse = 128,
};

enum GameCore::ObjPersistence {
	kPersist_Resident = 0,
	kPersist_Transient = 1,
};

enum GameCore::ObjType {
	kObjType_Unknown = -1,
	kObjType_TrackLayout = 0,
	kObjType_Event = 1,
	kObjType_Trigger = 2,
	kObjType_Marker = 3,
	kObjType_Character = 4,
	kObjType_Milestone = 5,
	kObjType_PointOfInterestDefinition = 6,
	kObjType_Count = 7,
};

enum GameCore::OpponentStatus {
	kOpponent_Free = 1,
	kOpponent_Joining = 2,
	kOpponent_Ready = 4,
	kOpponent_Queued = 8,
	kOpponent_Competing = 16,
	kOpponent_Disposable = 32,
	kOpponent_Disconnected = 1024,
	kOpponent_Described = 2048,
	kOpponent_Loaded = 4096,
	kOpponent_Collidable = 8192,
};

enum GameCore::PoiBehavior {
	kPoi_Icon = 1,
};

enum GameCore::PointOfInterest::Flags {
	None = 0,
	Enabled = 1,
	Hidden = 2,
	Disposable = 3,
};

enum GameCore::PowerupConfiguration {
	kPowerupConfigFreeRoam = 0,
	kPowerupConfigCircuit = 1,
	kPowerupConfigSprint = 2,
	kPowerupConfigPursuit = 3,
};

enum GameCore::PursuitInstigator {
	kInstigator_Force = 1,
	kInstigator_Collision = 2,
	kInstigator_Trespass = 4,
};

enum GameCore::RankingPacket::PursuitStatusEnum {
	PURSUITSTATUSENUM_PS_INITIAL_CHASE = 0,
	PURSUITSTATUSENUM_PS_BACKUP_REQUESTED = 1,
	PURSUITSTATUSENUM_PS_BUSTED = 2,
	PURSUITSTATUSENUM_PS_EVADED = 3,
	PURSUITSTATUSENUM_PS_COOLDOWN = 4,
	PURSUITSTATUSENUM_ITEM_COUNT = 5,
};

enum GameCore::RewardMode {
	kRewardMode_Singleplayer = 1,
	kRewardMode_Multiplayer = 2,
	kRewardMode_PrivateMatch = 3,
};

enum GameCore::VaultSlot {
	kVaultSlot_Unknown = -2,
	kVaultSlot_Core = -1,
	kVaultSlot_First = 0,
	kVaultSlot_Event = 0,
	kVaultSlot_Track = 1,
	kVaultSlot_Count = 2,
};

enum GameCore::Workflow::ActivityExecutionStatus {
	Initialized = 0,
	Executing = 1,
	Canceled = 2,
	Faulting = 3,
	Faulted = 4,
	Bookmarked = 5,
	Closed = 6,
};

enum GameCore::Workflow::BookmarkOptions {
	MultiResume = 0,
	SingleResume = 1,
};

enum PowerUps::GameMode {
	kPowerupGameMode_None = 0,
	kPowerupGameMode_Explore = 1,
	kPowerupGameMode_Circuit = 2,
	kPowerupGameMode_Sprint = 3,
	kPowerupGameMode_Pursuit = 4,
};

enum PowerUps::Restriction {
	kPowerupRestriction_None = 0,
	kPowerupRestriction_NobodyHasFinishedEvent = 1,
	kPowerupRestriction_IAmNotInFirstPlace = 2,
	kPowerupRestriction_IAmOnTheLastLap = 3,
	kPowerupRestriction_IHaveAFlatTire = 4,
	kPowerupRestriction_IHaveAtLeastOnePowerupRecharging = 5,
	kPowerupRestriction_IAmInPursuitCooldown = 6,
	kPowerupRestriction_IAmNotReversing = 7,
	kPowerupRestriction_IAmInARace = 8,
	kPowerupRestriction_IAmInAPursuit = 9,
	kPowerupRestriction_IAmNotNOSing = 10,
	kPowerupRestriction_IAmNotAutoDriving = 11,
	kPowerupRestriction_ICanOnlyUseThisOncePerRace = 12,
	kPowerupRestriction_WeCanOnlyUseThisOncePerRace = 13,
};

struct GameCore::ObjFlags {
	unsigned int mBits; // +0, 4 bytes
};

struct GameCore::BehaviorFlags {
	unsigned int mBits; // +0, 4 bytes
};

struct GameCore::BehaviorSlot {
	GameCore::StringKey mBehaviorChannel; // +0, 4 bytes
	GameCore::StringKey mBehaviorType; // +4, 4 bytes
	GameCore::BehaviorFlags mFlags; // +8, 4 bytes
};

struct GameCore::BlackBoardFlags {
	unsigned int mBits; // +0, 4 bytes
};

struct GameCore::BlackBoardSlot {
	GameCore::BlackBoardChannel mChannel; // +0, 4 bytes
	GameCore::StringKey mBlackBoardKey; // +4, 4 bytes
	GameCore::BlackBoardFlags mFlags; // +8, 4 bytes
};

struct GameCore::PoiBehaviorFlags {
	unsigned int mBits; // +0, 4 bytes
};

struct GameCore::LocalizationHash {
	unsigned int Hash; // +0, 4 bytes
};

struct GameCore::PursuitArbitrationRecord {
	GameCore::FinishReason mFinishReason; // +0, 4 bytes
	int mCopsDisabled; // +4, 4 bytes
	int mCopsEngaged; // +8, 4 bytes
	int mCopsTagged; // +12, 4 bytes
	int mCostToState; // +16, 4 bytes
	int mSpikeStripsDodged; // +20, 4 bytes
	int mRoadBlocksDodged; // +24, 4 bytes
	int mPursuitInfractions; // +28, 4 bytes
	float mEventTime; // +32, 4 bytes
};

struct GMapCurvePoint {
	float mX; // +0, 4 bytes
	float mY; // +4, 4 bytes
};

struct IndexedBarrierName {
	int mIndex; // +0, 4 bytes
};

struct GStringHash {
	unsigned int hash; // +0, 4 bytes
};

struct GMapCurve {
	unsigned short mPointStart; // +0, 2 bytes
	unsigned short mPointCount; // +2, 2 bytes
};

struct GMapCurveRef {
	unsigned short mCurveIndex; // +0, 2 bytes
	unsigned short mFlags; // +2, 2 bytes
};

struct GMapTriangle {
	unsigned short mPoint0; // +0, 2 bytes
	unsigned short mPoint1; // +2, 2 bytes
	unsigned short mPoint2; // +4, 2 bytes
};

struct GMapRegionInfo {
	const char * mName; // +0, 4 bytes
	unsigned short mCurveStart; // +4, 2 bytes
	unsigned short mCurveCount; // +6, 2 bytes
	unsigned short mTriangleStart; // +8, 2 bytes
	unsigned short mTriangleCount; // +10, 2 bytes
};

