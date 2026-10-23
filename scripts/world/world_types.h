// World's own types that neither Carbon's and Undercover's headers nor the
// 2010 beta's EASharp bindings (easharp_types.h) describe. No header of any
// World build is known: these come from NFS-VltEd's World classes (NFS-VltEd/,
// read by scripts/nfsms; it names some members and numbers others), the final
// build's data and the code of its nfsw.exe. scripts/gen_schema.py --game
// world reads this with easharp_types.h before Carbon's headers and lays it out
// naturally; a struct is used only where World's type table has its size.
// Where neither the data nor the code names a member, it is Value<n>, after
// NFS-VltEd's property number <n>.

// A StringHash32 of a name (gameplay.VehicleKey: copcarmus08gh, the
// BlackBoardMap's ai_event). EASharp exposes only GetKey().
struct GameCore::StringKey {
	unsigned int mKey;
};

// A string (commerce.bin's names and descriptions). NFS-VltEd reads one char*.
struct Commerce::LocalizedString {
	const char * String;
};

// NFS-VltEd's names. KeyHash is the StringHash32 of KeyString's text (a cop
// vehicle: copmidsize), as in CopCountRecord's CopTypeHash32; Value is the
// weight (1, 2, 5).
struct GameCore::KeyValuePair {
	const char * KeyString;
	unsigned int KeyHash;
	float Value;
};

// ecar.IntegratedKitWheelOffsetFront/Rear: a kit's Attrib::StringKey
// (CROWNCOP_KIT12_INTEGRATED_KIT) and an offset (20 to 110). NFS-VltEd numbers
// them 1, 2 (the string) and 3.
struct IntegratedKitWheelOffset {
	StringKey Kit;
	int Offset;
};

// partcollections.hat_bonus: the game reads +12 as the parts a set needs (2, 4,
// 7 in every set) and +0, +4, +8 as three stats (nfsw.exe 0x66CCB0). NFS-VltEd
// names them alphabetically, RequiredPartCount third; the stats keep its order.
struct Commerce::HatBonus {
	int Acceleration;
	int Handling;
	int TopSpeed;
	int RequiredPartCount;
};

// blackboard.IgnoreInteractions. NFS-VltEd's names.
struct GameCore::PhysicsEntityPair {
	int Entity1;
	int Entity2;
};

// powerup.GameplayEffects: an effect (powerup_gamegroup/slingshot_torque),
// then two ints. The game passes the last to the powerup's choice of targets
// (nfsw.exe 0x79C75A); the third is 0 in every record.
struct PowerUps::GameplayEffectAndTarget {
	unsigned int EffectClassKey;
	unsigned int EffectCollectionKey;
	int Value3;
	int Target;
};

// powerup.VisualEffects: the same with an emitter group (emittergroup/
// fxgame_sling) and a float (15, 20, 0.4).
struct PowerUps::EmitterEffectAndTarget {
	unsigned int EffectClassKey;
	unsigned int EffectCollectionKey;
	int Value3;
	int Target;
	float Value5;
};

// powerup.AudioEffects: { 8, 0 }, { 3, 1 }. The type table has ePowerUpTrigger
// and ePowerUpTarget, whose values no source names.
struct PowerupTriggerAndTarget {
	int Trigger;
	int Target;
};

// frontend_sfx.MixTriggerMapping: a front end message's hash and a trigger,
// as in Carbon's FEMsgToSoundTrigger.
struct FEMsgToMixTrigger {
	unsigned int FEngMsg;
	int MixTrigger;
};

// frontend_sfx.HudSoundMapping: a string id's hash and a trigger.
struct HudSFXStringIdToSoundTrigger {
	unsigned int StringId;
	int SoundTrigger;
};

// curvebased_cameratunings.tunings (the 2009 beta's data; the final build has
// the type and no value of it). NFS-VltEd reads 27 numbers; the pointers at
// +28, +36, +44 and +80, +88, +96 are two Curves' arrays. The two ints before
// them are 0 to 4.
struct CameraCauseAndEffect {
	int Cause;
	int Effect;
	Curve Curve1;
	int Value15;
	Curve Curve2;
};

// combat_abilities.abilities (the 2009 beta's; the final build has the type
// and no field of it): an ability, 3 to 15 (the type table has a 4-byte
// SpecialAbilityType), then a float (30, 20, -50), two ints (0, 1) and a
// float (0). NFS-VltEd numbers them 1 to 5.
struct SpecialAbility {
	int Type;
	float Value2;
	int Value3;
	int Value4;
	float Value5;
};
