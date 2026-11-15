// Swim -- on foot in water you swim instead of walking on the bottom, as plugins\mt_plugin_Swim.dll.
//
// The user, 2026-11-11: boats are sold moored in the water, "make the player float". 2026-11-12: grown into swimming:
// "an animation for swimming, generic on all characters ... dive in or dive out ... something that would cost your
// stamina ... oxygen ... floating up, being dependent on the material of the water".
//   water   any WaterBody: the sea, lakes, rivers (a lake or river only inside its bounds and where it has depth).
//           Salt water floats you higher (FloatDepth) than fresh (FreshFloatDepth); a river's current carries you.
//           At sea you ride the Boats plugin's swell (same formula, same clock), so you bob with the boats.
//   in      deeper than the float line, walking or falling -> the character's movement goes to flying (no gravity);
//           input moves it along the water, the plugin owns the vertical
//   dive    hold Ctrl: down at DiveSpeed. Underwater: Space swims up, no key drifts up (buoyancy, less in fresh water)
//   out     Space at the surface -> a jump of JumpOut cm/s; or walking up a beach puts you back on your feet
//   fix     the character can still stand on a vehicle the game destroyed (a spawn at BoaterTown drops the player off
//           the vehicle it just destroyed). The game reads that base when the movement mode changes -- the crash of
//           12/13 Nov (IndexToObject(-1) in the dumps). The base is cleared before the mode changes, and the game gets
//           a frame of falling first, so it re-sweeps its floor on the way.
//   perf    the surface under the player is asked for when the player moves, not every frame: the query cost the
//           frame 30 ms (the loader's "slow game-thread work" log).
//   oxygen  Oxygen seconds with the head under; refills at the surface. Beeps at 50% / 25% / 10%; at 0 you black out:
//           pulled to the surface and no diving until it is back to 30%. (No health in the game to take from yet.)
//   stamina Shift swims faster (SprintScale) for Stamina seconds, recovering when you don't
//   anim    the montages from qxZap_Swim_P (crawl moving, tread still, dive under), played on the character's own anim
//           blueprint through the emote slot, so it works on every character. No pak: you glide upright as before.
//
// Settings: plugins\swim.ini, [General] Disabled; [Swim] FloatDepth (60 cm: capsule centre under a salt surface),
// FreshFloatDepth (72), Speed (300 cm/s), SprintScale (1.6), Stamina (20 s), JumpOut (900), DiveSpeed (220),
// RiseSpeed (260), Drift (60 cm/s up when idle under salt water; fresh 2/3 of it), Oxygen (60 s), Swell (90 cm, only when the ocean has no wave list; otherwise the game's own waves, as Boats),
// DiveKey (17 = Ctrl, a Windows virtual-key code), CrawlMontage / TreadMontage / DiveMontage (object paths).
// shortcut: local only -- the montage and the swim pose are not sent to other players (they see you glide). Route it
// through the game's emote RPC (MTCharacter.EmoteMontage is replicated) once the swim emotes are table rows.
// shortcut: feedback is beeps; an oxygen/stamina bar needs a HUD widget.

#include <cmath>
#include <vector>
#include <mutex>
#include <set>
#include "../include/mt_plugin.hpp"
#include "sea_waves.hpp"

using namespace mt;

static const wchar_t* INI = L"swim.ini";
struct Vec { double x, y, z; };
struct FStr { const wchar_t* data; int num, max; };
enum : uint8_t { MOVE_Walking = 1, MOVE_Falling = 3, MOVE_Flying = 5 };

static uintptr_t cChar, cBody, cOcean, cRiver, fPawn, gameplayCDO, systemCDO, fLoc, fBodyComp, fSurface, fSetMode, fBounds, fMesh,
    fAnimInst, fPlay, fStop, fIsPlaying, fPath, fRef, fLoad, fSetBase;
static int oCMC = -1, oMode = -1, oVel = -1, oFly = -1, oSurfOut = -1, oSurfIncl = -1, oSurfVel = -1, oSurfDepth = -1, oCharMesh = -1,
    oMoveBase = -1, oPlayRet = -1, oCap = -1;
static double kDepth, kFresh, kSpeed, kSprint, kStamina, kJump, kDive, kRise, kDrift, kOxygen, kSwell, kLean;
static double floatDepth = 0;  // how far under the surface the capsule's centre sits: measured off the body
static int kDiveKey;
static volatile LONG queued, jump, keys;  // keys: 1 dive, 2 up (Space held), 4 sprint
static bool ours, saidBase;
static float flyWas;
static int oFallDist = -1;

// One water body. The surface answer is kept: the game's own query is not cheap (it cost the frame 30 ms in the loader
// log), the sea's level never changes (the waves are added from the game's own wave list) and a lake's does not either.
struct Water {
    uintptr_t o; int idx; bool sea, river; Vec lo, hi;
    uintptr_t comp;       // its WaterBodyComponent, asked once
    double z = 0, depth = 0, at = -1e9;  // the surface here, and when it was read
    Vec flow{}; bool bounds = false, ok = false;  // ok: the query answered
};
static std::vector<Water> waters;  // game thread only
static volatile LONG haveWater;

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static double nowS() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / f.QuadPart; }

// ---- is that object still there? ----
// The game destroys a vehicle and the character's movement base keeps pointing at it (a spawn at BoaterTown drops the
// player off the vehicle it just destroyed). The pointer still looks fine -- the object is dead but not collected yet --
// and the game reads it and makes a weak pointer of an index of -1: the crash of 12/13 Nov (IndexToObject(-1) in the
// dumps, 1-3 s after Swim switched the movement mode). The loader keeps the object array up to date: a destroyed
// object reads back as 0, so this asks the array and not the pointer.
static bool alive(uintptr_t o) {
    if (!o) return false;
    int i = get<int>(o + L().obj_index, -1);
    if (i < 0) return false;  // a destroyed object has no index any more
    return api->object(i) == o;
}

// Clear a movement base that points at a destroyed object, and let the game redo its based movement.
static void clearDeadBase(uintptr_t cmc) {
    if (oMoveBase < 0) return;
    uintptr_t base = get<uintptr_t>(cmc + oMoveBase);
    if (!base || alive(base)) return;
    MT_LOG("swim: the movement base is a destroyed object (%s, index %d) -- clearing it", objName(base).c_str(),
           get<int>(base + L().obj_index, -1));
    if (fSetBase) {
        struct { uintptr_t b; bool force; } q = {0, true};
        api->process_event(cmc, fSetBase, &q);
    } else {
        // The game has no SetBase to call (2026-11-14: it is not a function in this build), so the pointer is cleared
        // directly. The game re-finds the floor on the falling frame that follows.
        uintptr_t none = 0;
        wr(cmc + oMoveBase, &none, sizeof none);
    }
}

// The body leans with the water (2026-11-14: "it floats ok, but is not synced with the waves"). The swimmer went up
// and down with the swell but never leaned, so it did not read as part of the sea. The mesh is tilted -- the capsule
// stays upright, so the movement and the camera are untouched -- and the angle comes from the wave's slope in the
// character's own frame. A hull does the same thing, in the Boats plugin.
static int oMeshRot = -1, oCapRot = -1;
static double meshBase[3] = {0, 0, 0}, tiltPitch, tiltRoll;
static bool rotOk = true;

static void readRot(uintptr_t obj, int at, double* v) {
    v[0] = v[1] = v[2] = 0;
    if (!obj || at < 0) return;
    for (int i = 0; i < 3; i++) v[i] = get<double>(obj + at + 8 * i);
    // FRotator is three doubles (Pitch, Yaw, Roll). If the game kept it as floats, this reads nonsense -- and a
    // nonsense tilt would turn the character on its side, so it is checked once.
    if (rotOk && (v[0] > 400 || v[0] < -400 || v[1] > 400 || v[1] < -400 || v[2] > 400 || v[2] < -400)) {
        rotOk = false;
        MT_LOG("swim: the rotation is not a double -- the body will not lean with the water");
    }
}
static void writeRot(uintptr_t obj, int at, const double* v) {
    if (!obj || at < 0) return;
    for (int i = 0; i < 3; i++) wr(obj + at + 8 * i, &v[i], 8);
}

// put the body straight again, whatever way the swim ended
static void restoreLean(uintptr_t ch) {
    uintptr_t mesh = (oCharMesh >= 0 && ch) ? get<uintptr_t>(ch + oCharMesh) : 0;
    if (alive(mesh)) writeRot(mesh, oMeshRot, meshBase);
    tiltPitch = tiltRoll = 0;
}

// How deep the water sits on the body (2026-11-14: "character floating on top, not floating in the water"). The float
// line is a height on the *body* -- a chest deep -- measured off the character's own mesh, so a short character and a
// tall one both sit right. The old fixed 60 cm under the surface was right for one body only.
static int oBOrg = -1, oBExt = -1;
static double kWaterLine = 0.72;  // where the water sits, as a fraction of the body from the feet (the chest)

// The water line on this body: the capsule's centre has to sit this far under the surface.
static double floatDepthFor(uintptr_t ch, double capZ) {
    if (!ch || !fBounds || oBOrg < 0 || oBExt < 0) {
        MT_LOG("swim: the body's bounds are not available -- the water stays %.0f cm under the surface", kDepth);
        return kDepth;
    }
    unsigned char p[0x60] = {0};
    p[0] = 0;  // bOnlyCollidingComponents = false: the whole body, mesh and all
    api->process_event(ch, fBounds, p);
    double org[3], ext[3];
    memcpy(org, p + oBOrg, 24);
    memcpy(ext, p + oBExt, 24);
    double feet = org[2] - ext[2], head = org[2] + ext[2];
    if (head - feet < 20 || head - feet > 400) {
        MT_LOG("swim: the body's bounds are odd (%.0f..%.0f) -- the water stays %.0f cm under the surface", feet, head, kDepth);
        return kDepth;
    }
    double chest = feet + kWaterLine * (head - feet);
    double d = chest - capZ;
    MT_LOG("swim: the body is %.0f cm tall, the water sits %.0f cm over the feet, so the capsule's centre goes %.0f cm under the surface",
           head - feet, kWaterLine * (head - feet), d);
    return d > 0 ? d : kDepth;
}

// A stage of the tick that took too long: what the loader calls a slow game-thread job.
static void slowStage(const char* what, double ms) {
    if (ms > 4.0) MT_LOG("swim: %s took %.1f ms", what, ms);
}

// the Boats plugin's swell, copied so a swimmer rides the same sea (same waves, same clock: QPC is per machine)
static double swell(double x, double y, double t) {
    if (kSwell <= 0) return 0;
    static const struct { double len, dir, share; } W[4] = {{9000, 0.2, 0.45}, {3200, 0.7, 0.3}, {1300, 1.25, 0.15}, {700, -0.9, 0.1}};
    double z = 0;
    for (auto& w : W) {
        double k = 2 * 3.14159265 / w.len, om = sqrt(980 * k);
        z += w.share * sin(k * (x * cos(w.dir) + y * sin(w.dir)) - om * t);
    }
    return kSwell * z;
}

// vertical speed towards the float line, gently (no bounce at 60 fps)
static double holdSpeed(double z, double target) { return clampd((target - z) * 4.0, -400, 400); }

// what the swimmer does this frame, given where they are; pure so the demo can check it.
// ride is the water's own motion here (cm/s): it goes into the velocity, so the swimmer rides the wave exactly the
// way a hull does, instead of chasing the surface and lagging it by tens of centimetres (2026-11-14).
struct Step { double vz; bool blackout; };
static Step vertical(double z, double line, bool dive, bool up, bool blackedOut, bool fresh, double ride) {
    if (blackedOut) return {clampd((line - z) * 4.0, 0, 400) + ride, true};
    if (dive) return {-kDive + ride, false};
    if (z > line - 30) return {holdSpeed(z, line) + ride, false};  // at the surface
    if (up) return {kRise + ride, false};
    return {kDrift * (fresh ? 2.0 / 3 : 1) + ride, false};
}

static void setMode(uintptr_t cmc, uint8_t m) {
    struct { uint8_t mode, custom; } p = {m, 0};
    api->process_event(cmc, fSetMode, &p);
}

static uintptr_t loadAsset(const std::wstring& path) {
    if (!fPath || !fRef || !fLoad) return 0;
    unsigned char a[0x80] = {0}, b[0x80] = {0}, c[0x80] = {0};
    FStr s{path.c_str(), (int)path.size() + 1, (int)path.size() + 1};
    int in = off(fRef, "SoftObjectPath"), load = off(fLoad, "Asset"), got = off(fLoad, "ReturnValue");
    memcpy(a + off(fPath, "PathString"), &s, sizeof s);
    api->process_event(systemCDO, fPath, a);
    memcpy(b + in, a + off(fPath, "ReturnValue"), off(fRef, "ReturnValue") - in);
    api->process_event(systemCDO, fRef, b);
    memcpy(c + load, b + off(fRef, "ReturnValue"), got - load);
    api->process_event(systemCDO, fLoad, c);
    uintptr_t out = 0;
    memcpy(&out, c + got, 8);
    return rooted(out);
}

// ---- animation: one montage at a time on the character's own anim instance ----
enum Anim { A_None, A_Crawl, A_Tread, A_Dive, A_DiveIn, A_Surface, A_ClimbOut, A_N };
static const double kOneShot[A_N] = {0, 0, 0, 0, 1.0, 0.8, 0.8};  // seconds a one-shot runs before the loops take over again
static double oneShotEnd;
static uintptr_t montage[A_N];
static int montageIdx[A_N];
static bool montagesTried;
static Anim playing = A_None;
static void loadMontages() {
    montagesTried = true;
    const wchar_t* keyN[A_N] = {nullptr, L"CrawlMontage", L"TreadMontage", L"DiveMontage", L"DiveInMontage", L"SurfaceMontage", L"ClimbOutMontage"};
    const wchar_t* def[A_N] = {nullptr, L"/Game/Swim/Anims/Swim_Crawl_Montage.Swim_Crawl_Montage",
                               L"/Game/Swim/Anims/Swim_Tread_Montage.Swim_Tread_Montage", L"/Game/Swim/Anims/Swim_Dive_Montage.Swim_Dive_Montage",
                               L"/Game/Swim/Anims/Swim_DiveIn_Montage.Swim_DiveIn_Montage", L"/Game/Swim/Anims/Swim_Surface_Montage.Swim_Surface_Montage",
                               L"/Game/Swim/Anims/Swim_ClimbOut_Montage.Swim_ClimbOut_Montage"};
    int got = 0;
    for (int a = 1; a < A_N; a++) {
        wchar_t b[512] = {0};
        GetPrivateProfileStringW(L"Swim", keyN[a], def[a], b, 512, (gameDir() + L"plugins\\" + INI).c_str());
        montage[a] = loadAsset(b);
        for (int i = 0, n = api->num_objects(); montage[a] && i < n; i++) if (api->object(i) == montage[a]) { montageIdx[a] = i; break; }
        got += montage[a] != 0;
    }
    MT_LOG("swim animations: %d of 6 loaded%s", got, got ? "" : " (qxZap_Swim_P not installed or not enabled: gliding upright)");
}
static bool montageAlive(Anim a) { return montage[a] && api->object(montageIdx[a]) == montage[a]; }

// The skeleton a montage or a mesh belongs to. A montage on a skeleton the character does not have starts all the
// same and shows nothing -- that is a swimming animation that "does not kick in" on some characters (2026-11-14).
static std::string skeletonOf(uintptr_t o) {
    if (!o) return "";
    int at = off(classOf(o), "Skeleton");
    if (at < 0) return "";
    uintptr_t sk = get<uintptr_t>(o + at);
    return sk ? objName(sk) : "";
}
static std::string meshSkeleton(uintptr_t mesh) {
    if (!mesh) return "";
    int at = off(classOf(mesh), "SkeletalMeshAsset");
    if (at < 0) return "";
    uintptr_t asset = get<uintptr_t>(mesh + at);
    return skeletonOf(asset);
}
static uintptr_t animInstance(uintptr_t ch) {
    uintptr_t mesh = oCharMesh >= 0 ? get<uintptr_t>(ch + oCharMesh) : 0, ai = 0;
    // The character can be destroyed under us (a death, a respawn): a call on a dead object is the crash we are here for.
    if (!mesh || !alive(mesh)) return 0;
    if (fAnimInst) api->process_event(mesh, fAnimInst, &ai);
    return alive(ai) ? ai : 0;
}
static void play(uintptr_t ch, Anim a) {
    if (!fPlay || !fStop) return;
    uintptr_t ai = animInstance(ch);
    if (!ai) return;
    // 2026-11-12, "I don't get the floating animation at all times": the game stops a montage (landing, a jump, an
    // emote) without telling us, so ask whether ours still runs instead of trusting what we last started
    if (a == playing && (a == A_None || !fIsPlaying || !montageAlive(a))) return;
    if (a == playing) {
        struct { uintptr_t m; bool ret; } q = {montage[a], false};
        api->process_event(ai, fIsPlaying, &q);
        if (q.ret) return;
        // the game stopped our montage (a landing, a jump, an emote of its own): say so, and start it again
        static double lastStopLog;
        if (nowS() - lastStopLog > 2.0) { lastStopLog = nowS(); MT_LOG("swim: the game stopped the swim montage -- playing it again"); }
    }
    if (a == A_None || !montageAlive(a)) {
        if (playing != A_None && montageAlive(playing)) { struct { float blend; uintptr_t m; } s = {0.3f, montage[playing]}; api->process_event(ai, fStop, &s); }
        playing = A_None;
        return;
    }
    struct { uintptr_t m; float rate; uint8_t ret; float start; bool stopAll; float out; } p = {montage[a], 1.0f, 0, 0.0f, true, 0};
    api->process_event(ai, fPlay, &p);
    playing = a;
    // Montage_Play returns the length it started (0 = it did not start: the skeleton does not have it, or the slot is
    // busy). Say so once, or the swim looks like it has no animation at all.
    if (oPlayRet >= 0) {
        float len = 0;
        memcpy(&len, (unsigned char*)&p + oPlayRet, 4);
        static double lastFailLog;
        if (len <= 0 && nowS() - lastFailLog > 5.0) { lastFailLog = nowS(); MT_LOG("swim: the montage %d did not start", (int)a); }
    }
}

// ---- water under the player ----
// 2026-11-12: walking all ~520k objects on the game thread stalled it 36-40 ms every 10 s. The walk now runs on the
// plugin's own thread (it only reads); the game thread gets the few water bodies found and asks their bounds.
static std::mutex foundLock;
static std::vector<std::pair<int, uintptr_t>> foundBodies;
static void scanWaters() {  // plugin thread
    std::vector<std::pair<int, uintptr_t>> f;
    static bool saidWater;
    std::set<std::string> waterClasses;
    for (int i = 0, n = api->num_objects(); i < n; i++) {
        uintptr_t o = api->object(i);
        if (!o) continue;
        if (!isCDO(o) && api->is_a(classOf(o), cBody)) f.push_back({i, o});
        if (!saidWater && waterClasses.size() < 60) {  // what the game itself has for water
            std::string cn = objName(classOf(o));
            if (cn.find("Water") != std::string::npos || cn.find("Swim") != std::string::npos) waterClasses.insert(cn);
        }
    }
    sea::scanSeaMeshes();
    if (!saidWater && !waterClasses.empty()) {
        saidWater = true;
        std::string all;
        for (auto& c : waterClasses) all += c + " ";
        MT_LOG("the game's own water classes: %s", all.c_str());
    }
    std::lock_guard<std::mutex> g(foundLock);
    foundBodies.swap(f);
}
static void findWaters() {  // game thread
    std::vector<std::pair<int, uintptr_t>> f;
    { std::lock_guard<std::mutex> g(foundLock); f = foundBodies; }
    std::vector<Water> w;
    w.reserve(f.size());
    for (auto& [i, o] : f) {
        if (api->object(i) != o) continue;  // gone since the scan
        Water x{o, i, cOcean && api->is_a(classOf(o), cOcean), cRiver && api->is_a(classOf(o), cRiver), {}, {}};
        // What we learned about this body last time: a lake does not move, and its bounds and its component cost a
        // call each. Keep them.
        for (auto& old : waters)
            if (old.o == o) { x.lo = old.lo; x.hi = old.hi; x.bounds = old.bounds; x.comp = old.comp; break; }
        if (!x.bounds && fBounds) {
            struct { bool only; Vec org, ext; bool child; } b = {false, {}, {}, false};
            api->process_event(o, fBounds, &b);
            x.lo = {b.org.x - b.ext.x, b.org.y - b.ext.y, b.org.z - b.ext.z - 200};
            x.hi = {b.org.x + b.ext.x, b.org.y + b.ext.y, b.org.z + b.ext.z + 200};
            x.bounds = true;
        }
        w.push_back(x);
    }
    waters.swap(w);
    if (!montagesTried && !waters.empty()) loadMontages();  // load before the first swim, not during it
    static uintptr_t wavesFor;
    for (auto& x : waters)
        if (x.sea && x.o != wavesFor) {
            wavesFor = x.o;
            bool ok = sea::load(x.o);
            MT_LOG(ok ? "sea: following the game's %zu waves" : "sea: no wave list on the ocean (%zu): own swell", sea::list.size());
            if (ok) sea::logList();
            break;
        }
    // the ocean's own level: the water that is drawn (2026-11-14: the query answers the collision surface, which sits
    // above the mesh, so the swimmer stood on it and looked like it floated on top of the water)
    {
        std::vector<int> m;
        { std::lock_guard<std::mutex> g(sea::meshLock); m = sea::foundMeshes; }
        for (int i : m) {
            uintptr_t o = api->object(i);
            if (!o) continue;
            double v[3] = {0, 0, 0};
            if (!sea::compLocation(o, v)) continue;
            uintptr_t outer = outerOf(o);
            bool ours = false;
            for (auto& w : waters)
                if (w.sea && w.o == outer) ours = true;  // the mesh of the sea itself, not the zone's or a lake's
            MT_LOG("sea: a water mesh at %.0f %.0f %.0f (its outer is %s%s)", v[0], v[1], v[2], objName(outer).c_str(),
                   ours ? ", the sea's own" : "");
            if (ours && sea::base == 0) sea::base = v[2];
        }
    }
    InterlockedExchange(&haveWater, waters.empty() ? 0 : 1);
}

struct Surface { bool ok, sea, river; double z; Vec flow; };

// Ask one water body for its surface here. withDepth asks for the ground under the point too (a lake's edge: wade).
// The answer is kept in the body: the sea's level never changes and a lake's does not either.
static bool queryWater(Water& w, Vec at, bool withDepth) {
    w.at = nowS();  // even when it fails: a body that never answers must not be asked every frame
    w.ok = false;
    if (!alive(w.comp)) {
        uintptr_t c = 0;
        api->process_event(w.o, fBodyComp, &c);
        w.comp = c;
    }
    if (!w.comp) return false;
    unsigned char sp[0xa0] = {0};
    memcpy(sp, &at, sizeof at);
    if (oSurfIncl >= 0) sp[oSurfIncl] = withDepth ? 1 : 0;
    api->process_event(w.comp, fSurface, sp);
    Vec s; memcpy(&s, sp + oSurfOut, sizeof s);
    if (s.x == 0 && s.y == 0 && s.z == 0) return false;
    w.z = s.z;
    if (oSurfDepth >= 0) memcpy(&w.depth, sp + oSurfDepth, 4);
    if (w.river && oSurfVel >= 0) memcpy(&w.flow, sp + oSurfVel, sizeof(Vec));
    w.ok = true;
    return true;
}

// The surface under the player. The query is kept between calls: it cost the frame 30 ms in the loader log, and the
// answer only changes when the player moves.
static Surface surfaceAt(Vec at) {
    Surface best{false, false, false, -1e18, {}};
    static Vec lastAt{};
    double now = nowS();
    double dx = at.x - lastAt.x, dy = at.y - lastAt.y;
    bool moved = dx * dx + dy * dy > 50.0 * 50.0;
    for (auto& w : waters) {
        if (!alive(w.o)) continue;
        if (!w.sea && (at.x < w.lo.x || at.x > w.hi.x || at.y < w.lo.y || at.y > w.hi.y || at.z < w.lo.z || at.z > w.hi.z)) continue;
        bool stale = now - w.at > (w.sea ? 60.0 : 10.0) || (moved && now - w.at > 1.0);
        if (stale && !queryWater(w, at, !w.sea)) continue;
        if (!w.ok) continue;  // the body did not answer: it is not a surface
        if (!w.sea && w.depth < 80) continue;  // a lake's edge: wade
        if (w.z <= best.z) continue;
        best = {true, w.sea, w.river, w.z, w.flow};
    }
    lastAt = at;
    return best;
}

// Is there really sea under this point? The sea's surface height is the same everywhere, so ground lower than it
// (2026-11-12: a spot inland at 231542 -179122) read as "in the sea": swimming started on dry land and the game refused
// to let the player into vehicles or the dealer. Ask the ocean for its depth here (the costly query, so only before
// starting to swim, at most twice a second) and swim only where it is deep enough.
static bool seaHere(Vec at) {
    static double lastAsk, lastAnswer;
    static Vec lastAt{};
    double now = nowS();
    double dx = at.x - lastAt.x, dy = at.y - lastAt.y;
    // The answer is about the ground under this point: it only holds for the place it was asked at. The old code kept it
    // for half a second whatever the position, so a spot inland read as sea and swimming started on dry land.
    if (now - lastAsk < 0.5 && dx * dx + dy * dy < 50.0 * 50.0) return lastAnswer > 0;
    lastAsk = now;
    lastAt = at;
    lastAnswer = 0;
    if (oSurfDepth < 0 || oSurfIncl < 0) return true;  // shortcut: no depth field -> the old height-only test
    for (auto& w : waters) {
        if (!w.sea || !alive(w.o)) continue;
        uintptr_t comp = 0;
        if (!alive(w.comp)) {
            api->process_event(w.o, fBodyComp, &comp);
            w.comp = comp;
        } else comp = w.comp;
        if (!comp) continue;
        unsigned char sp[0xa0] = {0};
        memcpy(sp, &at, sizeof at);
        sp[oSurfIncl] = 1;
        api->process_event(comp, fSurface, sp);
        float d = 0; memcpy(&d, sp + oSurfDepth, 4);
        lastAnswer = d;
        static int saidDeep = -1;
        if ((d >= 80) != saidDeep) { saidDeep = d >= 80; MT_LOG("sea depth at %.0f %.0f: %.0f cm (%s)", at.x, at.y, d, saidDeep ? "swim" : "dry land under sea level: no swimming"); }
        return d >= 80;
    }
    return false;
}

// ---- breath and stamina, seconds ----
static double oxygen = -1, stamina = -1, lastT;
static bool blackedOut;
static int warned;  // 0 none, 1 50%, 2 25%, 3 10%
static void beep(int hz, int n) {
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        int hz = (int)((intptr_t)p >> 8), n = (int)((intptr_t)p & 0xff);
        for (int i = 0; i < n; i++) { Beep(hz, 110); Sleep(70); }
        return 0;
    }, (LPVOID)(intptr_t)((hz << 8) | n), 0, nullptr));
}
static void breathe(bool under, double dt) {
    if (oxygen < 0) oxygen = kOxygen;
    if (under && !blackedOut) {
        oxygen = clampd(oxygen - dt, 0, kOxygen);
        double share = oxygen / kOxygen;
        int lvl = share <= 0.1 ? 3 : share <= 0.25 ? 2 : share <= 0.5 ? 1 : 0;
        if (lvl > warned) { warned = lvl; beep(lvl == 3 ? 400 : 700, lvl); }
        if (oxygen <= 0) { blackedOut = true; beep(250, 4); MT_LOG("out of air: back to the surface"); }
    } else {
        oxygen = clampd(oxygen + dt * kOxygen / 6, 0, kOxygen);  // a full breath back in 6 s
        if (oxygen > kOxygen * 0.5) warned = 0;
        if (blackedOut && oxygen >= kOxygen * 0.3) { blackedOut = false; MT_LOG("breath back"); }
    }
}

static void oneShot(uintptr_t ch, Anim a) {
    if (!montageAlive(a)) return;
    playing = A_None;  // force a fresh start even if the same one-shot ran last
    play(ch, a);
    oneShotEnd = nowS() + kOneShot[a];
}

static void leave(uintptr_t ch, uintptr_t cmc, bool jumpOut) {
    clearDeadBase(cmc);  // the game reads the base when the mode changes: a dead one is the crash of 12/13 Nov
    restoreLean(ch);  // the body stops leaning with the water
    wr(cmc + oFly, &flyWas, sizeof flyWas);
    setMode(cmc, MOVE_Falling);
    if (jumpOut) { Vec v = get<Vec>(cmc + oVel); v.z = kJump; wr(cmc + oVel, &v, sizeof v); }
    if (jumpOut && montageAlive(A_ClimbOut)) oneShot(ch, A_ClimbOut);  // plays out on land by itself
    else play(ch, A_None);
    ours = false;
    MT_LOG("%s", jumpOut ? "jumped out of the water" : "walked out of the water");
}

// Entry guard (2026-11-13: five game crashes, each 1-3 s after "swimming at" / "jumped out of the water" -- three at
// BoaterTown's garage spot over open sea, where a spawn drops the player off the vehicle it just destroyed. Switching
// the movement mode straight away left the character's floor / movement base on that destroyed component, and the
// game then made a weak pointer of it). Swimming starts only after the player has been under the water line for 0.4 s
// straight -- falling ticks re-sweep the floor first -- and not within 2 s of the pawn changing (out of a vehicle, a
// spawn) or of a jump of more than 5 m in one tick (a teleport). And no jumping out in the first second of swimming.
static uintptr_t lastPawn, lastCmc;
static double pawnChangedAt = -1e9, candidateSince = 0, swimSince = 0;
static Vec lastAt{};

// Give the character its movement back: the mode and the fly speed we took over.
static void restore(uintptr_t cmc) {
    if (!cmc || !alive(cmc) || oFly < 0 || oMode < 0) return;
    wr(cmc + oFly, &flyWas, sizeof flyWas);
    if (get<uint8_t>(cmc + oMode) == MOVE_Flying) setMode(cmc, MOVE_Falling);
}

static void tick(void*) {
    InterlockedExchange(&queued, 0);
    LONG wantJump = InterlockedExchange(&jump, 0), k = keys;
    double t = nowS(), dt = lastT > 0 ? clampd(t - lastT, 0, 0.1) : 0;
    lastT = t;
    double tA = nowS();
    struct { uintptr_t ctx; int i; uintptr_t ret; } p = {waters.empty() ? 0 : waters[0].o, 0, 0};
    if (!p.ctx || api->object(waters[0].idx) != p.ctx) { ours = false; return; }
    api->process_event(gameplayCDO, fPawn, &p);
    if (!p.ret || !api->is_a(classOf(p.ret), cChar)) {
        // In a vehicle, or the world went away: the character must not stay in flying mode with our fly speed.
        if (ours) { restore(lastCmc); restoreLean(lastPawn); MT_LOG("swim: the pawn changed while swimming -- the movement is given back"); }
        ours = false; playing = A_None; return;
    }
    uintptr_t ch = p.ret, cmc = get<uintptr_t>(ch + oCMC);
    if (!alive(ch) || !cmc || !alive(cmc)) return;  // the character died under us
    lastCmc = cmc;
    double tB = nowS(); slowStage("the pawn", (tB - tA) * 1000);
    Vec at{};
    api->process_event(ch, fLoc, &at);
    {
        double dx = at.x - lastAt.x, dy = at.y - lastAt.y, dz = at.z - lastAt.z;
        // A fall into the water is not a teleport (2026-11-14: the player fell 5 m into the sea before the swim took
        // over, because the hold below waited for a "pawn change" that the fall itself caused). Only a new pawn or a
        // jump sideways counts now.
        if (ch != lastPawn || dx * dx + dy * dy > 500.0 * 500.0) { pawnChangedAt = t; candidateSince = 0; }
        lastPawn = ch; lastAt = at;
    }
    double tC = nowS(); slowStage("the location", (tC - tB) * 1000);
    Surface s = surfaceAt(at);
    double tD = nowS(); slowStage("the water surface", (tD - tC) * 1000);
    if (!s.ok) { if (ours) leave(ch, cmc, false); breathe(false, dt); return; }
    if (s.sea && !sea::list.empty()) sea::update(ch);
    // The one height model (2026-11-14): the game's flat surface for the body plus the game's own waves. The boats
    // float on the same one, so a swimmer and a boat stand at the same height in the same water.
    // the sea's level: the drawn one when we know it (the query answers the collision surface, which is higher)
    double baseZ = s.z;
    if (s.sea && sea::base != 0 && fabs(sea::base - s.z) < 500.0) {
        baseZ = sea::base;
        if (!saidBase) { saidBase = true; MT_LOG("sea: the drawn water is %.0f cm below the collision surface", s.z - sea::base); }
    }
    double surf = s.sea ? (sea::list.empty() ? baseZ + swell(at.x, at.y, t) : sea::surface(at.x, at.y, baseZ)) : s.z;
    double depth = s.sea ? floatDepth : kFresh;
    double line = surf - depth;
    uint8_t mode = get<uint8_t>(cmc + oMode);
    if (!ours) {
        breathe(false, dt);
        bool candidate = (mode == MOVE_Walking || mode == MOVE_Falling) && at.z < line - 10 && (!s.sea || seaHere(at));
        if (!candidate) { candidateSince = 0; return; }
        if (candidateSince == 0) candidateSince = t;
        // 0.15 s for the game to find its floor. After a pawn change (out of a vehicle, a spawn) it waits 0.5 s -- but
        // only while the character is on its feet: a fall into the water is the one moment we want to take over at once
        // (2026-11-14: the swimmer fell a metre and a half into the sea before the swim took over)
        if (t - candidateSince < 0.15 || (mode == MOVE_Walking && t - pawnChangedAt < 0.5)) return;
        // The character can still stand on a vehicle the game destroyed (a spawn at BoaterTown drops the player off the
        // vehicle it just destroyed). The game reads that base when the movement mode changes -- the crash of 12/13 Nov.
        // Clear it, and let the game re-sweep the floor in a frame of falling before we fly.
        clearDeadBase(cmc);
        if (mode != MOVE_Falling) {
            setMode(cmc, MOVE_Falling);
                    return;
        }
        {
            flyWas = get<float>(cmc + oFly);
            swimSince = t;
            ours = true;
            if (!montagesTried) loadMontages();
            MT_LOG("swimming at %.0f %.0f (%s, surface %.0f, base %s)", at.x, at.y, s.sea ? "sea" : s.river ? "river" : "lake", surf,
                   oMoveBase >= 0 && get<uintptr_t>(cmc + oMoveBase) ? (alive(get<uintptr_t>(cmc + oMoveBase)) ? "alive" : "destroyed") : "none");
            setMode(cmc, MOVE_Flying);
            // 2026-11-12, "the swimming animation doesn't instantly activate": no dive-in one-shot here -- it starts from
            // standing, and by the time we see the water the player is already in it. The loop starts this frame.
            oneShotEnd = 0;
            playing = A_None;
            {
                uintptr_t mesh = oCharMesh >= 0 ? get<uintptr_t>(ch + oCharMesh) : 0;
                {
                    std::string mine = meshSkeleton(mesh), theirs = skeletonOf(montage[A_Tread]);
                    if (!mine.empty() && !theirs.empty() && mine != theirs)
                        MT_LOG("swim: the montage is on the skeleton %s but this character has %s -- the animation will not show",
                               theirs.c_str(), mine.c_str());
                    else if (!mine.empty())
                        MT_LOG("swim: the character and the montage are on the same skeleton (%s)", mine.c_str());
                }
                readRot(mesh, oMeshRot, meshBase);
                tiltPitch = tiltRoll = 0;
                floatDepth = floatDepthFor(ch, at.z);
                if (oFallDist >= 0) { float zero = 0; wr(ch + oFallDist, &zero, 4); }  // no fall damage for the dive
            }
            play(ch, at.z > line - 30 ? A_Tread : A_Dive);
        }
        return;
    }
    if (mode != MOVE_Flying) {  // the game took the movement back
        ours = false;
        play(ch, A_None);
        wr(cmc + oFly, &flyWas, sizeof flyWas);
        restoreLean(ch);
        return;
    }
    clearDeadBase(cmc);  // the vehicle under the player can be destroyed while they swim
    bool atTop = at.z > line - 30, under = at.z + 70 < surf;  // head ~70 cm over the capsule centre
    breathe(under, dt);
    // the height model in numbers, once a second: the waves have to move (2026-11-14: they stood still)
    static double lastHeightLog;
    if (t - lastHeightLog > 1.0) {
        lastHeightLog = t;
        MT_LOG("swim: the water here is %.0f (the game's %.0f + the waves %+.0f, clock %s %.1f), the float line %.0f, the swimmer at %.0f",
               surf, s.z, surf - s.z, sea::clock, sea::t, line, at.z);
    }
    if (stamina < 0) stamina = kStamina;
    // lean with the water
    if (s.sea && !sea::list.empty() && rotOk && kLean > 0) {
        uintptr_t mesh = oCharMesh >= 0 ? get<uintptr_t>(ch + oCharMesh) : 0;
        uintptr_t cap = oCap >= 0 ? get<uintptr_t>(ch + oCap) : 0;
        if (alive(mesh)) {
            double gx, gy;
            sea::slope(at.x, at.y, gx, gy);
            double cr[3];
            readRot(cap, oCapRot, cr);
            double yaw = cr[1] * 3.14159265358979 / 180.0, c = cos(yaw), s = sin(yaw);
            double aF = gx * c + gy * s;   // the slope along the way the character faces
            double aR = -gx * s + gy * c;  // and to the right
            double p = -atan(aF) * 57.29577951308232, r = atan(aR) * 57.29577951308232;
            if (fabs(p) > kLean) p = p > 0 ? kLean : -kLean;
            if (fabs(r) > kLean) r = r > 0 ? kLean : -kLean;
            double k = dt * 6 > 1 ? 1 : dt * 6;
            tiltPitch += (p - tiltPitch) * k;
            tiltRoll += (r - tiltRoll) * k;
            double v[3] = {meshBase[0] + tiltPitch, meshBase[1], meshBase[2] + tiltRoll};
            writeRot(mesh, oMeshRot, v);
        }
    }
    bool sprint = (k & 4) && stamina > 0;
    stamina = clampd(stamina + (sprint ? -dt : dt * 0.5), 0, kStamina);
    float fly = (float)(kSpeed * (sprint ? kSprint : 1));
    wr(cmc + oFly, &fly, sizeof fly);
    if (at.z > line + 40) { leave(ch, cmc, false); return; }  // a beach or ramp pushed us up out of the water
    if (wantJump && atTop && !blackedOut && t - swimSince > 1) { leave(ch, cmc, true); return; }  // not in the first second: the in-and-out flicker
    double ride = (s.sea && !sea::list.empty()) ? sea::rate(at.x, at.y) : 0;  // the water's own motion here
    Step st = vertical(at.z, line, (k & 1) != 0, (k & 2) != 0, blackedOut, !s.sea, ride);
    Vec v = get<Vec>(cmc + oVel);
    v.z = st.vz;
    if (s.river) { v.x += s.flow.x * dt * 2; v.y += s.flow.y * dt * 2; }  // the current carries you (eased, no snap)
    wr(cmc + oVel, &v, sizeof v);
    double flat = sqrt(v.x * v.x + v.y * v.y);
    double tE = nowS(); slowStage("the swimming itself", (tE - tD) * 1000);
    if (t < oneShotEnd) return;  // a dive-in / surfacing still running
    if (atTop && playing == A_Dive) { oneShot(ch, A_Surface); return; }
    play(ch, !atTop ? A_Dive : flat > 60 ? A_Crawl : A_Tread);
}

static bool gameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Every class, function and field swimming needs; false while any is missing. Asked again for up to 2 minutes: the
// Water module's classes are not always there the moment the engine is (2026-11-13 17:18: ready 3 s after start,
// "functions or fields not found", no swimming that session).
static bool resolveAll() {
    cChar = api->find_object("Class", "Character", "/Script/Engine");
    uintptr_t cCMC = api->find_object("Class", "CharacterMovementComponent", "/Script/Engine");
    uintptr_t cMove = api->find_object("Class", "MovementComponent", "/Script/Engine");
    cBody = api->find_object("Class", "WaterBody", "/Script/Water");
    cOcean = api->find_object("Class", "WaterBodyOcean", "/Script/Water");
    cRiver = api->find_object("Class", "WaterBodyRiver", "/Script/Water");
    fPawn = api->find_object("Function", "GetPlayerPawn", "GameplayStatics");
    gameplayCDO = api->find_object("GameplayStatics", "Default__GameplayStatics", "/Script/Engine");
    systemCDO = api->find_object("KismetSystemLibrary", "Default__KismetSystemLibrary", "/Script/Engine");
    fLoc = api->find_object("Function", "K2_GetActorLocation", "Actor");
    fBounds = api->find_object("Function", "GetActorBounds", "Actor");
    fBodyComp = api->find_object("Function", "GetWaterBodyComponent", "WaterBody");
    fSurface = api->find_object("Function", "GetWaterSurfaceInfoAtLocation", "WaterBodyComponent");
    fSetMode = api->find_object("Function", "SetMovementMode", "CharacterMovementComponent");
    // SetBase is declared on the movement component (MovementComponent / PawnMovementComponent); the character one
    // has no own copy, so the lookup failed there and a destroyed base could not be cleared (2026-11-14)
    fSetBase = api->find_object("Function", "SetBase", "CharacterMovementComponent");
    if (!fSetBase) fSetBase = api->find_object("Function", "SetBase", "PawnMovementComponent");
    if (!fSetBase) fSetBase = api->find_object("Function", "SetBase", "MovementComponent");
    fAnimInst = api->find_object("Function", "GetAnimInstance", "SkeletalMeshComponent");
    fPlay = api->find_object("Function", "Montage_Play", "AnimInstance");
    fStop = api->find_object("Function", "Montage_Stop", "AnimInstance");
    fIsPlaying = api->find_object("Function", "Montage_IsPlaying", "AnimInstance");
    fPath = api->find_object("Function", "MakeSoftObjectPath", "KismetSystemLibrary");
    fRef = api->find_object("Function", "Conv_SoftObjPathToSoftObjRef", "KismetSystemLibrary");
    fLoad = api->find_object("Function", "LoadAsset_Blocking", "KismetSystemLibrary");
    if (cChar) { oCMC = off(cChar, "CharacterMovement"); oCharMesh = off(cChar, "Mesh"); oFallDist = off(cChar, "CurrentFallDistance"); oCap = off(cChar, "CapsuleComponent"); }
    if (fBounds) { oBOrg = off(fBounds, "Origin"); oBExt = off(fBounds, "BoxExtent"); }
    {
        uintptr_t cMesh = api->find_object("Class", "SkeletalMeshComponent", "/Script/Engine");
        uintptr_t cCap = api->find_object("Class", "CapsuleComponent", "/Script/Engine");
        if (cMesh) oMeshRot = off(cMesh, "RelativeRotation");
        if (cCap) oCapRot = off(cCap, "RelativeRotation");
    }
    if (fPlay) oPlayRet = off(fPlay, "ReturnValue");
    if (cCMC) { oMode = off(cCMC, "MovementMode"); oFly = off(cCMC, "MaxFlySpeed"); oMoveBase = off(cCMC, "MovementBase"); }
    if (cMove) oVel = off(cMove, "Velocity");
    if (fSurface) {
        oSurfOut = off(fSurface, "OutWaterSurfaceLocation"); oSurfIncl = off(fSurface, "bIncludeDepth");
        oSurfVel = off(fSurface, "OutWaterVelocity"); oSurfDepth = off(fSurface, "OutWaterDepth");
    }
    return cChar && cCMC && cMove && cBody && fPawn && gameplayCDO && fLoc && fBodyComp && fSurface && fSetMode &&
           oCMC >= 0 && oMode >= 0 && oFly >= 0 && oVel >= 0 && oSurfOut >= 0;
}

static DWORD WINAPI run(LPVOID) {
    if (!api->wait_engine(INFINITE)) return 0;
    int tries = 0;
    while (!resolveAll()) {
        if (++tries >= 120) { MT_LOG("FAILED: functions or fields not found after 2 minutes -- no swimming"); return 0; }
        Sleep(1000);
    }
    if (tries) MT_LOG("swim: everything found after %d s", tries);
    if (!fPlay || !fStop || !fAnimInst || oCharMesh < 0 || !systemCDO) MT_LOG("swim animations off: anim functions not found");
    if (!fSetBase || oMoveBase < 0)
        MT_LOG("swim: %s not found -- a destroyed movement base cannot be cleared",
               !fSetBase ? "SetBase" : "MovementBase");
    sea::declareSea();  // sea.ini: the wave set the mod wants (empty = the game's own)
    MT_LOG("Swim ready: float %.0f cm under salt water, %.0f fresh; Ctrl dives, Space up/out, Shift faster", kDepth, kFresh);
    bool spaceWas = false;
    for (int beat = 0;; beat++) {
        if (beat % (haveWater ? 1800 : 300) == 0 && api->game_thread_ready()) {  // 30 s (5 s until a world has water): levels stream
            scanWaters();
            api->run_on_game_thread([](void*) { findWaters(); }, nullptr);
        }
        bool f = gameFocused(), space = f && (GetAsyncKeyState(VK_SPACE) & 0x8000);
        LONG k = (f && (GetAsyncKeyState(kDiveKey) & 0x8000) ? 1 : 0) | (space ? 2 : 0) | (f && (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 4 : 0);
        InterlockedExchange(&keys, k);
        if (space && !spaceWas) InterlockedExchange(&jump, 1);
        spaceWas = space;
        if (haveWater && api->game_thread_ready() && !InterlockedCompareExchange(&queued, 1, 0)) api->run_on_game_thread(tick, nullptr);
        Sleep(16);
    }
}

static void demo() {
    auto closeTo = [](double a, double b) { return fabs(a - b) < 0.001; };  // "near" is a Windows keyword
    double d = kDive, r = kRise, dr = kDrift;
    kDive = 220; kRise = 260; kDrift = 60;
    bool ok = holdSpeed(-100, 0) == 400 && holdSpeed(0, 0) == 0 && holdSpeed(10, 0) == -40;
    ok &= closeTo(vertical(0, 0, true, false, false, false, 0).vz, -220);     // Ctrl dives, even from the surface
    ok &= closeTo(vertical(-500, 0, false, true, false, false, 0).vz, 260);   // Space swims up
    ok &= closeTo(vertical(-500, 0, false, false, false, true, 0).vz, 40);    // fresh water lifts less
    ok &= closeTo(vertical(-500, 0, true, false, true, false, 0).vz, 400);    // blacked out: up, whatever the keys
    ok &= closeTo(vertical(-10, 0, false, false, false, false, 0).vz, 40);    // at the surface: held to the line
    ok &= closeTo(vertical(-500, 0, false, false, false, false, 30).vz, 70);  // the water's own motion is ridden
    ok &= swell(0, 0, 0) == 0 || kSwell > 0;
    ok &= !alive(0);  // nothing is alive that is not there
    kDive = d; kRise = r; kDrift = dr;
    if (!ok) MT_LOG("SELF-CHECK FAILED: swim vertical");
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (iniDisabled(INI)) return 0;
    demo();
    kDepth = iniNum(INI, L"Swim", L"FloatDepth", 60);
    kFresh = iniNum(INI, L"Swim", L"FreshFloatDepth", 72);
    kSpeed = iniNum(INI, L"Swim", L"Speed", 300);
    kSprint = iniNum(INI, L"Swim", L"SprintScale", 1.6);
    kStamina = iniNum(INI, L"Swim", L"Stamina", 20);
    kJump = iniNum(INI, L"Swim", L"JumpOut", 900);
    kDive = iniNum(INI, L"Swim", L"DiveSpeed", 220);
    kRise = iniNum(INI, L"Swim", L"RiseSpeed", 260);
    kDrift = iniNum(INI, L"Swim", L"Drift", 60);
    kOxygen = iniNum(INI, L"Swim", L"Oxygen", 60);
    kSwell = iniNum(INI, L"Swim", L"Swell", 90);
    // Where the water sits on the body, as a fraction of the body's height from the feet. Breath of the Wild floats at
    // 78% (its water_effective_height 1.4 of a 1.8 body), Skyrim at 90% (fSwimHeightScale 0.9). 0.78 is the chest.
    kWaterLine = iniNum(INI, L"Swim", L"WaterLine", 0.78);
    // The lean with the wave, in degrees. The games heave without tilting (Valheim zeroes the angular velocity,
    // BotW stiffens pitch and roll), so this is off unless it is asked for.
    kLean = iniNum(INI, L"Swim", L"Lean", 0);
    kDiveKey = (int)iniNum(INI, L"Swim", L"DiveKey", VK_CONTROL);
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
