// The early Most Wanted demos' types that differ from retail Most Wanted's
// (the PS2 OPM and Xbox OXM beta demos; doc/vault-format.md). No header of
// those builds is known: these come from their files. Keys are StringHash64,
// so the types that hold them are wider; member names are retail Most
// Wanted's (exes/gamecube/mw-vanilla/NFSMWRELEASE.h). gen_schema.py --game mw
// --demo reads this before the retail header's other fallbacks and lays it
// out naturally; a struct is used only where the demo's type table has its
// size. Read from the data: the keys at these offsets resolve to class,
// collection and field keys, the RefSpecs' pointers are listed as null at +16,
// and the numbers after them read sensibly. EmitterDataRecord is inferred: no
// field of the demos uses it.

struct RefSpec /* size: 0x18 */  {
	unsigned long long mClassKey;
	unsigned long long mCollectionKey;
	const Collection * mCollectionPtr;
};

struct GCollectionKey /* size: 0x8 */  {
	unsigned long long mCollectionKey;
};

// Goal stays a StringHash32 (UCrc32 of an AI goal's name).
struct AICollisionReactionRecord /* size: 0x20 */  {
	unsigned int Goal;
	RefSpec Reaction;
};

// Retail's PriorityChance and PriorityTime aren't there yet.
struct LeaderSupport /* size: 0xc */  {
	LeaderSupportStrategy LeaderStrategy;
	unsigned int Chance;
	float Duration;
};

struct EffectLinkageRecord /* size: 0x38 */  {
	RefSpec mSurface;
	RefSpec mEffect;
	float mMinSpeed;
	float mMaxSpeed;
};

struct JunkmanMod /* size: 0x18 */  {
	unsigned long long ClassKey;
	unsigned long long DefinitionKey;
	float Scale;
};

// Retail's Percent isn't there yet.
struct TrafficPatternRecord /* size: 0x20 */  {
	RefSpec Vehicle;
	float Rate;
	unsigned int MaxInstances;
};

struct TireEffectRecord /* size: 0x20 */  {
	RefSpec mEmitter;
	float mMinSpeed;
	float mMaxSpeed;
};

struct CollisionStream /* size: 0x20 */  {
	RefSpec StreamMoment;
	unsigned char Threshold;
};

struct EmitterDataRecord /* size: 0x28 */  {
	RefSpec mEmitterData;
	float mMinIntensity;
	float mMaxIntensity;
	bool mNormalize;
};
