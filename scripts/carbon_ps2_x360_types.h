// The type Carbon's PS2, GameCube and retail 360 files (the three share a Vers
// value) size differently from the PC's: 20 bytes, against 28 in every header
// (the PS3 milestone's, Pro Street's, Undercover's). No header of a 20-byte
// build is known. gen_schema.py --game carbon --platform ps2 (carbon_ps2.txt,
// the PS2 and the GameCube) and --platform x360 (carbon_x360.txt) read this
// before the PS3 milestone's header and lay it out naturally; it has no bool, so
// 4-byte bools change nothing. It is used only where the type table has its
// size.

// The PC's without the two dead zones. The data fits that and nothing else
// tells: on all three platforms every value but controller/drive's is zero, and
// drive's three sensitivities hold 1, 1.0, 1.0, 1.0 and 2 steps (a u16 at +16,
// the 360's big-endian word 0x00020000), as the PC's hold them with dead zones
// of 0. A
// dead zone of 1.0 would take all of the stick, so the three floats are the
// curve's. The Wii's ApplySteeringCurve (0x800396E8, the 28-byte struct) reads
// all seven members, the dead zones at +16 and +20. The 360's default.xex never
// builds these three fields' keys, nor does NFSC.exe: the 360 doesn't read them.
// No PS2 or GameCube executable of Carbon is here to say.
struct SteeringSensitivityParameter {
	enum eSteeringCurveStyle CurveStyle;
	float CurvePower;
	float CurveMultiplierLowSpeed;
	float CurveMultiplierHighSpeed;
	unsigned short NumberOfSteps;
};
