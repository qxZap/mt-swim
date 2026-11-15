// The game's own sea waves, for plugins that float things (Boats, Swim).
//
// 2026-11-12, the user: "it's not really following the waves fully. That applies to the boats as well". The sea you see
// is the ocean WaterBody's Gerstner waves (Env/GerstnerWaves_Ocean); the game's height query without depth leaves them
// out (Daza answered a flat -22374), so the plugins used to add a swell of their own that matched nothing on screen.
// This reads the live wave list off the ocean (WaterBody.WaterWaves -> WaterWavesAssetReference.WaterWavesAsset ->
// WaterWavesAsset.WaterWaves -> GerstnerWaterWaves.GerstnerWaves) and sums them the way the water material does:
//   z = sum A cos(k . xy - w t + phase), with t the clock the water itself runs on.
// The game's own set (Env/GerstnerWaves_Ocean, read out of the asset): 6 waves, 30 m down to 3.3 m long, amplitudes
// 20 .. 4.4 cm -- the sea breathes about +-32 cm, a long swell and not a chop.
// shortcut: no shore attenuation and no horizontal Gerstner shift (UE's "simple" height): off by a little near
// beaches and on steep crests. Use the iterative solve if a hull ever visibly clips a crest.
//
// 2026-11-14, the user: "the height of the player doesn't seem necessarily to be affected exactly by the details of the
// waters themselves ... we have to pretty much find like a height map or something that is replicable for the code so
// that we understand exactly where players stand in terms of like the height in the water". So one height model, shared
// by the swimmer and the boats: sea::surface(). It is the game's flat surface plus the game's own waves -- the same
// numbers for both, so a swimmer and a boat stand at the same height and a small boat rides the same sea as a big one.
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include "../include/mt_plugin.hpp"

namespace sea {
using namespace mt;

struct Wave { double kx, ky, w, a, ph; };
inline std::vector<Wave> list;  // game thread
inline uintptr_t fTime, fWaterTime, gameplay, waterSub, cSub;
inline double t = -1;           // the clock the waves run on, in seconds
inline const char* clock = "none";  // which clock it is: "water" (the water's own) or "world"

// The value a time function returns, whatever its type: read as a double first and as a float second, and take the one
// that looks like a time. 2026-11-14: the world clock was read as a double; a float return made the wave sum garbage,
// the range check threw it away, the clock stood still -- and the waves with it, so nothing ever rode them.
inline double readTime(uintptr_t fn, uintptr_t obj, uintptr_t ctx) {
    int at = off(fn, "ReturnValue");
    if (at < 0) return -1;
    unsigned char p[0x40] = {0};
    if (ctx) memcpy(p, &ctx, sizeof ctx);
    api->process_event(obj, fn, p);
    float f = 0;
    double d = 0;
    memcpy(&f, p + at, 4);
    memcpy(&d, p + at, 8);
    if (d > 1.0 && d < 1e8) return d;
    if (f > 1.0 && f < 1e8) return f;
    return -1;
}

// The water's own clock. The waves are drawn with it ("GetWaterTime: sync Time across various places in Water"), so
// that is the clock the height model has to use; the world's GetTimeSeconds is the fallback.
// 2026-11-15: this walked every object in the game -- and then every object again, because the class was not loaded
// yet -- on the GAME thread, every frame. That froze the world (the clock never moved past a second, so the waves
// stood still) and the game piled up memory while it was frozen. It is looked up once now, on the plugin's thread.
inline uintptr_t waterSubsystem() {
    static bool tried;
    if (tried) return waterSub;
    tried = true;
    cSub = api->find_object("Class", "WaterSubsystem", "/Script/Water");
    if (!cSub) return 0;
    for (int i = 0, n = api->num_objects(); i < n; i++) {
        uintptr_t o = api->object(i);
        if (o && !isCDO(o) && api->is_a(classOf(o), cSub)) { waterSub = o; return o; }
    }
    return 0;
}

// once a frame on the game thread, with any world object as context
inline void update(uintptr_t ctx) {
    if (!fTime && !fWaterTime) {
        fTime = api->find_object("Function", "GetTimeSeconds", "GameplayStatics");
        gameplay = api->find_object("GameplayStatics", "Default__GameplayStatics", "/Script/Engine");
        fWaterTime = api->find_object("Function", "GetWaterTimeSeconds", "WaterSubsystem");
        MT_LOG("sea: the clocks: world %s, water %s", fTime ? "found" : "missing", fWaterTime ? "found" : "missing");
    }
    double v = -1;
    const char* src = "none";
    if (fWaterTime) {
        if (!waterSub) waterSub = waterSubsystem();
        if (waterSub) { v = readTime(fWaterTime, waterSub, ctx); src = "water"; }
    }
    if (v < 0 && fTime && gameplay) { v = readTime(fTime, gameplay, ctx); src = "world"; }
    if (v <= 0) return;
    t = v;
    clock = src;
    // the two clocks side by side, once: the waves are drawn on the water's own clock, so if the two differ, that is
    // the one the height model has to use (2026-11-14)
    static bool said;
    if (!said && fWaterTime && waterSub && fTime && gameplay) {
        double g = readTime(fTime, gameplay, ctx);
        if (g > 0) {
            said = true;
            MT_LOG("sea: the water's clock %.1f, the world's %.1f (%.1f apart) -- the waves run on the %s clock", v, g, v - g, src);
        }
    }
}

// The sea the mod wants (2026-11-14, the user: "change the game's waves to fit the logic of our mod" -- and only if
// that is what it takes). Written into the game's own wave object, so the sea on screen and the height the plugins
// float on are the same numbers by construction. sea.ini [Sea]:
//   Waves = "length cm:height cm, ..."   empty = the game's own set (6 waves, +-32 cm)
//   Steepness = 0                          empty = the game's own; 0 = no sideways push, so the height at a point is
//                                          exactly what the model computes
inline uintptr_t cWaterBody, cWaveRef, cWaveAsset, cWaveGer, sWave, fCompLoc;
inline bool classesTried;

// Look the water classes up once, on the plugin's own thread (2026-11-15: a lookup walks every object in the game --
// two seconds of it -- and doing that on the game thread froze the world: the waves stood still, the world clock
// never moved past a second, and the game's memory piled up. Nothing here may run on the game thread.)
inline void resolveClasses() {
    if (classesTried) return;
    cWaterBody = api->find_object("Class", "WaterBody", "/Script/Water");
    cWaveRef = api->find_object("Class", "WaterWavesAssetReference", "/Script/Water");
    cWaveAsset = api->find_object("Class", "WaterWavesAsset", "/Script/Water");
    cWaveGer = api->find_object("Class", "GerstnerWaterWaves", "/Script/Water");
    sWave = api->find_object("ScriptStruct", "GerstnerWave", "/Script/Water");
    fCompLoc = api->find_object("Function", "GetComponentLocation", "SceneComponent");
    classesTried = cWaterBody && cWaveRef && cWaveAsset && cWaveGer && sWave;
}

inline uintptr_t waveObj;  // the game's GerstnerWaterWaves object
inline int oArr = -1, oVec = -1, oSpd = -1, oAmp = -1, oPh = -1, oLen = -1, oSteep = -1, oDir = -1, oWKA = -1, oQ = -1, stride = 0;

inline void declareSea() {
    static bool done;
    if (done) return;
    done = true;
    iniSet(L"sea.ini", L"Sea", L"Waves", L"", false);
    iniSet(L"sea.ini", L"Sea", L"Steepness", L"", false);
}

// write our own wave set into the game's wave object
inline void applyProfile() {
    if (!waveObj || oArr < 0 || oLen < 0) return;
    std::wstring p = iniGet(L"sea.ini", L"Sea", L"Waves");
    if (p.empty()) return;
    std::vector<std::pair<double, double>> want;  // length cm, height cm
    for (size_t i = 0; i < p.size() && want.size() < 64;) {
        while (i < p.size() && (p[i] == L' ' || p[i] == L',')) i++;
        size_t st = i;
        while (i < p.size() && p[i] >= L'0' && p[i] <= L'9') i++;
        if (i == st) break;
        double len = _wtof(p.substr(st, i - st).c_str());
        double amp = 0;
        if (i < p.size() && p[i] == L':') {
            i++;
            st = i;
            while (i < p.size() && (p[i] == L'.' || (p[i] >= L'0' && p[i] <= L'9'))) i++;
            amp = _wtof(p.substr(st, i - st).c_str());
        }
        want.push_back({len, amp});
        while (i < p.size() && p[i] != L',') i++;
    }
    if (want.empty()) return;
    uintptr_t data = get<uintptr_t>(waveObj + oArr);
    int n = get<int>(waveObj + oArr + 8);
    if (!data || n <= 0) return;
    if ((int)want.size() > n) want.resize(n);
    std::wstring steepIn = iniGet(L"sea.ini", L"Sea", L"Steepness");
    for (size_t i = 0; i < want.size(); i++) {
        uintptr_t e = data + (uintptr_t)stride * i;
        double len = want[i].first, amp = want[i].second;
        if (len <= 0) continue;
        double dx = 1, dy = 0;
        if (oDir >= 0) { dx = get<double>(e + oDir); dy = get<double>(e + oDir + 8); }
        double k = 6.283185307 / len;
        double steep = oSteep >= 0 ? get<float>(e + oSteep) : 0.25;
        if (!steepIn.empty()) steep = _wtof(steepIn.c_str());
        float f = (float)len; wr(e + oLen, &f, 4);
        f = (float)amp; wr(e + oAmp, &f, 4);
        if (oSteep >= 0) { f = (float)steep; wr(e + oSteep, &f, 4); }
        if (oDir >= 0) { double d3[3] = {dx, dy, 0}; wr(e + oDir, d3, 24); }
        double kv[2] = {k * dx, k * dy};
        wr(e + oVec, kv, 16);
        f = (float)std::sqrt(980.0 * k); wr(e + oSpd, &f, 4);
        if (oWKA >= 0) { f = (float)(k * amp); wr(e + oWKA, &f, 4); }
        if (oQ >= 0) { f = (float)(steep / k); wr(e + oQ, &f, 4); }
        f = 0; wr(e + oPh, &f, 4);
    }
    MT_LOG("sea: the waves are now ours: %d of them, %s", (int)want.size(), steepIn.empty() ? "the game's steepness" : "our steepness");
}

// The sea's own level -- the water that is drawn.
//
// The water body's query answers a flat -22374, but that is the *collision* surface: UE puts the ocean mesh a little
// lower so the crests do not poke through it (the wave height offset). The player sees the mesh, so the swimmer stood
// on the collision surface and looked like it was floating on top of the water (2026-11-14). The mesh's own level is
// the one to float on.
inline double base = 0;          // the drawn sea's level; 0 = not known yet

inline bool compLocation(uintptr_t comp, double* v) {
    if (!fCompLoc || !comp) return false;
    api->process_event(comp, fCompLoc, v);
    return true;
}

// The ocean's mesh components, found on the plugin's own thread (the walk over every object is too slow for the
// game's). Handed over as object indices.
inline std::mutex meshLock;
inline std::vector<int> foundMeshes;  // object indices, handed over from the plugin's thread

// Find them: a component of the ocean (or of its zone) whose class is a water mesh.
inline void scanSeaMeshes() {  // plugin thread
    std::vector<int> m;
    resolveClasses();
    uintptr_t c1 = api->find_object("Class", "WaterBodyMeshComponent", "/Script/Water");
    uintptr_t c2 = api->find_object("Class", "WaterMeshComponent", "/Script/Water");
    if (!c1 && !c2) return;
    for (int i = 0, n = api->num_objects(); i < n; i++) {
        uintptr_t o = api->object(i);
        if (!o) continue;
        uintptr_t c = classOf(o);
        if (!((c1 && api->is_a(c, c1)) || (c2 && api->is_a(c, c2)))) continue;
        m.push_back(i);
    }
    std::lock_guard<std::mutex> g(meshLock);
    foundMeshes.swap(m);
}

// read the wave list off an ocean body; true when it has one
inline bool load(uintptr_t ocean) {
    list.clear();
    // The classes are found on the plugin's thread (resolveClasses). This runs on the game thread, where a lookup
    // would walk every object in the game and freeze it.
    uintptr_t cBody = cWaterBody, cRef = cWaveRef, cAsset = cWaveAsset, cGer = cWaveGer;
    if (!ocean || !cBody || !cRef || !cAsset || !cGer || !sWave) return false;
    uintptr_t w = get<uintptr_t>(ocean + off(cBody, "WaterWaves"));
    if (w && api->is_a(classOf(w), cRef)) w = get<uintptr_t>(w + off(cRef, "WaterWavesAsset"));
    if (w && api->is_a(classOf(w), cAsset)) w = get<uintptr_t>(w + off(cAsset, "WaterWaves"));
    if (!w || !api->is_a(classOf(w), cGer)) return false;
    oArr = off(cGer, "GerstnerWaves"); oVec = off(sWave, "WaveVector"); oSpd = off(sWave, "WaveSpeed");
    oAmp = off(sWave, "Amplitude"); oPh = off(sWave, "PhaseOffset");
    if (oArr < 0 || oVec < 0 || oSpd < 0 || oAmp < 0 || oPh < 0) return false;
    stride = (oPh + 4 + 7) & ~7;  // PhaseOffset is FGerstnerWave's last field; the struct aligns to 8 (doubles)
    // the fields the mod's own wave set needs (the lengths and the directions are there too)
    oLen = off(sWave, "WaveLength"); oSteep = off(sWave, "Steepness"); oDir = off(sWave, "Direction");
    oWKA = off(sWave, "WKA"); oQ = off(sWave, "Q");
    waveObj = w;
    uintptr_t data = get<uintptr_t>(w + oArr);
    int n = get<int>(w + oArr + 8);
    for (int i = 0; data && i < n && i < 64; i++) {
        uintptr_t e = data + (uintptr_t)stride * i;
        list.push_back({get<double>(e + oVec), get<double>(e + oVec + 8), get<float>(e + oSpd), get<float>(e + oAmp), get<float>(e + oPh)});
    }
    applyProfile();  // the mod's own sea, if the user asked for one
    if (!list.empty() && iniGet(L"sea.ini", L"Sea", L"Waves").size()) {
        list.clear();
        for (int i = 0; data && i < n && i < 64; i++) {
            uintptr_t e = data + (uintptr_t)stride * i;
            list.push_back({get<double>(e + oVec), get<double>(e + oVec + 8), get<float>(e + oSpd), get<float>(e + oAmp), get<float>(e + oPh)});
        }
    }
    return !list.empty();
}

// the wave list in words: what the code thinks the sea is
inline void logList() {
    double sum = 0;
    for (size_t i = 0; i < list.size(); i++) {
        const Wave& v = list[i];
        double k = std::sqrt(v.kx * v.kx + v.ky * v.ky);
        MT_LOG("sea: wave %d: %.1f m long, %.1f cm high, %.3f rad/s, from %.0f deg", (int)i + 1,
               k > 0 ? 6.283185307 / k / 100 : 0, v.a, v.w, std::atan2(v.ky, v.kx) * 57.29577951);
        sum += v.a;
    }
    MT_LOG("sea: %d waves, the sea breathes about +-%.0f cm", (int)list.size(), sum);
}

// the wave height at a point, cm over the sea's flat level
inline double height(double x, double y) {
    double z = 0;
    for (auto& v : list) z += v.a * std::cos(v.kx * x + v.ky * y - v.w * t + v.ph);
    return z;
}
inline double rate(double x, double y) {  // d height / dt, cm/s
    double r = 0;
    for (auto& v : list) r += v.a * v.w * std::sin(v.kx * x + v.ky * y - v.w * t + v.ph);
    return r;
}

// The water's height at a point: the game's own surface for that body (baseZ, flat) plus the game's own waves.
// The one height model: the swimmer floats on it and the boats float on it, so they agree.
inline double surface(double x, double y, double baseZ) { return baseZ + height(x, y); }

// How the water leans at a point: the slope of the height (d height / d x, d height / d y, cm per cm). A hull leans
// with it, and so does a swimmer (2026-11-14: "it floats ok, but is not synced with the waves" -- the body went up
// and down but never leaned, so it did not read as part of the water).
inline void slope(double x, double y, double& dx, double& dy) {
    dx = dy = 0;
    for (auto& v : list) {
        double s = std::sin(v.kx * x + v.ky * y - v.w * t + v.ph);
        dx -= v.a * v.kx * s;
        dy -= v.a * v.ky * s;
    }
}
}  // namespace sea
