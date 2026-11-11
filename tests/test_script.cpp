// Mod scripts (core/Script.h): the language, applying each kind of value and
// statement, and scripts made from databases, which must give back exactly
// the values they were made from.

#include "Test.h"

#include "core/Curve.h"
#include "core/Edit.h"
#include "core/Folder.h"
#include "core/Gameplay.h"
#include "core/Hash.h"
#include "core/Script.h"
#include "core/Writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

using namespace attrib;
namespace sc = attrib::script;

namespace {

std::string Vanilla(const char* file) { return test::DataPath(std::string("attrib/databases/pc/prostreet-vanilla/") + file); }

std::unique_ptr<Database> LoadAll(bool gameplay = true) {
    auto db = std::make_unique<Database>();
    db->addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    for (const char* f : {"ATTRIBUTES.BIN", "cars_vault.BIN", "FE_ATTRIB.BIN"}) db->loadPack(Vanilla(f));
    if (gameplay) db->loadPack(Vanilla("gameplay.lzc"));
    return db;
}

sc::ApplyReport Run(Database& db, const std::string& text, const sc::ApplySettings& st = {}) {
    return sc::Apply(db, sc::Parse(text, "test.amod"), st);
}

std::string Messages(const sc::ApplyReport& r) {
    std::string s;
    for (const sc::Message& m : r.messages) s += std::to_string(m.pos.line) + ": " + m.text + "\n";
    return s;
}

#define CHECK_OK(r)                                                                  \
    do {                                                                             \
        if (!(r).ok) {                                                               \
            std::fprintf(stderr, "%s", Messages(r).c_str());                         \
            CHECK((r).ok);                                                           \
        }                                                                            \
    } while (0)

Collection& Col(Database& db, const char* cls, const char* name) {
    Collection* c = db.findCollection(StringHash32(cls), StringHash32(name));
    if (!c) std::fprintf(stderr, "no %s/%s\n", cls, name);
    CHECK(c != nullptr);
    return *c;
}

Attribute& Own(Collection& c, const char* field) {
    Attribute* a = c.find(StringHash32(field));
    if (!a) std::fprintf(stderr, "no own %s\n", field);
    CHECK(a != nullptr);
    return *a;
}

float F32(const Attribute& a, uint32_t i = 0, uint32_t off = 0) { return loadf32(a.data.bytes.data() + i * a.elemSize + off, Endian::Little); }
uint32_t U32(const Attribute& a, uint32_t i = 0, uint32_t off = 0) { return load32(a.data.bytes.data() + i * a.elemSize + off, Endian::Little); }

// Statements that change something (not the header, meta lines or comments).
size_t Changes(const sc::Script& s) {
    size_t n = 0;
    for (const sc::Statement& st : s.statements)
        n += st.kind != sc::Statement::Kind::Header && st.kind != sc::Statement::Kind::Meta && st.kind != sc::Statement::Kind::Comment;
    return n;
}

} // namespace

TEST("script: parse and print reach a fixed point") {
    const std::string text = R"(# A test mod
attribulina 1
game prostreet_pc
title "Test \"mod\""
option power number "Torque scale" default 1.1 min 1 max 2
option tier choice "Tier" choices ["street", "race"] default "race"
option kits bool "Body kits" default true help "Adds the kits"

edit pvehicle/911gt3 {
    MAX_RPM = 9000 was 8400  # trailing comment
    ENGINE.TORQUE[2] *= $power
    "odd name"[1].x = -0.5
    TIRES = [(1, 2, 3, 4), (5, 6, 7, 8)]
    REF = pvehicle/default
    KEY = 0x1234ABCD
    BLOB = bytes "0a 0b"
    S = { a = 1, b = [1, 2], c = { d = "e" } }
    revert OLD
    resize ARR 3
    insert ARR[0] = 5
    remove ARR[1]
    if $kits and not ($tier == "street") {
        KITS = true
    } else if $power {
        KITS = false
    } else {
        KITS = 0
    }
}
collection pvehicle/new_car under pvehicle/911gt3 in vault cars exact {
    MAX_RPM = $power * 2 + 1
    CODE = lua """
function h_1234()
  print("x")
end
"""
}
create pvehicle/other from pvehicle/911gt3 under root
delete pvehicle/old with children
rename pvehicle/a to "b c"
move pvehicle/a under root
move pvehicle/a to vault cars
create vault my_vault in gameplay.lzc
delete vault my_vault
edit gameplay/prototype_tracks/autobahn/autobahn_drag_2 {
    racestart = "$1 CAYMAN"
}
)";
    const sc::Script a = sc::Parse(text, "t.amod");
    const std::string once = sc::Print(a);
    const std::string twice = sc::Print(sc::Parse(once, "t.amod"));
    if (once != twice) std::fprintf(stderr, "--- once\n%s--- twice\n%s", once.c_str(), twice.c_str());
    CHECK(once == twice);
    // Spot checks of the model.
    CHECK_EQ(a.meta("title"), std::string("Test \"mod\""));
    CHECK_EQ(a.options().size(), size_t(3));
    CHECK(a.options()[1]->choices == (std::vector<std::string>{"street", "race"}));
    const sc::Statement& edit = a.statements[7];
    CHECK(edit.kind == sc::Statement::Kind::Edit);
    CHECK_EQ(edit.target.cls, std::string("pvehicle"));
    CHECK_EQ(edit.target.name, std::string("911gt3"));
    CHECK_EQ(edit.body[0].comment, std::string(" trailing comment"));
    CHECK(edit.body[0].was && edit.body[0].was->text == "8400");
    CHECK_EQ(edit.body[1].path.text(), std::string("ENGINE.TORQUE[2]"));
    CHECK(edit.body[1].op == '*');
    CHECK_EQ(edit.body[2].path.field, std::string("odd name"));
    CHECK(once.find("    \"odd name\"[1].x = -0.5\n") != std::string::npos);
    CHECK(once.find("REF = pvehicle/default\n") != std::string::npos);
    CHECK(once.find("rename pvehicle/a to \"b c\"\n") != std::string::npos);
    CHECK(once.find("edit gameplay/prototype_tracks/autobahn/autobahn_drag_2 {") != std::string::npos);
    const sc::Statement& gp = a.statements.back();
    CHECK_EQ(gp.target.name, std::string("prototype_tracks/autobahn/autobahn_drag_2"));
    // The Lua source keeps its lines.
    CHECK(once.find("function h_1234()\n  print(\"x\")\nend\n") != std::string::npos);
}

TEST("script: mistakes are reported with their line and column") {
    auto error = [](const std::string& text) -> std::string {
        try {
            sc::Parse(text, "bad.amod");
        } catch (const sc::ScriptError& e) {
            return e.what();
        }
        return "no error";
    };
    CHECK(error("attribulina 1\nedit pvehicle/a {\n    MAX_RPM 9000\n}\n").find("bad.amod:3:13: expected '=' after MAX_RPM") != std::string::npos);
    CHECK(error("edit pvehicle {\n}\n").find("bad.amod:1:15: expected '/'") != std::string::npos);
    CHECK(error("title \"open\n").find("unterminated string") != std::string::npos);
    CHECK(error("attribulina 3\n").find("newer Attribulina") != std::string::npos);
    CHECK(error("MAX_RPM = 3\n").find("unknown statement 'MAX_RPM'") != std::string::npos);
    CHECK(error("edit a/b {\n  X = 1\n").find("expected '}'") != std::string::npos);
    CHECK(error("option x choice \"X\"\n").find("needs choices") != std::string::npos);
}

TEST("script: values of each kind are written as given") {
    auto db = LoadAll();
    Database& d = *db;
    const ValueType hud = TypeOfAttribute(Own(Col(d, "controller", "drive"), "ControllerHudType"), "ControllerHudType");
    CHECK(hud.en && hud.en->values.size() > 1);
    const std::string hudName = hud.en->values[1].first;
    const int64_t hudValue = hud.en->values[1].second;
    const ValueType axle = TypeOfAttribute(Own(Col(d, "vehicle", "default"), "SHOCK_EXT_STIFFNESS"), "SHOCK_EXT_STIFFNESS");
    CHECK(axle.st && axle.st->members.size() >= 2);
    const std::string axleMember = axle.st->members[1].name;
    const uint32_t axleOff = axle.st->members[1].offset;

    const std::string text = "attribulina 1\ngame prostreet_pc\n"
                             "option scale number default 2\n"
                             "edit audio_revlimiter/default {\n"
                             "    load_for_short = 0.75\n"
                             "    rpm_drop_ratio_short = 0.5\n"
                             "    total_frames_short = 12\n"
                             "}\n"
                             "edit car/mclarenf1 { isBoosterPack = true }\n"
                             "edit controller/drive { ControllerHudType = " + hudName + " }\n"
                             "edit pvehicle/default { BEHAVIOR_MECHANIC_RIGIDBODY = \"my_body\" }\n"
                             "edit effects/default { EmitterQuadratic = (1, 2.5, -3, 4) }\n"
                             "edit skidaudio/default { ShockSqueakThreshPavement[1] = 5 }\n"
                             "edit collisionsfx_damage/hood { VolMap = [1, 2, 3, 4] }\n"
                             "edit vehicle/default {\n"
                             "    SHOCK_EXT_STIFFNESS[0]." + axleMember + " = 7.25\n"
                             "    THROTTLE_CONTROL_CURVE = { xMin = 0, xMax = 10, yMin = 0, yMax = 1, X = [0, 0.5, 1], Y = [0, 0.25, 1] }\n"
                             "}\n"
                             "edit frontend/customization { LevelColours[0] = (255, 1, 2, 3) }\n"
                             "edit car/gtrproto { defaultPaint = \"red\" }\n"
                             "edit fe_global/default { default_car_tuner = pvehicle/street }\n"
                             "edit gameplay/prototype_tracks/autobahn/autobahn_drag_2 { racestart = my_start_key }\n"
                             "edit wheel_data/generic { inner_radius_min = -5 }\n"
                             "edit audio_revlimiter/default { load_for_short *= $scale }\n";
    const sc::ApplyReport r = Run(d, text);
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(0));

    Collection& rev = Col(d, "audio_revlimiter", "default");
    CHECK(F32(Own(rev, "load_for_short")) == 1.5f); // 0.75, then *= 2
    CHECK(F32(Own(rev, "rpm_drop_ratio_short")) == 0.5f);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 12u);
    CHECK_EQ(int(Own(Col(d, "car", "mclarenf1"), "isBoosterPack").data.bytes[0]), 1);
    CHECK_EQ(int64_t(U32(Own(Col(d, "controller", "drive"), "ControllerHudType"))), hudValue);
    const Attribute& sk = Own(Col(d, "pvehicle", "default"), "BEHAVIOR_MECHANIC_RIGIDBODY");
    CHECK_EQ(U32(sk), StringHash32("my_body"));
    CHECK(sk.data.ptrAt(4) && sk.data.ptrAt(4)->text == "my_body");
    const Attribute& q = Own(Col(d, "effects", "default"), "EmitterQuadratic");
    CHECK(F32(q, 0, 0) == 1 && F32(q, 0, 4) == 2.5f && F32(q, 0, 8) == -3 && F32(q, 0, 12) == 4);
    CHECK(F32(Own(Col(d, "skidaudio", "default"), "ShockSqueakThreshPavement"), 1) == 5);
    const Attribute& sv = Own(Col(d, "collisionsfx_damage", "hood"), "VolMap");
    CHECK_EQ(sv.count, 4u);
    CHECK_EQ(U32(sv, 3), 4u);
    Collection& veh = Col(d, "vehicle", "default");
    CHECK(F32(Own(veh, "SHOCK_EXT_STIFFNESS"), 0, axleOff) == 7.25f);
    const CurveValue cv = ReadCurve(Own(veh, "THROTTLE_CONTROL_CURVE"), 0, Endian::Little);
    CHECK(cv.xMax == 10 && cv.x.size() == 3 && cv.y[1] == 0.25f);
    CHECK(cv.y2 == NaturalY2(cv.x, cv.y));
    const Attribute& lc = Own(Col(d, "frontend", "customization"), "LevelColours");
    CHECK(lc.data.bytes[0] == 255 && lc.data.bytes[1] == 1 && lc.data.bytes[3] == 3);
    const Attribute& paint = Own(Col(d, "car", "gtrproto"), "defaultPaint");
    CHECK(paint.data.ptrAt(0) && paint.data.ptrAt(0)->text == "red");
    const Attribute& ref = Own(Col(d, "fe_global", "default"), "default_car_tuner");
    CHECK_EQ(U32(ref, 0, 0), StringHash32("pvehicle"));
    CHECK_EQ(U32(ref, 0, 4), StringHash32("street"));
    CHECK_EQ(U32(Own(Col(d, "gameplay", "prototype_tracks/autobahn/autobahn_drag_2"), "racestart")), StringHash32("my_start_key"));
    CHECK_EQ(int(int8_t(Own(Col(d, "wheel_data", "generic"), "inner_radius_min").data.bytes[0])), -5);
    // Names the script used are remembered.
    CHECK(d.names().find(StringHash32("my_start_key")) != nullptr);
    CHECK(std::find(r.newNames.begin(), r.newNames.end(), "my_start_key") != r.newNames.end());
    CHECK(r.touched.size() == 4);
    for (const LoadedPack* p : r.touched) CHECK(p->dirty);

    // Mistakes stop it, at their line.
    auto fails = [&](const std::string& body, const char* what) {
        const sc::ApplyReport x = Run(d, "attribulina 1\n" + body);
        if (x.ok || Messages(x).find(what) == std::string::npos) std::fprintf(stderr, "%s", Messages(x).c_str());
        CHECK(!x.ok);
        CHECK(Messages(x).find(what) != std::string::npos);
    };
    fails("edit pvehicle/street { HornType = 300 }\n", "2: 300 is out of range for");
    fails("edit pvehicle/street { HornType = 1.5 }\n", "takes whole numbers");
    fails("edit pvehicle/nope { HornType = 1 }\n", "no collection pvehicle/nope");
    fails("edit pvehicle/street { NOPE = 1 }\n", "pvehicle has no field NOPE");
    fails("edit collisionsfx_damage/hood { VolMap[9] = 1 }\n", "VolMap[9] is past the end (VolMap has 4 elements");
    fails("edit rigidbodyspecs/default { GROUND_FRICTION = [1, 2, 3] }\n", "GROUND_FRICTION holds at most 2 elements (its layout space)");
    fails("edit effects/default { EmitterQuadratic = (1, 2) }\n", "takes 4 values");
    fails("edit controller/drive { ControllerHudType = NOT_A_VALUE }\n", "has no value NOT_A_VALUE");
    fails("edit car/gtrproto { defaultPaint = 5 }\n", "text takes a string");
    fails("edit audio_revlimiter/default { load_for_short = $missing }\n", "no option $missing");
}

TEST("script: was, options and conditions") {
    auto db = LoadAll(false);
    Database& d = *db;
    Collection& rev = Col(d, "audio_revlimiter", "default");
    const float before = F32(Own(rev, "load_for_short"));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", double(before));

    // A matching `was` is quiet; another value warns, and stops a strict run.
    sc::ApplyReport r = Run(d, std::string("attribulina 1\nedit audio_revlimiter/default { load_for_short = 2 was ") + buf + " }\n");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(0));
    r = Run(d, "attribulina 1\nedit audio_revlimiter/default { load_for_short = 3 was 99 }\n");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(1));
    CHECK(Messages(r).find("load_for_short was 99 in the script's database") != std::string::npos);
    CHECK(F32(Own(rev, "load_for_short")) == 3);
    sc::ApplySettings strict;
    strict.strict = true;
    r = Run(d, "attribulina 1\nedit audio_revlimiter/default { load_for_short = 4 was 99 }\n", strict);
    CHECK(!r.ok);

    const std::string text = "attribulina 1\n"
                             "option scale number \"Scale\" default 2 min 1 max 10\n"
                             "option loud bool default false\n"
                             "option mode choice choices [\"soft\", \"hard\"] default soft\n"
                             "edit audio_revlimiter/default {\n"
                             "    load_for_short = $scale * 0.5 + 1\n"
                             "    if $loud {\n"
                             "        total_frames_short = 100\n"
                             "    } else if $mode == \"hard\" {\n"
                             "        total_frames_short = 50\n"
                             "    } else {\n"
                             "        total_frames_short = 10\n"
                             "    }\n"
                             "}\n";
    r = Run(d, text);
    CHECK_OK(r);
    CHECK(F32(Own(rev, "load_for_short")) == 2);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 10u);
    sc::ApplySettings st;
    st.options = {{"scale", "4"}, {"mode", "HARD"}};
    r = Run(d, text, st);
    CHECK_OK(r);
    CHECK(F32(Own(rev, "load_for_short")) == 3);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 50u);
    st.options = {{"loud", "yes"}};
    CHECK(Run(d, text, st).ok);
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 100u);

    for (const auto& bad : std::vector<std::map<std::string, std::string>>{
             {{"scale", "11"}}, {{"scale", "abc"}}, {{"loud", "maybe"}}, {{"mode", "medium"}}, {{"nope", "1"}}}) {
        sc::ApplySettings b;
        b.options = bad;
        CHECK(!Run(d, text, b).ok);
    }
    // A script for another game warns.
    r = Run(d, "attribulina 1\ngame carbon_pc\n");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(1));
}

TEST("script: format 2: append, remove by value, resize by, indices from the end, conditions on the database") {
    auto db = LoadAll(false);
    Database& d = *db;
    auto base = sc::CloneDatabase(d);
    Collection& all = Col(d, "car_list", "all_cars");
    auto cars = [&]() -> Attribute& { return Own(all, "Cars"); };
    const uint32_t n = cars().count;
    CHECK(n > 3);
    auto carAt = [&](int64_t i) { return U32(cars(), uint32_t(i < 0 ? cars().count + i : i), 4); }; // RefSpec's collection key
    CHECK_EQ(carAt(0), StringHash32("240sx"));
    // append adds at the end; if missing leaves what the array holds, so a car
    // mod applied twice lists its car once.
    const std::string add = "attribulina 2\nedit car_list/all_cars {\n"
                            "    append Cars = pvehicle/street if missing\n    append Cars = pvehicle/240sx if missing\n}\n";
    CHECK_OK(Run(d, add));
    CHECK_EQ(cars().count, n + 1);
    CHECK_EQ(carAt(-1), StringHash32("street"));
    CHECK_OK(Run(d, add));
    CHECK_EQ(cars().count, n + 1);
    // remove by value, [-1], resize by, insert from the end.
    CHECK_OK(Run(d, "attribulina 2\nedit car_list/all_cars {\n    remove Cars = pvehicle/street\n    Cars[-1] = pvehicle/street\n"
                    "    resize Cars by 2\n    insert Cars[-1] = pvehicle/default\n}\n"));
    CHECK_EQ(cars().count, n + 3);
    CHECK_EQ(carAt(n - 1), StringHash32("street"));
    CHECK_EQ(carAt(-2), StringHash32("default"));
    CHECK_EQ(carAt(-1), 0u);
    const sc::ApplyReport gone = Run(d, "attribulina 2\nedit car_list/all_cars {\n    resize Cars by -2\n    remove Cars = pvehicle/nothing_like_it\n}\n");
    CHECK_OK(gone);
    CHECK_EQ(cars().count, n + 1);
    CHECK(Messages(gone).find("holds no pvehicle/nothing_like_it") != std::string::npos);
    CHECK(!Run(d, "attribulina 2\nedit car_list/all_cars { resize Cars by -1000 }\n").ok);
    CHECK(!Run(d, "attribulina 2\nedit car_list/all_cars { Cars[-1000] = pvehicle/street }\n").ok);

    // Conditions on the database, at the top and on the collection in a block.
    const std::string cond = "attribulina 2\n"
                             "option scale number default 2\n"
                             "if exists class car_list and exists car_list/all_cars and exists car_list/all_* and exists vault db and\n"
                             "        exists field car_list Cars and not exists class no_such_class and not exists field car_list Nope and\n"
                             "        not exists car_list/no_such_list and $scale > 1.5 and $scale <= 2 {\n"
                             "    edit car_list/all_cars {\n"
                             "        if count Cars >= 4 and Cars contains pvehicle/240sx and has Cars[0] and own Cars and not has Cars[9999] and\n"
                             "                Cars[0] == pvehicle/240sx and Cars[0] != pvehicle/350z and not (Cars contains pvehicle/default) {\n"
                             "            append Cars = pvehicle/default\n"
                             "        }\n"
                             "        if count Cars < 3 or not own Cars { echo error \"not this\" }\n"
                             "    }\n"
                             "    edit collisionsfx_damage/hood {\n"
                             "        if VolMap[0] >= 0 and count VolMap == 3 and not has VolMap[3] { append VolMap = [7, 8] }\n"
                             "    }\n"
                             "}\n";
    CHECK_OK(Run(d, cond));
    CHECK_EQ(cars().count, n + 2);
    CHECK_EQ(carAt(-1), StringHash32("default"));
    CHECK_EQ(Own(Col(d, "collisionsfx_damage", "hood"), "VolMap").count, 5u);
    CHECK_EQ(U32(Own(Col(d, "collisionsfx_damage", "hood"), "VolMap"), 4), 8u);
    // has, own and count of a collection that inherits the field.
    CHECK_OK(Run(d, "attribulina 2\ncreate car_list/ns_kid from car_list/all_cars under car_list/all_cars\n"
                    "edit car_list/ns_kid {\n    revert Cars\n    if has Cars and not own Cars and count Cars > 3 { Name = \"inherits\" }\n}\n"));
    const Attribute& name = Own(Col(d, "car_list", "ns_kid"), "Name");
    CHECK(name.data.ptrAt(0) && name.data.ptrAt(0)->text == "inherits");
    // A comparison that needs a number, and one on a field the class lacks, are errors.
    CHECK(!Run(d, "attribulina 2\nedit car_list/all_cars { if Cars[0] > 1 { Name = \"x\" } }\n").ok);
    CHECK(!Run(d, "attribulina 2\nedit car_list/all_cars { if NoSuchField == 1 { Name = \"x\" } }\n").ok);

    // A format 2 statement under "attribulina 1" applies, with a warning; the
    // printer writes the lowest format that reads a script.
    const sc::ApplyReport old = Run(d, "attribulina 1\nedit car_list/all_cars { append Cars = pvehicle/240sx if missing }\n");
    CHECK_OK(old);
    CHECK(Messages(old).find("make it \"attribulina 2\"") != std::string::npos);
    CHECK_EQ(sc::Print(sc::Parse("attribulina 1\nedit a/b {\n    X = 1\n}\n")), std::string("attribulina 1\nedit a/b {\n    X = 1\n}\n"));
    const std::string every = "attribulina 2\n"
                              "if exists class a and exists vault v and exists field a X and exists a/b and not exists a/c and $o >= 2 {\n"
                              "    edit a/b {\n"
                              "        if has X[-1] and own X and count X < 4 and X[0] == 3 and X[1] <= 2.5 and X contains a/b and Y.z > 1 {\n"
                              "            append X = [1, 2] if missing\n"
                              "            append X\n"
                              "            remove X = a/b\n"
                              "            resize X by -3\n"
                              "            insert X[-2] = 4\n"
                              "            X[-1].y = 2\n"
                              "        }\n"
                              "    }\n"
                              "}\n";
    CHECK_EQ(sc::Print(sc::Parse(every)), every);
    CHECK_EQ(sc::Print(sc::Parse("attribulina 1\nedit a/b {\n    append X = 1\n}\n")), std::string("attribulina 2\nedit a/b {\n    append X = 1\n}\n"));
    CHECK_EQ(sc::FormatNeeded(sc::Parse("attribulina 1\nif $o == 1 {\n    edit a/b {\n        X[2] = 1\n    }\n}\n").statements), 1);

    // A diff of an array that only grew or only lost elements says so, and
    // applied to the original it makes the same database.
    auto grown = sc::CloneDatabase(*base);
    CHECK_OK(Run(*grown, "attribulina 2\nedit car_list/all_cars {\n    remove Cars = pvehicle/350z\n}\n"));
    const sc::Script diff = sc::Diff(*base, *grown);
    const std::string text = sc::Print(diff);
    CHECK(text.rfind("attribulina 2\n", 0) == 0);
    if (text.find("    remove Cars = pvehicle/350z\n") == std::string::npos) std::fprintf(stderr, "%s", text.c_str());
    CHECK(text.find("    remove Cars = pvehicle/350z\n") != std::string::npos);
    auto again = sc::CloneDatabase(*base);
    CHECK_OK(sc::Apply(*again, diff));
    CHECK_EQ(Changes(sc::Diff(*again, *grown)), size_t(0));
    auto appended = sc::CloneDatabase(*base);
    CHECK_OK(Run(*appended, "attribulina 2\nedit car_list/all_cars { append Cars = [pvehicle/default, pvehicle/street] }\n"));
    const std::string appendText = sc::Print(sc::Diff(*base, *appended));
    if (appendText.find("    append Cars = [pvehicle/default, pvehicle/street] if missing\n") == std::string::npos) std::fprintf(stderr, "%s", appendText.c_str());
    CHECK(appendText.find("    append Cars = [pvehicle/default, pvehicle/street] if missing\n") != std::string::npos);
}

TEST("script: collections are made, renamed, moved and deleted") {
    auto db = LoadAll(false);
    Database& d = *db;
    Collection& street = Col(d, "pvehicle", "street");
    const size_t own = street.attributes.size();
    const std::string text = "attribulina 1\n"
                             "create pvehicle/script_car from pvehicle/street {\n"
                             "    HornType = 3\n"
                             "}\n"
                             "collection pvehicle/script_base under pvehicle/default in vault db\n"
                             "rename pvehicle/script_base to script_root\n"
                             "move pvehicle/script_car under pvehicle/script_root\n"
                             "collection pvehicle/script_child under pvehicle/script_car\n"
                             "move pvehicle/script_child to vault cars\n";
    sc::ApplyReport r = Run(d, text);
    CHECK_OK(r);
    CHECK_EQ(r.created, size_t(3));
    CHECK_EQ(r.renamed, size_t(1));
    Collection& car = Col(d, "pvehicle", "script_car");
    Collection& root = Col(d, "pvehicle", "script_root");
    Collection& child = Col(d, "pvehicle", "script_child");
    CHECK(car.parent == &root);
    CHECK(root.parent == &Col(d, "pvehicle", "default"));
    CHECK(child.parent == &car);
    CHECK_EQ(child.vault->name, std::string("cars"));
    CHECK_EQ(car.attributes.size(), own); // a copy of street's values
    CHECK_EQ(int(Own(car, "HornType").data.bytes[0]), 3);
    CHECK(car.exportId == StringHash32("pvehicle/script_car"));
    // A copy's CollectionName that held the source's name follows the new name.
    const Attribute& cn = Own(car, "CollectionName");
    CHECK(cn.data.ptrAt(0) && cn.data.ptrAt(0)->text == "script_car");
    CHECK(d.findCollection(StringHash32("pvehicle"), StringHash32("script_base")) == nullptr);

    // Again: nothing to do but the create, which refuses.
    r = Run(d, "attribulina 1\ncollection pvehicle/script_root under pvehicle/default in vault db\n"
               "rename pvehicle/script_base to script_root\n");
    CHECK_OK(r);
    CHECK_EQ(r.summary(), std::string("no changes"));
    CHECK(!Run(d, text).ok);

    // `exact` reverts what it doesn't list.
    r = Run(d, "attribulina 1\ncollection pvehicle/script_car exact {\n    HornType = 4\n}\n");
    CHECK_OK(r);
    for (const Attribute& a : car.attributes) CHECK(a.inLayout || a.key == StringHash32("HornType"));

    // Deleting a parent needs `with children`.
    CHECK(!Run(d, "attribulina 1\ndelete pvehicle/script_root\n").ok);
    std::vector<Collection*> told;
    sc::ApplySettings st;
    st.beforeRemove = [&](Collection& c) { told.push_back(&c); };
    r = Run(d, "attribulina 1\ndelete pvehicle/script_root with children\ndelete pvehicle/script_root\n", st);
    CHECK_OK(r);
    CHECK_EQ(r.deleted, size_t(3));
    CHECK_EQ(told.size(), size_t(3));
    CHECK(told.back() == &root); // children first
    CHECK(d.findCollection(StringHash32("pvehicle"), StringHash32("script_car")) == nullptr);

    // Arrays: resize, insert, remove; and an edit of every match.
    r = Run(d, "attribulina 1\nedit collisionsfx_damage/hood {\n    resize VolMap 4\n    insert VolMap[0] = 9\n    remove VolMap[4]\n}\n"
               "edit pvehicle/str* { HornType = 2 }\n");
    CHECK_OK(r);
    const Attribute& sv = Own(Col(d, "collisionsfx_damage", "hood"), "VolMap");
    CHECK_EQ(sv.count, 4u);
    CHECK_EQ(U32(sv, 0), 9u);
    CHECK_EQ(int(Own(street, "HornType").data.bytes[0]), 2);

    // The changed packs still load.
    for (const auto& pk : d.packs()) {
        if (!pk->dirty) continue;
        Database fresh;
        fresh.addTypeSchema(d.sharedTypeSchema());
        if (pk->path.find("ATTRIBUTES") == std::string::npos) fresh.loadPack(Vanilla("ATTRIBUTES.BIN"));
        fresh.loadPack(pk->path, WritePack(d, *pk));
    }
}

TEST("script: a pointer array and a struct member path") {
    auto db = LoadAll(false);
    Database& d = *db;
    // FEPartData: camera arrays behind pointers with u8 counts, and a pointer to one FEPartDetail.
    Collection* part = nullptr;
    for (Collection* c : d.findClass(StringHash32("part_fedata"))->collections)
        if (c->find(StringHash32("partData")) && c->find(StringHash32("partData"))->count) part = c;
    CHECK(part != nullptr);
    const std::string target = "part_fedata/" + d.names().format(part->key);
    const Attribute& pd = Own(*part, "partData");
    const ValueType vt = TypeOfAttribute(pd, "partData");
    CHECK(vt.st && vt.st->name == "FEPartData");
    std::string ptr, cnt;
    for (const MemberDef& m : vt.st->members)
        if (m.prim == MemberPrim::Ptr && !m.ptrCount.empty() && ptr.empty()) {
            ptr = m.name;
            cnt = m.ptrCount;
        }
    CHECK(!ptr.empty());
    const ValueType mini = TypeOfNamed(d, "MiniRefSpec");
    CHECK(mini.st && !mini.st->members.empty());
    const std::string text = "attribulina 1\nedit " + target + " {\n    partData[0]." + ptr + " = []\n    insert partData[0]." + ptr +
                             "[0]\n    insert partData[0]." + ptr + "[0]\n    partData[0]." + ptr + "[1]." + mini.st->members[0].name +
                             " = 0x7\n}\n";
    const sc::ApplyReport r = Run(d, text);
    CHECK_OK(r);
    const Attribute& after = Own(*part, "partData");
    const MemberDef* pm = vt.st->member(ptr);
    const MemberDef* cm = vt.st->member(cnt);
    CHECK_EQ(int(after.data.bytes[cm->offset]), 2);
    const PtrSlot* s = after.data.ptrAt(pm->offset);
    CHECK(s && s->data && s->data->bytes.size() == 2 * mini.size);
    CHECK_EQ(int64_t(ReadInteger(*s->data, mini.size + mini.st->members[0].offset, TypeOfMember(mini.st->members[0]), Endian::Little)),
             int64_t(7));
    // Setting the count directly is refused.
    CHECK(!Run(d, "attribulina 1\nedit " + target + " { partData[0]." + cnt + " = 1 }\n").ok);
}

TEST("script: every collection written as a script applies back unchanged") {
    auto db = LoadAll();
    Database& d = *db;
    // A clone writes the same packs.
    auto clone = sc::CloneDatabase(d);
    for (size_t i = 0; i < d.packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(d, *d.packs()[i]));

    std::vector<const Collection*> all;
    for (const auto& cls : d.classes())
        for (const Collection* c : cls->collections) all.push_back(c);
    const std::string text = sc::Print(sc::NewScript(d, sc::Export(d, all)));
    const sc::Script parsed = sc::Parse(text, "everything.amod");
    const sc::ApplyReport r = sc::Apply(*clone, parsed);
    CHECK_OK(r);
    if (r.summary() != "no changes" || r.warnings()) {
        const std::string what = sc::Print(sc::Diff(d, *clone));
        std::fprintf(stderr, "%s\n%s\n%s", r.summary().c_str(), Messages(r).substr(0, 4000).c_str(), what.substr(0, 6000).c_str());
    }
    CHECK_EQ(r.summary(), std::string("no changes"));
    CHECK_EQ(r.warnings(), size_t(0));
    // Nothing moved either: the packs write back as they were read.
    for (size_t i = 0; i < d.packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(d, *d.packs()[i]));
    // Handlers come out as Lua source.
    CHECK(text.find("bytecode = lua \"\"\"") != std::string::npos);
}

TEST("script: a diff applied to the original gives the changed database") {
    auto base = LoadAll();
    auto cur = sc::CloneDatabase(*base);
    const sc::ApplyReport made = Run(*cur, "attribulina 1\n"
                                           "create pvehicle/diff_car from pvehicle/street { HornType = 3 }\n"
                                           "create pvehicle/diff_kid under pvehicle/diff_car\n"
                                           "edit audio_revlimiter/default { load_for_short = 0.125\n total_frames_short = 77 }\n"
                                           "edit collisionsfx_damage/hood { VolMap = [5, 6, 7, 8, 9] }\n"
                                           "edit vehicle/default { THROTTLE_CONTROL_CURVE.Y[1] = 0.5 }\n"
                                           "edit pvehicle/default { BEHAVIOR_MECHANIC_RIGIDBODY = \"changed\" }\n"
                                           "rename car/gtrproto to gtrproto2\n"
                                           "move pvehicle/street under root\n"
                                           "create vault diff_vault in gameplay.lzc\n"
                                           "move pvehicle/diff_kid to vault cars\n"
                                           "delete emittergroup/terraindriving with children\n");
    CHECK_OK(made);
    const sc::Script diff = sc::Diff(*base, *cur);
    const std::string text = sc::Print(diff);
    CHECK(text.find("rename car/gtrproto to gtrproto2\n") != std::string::npos);
    if (text.find("collection pvehicle/diff_car under ") == std::string::npos) std::fprintf(stderr, "%s", text.c_str());
    CHECK(text.find("collection pvehicle/diff_car under ") != std::string::npos);
    CHECK(text.find("collection pvehicle/diff_kid under pvehicle/diff_car in vault cars") != std::string::npos);
    CHECK(text.find("    total_frames_short = 77 was ") != std::string::npos);
    CHECK(text.find("delete emittergroup/terraindriving\n") != std::string::npos);
    CHECK(text.find("create vault diff_vault in gameplay.lzc\n") != std::string::npos);

    auto target = sc::CloneDatabase(*base);
    const sc::ApplyReport r = sc::Apply(*target, sc::Parse(text, "diff.amod"));
    CHECK_OK(r);
    if (r.warnings()) std::fprintf(stderr, "%s", Messages(r).c_str());
    CHECK_EQ(r.warnings(), size_t(0));
    const sc::Script again = sc::Diff(*target, *cur);
    if (Changes(again)) std::fprintf(stderr, "%s", sc::Print(again).c_str());
    CHECK_EQ(Changes(again), size_t(0));
    // And once more: it applies again without changing anything.
    const sc::ApplyReport twice = sc::Apply(*target, sc::Parse(text, "diff.amod"));
    CHECK_OK(twice);
}

TEST("script: the 360's and the Wii's databases read as PC does, and their edits write and read back") {
    // Every pack of a platform's folder, with both schemas, as the editor opens it.
    auto load = [](const char* platform) {
        auto db = std::make_unique<Database>();
        db->addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
        db->addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_ps2.txt")));
        db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
        for (const FolderFile& f : FolderPacks(test::DataPath(std::string("attrib/databases/") + platform + "/prostreet-vanilla")))
            if (f.skipped.empty()) db->loadPack(f.path);
        return db;
    };
    auto pc = load("pc");
    // These hold the same data on all three.
    const std::vector<std::pair<const char*, const char*>> same = {
        {"pvehicle", "default"}, {"pvehicle", "street"}, {"vehicle", "default"}, {"audio_revlimiter", "default"},
        {"car_part", "t2_low_tire_package"}, {"part_fedata", "t3_medium_engine_package"}, {"car", "gti"}};
    auto exported = [&](Database& db, const std::vector<std::pair<const char*, const char*>>& names) {
        std::vector<const Collection*> cs;
        for (auto [cls, name] : names) cs.push_back(&Col(db, cls, name));
        std::string text = sc::Print(sc::NewScript(db, sc::Export(db, cs)));
        const size_t game = text.find("game " + sc::GameId(db) + "\n");
        CHECK(game != std::string::npos);
        return text.erase(game, sc::GameId(db).size() + 6); // the rest is the same on each
    };
    // One script for every platform edits each the same way: values of most
    // kinds, an FEPartData member, a bool.
    const std::string edits = "attribulina 1\n"
                              "game prostreet_all\n"
                              "create pvehicle/be_car from pvehicle/street { HornType = 3 }\n"
                              "edit audio_revlimiter/default { load_for_short = 0.125\n total_frames_short = 77 }\n"
                              "edit vehicle/default { THROTTLE_CONTROL_CURVE.Y[1] = 0.5 }\n"
                              "edit pvehicle/default { BEHAVIOR_MECHANIC_RIGIDBODY = \"changed\" }\n"
                              "edit car_part/t2_low_tire_package { name = \"a longer name than before\"\n kit_max = 13\n physics[12] = part_physics/tires_stock }\n"
                              "edit part_fedata/t3_medium_engine_package { partData[0].grip = false }\n"
                              "edit car/gti { isStandardDef = false }\n";
    auto pcEdited = sc::CloneDatabase(*pc);
    const sc::ApplyReport onPc = Run(*pcEdited, edits);
    CHECK_OK(onPc);
    CHECK_EQ(onPc.warnings(), size_t(0));
    auto changed = same;
    changed.push_back({"pvehicle", "be_car"});

    for (const char* platform : {"360", "wii"}) {
        auto x = load(platform);
        for (const auto& p : x->packs())
            for (const auto& v : p->vaults) {
                CHECK(v->raw.endian == Endian::Big);
                CHECK(v->warnings.empty());
            }
        // The PC's schema (same layouts), each its own platform.
        CHECK_EQ(sc::GameId(*x), std::string(platform) == "wii" ? std::string("prostreet_wii") : std::string("prostreet_x360"));
        CHECK(x->typeSchema() && x->typeSchema()->source.find("prostreet_pc") != std::string::npos);
        // Big-endian values read the same.
        CHECK_EQ(exported(*x, same), exported(*pc, same));

        // Every collection as a script applies back with no change.
        auto clone = sc::CloneDatabase(*x);
        std::vector<const Collection*> all;
        for (const auto& cls : x->classes())
            for (const Collection* c : cls->collections) all.push_back(c);
        const sc::ApplyReport back = sc::Apply(*clone, sc::Parse(sc::Print(sc::NewScript(*x, sc::Export(*x, all))), "everything.amod"));
        CHECK_OK(back);
        CHECK_EQ(back.summary(), std::string("no changes"));
        for (size_t i = 0; i < x->packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(*x, *x->packs()[i]));

        // The edits come out as on PC.
        const sc::ApplyReport r = Run(*clone, edits);
        CHECK_OK(r);
        CHECK_EQ(r.warnings(), size_t(0));
        CHECK_EQ(exported(*clone, changed), exported(*pcEdited, changed));
        // Written out and read again, nothing differs.
        Database again;
        again.setTarget(clone->target());
        again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
        again.names().loadFile(test::DataPath("data/attrib_keys.txt"));
        std::vector<Bytes> written;
        for (const auto& p : clone->packs()) written.push_back(WritePack(*clone, *p));
        for (size_t i = 0; i < written.size(); ++i) again.loadPack(clone->packs()[i]->path, written[i]);
        const sc::Script diff = sc::Diff(*clone, again);
        if (Changes(diff)) std::fprintf(stderr, "%s", sc::Print(diff).substr(0, 4000).c_str());
        CHECK_EQ(Changes(diff), size_t(0));
        CHECK_EQ(exported(again, changed), exported(*pcEdited, changed));
    }
}

TEST("script: a PS2 database reads as PC does; its 4-byte bools edit and write back") {
    auto load = [](const char* platform) {
        auto db = std::make_unique<Database>();
        db->addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
        db->addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_ps2.txt")));
        db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
        for (const FolderFile& f : FolderPacks(test::DataPath(std::string("attrib/databases/") + platform + "/prostreet-vanilla")))
            if (f.skipped.empty()) db->loadPack(f.path);
        return db;
    };
    auto pc = load("pc");
    auto p2 = load("ps2");
    CHECK_EQ(sc::GameId(*p2), std::string("prostreet_ps2"));
    // The same data on both, FEPartData and bools included.
    const std::vector<std::pair<const char*, const char*>> same = {
        {"part_fedata", "t3_medium_engine_package"}, {"car_part", "t2_low_tire_package"}, {"car", "gti"},
        {"pvehicle", "default"}, {"vehicle", "default"}};
    auto exported = [&](Database& db) {
        std::vector<const Collection*> cs;
        for (auto [cls, name] : same) cs.push_back(&Col(db, cls, name));
        sc::Script s = sc::NewScript(db, sc::Export(db, cs));
        s.statements.erase(std::remove_if(s.statements.begin(), s.statements.end(),
                                          [](const sc::Statement& st) { return st.kind == sc::Statement::Kind::Meta; }),
                           s.statements.end()); // the game line differs
        return sc::Print(s);
    };
    CHECK_EQ(exported(*p2), exported(*pc));

    // Every collection as a script applies back with no change.
    auto clone = sc::CloneDatabase(*p2);
    std::vector<const Collection*> all;
    for (const auto& cls : p2->classes())
        for (const Collection* c : cls->collections) all.push_back(c);
    const sc::ApplyReport back = sc::Apply(*clone, sc::Parse(sc::Print(sc::NewScript(*p2, sc::Export(*p2, all))), "everything.amod"));
    CHECK_OK(back);
    CHECK_EQ(back.summary(), std::string("no changes"));
    for (size_t i = 0; i < p2->packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(*p2, *p2->packs()[i]));

    // A PC script edits a 4-byte bool and an FEPartData member; it warns that
    // it was made for the PC. Both values read back after a write.
    const Attribute& part = Own(Col(*clone, "part_fedata", "t3_medium_engine_package"), "partData");
    CHECK_EQ(U32(part, 0, 24), 1u); // grip, after HAL ids, price, tier and two 4-byte bools
    const sc::ApplyReport r = Run(*clone, "attribulina 1\ngame prostreet_pc\n"
                                          "edit car/gti { isStandardDef = false }\n"
                                          "edit part_fedata/t3_medium_engine_package { partData[0].grip = false }\n");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(1));
    CHECK_EQ(Messages(r), std::string("2: the script is for Pro Street on PC, and this database is Pro Street on PS2\n"));
    const Attribute& isStd = Own(Col(*clone, "car", "gti"), "isStandardDef");
    CHECK_EQ(isStd.data.bytes.size(), size_t(4));
    CHECK_EQ(U32(isStd), 0u);
    CHECK_EQ(U32(part, 0, 24), 0u);
    CHECK_EQ(U32(part, 0, 20), 1u); // drag is as it was
    Database again;
    again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_ps2.txt")));
    again.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    std::vector<Bytes> written;
    for (const auto& p : clone->packs()) written.push_back(WritePack(*clone, *p));
    for (size_t i = 0; i < written.size(); ++i) {
        LoadedPack& p = again.loadPack(clone->packs()[i]->path, written[i]);
        for (const auto& v : p.vaults) CHECK(v->warnings.empty());
    }
    const sc::Script diff = sc::Diff(*clone, again);
    if (Changes(diff)) std::fprintf(stderr, "%s", sc::Print(diff).substr(0, 4000).c_str());
    CHECK_EQ(Changes(diff), size_t(0));
}

TEST("script: Carbon's and the Wii's Undercover databases write as scripts and apply back unchanged; one carbon_all script "
     "edits each Carbon the same") {
    auto load = [](const std::string& folder) {
        auto db = std::make_unique<Database>();
        for (const char* s :
             {"prostreet_pc", "prostreet_ps2", "carbon_pc", "carbon_ps2", "carbon_x360", "carbon_x360_demo", "carbon_ps2_demo", "undercover_wii",
              "undercover_ps2"})
            db->addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
        for (const FolderFile& f : FolderPacks(test::DataPath("attrib/databases/" + folder)))
            if (f.skipped.empty()) db->loadPack(f.path);
        return db;
    };
    // The same data on every platform.
    const std::vector<std::pair<const char*, const char*>> same = {
        {"pvehicle", "default"}, {"pvehicle", "240sx"}, {"emittergroup", "fxgame_icongrp"}, {"tires", "default"}, {"engine", "default"}};
    auto exported = [&](Database& db, const std::vector<std::pair<const char*, const char*>>& names) {
        std::vector<const Collection*> cs;
        for (auto [cls, name] : names) cs.push_back(&Col(db, cls, name));
        std::string text = sc::Print(sc::NewScript(db, sc::Export(db, cs)));
        return text.erase(0, text.find('\n', text.find("\ngame ") + 1)); // the rest is the same on each
    };
    // Values of several kinds; a bool attribute is 4 bytes on the PS2 and the GameCube.
    const std::string edits = "attribulina 1\n"
                              "game carbon_all\n"
                              "create pvehicle/be_car from pvehicle/240sx { MASS = 1500 }\n"
                              "edit pvehicle/default { MASS = 1234.5\n CrewCommentable = true\n RacingTier = kRaceTier_2\n TENSOR_SCALE = (1, 2, 3, 0) }\n"
                              "edit emittergroup/fxgame_icongrp { Emitters[0] = emitterdata/fxgame_icon_geo2blue\n FarClip = 250 }\n";
    auto changed = same;
    changed.push_back({"pvehicle", "be_car"});
    std::string pcBefore, pcAfter;
    // Handlers left as bytes: JDLZ blocks whose last byte holds whatever the
    // encoder read past its input. The 360's late demo has Most Wanted's
    // vault layout and older data, so the edits skip it.
    const std::vector<std::pair<std::string, size_t>> folders = {
        {"pc/carbon-vanilla", 2},       {"ps3/carbon-vanilla", 2},      {"wii/carbon-vanilla", 2},
        {"ps2/carbon-vanilla", 2},      {"gamecube/carbon-vanilla", 2}, {"360/carbon-vanilla", 2},
        {"wii/undercover-vanilla", 4},  {"360/carbon-latedemo", 2}};
    for (const auto& [folder, handlersAsBytes] : folders) {
        if (!std::filesystem::exists(test::DataPath("attrib/databases/" + folder))) continue;
        auto db = load(folder);
        const bool carbon = db->target().game == Game::Carbon && folder.find("demo") == std::string::npos;
        for (const auto& p : db->packs())
            for (const auto& v : p->vaults) CHECK(v->warnings.empty());
        // Every collection as a script applies back with no change. The
        // handlers come out as Lua, even those whose mSize is short of their
        // data.
        auto clone = sc::CloneDatabase(*db);
        std::vector<const Collection*> all;
        for (const auto& cls : db->classes())
            for (const Collection* c : cls->collections) all.push_back(c);
        const std::string text = sc::Print(sc::NewScript(*db, sc::Export(*db, all)));
        CHECK(text.find("\ngame " + db->target().id() + "\n") != std::string::npos);
        size_t asBytes = 0;
        for (size_t at = text.find("bytecode = bytes"); at != std::string::npos; at = text.find("bytecode = bytes", at + 1)) ++asBytes;
        CHECK_EQ(asBytes, handlersAsBytes);
        const sc::ApplyReport back = sc::Apply(*clone, sc::Parse(text, "everything.amod"));
        CHECK_OK(back);
        if (back.summary() != "no changes")
            std::fprintf(stderr, "%s: %s\n", folder.c_str(), sc::Print(sc::Diff(*db, *clone)).substr(0, 3000).c_str());
        CHECK_EQ(back.summary(), std::string("no changes"));
        CHECK_EQ(back.warnings(), size_t(0));
        for (size_t i = 0; i < db->packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(*db, *db->packs()[i]));
        if (folder == "360/carbon-latedemo") {
            // Most Wanted's 16-byte StringKey { u64 hash64; u32 hash32; char* }:
            // a new text sets both hashes.
            CHECK_OK(Run(*clone, "attribulina 1\nedit controller/fly { GAMEACTION_STEERLEFT[0].mDeviceID = \"ALX_R\" }\n"));
            const Attribute& a = Own(Col(*clone, "controller", "fly"), "GAMEACTION_STEERLEFT");
            CHECK(load64(a.data.bytes.data(), Endian::Big) == StringHash64("ALX_R"));
            CHECK_EQ(load32(a.data.bytes.data() + 8, Endian::Big), StringHash32("ALX_R"));
            CHECK(a.data.ptrAt(12) && a.data.ptrAt(12)->text == "ALX_R");
            std::vector<const Collection*> fly = {&Col(*clone, "controller", "fly")};
            CHECK(sc::Print(sc::NewScript(*clone, sc::Export(*clone, fly))).find("mDeviceID = \"ALX_R\"") != std::string::npos);
        }
        if (folder == "360/carbon-vanilla" || folder == "ps2/carbon-vanilla" || folder == "gamecube/carbon-vanilla") {
            // The 20-byte SteeringSensitivityParameter of the retail 360, the PS2
            // and the GameCube: the PC's members without the dead zones,
            // NumberOfSteps a u16 at +16.
            std::vector<const Collection*> drive = {&Col(*clone, "controller", "drive")};
            const std::string t = sc::Print(sc::NewScript(*clone, sc::Export(*clone, drive)));
            CHECK(t.find("NumberOfSteps = 2") != std::string::npos);
            CHECK(t.find("CurveMultiplierHighSpeed = 1") != std::string::npos);
            CHECK(t.find("InnerDeadZone") == std::string::npos);
            CHECK_OK(Run(*clone, "attribulina 1\nedit controller/drive { LowSensitivitySteeringParameters.NumberOfSteps = 7 }\n"));
            const Attribute& a = Own(Col(*clone, "controller", "drive"), "LowSensitivitySteeringParameters");
            CHECK_EQ(a.elemSize, 20u);
            const Endian e = Col(*clone, "controller", "drive").vault->raw.endian;
            CHECK(e == (folder == "ps2/carbon-vanilla" ? Endian::Little : Endian::Big));
            CHECK_EQ(load32(a.data.bytes.data() + 16, e), e == Endian::Big ? 0x00070000u : 7u);
        }
        if (!carbon) continue;

        // The edits, without a warning, read back as on PC.
        if (pcBefore.empty()) pcBefore = exported(*db, same);
        CHECK_EQ(exported(*db, same), pcBefore);
        const sc::ApplyReport r = Run(*clone, edits);
        CHECK_OK(r);
        CHECK_EQ(r.warnings(), size_t(0));
        if (pcAfter.empty()) pcAfter = exported(*clone, changed);
        CHECK_EQ(exported(*clone, changed), pcAfter);
        // Written out and read again, nothing differs.
        Database again;
        again.setTarget(clone->target());
        for (const char* s : {"carbon_pc", "carbon_ps2", "carbon_x360"})
            again.addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        again.names().loadFile(test::DataPath("data/attrib_keys.txt"));
        for (const auto& p : clone->packs()) again.loadPack(p->path, WritePack(*clone, *p));
        CHECK_EQ(Changes(sc::Diff(*clone, again)), size_t(0));
    }
    CHECK(pcAfter.find("MASS = 1234.5") != std::string::npos && pcAfter.find("CrewCommentable = true") != std::string::npos);
}

TEST("script: current-gen Undercover's databases (360, PC) write as scripts and apply back unchanged; one undercover_all "
     "script edits class references and 48-byte curves the same on both") {
    auto load = [](const std::string& folder) {
        auto db = std::make_unique<Database>();
        for (const char* s : {"prostreet_pc", "carbon_pc", "undercover_wii", "undercover_ps2", "undercover_pc"})
            db->addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
        for (const FolderFile& f : FolderPacks(test::DataPath("attrib/databases/" + folder)))
            if (f.skipped.empty()) db->loadPack(f.path);
        return db;
    };
    const std::string edits = "attribulina 1\n"
                              "game undercover_all\n"
                              "edit aud_world_reverb/world_reverb_05 { default_wall_verb = canyonwall }\n"
                              "edit nis_car_sets/trf_mixed { car_item[1] = trf_van }\n"
                              "edit aiperformance/default { TopSpeedScale = { xMax = 2\n graphScale = 0.5\n X = [0, 1]\n Y = [0.5, 1] } }\n";
    const std::vector<std::pair<const char*, const char*>> edited = {
        {"aud_world_reverb", "world_reverb_05"}, {"nis_car_sets", "trf_mixed"}, {"aiperformance", "default"}};
    std::string firstAfter;
    for (const char* folder : {"360/undercover-vanilla", "pc/undercover-vanilla"}) {
        if (!std::filesystem::exists(test::DataPath(std::string("attrib/databases/") + folder))) continue;
        auto db = load(folder);
        for (const auto& p : db->packs())
            for (const auto& v : p->vaults) CHECK(v->warnings.empty());
        auto clone = sc::CloneDatabase(*db);
        std::vector<const Collection*> all;
        for (const auto& cls : db->classes())
            for (const Collection* c : cls->collections) all.push_back(c);
        const std::string text = sc::Print(sc::NewScript(*db, sc::Export(*db, all)));
        // All 214 handlers come out as Lua 5.1 source, which compiles back to their bytes.
        size_t asBytes = 0, asLua = 0;
        for (size_t at = text.find("bytecode = bytes"); at != std::string::npos; at = text.find("bytecode = bytes", at + 1)) ++asBytes;
        for (size_t at = text.find("bytecode = lua"); at != std::string::npos; at = text.find("bytecode = lua", at + 1)) ++asLua;
        CHECK_EQ(asBytes, size_t(0));
        CHECK_EQ(asLua, size_t(214));
        CHECK(text.find("graphScale = ") != std::string::npos);
        const sc::ApplyReport back = sc::Apply(*clone, sc::Parse(text, "everything.amod"));
        CHECK_OK(back);
        if (back.summary() != "no changes")
            std::fprintf(stderr, "%s: %s\n", folder, sc::Print(sc::Diff(*db, *clone)).substr(0, 3000).c_str());
        CHECK_EQ(back.summary(), std::string("no changes"));
        CHECK_EQ(back.warnings(), size_t(0));
        for (size_t i = 0; i < db->packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(*db, *db->packs()[i]));

        // The edits read back the same on both platforms.
        const sc::ApplyReport r = Run(*clone, edits);
        CHECK_OK(r);
        CHECK_EQ(r.warnings(), size_t(0));
        const Endian e = Col(*clone, "aud_world_reverb", "world_reverb_05").vault->raw.endian;
        const Attribute& wall = Own(Col(*clone, "aud_world_reverb", "world_reverb_05"), "default_wall_verb");
        CHECK_EQ(load32(wall.data.bytes.data(), e), StringHash32("canyonwall"));
        CHECK_EQ(load32(wall.data.bytes.data() + 4, e), 0u); // the pointer the game sets
        const CurveValue c = ReadCurve(Own(Col(*clone, "aiperformance", "default"), "TopSpeedScale"), 0, e);
        CHECK(c.graphScale == 0.5f && c.xMax == 2.0f && c.x.size() == 2 && c.y2 == NaturalY2(c.x, c.y));
        std::vector<const Collection*> cs;
        for (auto [cls, name] : edited) cs.push_back(&Col(*clone, cls, name));
        std::string after = sc::Print(sc::NewScript(*clone, sc::Export(*clone, cs)));
        after.erase(0, after.find('\n', after.find("\ngame ") + 1));
        CHECK(after.find("default_wall_verb = canyonwall") != std::string::npos);
        CHECK(after.find("trf_sml, trf_van, trf_sed") != std::string::npos);
        if (firstAfter.empty()) firstAfter = after;
        CHECK_EQ(after, firstAfter);
        // Written out and read again, nothing differs.
        Database again;
        again.setTarget(clone->target());
        again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/undercover_pc.txt")));
        again.names().loadFile(test::DataPath("data/attrib_keys.txt"));
        for (const auto& p : clone->packs()) again.loadPack(p->path, WritePack(*clone, *p));
        CHECK_EQ(Changes(sc::Diff(*clone, again)), size_t(0));
    }
}

TEST("script: Undercover's 360 alpha writes as a script and applies back unchanged; the types it sizes otherwise edit by their "
     "members") {
    const std::string dir = test::DataPath("attrib/databases/360/undercover-alpha161");
    if (!std::filesystem::exists(dir)) return;
    auto load = [&] {
        auto db = std::make_unique<Database>();
        for (const char* s : {"undercover_wii", "undercover_ps2", "undercover_pc", "undercover_x360_alpha"})
            db->addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
        db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
        for (const FolderFile& f : FolderPacks(dir))
            if (f.skipped.empty()) db->loadPack(f.path);
        return db;
    };
    auto db = load();
    CHECK(db->typeSchema() && db->typeSchema()->source.find("undercover_x360_alpha.txt") != std::string::npos);
    auto clone = sc::CloneDatabase(*db);
    std::vector<const Collection*> all;
    for (const auto& cls : db->classes())
        for (const Collection* c : cls->collections) all.push_back(c);
    const std::string text = sc::Print(sc::NewScript(*db, sc::Export(*db, all)));
    CHECK(text.find("\ngame undercover_x360\n") != std::string::npos);
    // Its 207 handlers come out as Lua 5.1 source.
    size_t asBytes = 0, asLua = 0;
    for (size_t at = text.find("bytecode = bytes"); at != std::string::npos; at = text.find("bytecode = bytes", at + 1)) ++asBytes;
    for (size_t at = text.find("bytecode = lua"); at != std::string::npos; at = text.find("bytecode = lua", at + 1)) ++asLua;
    CHECK_EQ(asBytes, size_t(0));
    CHECK_EQ(asLua, size_t(207));
    // The alpha's own layouts, by their members.
    CHECK(text.find("mZFunc = ") != std::string::npos);
    CHECK(text.find("mBrilinearSlope") == std::string::npos);
    CHECK(text.find("LoopXFadeRange = [10, 150]") != std::string::npos);
    CHECK(text.find("StunTime = ") != std::string::npos);
    const sc::ApplyReport back = sc::Apply(*clone, sc::Parse(text, "everything.amod"));
    CHECK_OK(back);
    if (back.summary() != "no changes") std::fprintf(stderr, "%s\n", sc::Print(sc::Diff(*db, *clone)).substr(0, 3000).c_str());
    CHECK_EQ(back.summary(), std::string("no changes"));
    CHECK_EQ(back.warnings(), size_t(0));
    for (size_t i = 0; i < db->packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(*db, *db->packs()[i]));

    const sc::ApplyReport r = Run(*clone, "attribulina 1\n"
                                          "game undercover_x360\n"
                                          "edit pursuitaudio/default { SirenTune.LoopXFadeRange[1] = 160 }\n"
                                          "edit highway_pattern/light7 { CarsInPattern[1].mChange = LANECHG_RIGHT }\n"
                                          "edit presetride/terlingua { Paints[0].mVariance = 0.5 }\n"
                                          "edit physics_upgrade_set/nos { PhysicsUpgrades[0].mMemberIndex = 3 }\n");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(0));
    const Endian e = Endian::Big;
    auto f32 = [&](const uint8_t* p) {
        const uint32_t w = load32(p, e);
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    };
    CHECK(f32(Own(Col(*clone, "pursuitaudio", "default"), "SirenTune").data.bytes.data() + 48) == 160.0f);
    const Attribute& cars = Own(Col(*clone, "highway_pattern", "light7"), "CarsInPattern");
    CHECK_EQ(cars.elemSize, 24u);
    CHECK_EQ(load32(cars.elem(1) + 20, e), 2u);
    const Attribute& paints = Own(Col(*clone, "presetride", "terlingua"), "Paints");
    CHECK_EQ(paints.elemSize, 28u);
    CHECK(f32(paints.elem(0) + 24) == 0.5f);
    CHECK_EQ(load32(Own(Col(*clone, "physics_upgrade_set", "nos"), "PhysicsUpgrades").elem(0) + 12, e), 3u);
    // Written out and read again, nothing differs.
    Database again;
    again.setTarget(clone->target());
    again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/undercover_pc.txt")));
    again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/undercover_x360_alpha.txt")));
    again.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    for (const auto& p : clone->packs()) again.loadPack(p->path, WritePack(*clone, *p));
    CHECK_EQ(Changes(sc::Diff(*clone, again)), size_t(0));
}

TEST("script: World's databases (the final build and its betas, event vaults too) write as scripts and apply back unchanged; "
     "its own types edit") {
    auto load = [](Database& db, const std::string& dir) {
        db.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/world_pc.txt")));
        db.names().loadFile(test::DataPath("data/attrib_keys.txt"));
        for (const FolderFile& f : FolderPacks(dir))
            if (f.skipped.empty()) db.loadPack(f.path);
    };
    for (const char* beta : {"pc/world-2010-beta", "pc/world-2009-beta"}) {
        const std::string dir = test::DataPath(std::string("attrib/databases/") + beta);
        if (!std::filesystem::exists(dir)) continue;
        Database db;
        load(db, dir);
        auto clone = sc::CloneDatabase(db);
        std::vector<const Collection*> all;
        for (const auto& cls : db.classes())
            for (const Collection* c : cls->collections) all.push_back(c);
        const sc::ApplyReport back = sc::Apply(*clone, sc::Parse(sc::Print(sc::NewScript(db, sc::Export(db, all))), "everything.amod"));
        CHECK_OK(back);
        if (back.summary() != "no changes") std::fprintf(stderr, "%s: %s\n", beta, sc::Print(sc::Diff(db, *clone)).substr(0, 3000).c_str());
        CHECK_EQ(back.summary(), std::string("no changes"));
        CHECK_EQ(back.warnings(), size_t(0));
        for (size_t i = 0; i < db.packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(db, *db.packs()[i]));
    }

    const std::string dir = test::DataPath("attrib/databases/pc/world-final");
    if (!std::filesystem::exists(dir)) return;
    Database db;
    load(db, dir);
    auto clone = sc::CloneDatabase(db);
    std::vector<const Collection*> all;
    for (const auto& cls : db.classes())
        for (const Collection* c : cls->collections) all.push_back(c);
    const std::string text = sc::Print(sc::NewScript(db, sc::Export(db, all)));
    CHECK(text.find("mChannel = kBlackBoard_AI") != std::string::npos);
    CHECK(text.find("KeyString = \"copmidsize\"") != std::string::npos);
    const sc::ApplyReport back = sc::Apply(*clone, sc::Parse(text, "everything.amod"));
    CHECK_OK(back);
    if (back.summary() != "no changes") std::fprintf(stderr, "%s\n", sc::Print(sc::Diff(db, *clone)).substr(0, 3000).c_str());
    CHECK_EQ(back.summary(), std::string("no changes"));
    CHECK_EQ(back.warnings(), size_t(0));
    for (size_t i = 0; i < db.packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(db, *db.packs()[i]));

    // World's own types: an EASharp enum and struct, NFS-VltEd's names, a
    // string and its key, commerce.bin's localized strings.
    const sc::ApplyReport r = Run(*clone, "attribulina 1\n"
                                          "game world_pc\n"
                                          "edit gameplay/basicrace { BlackBoardMap[0].mChannel = kBlackBoard_WorldMap\n"
                                          "  BlackBoardMap[0].mFlags.mBits = 1 }\n"
                                          "edit partcollections/0032 { hat_bonus[0].RequiredPartCount = 3 }\n"
                                          "edit pursuitlevels/heat_03 { Roadblock_CopTypes[1] = { KeyString = \"copsuv\"\n"
                                          "  KeyHash = copsuv\n Value = 2.5 } }\n");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(0));
    const Attribute& map = Own(Col(*clone, "gameplay", "basicrace"), "BlackBoardMap");
    CHECK_EQ(load32(map.elem(0), Endian::Little), 3u); // kBlackBoard_WorldMap
    CHECK_EQ(load32(map.elem(0) + 8, Endian::Little), 1u);
    CHECK_EQ(load32(Own(Col(*clone, "partcollections", "0032"), "hat_bonus").elem(0) + 12, Endian::Little), 3u);
    const Attribute& cops = Own(Col(*clone, "pursuitlevels", "heat_03"), "Roadblock_CopTypes");
    CHECK_EQ(load32(cops.elem(1) + 4, Endian::Little), StringHash32("copsuv"));
    const PtrSlot* text1 = cops.data.ptrAt(cops.elemSize + 0);
    CHECK(text1 && text1->isString && text1->text == "copsuv");
    // Written out and read again, nothing differs.
    Database again;
    again.setTarget(clone->target());
    again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/world_pc.txt")));
    again.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    for (const auto& p : clone->packs()) again.loadPack(p->path, WritePack(*clone, *p));
    CHECK_EQ(Changes(sc::Diff(*clone, again)), size_t(0));
}

TEST("script: Most Wanted's databases write as scripts and apply back unchanged; one mw_all script edits each the same") {
    // Handlers left as bytes: blobs the game's compressor packed in a way
    // CompressBest doesn't repeat (the PS2 beta has 236 handlers, not 258).
    const std::vector<std::pair<std::string, size_t>> folders = {
        {"pc/mw-vanilla", 5}, {"xbox/mw-vanilla", 5}, {"gamecube/mw-vanilla", 5}, {"360/mw-beta", 5}, {"ps2/mw-beta", 6}};
    // Values of several kinds; a bool is 4 bytes on the GameCube and the PS2.
    const std::string edits = "attribulina 1\n"
                              "game mw_all\n"
                              "create pvehicle/mw_car from pvehicle/default { MASS = 1500 }\n"
                              "edit pvehicle/default { MASS = 1234.5\n HornType = 3\n TruckSndFX = true\n TENSOR_SCALE = (1, 2, 3, 0)\n"
                              " OnScrapeGround[0].mSurface = simsurface/organic\n OnScrapeGround[0].mMinSpeed = 7 }\n";
    auto values = [](Database& db) {
        std::string out;
        auto add = [&](const char* cls, const char* name, const char* field) {
            std::string t = sc::Print(sc::NewScript(db, {sc::ExportValue(db, Col(db, cls, name), StringHash32(field))}));
            out += t.erase(0, t.find('\n', t.find("\ngame ") + 1));
        };
        for (const char* f : {"MASS", "HornType", "TruckSndFX", "TENSOR_SCALE", "OnScrapeGround"}) add("pvehicle", "default", f);
        add("pvehicle", "mw_car", "MASS");
        return out;
    };
    std::string pcAfter;
    for (const auto& [folder, handlersAsBytes] : folders) {
        auto load = [&]() {
            auto db = std::make_unique<Database>();
            for (const char* s : {"mw_pc", "mw_gamecube"}) db->addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
            db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
            return db;
        };
        auto db = load();
        for (const FolderFile& f : FolderPacks(test::DataPath("attrib/databases/" + folder)))
            if (f.skipped.empty()) db->loadPack(f.path);
        for (const auto& p : db->packs())
            for (const auto& v : p->vaults) CHECK(v->warnings.empty());
        // Every collection as a script applies back with no change.
        auto clone = sc::CloneDatabase(*db);
        std::vector<const Collection*> all;
        for (const auto& cls : db->classes())
            for (const Collection* c : cls->collections) all.push_back(c);
        const std::string text = sc::Print(sc::NewScript(*db, sc::Export(*db, all)));
        CHECK(text.find("\ngame " + db->target().id() + "\n") != std::string::npos);
        size_t asBytes = 0;
        for (size_t at = text.find("bytecode = bytes"); at != std::string::npos; at = text.find("bytecode = bytes", at + 1)) ++asBytes;
        CHECK_EQ(asBytes, handlersAsBytes);
        const sc::ApplyReport back = sc::Apply(*clone, sc::Parse(text, "everything.amod"));
        CHECK_OK(back);
        if (back.summary() != "no changes")
            std::fprintf(stderr, "%s: %s\n", folder.c_str(), sc::Print(sc::Diff(*db, *clone)).substr(0, 3000).c_str());
        CHECK_EQ(back.summary(), std::string("no changes"));
        CHECK_EQ(back.warnings(), size_t(0));
        for (size_t i = 0; i < db->packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(*db, *db->packs()[i]));

        // The edits, without a warning, read back as on the PC.
        const sc::ApplyReport r = Run(*clone, edits);
        CHECK_OK(r);
        CHECK_EQ(r.warnings(), size_t(0));
        if (pcAfter.empty()) pcAfter = values(*clone);
        CHECK_EQ(values(*clone), pcAfter);
        // Written out and read again, nothing differs.
        auto again = load();
        again->setTarget(clone->target());
        for (const auto& p : clone->packs()) again->loadPack(p->path, WritePack(*clone, *p));
        CHECK_EQ(Changes(sc::Diff(*clone, *again)), size_t(0));
    }
    CHECK(pcAfter.find("MASS = 1234.5") != std::string::npos && pcAfter.find("TruckSndFX = true") != std::string::npos);
    CHECK(pcAfter.find("mSurface = simsurface/organic") != std::string::npos && pcAfter.find("MASS = 1500") != std::string::npos);
}

TEST("script: the early Most Wanted demos write as scripts and apply back unchanged; their 64-bit keys take names") {
    struct Demo {
        std::string folder;
        size_t handlersAsBytes;
    };
    // Handlers left as bytes: the three JDLZ blocks (gpcore), as in retail's few.
    const std::vector<Demo> demos = {{"xbox/mw-early-oxm-betademo", 3}, {"ps2/mw-early-opm-betademo", 3}};
    // A reference, a key (GCollectionKey: a u64), a new collection, and a
    // name no dictionary has, which the file then holds as its StringHash64.
    const std::string edits = "attribulina 1\n"
                              "game mw_all\n"
                              "create pvehicle/demo_car from pvehicle/default { MASS = 1500 }\n"
                              "edit pvehicle/default { MASS = 1234.5\n OnScrapeGround[0].mSurface = simsurface/organic\n"
                              " OnScrapeGround[1].mEffect = effects/attribulina_new_effect }\n";
    std::string first;
    for (const auto& [folder, handlersAsBytes] : demos) {
        auto load = [&]() {
            auto db = std::make_unique<Database>();
            for (const char* s : {"mw_pc", "mw_gamecube", "mw_xbox_demo", "mw_ps2_demo"})
                db->addTypeSchema(TypeSchema::Load(test::DataPath(std::string("data/schema/") + s + ".txt")));
            db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
            return db;
        };
        auto db = load();
        for (const FolderFile& f : FolderPacks(test::DataPath("attrib/databases/" + folder)))
            if (f.skipped.empty()) db->loadPack(f.path);
        CHECK(db->keys64());
        for (const auto& p : db->packs())
            for (const auto& v : p->vaults) CHECK(v->warnings.empty());
        auto clone = sc::CloneDatabase(*db);
        std::vector<const Collection*> all;
        for (const auto& cls : db->classes())
            for (const Collection* c : cls->collections) all.push_back(c);
        const std::string text = sc::Print(sc::NewScript(*db, sc::Export(*db, all)));
        size_t asBytes = 0;
        for (size_t at = text.find("bytecode = bytes"); at != std::string::npos; at = text.find("bytecode = bytes", at + 1)) ++asBytes;
        CHECK_EQ(asBytes, handlersAsBytes);
        const sc::ApplyReport back = sc::Apply(*clone, sc::Parse(text, "everything.amod"));
        CHECK_OK(back);
        if (back.summary() != "no changes")
            std::fprintf(stderr, "%s: %s\n", folder.c_str(), sc::Print(sc::Diff(*db, *clone)).substr(0, 3000).c_str());
        CHECK_EQ(back.summary(), std::string("no changes"));
        CHECK_EQ(back.warnings(), size_t(0));
        for (size_t i = 0; i < db->packs().size(); ++i) CHECK(WritePack(*clone, *clone->packs()[i]) == WritePack(*db, *db->packs()[i]));

        const sc::ApplyReport r = Run(*clone, edits);
        CHECK_OK(r);
        Collection& car = Col(*clone, "pvehicle", "default");
        const Attribute* scrape = car.find(StringHash32("OnScrapeGround"));
        CHECK(scrape && scrape->elemSize == 56);
        if (scrape) {
            const uint8_t* e0 = scrape->elem(0);
            CHECK_EQ(load64(e0, Endian::Little), StringHash64("simsurface"));
            CHECK_EQ(load64(e0 + 8, Endian::Little), StringHash64("organic"));
            CHECK_EQ(load64(scrape->elem(1) + 24 + 8, Endian::Little), StringHash64("attribulina_new_effect"));
        }
        // Written out and read again: the same values, the new names by their StringHash64.
        auto again = load();
        again->setTarget(clone->target());
        again->names().add("attribulina_new_effect");
        for (const auto& p : clone->packs()) again->loadPack(p->path, WritePack(*clone, *p));
        CHECK_EQ(Changes(sc::Diff(*clone, *again)), size_t(0));
        const Collection* made = again->findCollection(StringHash32("pvehicle"), StringHash32("demo_car"));
        CHECK(made && made->exportId == StringHash64("pvehicle/demo_car"));
        std::string t = sc::Print(sc::NewScript(*again, {sc::ExportValue(*again, Col(*again, "pvehicle", "default"), StringHash32("OnScrapeGround"))}));
        t.erase(0, t.find("\ngame "));
        t.erase(0, t.find('\n', 1));
        if (first.empty()) first = t;
        CHECK_EQ(t, first);
    }
    CHECK(first.find("mSurface = simsurface/organic") != std::string::npos);
    CHECK(first.find("mEffect = effects/attribulina_new_effect") != std::string::npos);
}

TEST("script: the game line names a game and a platform; another one warns, strict stops") {
    auto db = LoadAll(false);
    CHECK_EQ(sc::GameId(*db), std::string("prostreet_pc"));
    const std::string body = "edit audio_revlimiter/default { total_frames_short = 9 }\n";
    auto with = [&](const std::string& game, bool strict = false) {
        auto copy = sc::CloneDatabase(*db);
        sc::ApplySettings st;
        st.strict = strict;
        return Run(*copy, "attribulina 1\ngame " + game + "\n" + body, st);
    };
    sc::ApplyReport r = with("prostreet_pc");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(0));
    r = with("ProStreet_PC"); // either case
    CHECK_EQ(r.warnings(), size_t(0));
    r = with("prostreet_x360");
    CHECK_OK(r);
    CHECK_EQ(Messages(r), std::string("2: the script is for Pro Street on Xbox 360, and this database is Pro Street on PC\n"));
    r = with("carbon_pc");
    CHECK_EQ(Messages(r), std::string("2: the script is for Carbon on PC, and this database is Pro Street on PC\n"));
    r = with("prostreet");
    CHECK_EQ(Messages(r), std::string("2: unknown game \"prostreet\": it names a game and a platform, such as prostreet_pc or prostreet_ps2, "
                                      "or prostreet_all for every platform\n"));
    // <game>_all fits every platform of the game.
    r = with("prostreet_all");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(0));
    CHECK_EQ(with("ProStreet_ALL").warnings(), size_t(0));
    r = with("carbon_all");
    CHECK_EQ(Messages(r), std::string("2: the script is for Carbon on every platform, and this database is Pro Street on PC\n"));
    CHECK_EQ(Messages(with("all")), std::string("2: unknown game \"all\": it names a game and a platform, such as prostreet_pc or "
                                                 "prostreet_ps2, or prostreet_all for every platform\n"));
    {
        // bytes keep one byte order: a script for every platform warns once.
        auto copy = sc::CloneDatabase(*db);
        r = Run(*copy, "attribulina 1\ngame prostreet_all\nedit audio_revlimiter/default {\n    total_frames_short = bytes \"09 00 00 00\"\n"
                       "    total_frames_long = bytes \"09 00 00 00\"\n}\n");
        CHECK_OK(r);
        CHECK_EQ(Messages(r), std::string("4: the script is for every platform, and a bytes value keeps the byte order it was written in\n"));
        r = Run(*copy, "attribulina 1\ngame prostreet_pc\nedit audio_revlimiter/default { total_frames_short = bytes \"09 00 00 00\" }\n");
        CHECK_EQ(r.warnings(), size_t(0));
    }
    r = with("prostreet_ps2", true);
    CHECK(!r.ok);
    CHECK_EQ(r.summary(), std::string("no changes"));
    // Scripts made from a database name it.
    std::vector<const Collection*> one = {&Col(*db, "audio_revlimiter", "default")};
    CHECK(sc::Print(sc::NewScript(*db, sc::Export(*db, one))).find("\ngame prostreet_pc\n") != std::string::npos);
}

TEST("script: an array may go past the count its definition allows, with a warning (strict stops); a layout array keeps its space") {
    auto db = LoadAll(false);
    // vehicle's GEAR_RATIO is an entry array whose definition allows 9 elements.
    const Attribute& vanilla = Own(Col(*db, "vehicle", "997gt3rs"), "GEAR_RATIO");
    CHECK_EQ(vanilla.count, 8u);
    CHECK_EQ(ElementLimit(vanilla), 9u);
    CHECK_EQ(MaxElements(vanilla), 9u);          // the editor's default
    CHECK_EQ(MaxElements(vanilla, true), 65535u); // scripts, and the editor with its option on
    const Attribute& layout = Own(Col(*db, "rigidbodyspecs", "default"), "GROUND_FRICTION");
    CHECK_EQ(MaxElements(layout, true), 2u);
    CHECK_EQ(ElementLimit(layout), 0u);

    auto copy = sc::CloneDatabase(*db);
    sc::ApplyReport r = Run(*copy, "attribulina 1\nedit vehicle/997gt3rs {\n"
                                   "    GEAR_RATIO = [3.446, 0, 3.82, 2.14, 1.48, 1.18, 0.97, 0.775, 0.66, 0.58]\n}\n");
    CHECK_OK(r);
    CHECK_EQ(Messages(r), std::string("3: GEAR_RATIO has 10 elements, more than the 9 its definition allows; game code made for 9 "
                                      "may ignore or mishandle the rest\n"));
    // insert and resize go past it too, each with a warning.
    r = Run(*copy, "attribulina 1\nedit vehicle/997gt3rs { insert GEAR_RATIO[10] = 0.5 }\n");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(1));
    r = Run(*copy, "attribulina 1\nedit vehicle/997gt3rs { resize GEAR_RATIO 12 }\n");
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(1));
    const Attribute& g = Own(Col(*copy, "vehicle", "997gt3rs"), "GEAR_RATIO");
    CHECK_EQ(g.count, 12u);
    CHECK_EQ(F32(g, 9), 0.58f);
    CHECK_EQ(F32(g, 10), 0.5f);
    CHECK_EQ(F32(g, 11), 0.0f);

    // Written out and read again, the 12 are there, and the packs write the same again.
    Database again;
    again.setTarget(copy->target());
    again.addTypeSchema(TypeSchema::Load(test::DataPath("data/schema/prostreet_pc.txt")));
    again.names().loadFile(test::DataPath("data/attrib_keys.txt"));
    for (const auto& p : copy->packs()) again.loadPack(p->path, WritePack(*copy, *p));
    for (const auto& p : again.packs())
        for (const auto& v : p->vaults) CHECK(v->warnings.empty());
    CHECK_EQ(Own(Col(again, "vehicle", "997gt3rs"), "GEAR_RATIO").count, 12u);
    CHECK_EQ(Changes(sc::Diff(*copy, again)), size_t(0));
    for (size_t i = 0; i < again.packs().size(); ++i) CHECK(WritePack(again, *again.packs()[i]) == WritePack(*copy, *copy->packs()[i]));

    // Its export applies back with no change and no warning; on the original it warns.
    std::vector<const Collection*> one = {&Col(*copy, "vehicle", "997gt3rs")};
    const sc::Script exported = sc::NewScript(*copy, sc::Export(*copy, one));
    r = sc::Apply(again, exported);
    CHECK_OK(r);
    CHECK_EQ(r.summary(), std::string("no changes"));
    CHECK_EQ(r.warnings(), size_t(0));
    auto fresh = sc::CloneDatabase(*db);
    r = sc::Apply(*fresh, exported);
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(1));

    // Strict stops at the warning and changes nothing.
    auto strict = sc::CloneDatabase(*db);
    sc::ApplySettings st;
    st.strict = true;
    r = Run(*strict, "attribulina 1\nedit vehicle/997gt3rs { resize GEAR_RATIO 10 }\n", st);
    CHECK(!r.ok);
    CHECK_EQ(Own(Col(*strict, "vehicle", "997gt3rs"), "GEAR_RATIO").count, 8u);

    // A layout array can't: the game's compiled layout fixes its space.
    r = Run(*copy, "attribulina 1\nedit rigidbodyspecs/default { resize GROUND_FRICTION 3 }\n");
    CHECK(!r.ok);
    CHECK(Messages(r).find("GROUND_FRICTION holds at most 2 elements (its layout space)") != std::string::npos);
    r = Run(*copy, "attribulina 1\nedit rigidbodyspecs/default { insert GROUND_FRICTION[0] = 1 }\n");
    CHECK(!r.ok);
}

TEST("script: Merge folds values into an existing edit") {
    sc::Script s = sc::Parse("attribulina 1\nedit a/b {\n    X = 1\n}\n");
    sc::Statement e;
    e.kind = sc::Statement::Kind::Edit;
    e.target.cls = "a";
    e.target.name = "b";
    sc::Statement x;
    x.kind = sc::Statement::Kind::Set;
    x.path.field = "X";
    x.value = sc::MakeNumber("2");
    sc::Statement y = x;
    y.path.field = "Y";
    e.body = {x, y};
    sc::Merge(s, {e});
    CHECK_EQ(sc::Print(s), std::string("attribulina 1\nedit a/b {\n    X = 2\n    Y = 2\n}\n"));

    // A vault two additions both make is made once.
    sc::Script v = sc::Parse("attribulina 1\ncreate vault my_vault in gameplay.lzc\n");
    sc::Statement cv;
    cv.kind = sc::Statement::Kind::CreateVault;
    cv.vault = "MY_vault";
    cv.word = "gameplay.lzc";
    sc::Statement other = cv;
    other.vault = "other";
    sc::Merge(v, {cv, other});
    CHECK_EQ(sc::Print(v), std::string("attribulina 1\ncreate vault my_vault in gameplay.lzc\ncreate vault other in gameplay.lzc\n"));
}

TEST("script: a vault written as a script, with its collections, is made again in a database without it") {
    auto base = LoadAll();
    auto cur = sc::CloneDatabase(*base);
    {
        GameplayModel m(*cur);
        Collection& src = Col(*cur, "gameplay", "tracks/autobahn_gd/autobahn_gd_grip_1");
        DuplicateGameplayElement(*cur, m, src, "tracks/autobahn_gd/autobahn_gd_grip_9", true);
    }
    Vault* made = FindVault(*cur, "autobahn_gd_grip_9");
    CHECK(made && made->collections.size() > 10);

    // `create vault`, then each collection in it.
    std::vector<sc::Statement> st{sc::CreateVaultStatement(*made)};
    const std::vector<sc::Statement> cols = sc::Export(*cur, std::vector<const Collection*>(made->collections.begin(), made->collections.end()));
    st.insert(st.end(), cols.begin(), cols.end());
    const std::string text = sc::Print(sc::NewScript(*cur, st));
    CHECK(text.find("create vault autobahn_gd_grip_9 in gameplay.lzc\n") != std::string::npos);
    CHECK(text.find("collection gameplay/tracks/autobahn_gd/autobahn_gd_grip_9/startgrid1 under ") != std::string::npos);

    auto target = sc::CloneDatabase(*base);
    const sc::ApplyReport r = sc::Apply(*target, sc::Parse(text, "vault.amod"));
    CHECK_OK(r);
    CHECK_EQ(r.warnings(), size_t(0));
    Vault* again = FindVault(*target, "autobahn_gd_grip_9");
    CHECK(again && again->pack == target->packs().back().get());
    CHECK_EQ(again->collections.size(), made->collections.size());
    for (const Collection* c : made->collections) {
        const Collection* t = target->findCollection(c->classKey, c->key);
        CHECK(t && t->vault == again);
        if (t) CHECK(sc::DiffValues(*target, *t, *cur, *c).body.empty());
    }
    // Applied again, it finds the vault there.
    const sc::ApplyReport twice = sc::Apply(*target, sc::Parse(text, "vault.amod"));
    CHECK_OK(twice);
    CHECK_EQ(twice.summary(), std::string("no changes"));
}

TEST("script: a gameplay folder written as a script makes it, its vaults and its place in its owner's Children") {
    auto base = LoadAll();
    auto cur = sc::CloneDatabase(*base);
    {
        // A copy of a race bin: new elements in new vaults, listed in prostreet_career/bins.
        GameplayModel m(*cur);
        Collection& bin = Col(*cur, "gameplay", "prostreet_career/bins/race_bin_an_2_1");
        DuplicateGameplayElement(*cur, m, bin, "prostreet_career/bins/race_bin_an_2_9", true);
    }
    std::vector<std::string> newVaults;
    for (const auto& v : cur->packs().back()->vaults)
        if (!FindVault(*base, v->name)) newVaults.push_back(v->name);
    CHECK(!newVaults.empty());

    GameplayModel m(*cur);
    const GameplayModel::Node* folder = m.node("prostreet_career/bins/race_bin_an_2_9");
    CHECK(folder && folder->collection);
    // Its elements are made from templates: none are made from it.
    CHECK(folder->collection->children.empty());
    const std::vector<Collection*> elements = GameplayFolderContents(m, *folder);
    CHECK(elements.size() > 5);
    CHECK(elements.front() == folder->collection);
    const std::string text = sc::Print(
        sc::NewScript(*cur, sc::ExportGameplay(*cur, m, std::vector<const Collection*>(elements.begin(), elements.end()))));
    for (const std::string& v : newVaults) CHECK(text.find("create vault " + v + " in gameplay.lzc\n") != std::string::npos);
    CHECK(text.find("collection gameplay/prostreet_career/bins/race_bin_an_2_9 under ") != std::string::npos);
    // The bins folder lists the new bin; the bins themselves come with the folder.
    CHECK(text.find("edit gameplay/prostreet_career/bins {\n    Children = [") != std::string::npos);
    CHECK(text.find("edit gameplay/prostreet_career/bins/race_bin_an_2_9 {") == std::string::npos);
    // Vaults first.
    CHECK(text.find("create vault ") < text.find("collection gameplay/"));

    // Applied to the database without it, the two match.
    auto target = sc::CloneDatabase(*base);
    const sc::ApplyReport r = sc::Apply(*target, sc::Parse(text, "bin.amod"));
    CHECK_OK(r);
    if (r.warnings()) std::fprintf(stderr, "%s", Messages(r).c_str());
    CHECK_EQ(r.warnings(), size_t(0));
    const sc::Script left = sc::Diff(*target, *cur);
    if (Changes(left)) std::fprintf(stderr, "%s", sc::Print(left).substr(0, 4000).c_str());
    CHECK_EQ(Changes(left), size_t(0));
    const sc::ApplyReport twice = sc::Apply(*target, sc::Parse(text, "bin.amod"));
    CHECK_OK(twice);
    CHECK_EQ(twice.summary(), std::string("no changes"));

    // A retail race lists its checkpoints, kept under its track's path: they come
    // with it, and the script changes nothing in the database it came from.
    GameplayModel bm(*base);
    const GameplayModel::Node* race = bm.node("race_bin_exotic/4_5_2/shortcut06");
    CHECK(race && race->collection && race->children.empty());
    const std::vector<Collection*> withCheckpoints = GameplayFolderContents(bm, *race);
    CHECK(std::find(withCheckpoints.begin(), withCheckpoints.end(),
                    &Col(*base, "gameplay", "tracks/leipzig/leipzig_grip_1_gp/checkpoint1")) != withCheckpoints.end());
    const std::string raceText = sc::Print(sc::NewScript(
        *base, sc::ExportGameplay(*base, bm, std::vector<const Collection*>(withCheckpoints.begin(), withCheckpoints.end()))));
    auto same = sc::CloneDatabase(*base);
    const sc::ApplyReport none = sc::Apply(*same, sc::Parse(raceText, "race.amod"));
    CHECK_OK(none);
    CHECK_EQ(none.summary(), std::string("no changes"));
    for (size_t i = 0; i < base->packs().size(); ++i) CHECK(WritePack(*same, *same->packs()[i]) == WritePack(*base, *base->packs()[i]));
}

TEST("script: create vault finds a vault of that name in another file, in any case") {
    auto db = LoadAll();
    const Vault* cars = FindVault(*db, "cars");
    CHECK(cars && cars->pack != db->packs().back().get());
    const size_t vaults = db->packs().back()->vaults.size();
    const sc::ApplyReport r = Run(*db, "attribulina 1\ncreate vault CARS in gameplay.lzc\n");
    CHECK_OK(r);
    CHECK_EQ(r.summary(), std::string("no changes"));
    CHECK(Messages(r).find("vault cars exists already in ") != std::string::npos);
    CHECK_EQ(db->packs().back()->vaults.size(), vaults);
}

TEST("script: echo prints text, options and field values; echo error stops") {
    const std::string text = "attribulina 1\n"
                             "option scale number default 2\n"
                             "echo \"Scale is\" $scale\n"
                             "echo warning \"careful\", $scale * 2\n"
                             "edit pvehicle/street {\n"
                             "    echo \"horn\" HornType\n"
                             "    HornType = 4\n"
                             "    echo \"now\" HornType\n"
                             "}\n";
    const sc::Script s = sc::Parse(text, "echo.amod");
    CHECK_EQ(sc::Print(s), std::string("attribulina 1\noption scale number default 2\necho \"Scale is\" $scale\n"
                                       "echo warning \"careful\" $scale * 2\nedit pvehicle/street {\n    echo \"horn\" HornType\n"
                                       "    HornType = 4\n    echo \"now\" HornType\n}\n"));
    auto db = LoadAll(false);
    const sc::ApplyReport r = sc::Apply(*db, s);
    CHECK_OK(r);
    std::vector<std::string> echoes, warnings;
    for (const sc::Message& m : r.messages) {
        if (m.level == sc::Message::Level::Echo) echoes.push_back(std::to_string(m.pos.line) + " " + m.text);
        if (m.level == sc::Message::Level::Warning) warnings.push_back(m.text);
    }
    CHECK(echoes == (std::vector<std::string>{"3 Scale is 2", "6 horn 5", "8 now 4"}));
    CHECK(warnings == std::vector<std::string>{"careful 4"});
    // echo error stops the script, with its text.
    const sc::ApplyReport e = Run(*db, "attribulina 1\necho error \"this mod needs the 1.1 patch\"\nedit pvehicle/street { HornType = 1 }\n");
    CHECK(!e.ok);
    CHECK(Messages(e).find("2: this mod needs the 1.1 patch") != std::string::npos);
    CHECK_EQ(int(Own(Col(*db, "pvehicle", "street"), "HornType").data.bytes[0]), 4);
}

TEST("script: include runs other scripts, from the including script's folder") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "attribulina_include_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "parts");
    auto write = [&](const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; };
    const std::string folder = dir.string();
    auto parse = [&](const std::string& text) { return sc::Parse(text, "mod.amod", folder); };

    write(dir / "mod.amod", "attribulina 1\n"
                            "option frames integer \"Frames\" default 5\n"
                            "include parts/a.amod\n"
                            "if $loud { include \"parts/b.amod\" }\n"
                            "include \"parts/b.amod\"  # twice: its option comes once\n");
    write(dir / "parts" / "a.amod", "attribulina 1\n"
                                    "game prostreet_pc\n"
                                    "option loud bool \"Loud\" default true\n"
                                    "edit audio_revlimiter/default { total_frames_short = $frames }\n"
                                    "include ../c.amod\n");
    write(dir / "parts" / "b.amod", "option long integer default 40\n"
                                    "edit audio_revlimiter/default { total_frames_long = $long }\n");
    write(dir / "c.amod", "collection pvehicle/inc_car under pvehicle/street in vault db { HornType = 2 }\n");

    std::ifstream in(dir / "mod.amod", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    sc::Script s = parse(text);
    // Printed as written; the included scripts aren't part of it.
    CHECK_EQ(sc::Print(s), std::string("attribulina 1\noption frames integer \"Frames\" default 5\ninclude \"parts/a.amod\"\n"
                                       "if $loud {\n    include \"parts/b.amod\"\n}\ninclude \"parts/b.amod\"  # twice: its option comes once\n"));
    CHECK(sc::HasUnreadIncludes(s));
    CHECK_EQ(s.options().size(), size_t(1));
    sc::LoadIncludes(s);
    CHECK(!sc::HasUnreadIncludes(s));
    std::vector<std::string> ids;
    for (const sc::OptionDef* o : s.options()) ids.push_back(o->id);
    CHECK(ids == (std::vector<std::string>{"frames", "loud", "long"}));
    CHECK(s.statements[4].included == s.statements[3].body[0].included); // read once

    auto db = LoadAll(false);
    sc::ApplySettings st;
    st.options = {{"frames", "9"}, {"long", "41"}};
    CHECK_OK(sc::Apply(*db, s, st));
    Collection& rev = Col(*db, "audio_revlimiter", "default");
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 9u);
    CHECK_EQ(U32(Own(rev, "total_frames_long")), 41u);
    CHECK_EQ(int(Own(Col(*db, "pvehicle", "inc_car"), "HornType").data.bytes[0]), 2);
    // Apply reads them itself when the caller hasn't.
    CHECK_OK(sc::Apply(*db, parse(text), {}));
    CHECK_EQ(U32(Own(rev, "total_frames_short")), 5u);

    // A mistake in an included script stops at the include line, naming the
    // file and its line; through two includes, the innermost one.
    write(dir / "parts" / "bad.amod", "attribulina 1\necho error \"stop here\"\n");
    write(dir / "parts" / "wrap.amod", "# wraps bad\ninclude bad.amod\n");
    auto error = [&](const sc::ApplyReport& r) {
        CHECK(!r.ok);
        for (const sc::Message& m : r.messages)
            if (m.level == sc::Message::Level::Error) return m;
        return sc::Message();
    };
    sc::Message m = error(sc::Apply(*db, parse("attribulina 1\n\ninclude parts/wrap.amod\n")));
    CHECK_EQ(m.pos.line, 3);
    CHECK_EQ(m.text, std::string("stop here"));
    CHECK_EQ(fs::path(m.file).filename().string(), std::string("bad.amod"));
    CHECK_EQ(m.fileLine, 2);

    // So does a script that doesn't read.
    write(dir / "parts" / "typo.amod", "attribulina 1\nedit audio_revlimiter/default {\n    total_frames_short =\n}\n");
    sc::Script typo = parse("attribulina 1\ninclude parts/typo.amod\n");
    try {
        sc::LoadIncludes(typo);
        CHECK(false);
    } catch (const sc::ScriptError& e) {
        CHECK_EQ(e.pos.line, 2);
        CHECK_EQ(e.filePos.line, 3);
        CHECK(std::string(e.what()).find("typo.amod:3:") != std::string::npos);
    }
    m = error(sc::Apply(*db, typo));
    CHECK_EQ(m.pos.line, 2);
    CHECK_EQ(m.fileLine, 3);

    // Warnings, and references checked at the end, name the included script too.
    write(dir / "parts" / "warn.amod", "attribulina 1\n"
                                       "edit audio_revlimiter/default { total_frames_short = 6 was 99 }\n"
                                       "edit pvehicle/street { aivehicle = aivehicle/inc_missing }\n");
    const sc::ApplyReport w = sc::Apply(*db, parse("attribulina 1\ninclude parts/warn.amod\n"));
    CHECK_OK(w);
    std::vector<int> lines;
    for (const sc::Message& x : w.messages)
        if (x.level == sc::Message::Level::Warning) {
            CHECK_EQ(x.pos.line, 2);
            CHECK_EQ(fs::path(x.file).filename().string(), std::string("warn.amod"));
            lines.push_back(x.fileLine);
        }
    CHECK(lines == (std::vector<int>{2, 3}));
    sc::ApplySettings strict;
    strict.strict = true;
    m = error(sc::Apply(*db, parse("attribulina 1\ninclude parts/warn.amod\n"), strict));
    CHECK_EQ(m.fileLine, 2);

    // An included script's option mistakes name it as well.
    write(dir / "parts" / "opt.amod", "option frames integer default 3\n");
    m = error(sc::Apply(*db, parse("attribulina 1\noption frames integer default 5\ninclude parts/opt.amod\n")));
    CHECK_EQ(m.pos.line, 3);
    CHECK_EQ(m.fileLine, 1);
    CHECK(m.text.find("declared twice") != std::string::npos);

    // Missing files, scripts that include themselves, and relative paths from
    // a script that isn't saved.
    m = error(sc::Apply(*db, parse("attribulina 1\ninclude nope.amod\n")));
    CHECK(m.text.find("can't read") != std::string::npos);
    CHECK_EQ(m.pos.line, 2);
    write(dir / "self.amod", "attribulina 1\ninclude self.amod\n");
    m = error(sc::Apply(*db, parse("attribulina 1\ninclude self.amod\n")));
    CHECK(m.text.find("can't include itself") != std::string::npos);
    write(dir / "loop_a.amod", "include loop_b.amod\n");
    write(dir / "loop_b.amod", "include loop_a.amod\n");
    m = error(sc::Apply(*db, parse("attribulina 1\ninclude loop_a.amod\n")));
    CHECK(m.text.find("includes this one") != std::string::npos);
    CHECK_EQ(fs::path(m.file).filename().string(), std::string("loop_b.amod"));
    m = error(sc::Apply(*db, sc::Parse("attribulina 1\ninclude parts/a.amod\n", "Script panel")));
    CHECK(m.text.find("isn't saved") != std::string::npos);
    // A full path needs no folder.
    CHECK_OK(sc::Apply(*db, sc::Parse("attribulina 1\ninclude " + sc::Quote((dir / "parts" / "b.amod").string()) + "\n", "Script panel")));
    fs::remove_all(dir);
}

TEST("script: PlaceAt names the collection or value under the caret, in a script with a mistake elsewhere too") {
    const std::string text = "attribulina 2\n"                                                    // 1
                             "game prostreet_pc\n"                                                // 2
                             "edit pvehicle/911gt3 {\n"                                           // 3
                             "    MAX_RPM = 9000 was 8400\n"                                      // 4
                             "    Cars = [vehicle/a, vehicle/b]\n"                                // 5
                             "    Parts[7] = { mKitNumber = 3, mPart = car_part/body }\n"        // 6
                             "}\n"                                                                // 7
                             "create pvehicle/x from pvehicle/911gt3 under pvehicle/default\n"   // 8
                             "# a comment\n"                                                      // 9
                             "if exists vehicle/240sx {\n"                                        // 10
                             "    edit vehicle/240sx {\n"                                         // 11
                             "        TORQUE.yMax = 300\n"                                        // 12
                             "    }\n"                                                            // 13
                             "}\n"                                                                // 14
                             "edit pvehicle/911gt3 {\n"                                           // 15
                             "    append Cars = [vehicle/c, vehicle/d]\n"                         // 16
                             "}\n";                                                               // 17
    auto at = [](const std::string& t, int line, int col) { return sc::PlaceAt(t, sc::Pos{line, col}); };
    auto value = [](const sc::ScriptPlace& p) { return p.target.text() + (p.hasPath ? " " + p.path.text() : std::string()); };
    for (const std::string& t : {text, text + "edit pvehicle/ {\n"}) {
        CHECK_EQ(value(at(t, 4, 6)), std::string("pvehicle/911gt3 MAX_RPM"));
        CHECK_EQ(value(at(t, 4, 15)), std::string("pvehicle/911gt3 MAX_RPM")); // the value is the field's
        CHECK_EQ(value(at(t, 4, 1)), std::string("pvehicle/911gt3 MAX_RPM")); // before the first word: the line's first
        CHECK_EQ(value(at(t, 3, 10)), std::string("pvehicle/911gt3"));
        CHECK_EQ(value(at(t, 5, 6)), std::string("pvehicle/911gt3 Cars"));
        const sc::ScriptPlace b = at(t, 5, 26);
        CHECK_EQ(value(b), std::string("pvehicle/911gt3 Cars[1]"));
        CHECK(b.named && b.named->text() == "vehicle/b");
        CHECK_EQ(value(at(t, 6, 20)), std::string("pvehicle/911gt3 Parts[7].mKitNumber"));
        CHECK_EQ(value(at(t, 6, 35)), std::string("pvehicle/911gt3 Parts[7].mPart"));
        const sc::ScriptPlace part = at(t, 6, 45);
        CHECK_EQ(value(part), std::string("pvehicle/911gt3 Parts[7].mPart"));
        CHECK(part.named && part.named->text() == "car_part/body");
        CHECK_EQ(value(at(t, 8, 3)), std::string("pvehicle/x"));
        CHECK_EQ(value(at(t, 8, 30)), std::string("pvehicle/911gt3"));
        CHECK_EQ(value(at(t, 8, 50)), std::string("pvehicle/default"));
        CHECK(at(t, 9, 3).target.cls.empty());
        CHECK(at(t, 7, 1).target.cls.empty());
        CHECK_EQ(value(at(t, 10, 13)), std::string("vehicle/240sx"));
        CHECK_EQ(value(at(t, 12, 12)), std::string("vehicle/240sx TORQUE.yMax"));
        // An appended list's items aren't the array's first elements.
        const sc::ScriptPlace d = at(t, 16, 34);
        CHECK_EQ(value(d), std::string("pvehicle/911gt3 Cars"));
        CHECK(d.named && d.named->text() == "vehicle/d");
    }
    // A statement that doesn't read itself names nothing.
    CHECK(at(text + "edit pvehicle/ {\n", 18, 6).target.cls.empty());

    // Its collections in a database: one, every match of a wildcard, or none.
    auto db = std::make_unique<Database>();
    db->names().loadFile(test::DataPath("data/attrib_keys.txt"));
    db->loadPack(test::DataPath("attrib/databases/pc/prostreet-vanilla/ATTRIBUTES.BIN"));
    sc::Target t;
    t.cls = "pvehicle";
    t.name = "street";
    CHECK_EQ(sc::TargetCollections(*db, t).size(), size_t(1));
    t.name = "*";
    CHECK(sc::TargetCollections(*db, t).size() > 1);
    t.name = "no_such_car";
    CHECK(sc::TargetCollections(*db, t).empty());
    t.cls = "no_such_class";
    CHECK(sc::TargetCollections(*db, t).empty());
    CHECK_EQ(sc::NameKey("0x12345678"), 0x12345678u);
    CHECK_EQ(sc::NameKey("\"0x12345678\""), StringHash32("0x12345678"));
}

TEST("script: PlaceAt reads the statement around the caret, inside a long if, past a handler's Lua and a mistake in the same block") {
    // A converted nfsms script is one long if per ui_option block; F12 parsed all of it.
    std::string text = "attribulina 2\n"                                       // 1
                       "game prostreet_pc\n"                                   // 2
                       "if $big {\n"                                           // 3
                       "    edit gameplay/h {\n"                               // 4
                       "        bytecode = lua \"\"\"\n"                       // 5
                       "function f() local t = { \"}\" } # not a comment\n"     // 6
                       "edit pvehicle/inside_the_string {\n"                   // 7
                       "end\n"                                                 // 8
                       "        \"\"\"\n"                                       // 9
                       "    }\n";                                              // 10
    const int cars = 5000;
    for (int i = 0; i < cars; ++i) text += "    edit pvehicle/car" + std::to_string(i) + " {\n        MAX_RPM = " + std::to_string(i) + "\n    }\n";
    const int after = 11 + 3 * cars;
    text += "    edit pvehicle/broken {\n"                 // after
            "        MAX_RPM = = 3\n"                      // after + 1
            "    }\n"                                      // after + 2
            "    edit pvehicle/last {\n"                   // after + 3
            "        Parts[2] = { mKitNumber = 4 }\n"      // after + 4
            "    }\n"
            "}\n";
    auto value = [&](int line, int col) {
        const sc::ScriptPlace p = sc::PlaceAt(text, sc::Pos{line, col});
        return p.target.text() + (p.hasPath ? " " + p.path.text() : std::string());
    };
    CHECK_EQ(value(11 + 3 * 1234, 12), std::string("pvehicle/car1234"));
    CHECK_EQ(value(12 + 3 * 1234, 9), std::string("pvehicle/car1234 MAX_RPM"));
    CHECK_EQ(value(after + 3, 12), std::string("pvehicle/last"));
    CHECK_EQ(value(after + 4, 22), std::string("pvehicle/last Parts[2].mKitNumber"));
    CHECK_EQ(value(4, 12), std::string("gameplay/h"));
    CHECK_EQ(value(5, 9), std::string("gameplay/h bytecode"));
    // Inside the Lua: no statement, whatever it looks like.
    CHECK(sc::PlaceAt(text, sc::Pos{7, 3}).target.cls.empty());
    CHECK(sc::PlaceAt(text, sc::Pos{after + 1, 9}).target.cls.empty()); // the mistake
}
