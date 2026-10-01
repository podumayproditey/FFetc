// libmod.so - авто-ремонт сломавшегося двигателя, Firefight 13.0.2 (arm64)
// тестировалось. Хуки: ShadowHook (Bytedance).
#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <unordered_map>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "shadowhook.h"

#define TAG "libmod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ---- настройки ----
static const float REPAIR_SECONDS = 45.0f;   // время ремонта
static const float REPAIR_SECONDS_HIT = 90.0f; // ремонт после попадания в двигатель (причина 8)
static const bool  REPAIR_ENGINE_HIT = true;
// ---- траки (своя механика: в игре её нет) ----
// ---- взрыв баков: пробитие двигателя с борта ----
static const bool  FUEL_ENABLED = true;
static const float FUEL_CHANCE = 0.35f;         // шанс взрыва баков при пробитии двигателя с борта
static const float FUEL_DELAY_MIN = 2.0f;       // задержка до взрыва, секунды
static const float FUEL_DELAY_MAX = 5.0f;
static const int   FUEL_FACE_RIGHT = 3;  
static const int   FUEL_FACE_LEFT = 2;// 2 = борт (по калибровке)
static const bool  TRACKS_ENABLED = true;
static const float TRACK_HIT_CHANCE = 0.60f;    // шанс сбить трак при подходящем непробитом попадании
static const float TRACK_REPAIR_SECONDS = 60.0f;
static const int   TRACK_HL   = 1;              // какое значение HighOrLow считать «низом»; -1 = любое (калибровка)
static const int   TRACK_FACE_RIGHT = 3;              // какое значение Face считать «бортом»; -1 = любое (калибровка)
static const int   TRACK_FACE_LEFT = 2;
static const bool  DEBUG_HITS = true;           // радио: параметры каждого непробитого попадания (для калибровки)
static const bool  DEBUG_RADIO = true;      // ОТЛАДКА: выводить состояние техники в игровое радио
static const bool  REPAIR_NOTIFY = true;     // радио-сообщение «vehicle repaired»
static const float GRACE_SECONDS  = 20.0f;   // иммунитет к новой поломке после ремонта
static const bool  TEST_ALL_BROKEN = false;  // ТЕСТ: каждая новая техника ломается при первом тике
static const bool  LOG_STATE_CHANGES = true; // лог смены isImmobilized/disabledBecause (найти другие виды поломок)

// ---- экспортированные функции libmain.so (Itanium mangling) ----
// Piece/Vehicle: this-указатель в x0, аргументы по ABI AArch64.
using GetStatus_t   = unsigned char (*)(void* piece);
using IsImmob_t     = unsigned char (*)(void* piece);
using SetImmob_t    = void (*)(void* piece, unsigned char v);
using GetDisabled_t = unsigned char (*)(void* vehicle);
using SetDisabled_t = void (*)(void* vehicle, unsigned char v);
using SetBrew_t     = void (*)(void* vehicle, float t);
struct POSITION { float x, y; };   // по дизассемблеру Piece::getPosition: 2 float (передаётся в s0,s1)
using GetPos_t      = POSITION (*)(void* piece);
using GetU8_t       = unsigned char (*)(void* piece);
using Radio_t       = void (*)(void* game, const char* msg, unsigned char urgency, unsigned char forSide,
                               POSITION pos, unsigned char side, unsigned char squad, unsigned char piece);
using HasBecome_t   = void (*)(void* vehicle, unsigned char why);
using GetPtrEngine_t= void* (*)(void* vehicle);
using SteerType_t   = unsigned char (*)(void* engine);
using NotPen_t      = void (*)(void* game, void* piece, bool isTurret, int structure, int highOrLow,
                               int face, bool mantlet, void* shot, unsigned char method);
using Pen_t         = void (*)(void* game, void* v, unsigned char ks, unsigned char ki, unsigned char method,
                               void* shot, bool turret, bool superstr, bool mantlet, int face, int hl, int fob, float over);
using Destroy_t     = void (*)(void* game, void* v, unsigned char ks, unsigned char ki, unsigned char method, const char* hitOn);
using Halt_t        = void (*)(void* piece);
using Breakdown_t   = unsigned char (*)(void* vehicle, unsigned char gridType, float dt);

static GetStatus_t   getStatus;
static IsImmob_t     isImmobilized;
static SetImmob_t    setImmobilized;
static GetDisabled_t getDisabledBecause;
static SetDisabled_t setDisabledBecause;
static Halt_t        haltCurrentOption;
static Pen_t         orig_pen;
static Destroy_t     destroyVehicle;
static HasBecome_t   hasBecomeDisabledBecause;
static GetPtrEngine_t getPointerToEngine;
static SteerType_t   getSteeringType;
static NotPen_t      orig_notpen;
static GetPos_t      getPosition;
static GetU8_t       getSide, getSquadI, getI;
static Radio_t       orig_radio;      // оригинал sendRadioMessage_..._Piece
static void*         g_game;          // Game*, перехватываем из вызовов радио
static SetBrew_t     setBrewUpTimer;
static Breakdown_t   orig_breakdown;

struct State { float repairLeft = -1; float grace = 0; bool seen = false; int lastImm = -1; int lastWhy = -1;
               int fireIdx = -1; float fx = 0, fy = 0;
               float fuelTimer = -1; unsigned char fuelKS = 0, fuelKI = 0; };
static std::unordered_map<void*, State> g_state;
static std::mutex g_mu;

// Перехват радио: запоминаем Game* и пропускаем вызов дальше без изменений
static void hook_radio(void* game, const char* msg, unsigned char urgency, unsigned char forSide,
                       POSITION pos, unsigned char side, unsigned char squad, unsigned char piece) {
    g_game = game;
    orig_radio(game, msg, urgency, forSide, pos, side, squad, piece);
}

static void sendMsg(void* v, const char* text) {
    if (!g_game || !orig_radio) return;   // пока не видели ни одного радио-сообщения, молчим
    unsigned char side = getSide(v);
    orig_radio(g_game, text, 2, side, getPosition(v), side, getSquadI(v), getI(v));
}

static void notifyRepaired(void* v) {
    if (!REPAIR_NOTIFY) return;
    char buf[96];
    snprintf(buf, sizeof buf, "Squad %d: vehicle repaired", (int)getSquadI(v) + 1);
    sendMsg(v, buf);
}

// ---- огонь от попадания в двигатель ----
// Раскладка по дизассемблеру createFireAt / timerTickFiresAndSmokes (13.0.x):
//   Game+0x1218 = FIRE*, Game+0x1214 = число слотов; sizeof(FIRE)=0x1c
//   FIRE: +0 активен, +1 тип, +4 x, +8 y, +0xC таймер дыма, +0x10 размер дыма, +0x14 TTL (счёт вниз, <0 = вечный), +0x18 возраст
static const size_t G_FIRES = 0x1218, G_FIRES_N = 0x1214, FIRE_SZ = 0x1c;
static float rdf(const char* p, size_t off) { float f; memcpy(&f, p + off, 4); return f; }

static void findFire(void* v, State& s) {   // ищем только что созданный огонь возле техники
    if (!g_game) return;
    char* base = (char*)g_game;
    uint32_t n = *(uint32_t*)(base + G_FIRES_N);
    char* arr = *(char**)(base + G_FIRES);
    if (!arr || n == 0 || n > 100000) return;
    POSITION vp = getPosition(v);
    int best = -1; float bd = 1e30f;
    for (uint32_t i = 0; i < n; i++) {
        char* f = arr + i * FIRE_SZ;
        if (f[0] != 1 || rdf(f, 0x18) > 1.5f) continue;     // только свежие (возраст < 1.5 c)
        float dx = rdf(f, 4) - vp.x, dy = rdf(f, 8) - vp.y, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = (int)i; }
    }
    if (best >= 0) {
        char* f = arr + best * FIRE_SZ;
        s.fireIdx = best; s.fx = rdf(f, 4); s.fy = rdf(f, 8);
        LOGI("fire slot %d found, dist2=%f", best, bd);
    }
}

static void extinguishFire(State& s) {      // ставим TTL ~0: игра сама погасит огонь на следующем тике
    if (s.fireIdx < 0 || !g_game) return;
    char* base = (char*)g_game;
    uint32_t n = *(uint32_t*)(base + G_FIRES_N);
    char* arr = *(char**)(base + G_FIRES);
    if (arr && (uint32_t)s.fireIdx < n) {
        char* f = arr + (size_t)s.fireIdx * FIRE_SZ;
        if (f[0] == 1 && rdf(f, 4) == s.fx && rdf(f, 8) == s.fy) {   // проверка, что слот не переиспользован
            float ttl = 0.01f; memcpy(f + 0x14, &ttl, 4);
            LOGI("fire slot %d extinguished", s.fireIdx);
        }
    }
    s.fireIdx = -1;
}

// ---- траки: перехват «попал, но не пробил» ----
static uint32_t g_rng = 2463534242u;
static float rand01() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng & 0xFFFFFF) / 16777216.0f; }

static bool isTracked(void* v) {   // стиринг 0..6 = гусеничные схемы (Clutch&brake ... Twin transmission); 10 = колёса
    void* e = getPointerToEngine(v);
    return e && getSteeringType(e) <= 6;
}

static void hook_notpen(void* game, void* piece, bool isTurret, int structure, int hl, int face,
                        bool mantlet, void* shot, unsigned char method) {
    g_game = game;
    orig_notpen(game, piece, isTurret, structure, hl, face, mantlet, shot, method);

    if (DEBUG_HITS) {
        char b[96];
        snprintf(b, sizeof b, "HIT turret=%d struct=%d hl=%d face=%d mant=%d", (int)isTurret, structure, hl, face, (int)mantlet);
        sendMsg(piece, b);
    }
    if (!TRACKS_ENABLED || isTurret || mantlet || !piece) return;
    if (getStatus(piece) != 0 || isImmobilized(piece)) return;
    if (TRACK_HL >= 0 && hl != TRACK_HL) return;
    if (face != TRACK_FACE_LEFT && face != TRACK_FACE_RIGHT) return;
    if (!isTracked(piece)) return;
    if (rand01() >= TRACK_HIT_CHANCE) return;

    hasBecomeDisabledBecause(piece, 2);      // бит 2 = «трак» (в ваниле не используется)
    char msg[96];
    snprintf(msg, sizeof msg, "Squad %d: track damaged", (int)getSquadI(piece) + 1);
    sendMsg(piece, msg);
    LOGI("track hit on %p", piece);
}

// ---- взрыв баков: хук пробития ----
static void hook_pen(void* game, void* v, unsigned char ks, unsigned char ki, unsigned char method,
                     void* shot, bool turret, bool superstr, bool mantlet, int face, int hl, int fob, float over) {
    g_game = game;
    int before = (v && getStatus(v) == 0) ? (getDisabledBecause(v) & 8) : 1;
    orig_pen(game, v, ks, ki, method, shot, turret, superstr, mantlet, face, hl, fob, over);
    if (!FUEL_ENABLED || !v || before) return;
    if (getStatus(v) != 0 || !(getDisabledBecause(v) & 8)) return;   // двигатель только что подбит, техника жива
    if (turret) return;
    if (face != FUEL_FACE_LEFT && face != FUEL_FACE_RIGHT) return;
    if (rand01() >= FUEL_CHANCE) return;
    std::lock_guard<std::mutex> lk(g_mu);
    State& s = g_state[v];
    s.fuelTimer = FUEL_DELAY_MIN + rand01() * (FUEL_DELAY_MAX - FUEL_DELAY_MIN);
    s.fuelKS = ks; s.fuelKI = ki;
    LOGI("fuel fire scheduled on %p in %.1fs", v, s.fuelTimer);
}

static unsigned char hook_breakdown(void* v, unsigned char grid, float dt) {
    std::lock_guard<std::mutex> lk(g_mu);
    State& s = g_state[v];

    if (TEST_ALL_BROKEN && !s.seen && getStatus(v) == 0) {   // первый тик этой техники
        s.seen = true;
        setImmobilized(v, 1);
        setDisabledBecause(v, 1);
        haltCurrentOption(v);
        LOGI("test: broke %p", v);
    }
    s.seen = true;

    if (s.grace > 0) {                 // после ремонта не даём сразу сломаться снова
        s.grace -= dt;
        if (isImmobilized(v) == 0) return 0;   // оригинал не зовём
    }

    if (s.fuelTimer >= 0) {                       // баки горят: ждём и взрываем
        s.fuelTimer -= dt;
        if (s.fuelTimer < 0) {
            s.fuelTimer = -1;
            if (getStatus(v) == 0 && g_game && destroyVehicle) {
                destroyVehicle(g_game, v, s.fuelKS, s.fuelKI, 40 /* vehicle explosion */, "fuel tank");
                LOGI("fuel tank exploded %p", v);
            }
            g_state.erase(v);
            return 0;
        }
    }

    unsigned char r = orig_breakdown(v, grid, dt);

    if (LOG_STATE_CHANGES) {
        int imm = isImmobilized(v), why = getDisabledBecause(v);
        if (imm != s.lastImm || why != s.lastWhy) {
            LOGI("vehicle %p: immobilized=%d why=%d status=%d", v, imm, why, getStatus(v));
            if (DEBUG_RADIO && s.lastImm != -1) {   // первое чтение (начальное состояние) пропускаем
                char buf[96];
                snprintf(buf, sizeof buf, "DBG Sq%d: immob=%d why=%d status=%d",
                         (int)getSquadI(v) + 1, imm, why, (int)getStatus(v));
                sendMsg(v, buf);
            }
            s.lastImm = imm; s.lastWhy = why;
        }
    }
    bool alive  = getStatus(v) == 0;
    int why = getDisabledBecause(v);   // 1 = поломка на местности; 8 = попадание в двигатель / огонь
    // биты: 1 = поломка на местности, 2 = трак, 8 = двигатель/огонь. Ремонтируем, если других битов нет
    bool broken = isImmobilized(v) != 0 && why != 0 && (why & ~(1 | 2 | 8)) == 0
                  && (!(why & 8) || REPAIR_ENGINE_HIT);
    if (alive && broken) {
        if (s.repairLeft < 0) {
            s.repairLeft = (why & 8) ? REPAIR_SECONDS_HIT : (why & 2) ? TRACK_REPAIR_SECONDS : REPAIR_SECONDS;
            if (why & 8) findFire(v, s);        // запоминаем огонь от попадания
        }
        s.repairLeft -= dt;
        // TODO: проверять, что в экипаже есть живой (Man::getDisability), и что нет боя рядом
        if (s.repairLeft <= 0) {
            if (why & 8) { setBrewUpTimer(v, 0.0f); extinguishFire(s); }   // отменяем таймер взрыва и тушим огонь
            setImmobilized(v, 0);
            setDisabledBecause(v, 0);
            s.repairLeft = -1;
            s.grace = GRACE_SECONDS;
            notifyRepaired(v);
            LOGI("repaired %p", v);
        }
    } else {
        if (!alive) g_state.erase(v);  // убитую технику выкидываем из карты (TODO: чистить и при удалении Vehicle)
        else s.repairLeft = -1;
    }
    return r;
}

template <class T> static bool sym(void* h, const char* n, T& out) {
    out = reinterpret_cast<T>(dlsym(h, n));
    if (!out) LOGI("symbol not found: %s", n);
    return out != nullptr;
}

#ifndef STAGE
#define STAGE 3   // 1 = только загрузка, 2 = + init ShadowHook, 3 = полный хук
#endif

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
    LOGI("JNI_OnLoad stage %d", STAGE);
    if (STAGE <= 1) return JNI_VERSION_1_6;
    void* h = dlopen("libmain.so", RTLD_NOW | RTLD_NOLOAD);   // libmain уже загружена SDLActivity
    if (!h) { LOGI("libmain.so not loaded"); return JNI_VERSION_1_6; }

    bool ok = sym(h, "_ZN5Piece9getStatusEv", getStatus)
           && sym(h, "_ZN5Piece13isImmobilizedEv", isImmobilized)
           && sym(h, "_ZN5Piece14setImmobilizedEh", setImmobilized)
           && sym(h, "_ZN7Vehicle18getDisabledBecauseEv", getDisabledBecause)
           && sym(h, "_ZN7Vehicle18setDisabledBecauseEh", setDisabledBecause)
           && sym(h, "_ZN5Piece17haltCurrentOptionEv", haltCurrentOption)
           && sym(h, "_ZN7Vehicle14setBrewUpTimerEf", setBrewUpTimer)
           && sym(h, "_ZN5Piece11getPositionEv", getPosition)
           && sym(h, "_ZN5Piece7getSideEv", getSide)
           && sym(h, "_ZN5Piece9getSquadIEv", getSquadI)
           && sym(h, "_ZN5Piece4getIEv", getI)
           && sym(h, "_ZN7Vehicle24hasBecomeDisabledBecauseEh", hasBecomeDisabledBecause)
           && sym(h, "_ZN7Vehicle18getPointerToEngineEv", getPointerToEngine)
           && sym(h, "_ZN6Engine15getSteeringTypeEv", getSteeringType)
           && sym(h, "_ZN4Game66destroyVehicleAndEvacuateCrewmen_KillerSide_KillerI_ByMethod_HitOnEP7VehiclehhhPKc", destroyVehicle);
    void* penTarget = dlsym(h, "_ZN4Game180vehicleIsPenetrated_KillerSide_KillerI_ByMethod_Shot_IsHitOnTurret_IsHitOnSuperstructure_IsHitOnMantlet_IsHitOnFace_IsHitOnHighOrLow_IsHitOnFrontOrBack_OverPenetrationInMillimetresEP7VehiclehhhP4Shotbbbiiif");
    void* notPenTarget = dlsym(h, "_ZN4Game95radioThatVehicleIsHitButNotPenetrated_IsTurret_Structure_HighOrLow_Face_Mantlet_ByShot_ByMethodEP5PiecebiiibP4Shoth");
    void* radioTarget = dlsym(h, "_ZN4Game58sendRadioMessage_Urgency_ForSide_Position_Side_Squad_PieceEPKchh8POSITIONhhh");
    void* target = dlsym(h, "_ZN7Vehicle50considerIfVehicleBreaksDownOverTerrain_TimeElapsedEhf");
    if (!ok || !target) { LOGI("init failed"); return JNI_VERSION_1_6; }

    int ir = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    LOGI("shadowhook_init = %d", ir);
    if (STAGE == 2) return JNI_VERSION_1_6;
    void* stub = shadowhook_hook_func_addr(target, (void*)hook_breakdown, (void**)&orig_breakdown);
    if (!stub) { LOGI("hook failed, errno=%d", shadowhook_get_errno()); return JNI_VERSION_1_6; }
    if (radioTarget)
        shadowhook_hook_func_addr(radioTarget, (void*)hook_radio, (void**)&orig_radio);
    if (notPenTarget)
        shadowhook_hook_func_addr(notPenTarget, (void*)hook_notpen, (void**)&orig_notpen);
    if (penTarget)
        shadowhook_hook_func_addr(penTarget, (void*)hook_pen, (void**)&orig_pen);
    LOGI("hook installed, radio=%p notpen=%p pen=%p", radioTarget, notPenTarget, penTarget);
    return JNI_VERSION_1_6;
}
