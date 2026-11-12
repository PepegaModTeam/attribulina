// Undercover's Wii and PS2 types that differ from Carbon's, whose engine the
// game runs on (gen_schema.py --game undercover --platform wii/ps2 reads this
// before Carbon's header). No header of those builds is known.
//
// eLightFlareTextures: light_flares_cg's flare_texture, which
// UpdateLightFlareParameters copies into eLightFlareParams::TextureID
// (0x8035BB28 in the Wii's NfsMWEuropeEnDuRelease.elf). The Wii's
// eLightFlareTextureNameHashes (0x805F3150) lists eleven textures: Carbon's
// five, then the sun's six (sun_glare uses 5, sun_flare 6). Those six values
// are named after their textures (bStringHash names), as the others are.
// The PS2's executable isn't read here; it is the Wii's game.

enum eLightFlareTextures {
	ESLF_TEX_HEADLIGHT_INNER = 0,
	ESLF_TEX_HEADLIGHT_OUTER = 1,
	ESLF_TEX_HEADLIGHT_GLOW = 2,
	ESLF_TEX_CATSEYE = 3,
	ESLF_TEX_LAMPPOSTFLARE = 4,
	ESLF_TEX_SUNCENTER = 5,
	ESLF_TEX_SUNCOMBINED = 6,
	ESLF_TEX_SUNHALO = 7,
	ESLF_TEX_SUNMINORRAYS = 8,
	ESLF_TEX_SUNMAJORRAYS = 9,
	ESLF_TEX_SUNRING = 10,
	ESLF_NUM_TEXTURES = 11,
};

// EmittedGeo: EmittedGeometryRecord's mEnum (emitterdata). Each value is the
// bStringHash of the geometry's name in capitals, without the prefix (Carbon's
// DEBRIS_CHUNKY_01 is 1283620681). Carbon's list, then the five checkpoint
// geometries its files hold too, named by their hashes. EmittedGeo_NUM is
// Carbon's count.

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
