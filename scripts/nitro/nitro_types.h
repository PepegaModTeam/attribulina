// Nitro's own types that neither the old Wii build's DWARF
// (exes/wii/nitro-japan-oldelf/nfs_na_f.elf) nor Carbon's and Undercover's
// headers describe as retail has them. They come from the retail files
// (attrib/databases/wii/nitro-vanilla), the old build's types and the retail
// code (exes/wii/nitro-vanilla/main.dol, no symbols).
// scripts/gen_schema.py --game nitro --platform wii reads this before the
// DWARF and lays it out naturally; a struct is used only where Nitro's type
// table has its size. A member nothing names is Value<offset>.

// interactive_music_settings/tracks.Music_Tracks: "AlltheCash", Evil 9 - "All
// The Cash (Alex Metric Remix) Feat. El-P", 0.91. The old build's sMusicTrack,
// MusicTracksManager's copy of one, is these members and an IsEnable.
struct MusicTrack {
	char * Name;
	char * ScreenName;
	float Volume;
};

// frontend_sfx/feng_sound_map.AEMSSoundMapping: the bStringHash of a front end
// message, named as its sound is (BTN_BACK), then the old build's
// FESoundMapping members (mSoundId 3, mIsEnabled 1, Volume 0.25). FEngMsg as
// in FEMsgToSoundTrigger, the message to sound table Carbon has.
struct FEMsgToAEMSId {
	type_bStringHash FEngMsg;
	char * mSoundName;
	unsigned short mSoundId;
	bool mIsEnabled;
	float Volume;
};

// presetridepackage.PresetRidesAndName: a preset ride, by the bStringHash of
// its name in capitals (MCOROLLA_MADRID_01; gameplay names it in lower case),
// and the AI racer who drives it (MADRID_AI_RACER_2, $JFCHABOT).
struct GPresetRidesAndName {
	type_bStringHash PresetRide;
	char * Name;
};

// parameter_maps/reverb.ReverbLayers (NoReverb, Tunnel, Alcove). The old
// build's tag is 24 bytes: SpamLayerId, AemsId, DebugName, then Delay,
// SpaceSize and Brightness, the Xbox 360's reverb (its stXENONREVERB). Retail
// drops AemsId and has seven floats. Its reader (main.dol 0x800D8810 copies a
// tag, 0x800D8588 uses it) copies +8, +12, +20, +24 and +28 into a block of
// six floats, +28 twice, and +16 apart; +32 isn't read there. The old build's
// stREVERB_PARAMS is six floats, GinsuWet, GinsuDry, AemsWet, AemsDry,
// FadeOut and FadeIn, which these take their names from in the order the
// reader fills the block (inferred: no retail code names them). Tunnel is
// 0.13, -0.2, 1.0, 0.2, 0.2, 1.0, 1.0.
struct ParamaterMapReverbTag {
	int SpamLayerId;
	char * DebugName;
	float GinsuWet;
	float GinsuDry;
	float Value16;
	float AemsWet;
	float AemsDry;
	float FadeOut;
	float FadeIn;
};

// EmittedGeo: EmittedGeometryRecord's mEnum (emitterdata). Each value is the
// bStringHash of the geometry's name in capitals, without the prefix (Carbon's
// DEBRIS_CHUNKY_01 is 1283620681). Carbon's list, then the five checkpoint
// geometries the files hold (Undercover's Wii files too), named by their
// hashes. EmittedGeo_NUM is Carbon's count.

enum EmittedGeo {
	EmittedGeo_NONE = 0,
	EmittedGeo_DEBRIS_CHUNKY_01 = 1283620681,
	EmittedGeo_DEBRIS_CUBE_01 = 1563803062,
	EmittedGeo_DEBRIS_GEOMETRY_FLARERING_00 = -773148581,
	EmittedGeo_DEBRIS_LEAF_01 = -325203377,
	EmittedGeo_DEBRIS_PAPER_01 = 542794927,
	EmittedGeo_DEBRIS_PAPER_02 = 542794928,
	EmittedGeo_DEBRIS_PLANK_01 = 1287431533,
	EmittedGeo_DEBRIS_PLANK_02 = 1287431534,
	EmittedGeo_DEBRIS_ROCK_01 = -772424698,
	EmittedGeo_DEBRIS_STICK_01 = -1959378539,
	EmittedGeo_FX_MARKER_RING_FUSHIA = -694503828,
	EmittedGeo_FX_MARKER_RING_GREEN = -540584899,
	EmittedGeo_FX_MARKER_RING_LAVENDER = 1961640925,
	EmittedGeo_FX_MARKER_RING_ORANGE = -346483448,
	EmittedGeo_FX_MARKER_RING_TFK = 713373265,
	EmittedGeo_FX_MARKER_RING_WHITE = -521964691,
	EmittedGeo_FX_MARKER_RING_YELLOW = 29846920,
	EmittedGeo_CHECKPOINT_GEOMETRY_DRAG_00 = -924805330,
	EmittedGeo_CHECKPOINT_GEOMETRY_HIDECAR_00 = -765907104,
	EmittedGeo_CHECKPOINT_GEOMETRY_KNOCKOUT_00 = 1934256734,
	EmittedGeo_CHECKPOINT_GEOMETRY_SAFEHOUSE_00 = -1640881197,
	EmittedGeo_CHECKPOINT_GEOMETRY_SPEEDTRAP_00 = 842195704,
	EmittedGeo_NUM = 18,
};

// audio_lod's AiCarSfx, CopCarSfx, TrafficCarSfx and TruckSfx (no collection
// sets them): four bools each, 1-aligned as the class stores them. The old
// build's GetFlags (nfs_na_f.elf 0x800D69B0 to 0x800D6B04) turns each into a
// flag bit, and PlayerCarSfx's and MultiplayerCarSfx's members name those bits
// (0x1 useEngine, 0x4 useTurbos, 0x20 useSkids, 0x4000 useSiren, ...). A bool
// whose bit they don't name is Value<offset>; AiCarSfx's last isn't read.
struct nfs::Audio::AiCarSfx {
	bool useEngine;
	bool useTurbos;
	bool useSkids;
	bool Value3;
};

struct nfs::Audio::CopCarSfx {
	bool useEngine;
	bool useSiren;
	bool useSkids;
	bool Value3;
};

struct nfs::Audio::TrafficCarSfx {
	bool useEngine;
	bool useNitro;
	bool useShifts;
	bool useTurbos;
};

struct nfs::Audio::TruckSfx {
	bool useEngine;
	bool useTruckFx;
	bool Value2;
	bool Value3;
};

// stylepoints/feedback_table's Air_, Draft_, Drift_ and NOS_feedback: four
// brackets of style points each, 2-aligned as the class stores them. Air's are
// 100 to 175, 176 to 250, 251 to 325, then 326 and 0 (no top). Named from the
// data.
struct StylePointsFeedback {
	unsigned short Min;
	unsigned short Max;
};
