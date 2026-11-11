// The types Undercover's 360 alpha (build 161, 360/undercover-alpha161) sizes
// differently from retail. No header of that build is known: these come from
// its files, read against retail's members (nfs-data/SpeedDebugOpt-enUS.h, the
// 360 debug build, whose structs these are with members left out or Pro
// Street's). gen_schema.py --game undercover --platform x360 --alpha reads
// this before the retail header and lays it out naturally; a struct is used
// only where the alpha's type table has its size.

// Pro Street's (ProStreet08Milestone.h, 24 bytes), with retail's name for
// the first member (Pro Street's Elasticity); retail has no stun members. The
// first four floats hold retail's values (0, 0, 0.5, 2.6 in seven
// FRONT_REACTION values, as in retail); the stun members are 0 in every
// collisionreactions value.
struct CollisionReactionRecord {
	float Restitution;
	float RollHeight;
	float WeightBias;
	float MassScale;
	float StunSpeed;
	float StunTime;
};

// Pro Street's (20 bytes). mIsMember is 1 in speed_limiter's elements and
// mMemberIndex counts them (0, 1); mBlendingPower is 1.0 as in retail.
struct PhysicsUpgrade {
	unsigned int mClassKey;
	unsigned int mDefinitionKey;
	bool mIsMember;
	unsigned int mMemberIndex;
	float mBlendingPower;
};

// Retail's without mChangeDist (0 in every retail element). The last word
// is 0, 1 or 2 (LANECHG_NONE, LEFT, RIGHT), as retail's mChange is.
struct HighwayPatternCarData {
	int row;
	int lane;
	Attrib::RefSpec mVehicle;
	enum EAILaneChangeType mChange;
};

// Retail's without mMaterialFlags, whose type ePartMaterialFlags the alpha's
// type table lacks. SpeedDebugOpt-enUS.h defines it this way too, beside
// retail's.
struct PresetRidePaint {
	enum ePaintSlot mSlotID;
	Attrib::RefSpec mGroup;
	unsigned char mSwatch;
	unsigned char __align0[3];
	float mSaturation;
	float mVariance;
};

// 32 bytes: the 28-byte paint, then chance (33 in trf_sem, as retail).
struct PresetRideRandomPaint {
	PresetRidePaint paint;
	float chance;
};

// Retail's without mBrilinearSlope, mConv and mDoGammaCorrectionPS3, whose
// enums (State_TrilinearThreshold_PS3, State_Conv_PS3,
// State_GammaCorrection_PS3) the alpha's type table lacks; it has
// State_ZFunc_PS3. The first twelve words hold retail's values, and the last
// is 0 in every sampler, as retail's mZFunc is (retail's mBrilinearSlope is
// mostly 31).
struct RenderReflect::cSamplerStateAttribDefinition {
	char * mName;
	enum RenderReflect::State_SamplerAddressingMode mAddressU;
	enum RenderReflect::State_SamplerAddressingMode mAddressV;
	enum RenderReflect::State_SamplerAddressingMode mAddressW;
	float mBias;
	unsigned int mMaxLevel;
	unsigned int mMinLevel;
	unsigned int mMaxAnisotropy;
	enum RenderReflect::State_SamplerFilterMode mMagFilter;
	enum RenderReflect::State_SamplerFilterMode mMinFilter;
	enum RenderReflect::State_SamplerFilterMode mMipFilter;
	enum RenderReflect::State_TrilinearThreshold_XENON mTrilinearThreshold;
	enum RenderReflect::State_ZFunc_PS3 mZFunc;
};

// Retail's first 52 bytes: pursuitaudio's SirenTune values are retail's
// there (200, 30, 0.4, 1, ...). Retail's MaxWailing, MaxYelping,
// MaxPriorityCount and PriorityRadius_Online come after.
struct Sound::SirenTuning {
	float OpRadiusLimit[2];
	float SpeedThresh;
	float HornLimit[2];
	float PriorityLimit[2];
	float WailLimit[2];
	float YelpLimit[2];
	float LoopXFadeRange[2];
};
