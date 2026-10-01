// libmod.so - авто-ремонт сломавшегося двигателя, Firefight 13.0.2 (arm64)
// Хуки: ShadowHook (Bytedance).
#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <unordered_map>
#include <vector>
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

// ---- взрыв баков: пробитие двигателя с борта ----
static const bool  FUEL_ENABLED = true;
static const int   FUEL_FACE_RIGHT = 3;
static const int   FUEL_FACE_LEFT = 2;          // 2/3 = борт (по калибровке)
// растущий шанс: каждая проверка +STEP, потолок MAX, пока двигатель не отремонтирован
static const float FUEL_IGNITE_CHANCE  = 0.60f; // шанс, что пробитие двигателя вообще запустит протечку/возгорание баков
static const float FUEL_FIRST_CHECK    = 4.0f;  // сек после пробития до первой проверки
static const float FUEL_CHECK_INTERVAL = 3.0f;  // как часто бросаем кубик
static const float FUEL_CHANCE_START   = 0.03f; // шанс на первой проверке
static const float FUEL_CHANCE_STEP    = 0.03f; // +3% за каждую проверку
static const float FUEL_CHANCE_MAX     = 0.50f; // потолок шанса
static const float FUEL_FUSE_MIN       = 4.0f;  // после «зажигания» горит до взрыва, сек
static const float FUEL_FUSE_MAX       = 8.0f;
// ---- боекомплект (БК): пробитие корпуса/башни может поджечь БК ----
static const bool  AMMO_ENABLED         = true;
// (а) пробитие, после которого техника ЖИВА:
static const float AMMO_IGNITE_CHANCE   = 0.20f; // шанс запустить горение БК
static const float AMMO_INSTANT_CHANCE  = 0.06f; // шанс мгновенного подрыва БК
// (б) техника УНИЧТОЖЕНА (любым способом) и в ней есть ВВ-боеприпасы — ваниль сама назначила бы взрыв,
//     теперь решаем мы: сразу / после горения с искрами / вообще без взрыва (остаётся остов)
static const float AMMO_KILL_INSTANT    = 0.25f; // взрыв сразу
static const float AMMO_KILL_BURN       = 0.45f; // горит AMMO_BURN_MIN..MAX сек с искрами, потом взрыв (остаток шанса = без взрыва)
static const float AMMO_BURN_MIN        = 10.0f; // горит до взрыва (сек), случайно в этом диапазоне
static const float AMMO_BURN_MAX        = 60.0f; // максимум минута с начала возгорания
static const float AMMO_SPARK_MIN       = 3.0f;  // редкие искры: интервал между ними (сек)
static const float AMMO_SPARK_MAX       = 9.0f;
static const float AMMO_SPARK_LB        = 2.0f;  // «мощность» искры (фунты ВВ, только визуал)
static const float AMMO_BOOM_MULT       = 1.6f;  // взрыв БК мощнее взрыва баков
// мощность взрыва от массы (фунты ВВ, линейно, с ограничением)
static const float BOOM_LB_PER_KG = 0.004f;     // 26 т -> ~104 lb, 60 т -> ~240 lb
static const float BOOM_LB_MIN    = 8.0f;
static const float BOOM_LB_MAX    = 400.0f;
static const float BOOM_FIRE_TTL  = 45.0f;      // сколько горит остов после взрыва
static const unsigned char BOOM_TYPE = 1;       // КАЛИБРОВАТЬ: тип эффекта взрыва (createExplosionAt)
static const unsigned char BOOM_FIRE_TYPE = 0;  // КАЛИБРОВАТЬ: тип огня (createFireAt)
static const char* BOOM_SOUND = "explode_tank"; // строка есть в libmain.so

// ---- траки (своя механика: в игре её нет) ----
static const bool  TRACKS_ENABLED = true;
static const float TRACK_HIT_CHANCE = 0.60f;    // шанс сбить трак при подходящем непробитом попадании
static const float TRACK_REPAIR_SECONDS = 60.0f;
static const int   TRACK_HL   = 1;              // какое значение HighOrLow считать «низом»; -1 = любое (калибровка)
static const int   TRACK_FACE_RIGHT = 3;        // какое значение Face считать «бортом»; -1 = любое (калибровка)
static const int   TRACK_FACE_LEFT = 2;
static const bool  DEBUG_HITS = true;           // радио: параметры каждого непробитого попадания (для калибровки)
static const bool  DEBUG_RADIO = true;          // ОТЛАДКА: выводить состояние техники в игровое радио
static const bool  REPAIR_NOTIFY = true;        // радио-сообщение «vehicle repaired»
static const float GRACE_SECONDS  = 20.0f;      // иммунитет к новой поломке после ремонта
static const bool  TEST_ALL_BROKEN = false;     // ТЕСТ: каждая новая техника ломается при первом тике
static const bool  LOG_STATE_CHANGES = true;    // лог смены isImmobilized/disabledBecause

// ---- экспортированные функции libmain.so (Itanium mangling) ----
// Piece/Vehicle: this-указатель в x0, аргументы по ABI AArch64.
using GetStatus_t   = unsigned char (*)(void* piece);
using IsImmob_t     = unsigned char (*)(void* piece);
using SetImmob_t    = void (*)(void* piece, unsigned char v);
using GetDisabled_t = unsigned char (*)(void* vehicle);
using SetDisabled_t = void (*)(void* vehicle, unsigned char v);
using SetBrew_t     = void (*)(void* vehicle, float t);
struct POSITION { float x, y; };   // по дизассемблеру Piece::getPosition: 2 float (передаётся в s0,s1)
struct VECTOR   { float x, y, z; };// ПРОВЕРИТЬ: 3 float (HFA в s0..s2)
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
// взрыв
using GetMass_t     = float (*)(void* vehicle);   // ldr s0,[x0,#0x568]; ret
using CreateExpl_t  = void (*)(void* game, POSITION pos, unsigned char type, unsigned char visTo, float pounds);
using EffectExpl_t  = void (*)(void* game, VECTOR at, unsigned char type, const char* sound, float pounds,
                               bool hitRoof, unsigned char shooterSide, unsigned char shooterI,
                               unsigned char method, bool makeCrater);
using Sparks_t      = void (*)(void* game, void* piece);
using CreateFire_t  = void (*)(void* game, POSITION pos, float smokeSize, unsigned char type, float ttl);

static GetStatus_t   getStatus;
static IsImmob_t     isImmobilized;
static SetImmob_t    setImmobilized;
static GetDisabled_t getDisabledBecause;
static SetDisabled_t setDisabledBecause;
static Halt_t        haltCurrentOption;
static Pen_t         orig_pen;
static Destroy_t     destroyVehicle;
using TickBrew_t = unsigned char (*)(void* v, float dt);
static TickBrew_t    orig_tickbrew;
static Destroy_t     orig_destroy;
static bool          g_ownDestroy = false;   // true, пока destroyVehicle зовём мы сами (хук его не трогает)
static HasBecome_t   hasBecomeDisabledBecause;
static GetPtrEngine_t getPointerToEngine;
static SteerType_t   getSteeringType;
static NotPen_t      orig_notpen;
static GetPos_t      getPosition;
static GetU8_t       getSide, getSquadI, getI, getKilledBySide, getKilledByI;
static Radio_t       orig_radio;      // оригинал sendRadioMessage_..._Piece
static void*         g_game;          // Game*, перехватываем из вызовов радио
static SetBrew_t     setBrewUpTimer;
static Breakdown_t   orig_breakdown;
static GetMass_t     getMassKG;
static CreateExpl_t  createExplosionAt;   // необязательные: могут быть nullptr
static EffectExpl_t  effectOfExplosionAt;
static Sparks_t      createSparks;
static CreateFire_t  createFireAt;

struct FireRef { int idx = -1; float x = 0, y = 0; };   // слот огня в Game + позиция (чтобы отличить переиспользованный слот)
struct State { float repairLeft = -1; float grace = 0; bool seen = false; int lastImm = -1; int lastWhy = -1;
               FireRef engineFire;   // вечный огонь от пробития двигателя (создаётся внутри vehicleIsPenetrated)
               FireRef fuseFire;     // огонь «запала» баков (создаём сами)
               // баки: fuelLeak = идут проверки с растущим шансом; fuelTimer >= 0 = запал горит, ждём взрыв
               bool fuelLeak = false; float fuelCheckIn = 0; int fuelChecks = 0;
               float fuelTimer = -1; unsigned char fuelKS = 0, fuelKI = 0;
               // БК: ammoTimer >= 0 = горит, до взрыва осталось; ammoSparkIn = до следующей искры
               float ammoTimer = -1, ammoSparkIn = 0; unsigned char ammoKS = 0, ammoKI = 0; };
static std::unordered_map<void*, State> g_state;
static std::recursive_mutex g_mu;   // рекурсивный: взрыв внутри хука может вызвать другие хуки в том же потоке

// Перехват радио: запоминаем Game* и пропускаем вызов дальше без изменений
static void hook_radio(void* game, const char* msg, unsigned char urgency, unsigned char forSide,
                       POSITION pos, unsigned char side, unsigned char squad, unsigned char piece) {
    g_game = game;
    orig_radio(game, msg, urgency, forSide, pos, side, squad, piece);
}

// urgency определяет цвет строки в радио: 0 = зелёный, 1 = жёлтый, 2 = оранжевый (ваниль), 3 = белый
enum { URG_GREEN = 0, URG_YELLOW = 1, URG_ORANGE = 2, URG_WHITE = 3 };
static void sendMsg(void* v, const char* text, unsigned char urgency = URG_ORANGE) {
    if (!g_game || !orig_radio) return;   // пока не видели ни одного радио-сообщения, молчим
    unsigned char side = getSide(v);
    orig_radio(g_game, text, urgency, side, getPosition(v), side, getSquadI(v), getI(v));
}

static void notifyRepaired(void* v) {
    if (!REPAIR_NOTIFY) return;
    char buf[96];
    snprintf(buf, sizeof buf, "Squad %d: vehicle repaired", (int)getSquadI(v) + 1);
    sendMsg(v, buf, URG_GREEN);                       // починка = зелёный
}

// ---- огонь от попадания в двигатель ----
// Раскладка по дизассемблеру createFireAt / timerTickFiresAndSmokes (13.0.x):
//   Game+0x1218 = FIRE*, Game+0x1214 = число слотов; sizeof(FIRE)=0x1c
//   FIRE: +0 активен, +1 тип, +4 x, +8 y, +0xC таймер дыма, +0x10 размер дыма, +0x14 TTL (счёт вниз, <0 = вечный), +0x18 возраст
static const size_t G_FIRES = 0x1218, G_FIRES_N = 0x1214, FIRE_SZ = 0x1c;
static float rdf(const char* p, size_t off) { float f; memcpy(&f, p + off, 4); return f; }
// Vehicle+0x650 — оставшееся время до ванильного взрыва («brew up»); >0 только у уничтоженной техники с ВВ-боеприпасами
static const size_t VEH_BREWUP = 0x650;
static float& brewTimer(void* v) { return *(float*)((char*)v + VEH_BREWUP); }
static std::unordered_map<void*, float> g_sparks;   // горящая уничтоженная техника → сек до следующей искры

// Снимок «какие слоты огня сейчас активны». Берём ДО вызова, который создаёт огонь,
// и сравниваем ПОСЛЕ: так находим именно новый огонь, без гаданий по возрасту/времени.
static std::vector<uint8_t> fireSnapshot() {
    std::vector<uint8_t> r;
    if (!g_game) return r;
    char* base = (char*)g_game;
    uint32_t n = *(uint32_t*)(base + G_FIRES_N);
    char* arr = *(char**)(base + G_FIRES);
    if (!arr || n == 0 || n > 100000) return r;
    r.resize(n);
    for (uint32_t i = 0; i < n; i++) r[i] = (arr[i * FIRE_SZ] == 1);
    return r;
}

// Ищет слот, который стал активным после снимка (createFireAt занимает первый свободный слот,
// обнуляя возраст). Из нескольких кандидатов берём ближайший к `near`.
static bool grabNewFire(const std::vector<uint8_t>& before, POSITION near, FireRef& out) {
    out = FireRef();
    if (!g_game) return false;
    char* base = (char*)g_game;
    uint32_t n = *(uint32_t*)(base + G_FIRES_N);
    char* arr = *(char**)(base + G_FIRES);
    if (!arr || n == 0 || n > 100000) return false;
    int best = -1; float bd = 1e30f;
    for (uint32_t i = 0; i < n; i++) {
        char* f = arr + i * FIRE_SZ;
        if (f[0] != 1) continue;
        if (i < before.size() && before[i]) continue;          // этот огонь уже горел до вызова
        float dx = rdf(f, 4) - near.x, dy = rdf(f, 8) - near.y, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = (int)i; }
    }
    if (best < 0) return false;
    char* f = arr + (size_t)best * FIRE_SZ;
    out.idx = best; out.x = rdf(f, 4); out.y = rdf(f, 8);
    LOGI("fire slot %d captured, dist2=%f", best, bd);
    return true;
}

// Запасной вариант: ищем свежий огонь возле техники по возрасту (если захват в hook_pen не сработал)
static void findFire(void* v, FireRef& out) {
    if (!g_game) return;
    char* base = (char*)g_game;
    uint32_t n = *(uint32_t*)(base + G_FIRES_N);
    char* arr = *(char**)(base + G_FIRES);
    if (!arr || n == 0 || n > 100000) return;
    POSITION vp = getPosition(v);
    int best = -1; float bd = 1e30f;
    for (uint32_t i = 0; i < n; i++) {
        char* f = arr + i * FIRE_SZ;
        if (f[0] != 1 || rdf(f, 0x18) > 3.0f) continue;     // только свежие (возраст < 3 c)
        float dx = rdf(f, 4) - vp.x, dy = rdf(f, 8) - vp.y, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = (int)i; }
    }
    if (best >= 0) {
        char* f = arr + best * FIRE_SZ;
        out.idx = best; out.x = rdf(f, 4); out.y = rdf(f, 8);
        LOGI("fire slot %d found (fallback), dist2=%f", best, bd);
    }
}

static void extinguishFire(FireRef& r) {    // ставим TTL ~0: игра сама погасит огонь на следующем тике
    if (r.idx < 0 || !g_game) return;
    char* base = (char*)g_game;
    uint32_t n = *(uint32_t*)(base + G_FIRES_N);
    char* arr = *(char**)(base + G_FIRES);
    if (arr && (uint32_t)r.idx < n) {
        char* f = arr + (size_t)r.idx * FIRE_SZ;
        if (f[0] == 1 && rdf(f, 4) == r.x && rdf(f, 8) == r.y) {   // проверка, что слот не переиспользован
            float ttl = 0.01f; memcpy(f + 0x14, &ttl, 4);
            LOGI("fire slot %d extinguished", r.idx);
        }
    }
    r = FireRef();
}

// ---- траки: перехват «попал, но не пробил» ----
static uint32_t g_rng = 2463534242u;
static float rand01() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng & 0xFFFFFF) / 16777216.0f; }

static bool isTracked(void* v) {   // стиринг 0..6 = гусеничные схемы (Clutch&brake ... Twin transmission); 10 = колёса
    void* e = getPointerToEngine(v);
    return e && getSteeringType(e) <= 6;
}

// Любое попадание по технике (пробитие или нет) сбрасывает прогресс ремонта: чинить под обстрелом нельзя.
static void resetRepairOnHit(void* v) {
    if (!v) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_state.find(v);                        // только существующие записи, новые не создаём
    if (it != g_state.end() && it->second.repairLeft >= 0) {
        it->second.repairLeft = -1;
        LOGI("repair timer reset by hit on %p", v);
    }
}

static void explodeVehicle(void* v, unsigned char ks, unsigned char ki, float lbMult, const char* why);

// Пробитие: шанс сразу подорвать БК или запустить его горение (один раз на технику)
static void tryAmmoHit(void* v, unsigned char ks, unsigned char ki) {
    if (!AMMO_ENABLED) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    State& s = g_state[v];
    if (s.ammoTimer >= 0) return;                     // уже горит
    float r = rand01();
    if (r < AMMO_INSTANT_CHANCE) {
        LOGI("ammo instant detonation on %p", v);
        g_state.erase(v);
        explodeVehicle(v, ks, ki, AMMO_BOOM_MULT, "ammunition");
        return;
    }
    if (r < AMMO_INSTANT_CHANCE + AMMO_IGNITE_CHANCE) {
        s.ammoTimer = AMMO_BURN_MIN + rand01() * (AMMO_BURN_MAX - AMMO_BURN_MIN);
        s.ammoSparkIn = AMMO_SPARK_MIN + rand01() * (AMMO_SPARK_MAX - AMMO_SPARK_MIN);
        s.ammoKS = ks; s.ammoKI = ki;
        if (createFireAt) createFireAt(g_game, getPosition(v), 0.8f, BOOM_FIRE_TYPE, s.ammoTimer + 2.0f);
        LOGI("ammo fire started on %p, boom in %.1fs", v, s.ammoTimer);
    }
}

static void hook_notpen(void* game, void* piece, bool isTurret, int structure, int hl, int face,
                        bool mantlet, void* shot, unsigned char method) {
    g_game = game;
    orig_notpen(game, piece, isTurret, structure, hl, face, mantlet, shot, method);
    resetRepairOnHit(piece);

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
    sendMsg(piece, msg, URG_YELLOW);                 // некритическая поломка (трак) = жёлтый
    LOGI("track hit on %p", piece);
}

// ---- взрыв баков: хук пробития ----
// Пробитие двигателя с борта не взрывает сразу, а запускает «протечку»:
// дальше hook_breakdown бросает кубик с растущим шансом, пока двигатель не починят.
static void hook_pen(void* game, void* v, unsigned char ks, unsigned char ki, unsigned char method,
                     void* shot, bool turret, bool superstr, bool mantlet, int face, int hl, int fob, float over) {
    g_game = game;
    bool wasAlive = v && getStatus(v) == 0;
    int before = wasAlive ? (getDisabledBecause(v) & 8) : 1;
    std::vector<uint8_t> snap;
    if (!before) snap = fireSnapshot();                 // до вызова: какие огни уже горят
    orig_pen(game, v, ks, ki, method, shot, turret, superstr, mantlet, face, hl, fob, over);
    if (!v) return;
    if (wasAlive) resetRepairOnHit(v);                // пробитие тоже сбрасывает ремонт
    if (wasAlive && getStatus(v) == 0) tryAmmoHit(v, ks, ki);   // шанс поджечь / сразу подорвать БК
    if (before) return;
    if (getStatus(v) != 0 || !(getDisabledBecause(v) & 8)) return;   // двигатель только что подбит, техника жива

    std::lock_guard<std::recursive_mutex> lk(g_mu);
    State& s = g_state[v];
    // Игра создаёт вечный огонь прямо внутри orig_pen: запоминаем его СРАЗУ, чтобы потом точно погасить при ремонте
    grabNewFire(snap, getPosition(v), s.engineFire);

    if (!FUEL_ENABLED || turret) return;
    if (rand01() >= FUEL_IGNITE_CHANCE) { LOGI("fuel: no leak this time on %p", v); return; }   // шанс на шанс
    if (face != FUEL_FACE_LEFT && face != FUEL_FACE_RIGHT) return;
    if (s.fuelLeak || s.fuelTimer >= 0) return;
    s.fuelLeak = true;
    s.fuelChecks = 0;
    s.fuelCheckIn = FUEL_FIRST_CHECK;
    s.fuelKS = ks; s.fuelKI = ki;
    LOGI("fuel leak started on %p", v);
}

// ---- сам взрыв: мощность зависит от массы ----
// Только эффекты (взрыв, ударная волна, искры, огонь): для уже уничтоженной техники.
static void boomEffects(void* v, unsigned char ks, unsigned char ki, float lbMult, const char* why) {
    if (!g_game) return;
    float kg = getMassKG ? getMassKG(v) : 0.0f;
    if (!(kg > 0.0f) || kg > 1e6f) kg = 20000.0f;          // защита от мусора
    float lb = kg * BOOM_LB_PER_KG * lbMult;
    if (lb < BOOM_LB_MIN) lb = BOOM_LB_MIN;
    if (lb > BOOM_LB_MAX * lbMult) lb = BOOM_LB_MAX * lbMult;
    POSITION p = getPosition(v);
    if (createExplosionAt) createExplosionAt(g_game, p, BOOM_TYPE, 255 /* видят все */, lb);
    if (effectOfExplosionAt)   // ударная волна по соседям + звук, воронку не создаём
        effectOfExplosionAt(g_game, VECTOR{p.x, p.y, 0.0f}, BOOM_TYPE, BOOM_SOUND, lb * 0.5f,
                            false, ks, ki, 40, false);
    if (createSparks) createSparks(g_game, v);
    if (createFireAt) createFireAt(g_game, p, 1.0f + lb / 100.0f, BOOM_FIRE_TYPE, BOOM_FIRE_TTL);
    LOGI("boom (%s) %p mass=%.0fkg lb=%.1f", why, v, kg, lb);
}

// Живую технику уничтожаем и взрываем.
static void explodeVehicle(void* v, unsigned char ks, unsigned char ki, float lbMult, const char* why) {
    if (!g_game || !destroyVehicle || getStatus(v) != 0) return;
    g_ownDestroy = true;                                   // наш вызов: хук destroy его не обрабатывает
    destroyVehicle(g_game, v, ks, ki, 40 /* vehicle explosion */, why);
    g_ownDestroy = false;
    // destroyVehicle ставит ванильный таймер взрыва (Vehicle+0x650) — обнуляем, взрываем сами
    setBrewUpTimer(v, 0.0f);
    boomEffects(v, ks, ki, lbMult, why);
}

static void explodeFuel(void* v, State& s) { explodeVehicle(v, s.fuelKS, s.fuelKI, 1.0f, "fuel tank"); }

// Любое уничтожение техники (пробитие, взрыв рядом, арта...). Если ваниль назначила взрыв БК
// (таймер >0, значит в технике есть ВВ-боеприпасы), решаем сами: сразу / с горением и искрами / без взрыва.
static void hook_destroy(void* game, void* v, unsigned char ks, unsigned char ki, unsigned char method, const char* hitOn) {
    g_game = game;
    orig_destroy(game, v, ks, ki, method, hitOn);
    if (g_ownDestroy || !AMMO_ENABLED || !v || getStatus(v) == 0) return;
    if (!(brewTimer(v) > 0.0f)) return;                    // ванильного взрыва нет — не трогаем
    float r = rand01();
    if (r < AMMO_KILL_INSTANT) {
        brewTimer(v) = 0.0f;
        boomEffects(v, ks, ki, AMMO_BOOM_MULT, "ammunition");
        LOGI("ammo: instant detonation of destroyed %p", v);
    } else if (r < AMMO_KILL_INSTANT + AMMO_KILL_BURN) {
        float t = AMMO_BURN_MIN + rand01() * (AMMO_BURN_MAX - AMMO_BURN_MIN);
        brewTimer(v) = t;                                  // ваниль сама отсчитает, взрыв подменяет hook_tickbrew
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        g_sparks[v] = AMMO_SPARK_MIN + rand01() * (AMMO_SPARK_MAX - AMMO_SPARK_MIN);
        LOGI("ammo: destroyed %p burns for %.1fs", v, t);
    } else {
        brewTimer(v) = 0.0f;                               // повезло: БК не сдетонировал
        LOGI("ammo: destroyed %p, no detonation", v);
    }
}

// Тик таймера взрыва у уничтоженной техники: редкие искры, а по истечении — наш взрыв вместо ванильного.
static unsigned char hook_tickbrew(void* v, float dt) {
    bool burning = AMMO_ENABLED && v && brewTimer(v) > 0.0f;
    unsigned char r = orig_tickbrew(v, dt);
    if (!burning) return r;
    if (r) {                                               // таймер истёк: ваниль взорвала бы сама
        { std::lock_guard<std::recursive_mutex> lk(g_mu); g_sparks.erase(v); }
        boomEffects(v, getKilledBySide ? getKilledBySide(v) : 0, getKilledByI ? getKilledByI(v) : 0,
                    AMMO_BOOM_MULT, "ammunition");
        return 0;                                          // ванильный взрыв подавлен
    }
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_sparks.find(v);
    if (it != g_sparks.end()) {
        it->second -= dt;
        if (it->second <= 0) {
            it->second = AMMO_SPARK_MIN + rand01() * (AMMO_SPARK_MAX - AMMO_SPARK_MIN);
            if (createExplosionAt) createExplosionAt(g_game, getPosition(v), BOOM_TYPE, 255, AMMO_SPARK_LB);
        }
    }
    return r;
}

static unsigned char hook_breakdown(void* v, unsigned char grid, float dt) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    State& s = g_state[v];

    if (TEST_ALL_BROKEN && !s.seen && getStatus(v) == 0) {   // первый тик этой техники
        s.seen = true;
        setImmobilized(v, 1);
        setDisabledBecause(v, 1);
        haltCurrentOption(v);
        LOGI("test: broke %p", v);
    }
    s.seen = true;

    // горение БК: редкие искры, затем взрыв. Ремонт двигателя это не тушит.
    if (s.ammoTimer >= 0) {
        if (getStatus(v) != 0) { g_state.erase(v); return orig_breakdown(v, grid, dt); }
        s.ammoTimer -= dt;
        s.ammoSparkIn -= dt;
        if (s.ammoSparkIn <= 0 && s.ammoTimer > 0) {
            s.ammoSparkIn = AMMO_SPARK_MIN + rand01() * (AMMO_SPARK_MAX - AMMO_SPARK_MIN);
            if (createExplosionAt) createExplosionAt(g_game, getPosition(v), BOOM_TYPE, 255, AMMO_SPARK_LB);
        }
        if (s.ammoTimer <= 0) {
            unsigned char ks = s.ammoKS, ki = s.ammoKI;
            g_state.erase(v);
            explodeVehicle(v, ks, ki, AMMO_BOOM_MULT, "ammunition");
            return 0;
        }
    }

    if (s.grace > 0) {                 // после ремонта не даём сразу сломаться снова
        s.grace -= dt;
        if (isImmobilized(v) == 0) return 0;   // оригинал не зовём
    }

    // протечка топлива: растущий шанс, пока двигатель не отремонтирован
    if (s.fuelLeak && getStatus(v) == 0) {
        s.fuelCheckIn -= dt;
        if (s.fuelCheckIn <= 0) {
            float chance = FUEL_CHANCE_START + FUEL_CHANCE_STEP * s.fuelChecks;
            if (chance > FUEL_CHANCE_MAX) chance = FUEL_CHANCE_MAX;
            s.fuelChecks++;
            s.fuelCheckIn = FUEL_CHECK_INTERVAL;
            if (rand01() < chance) {                  // баки загорелись: запускаем запал
                s.fuelLeak = false;
                s.fuelTimer = FUEL_FUSE_MIN + rand01() * (FUEL_FUSE_MAX - FUEL_FUSE_MIN);
                if (createFireAt) {
                    std::vector<uint8_t> snap = fireSnapshot();
                    createFireAt(g_game, getPosition(v), 0.8f, BOOM_FIRE_TYPE, s.fuelTimer + 2.0f);
                    grabNewFire(snap, getPosition(v), s.fuseFire);   // чтобы потушить при ремонте
                }
                LOGI("fuel fuse lit on %p, boom in %.1fs (chance was %.2f)", v, s.fuelTimer, chance);
            }
        }
    }

    if (s.fuelTimer >= 0) {                       // баки горят: ждём и взрываем
        s.fuelTimer -= dt;
        if (s.fuelTimer < 0) {
            s.fuelTimer = -1;
            if (getStatus(v) == 0 && g_game && destroyVehicle) explodeFuel(v, s);
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
            if ((why & 8) && s.engineFire.idx < 0) findFire(v, s.engineFire);   // запасной поиск, если hook_pen не поймал
        }
        s.repairLeft -= dt;
        // TODO: проверять, что в экипаже есть живой (Man::getDisability), и что нет боя рядом
        if (s.repairLeft <= 0) {
            if (why & 8) {
                setBrewUpTimer(v, 0.0f);                      // отменяем таймер взрыва
                extinguishFire(s.engineFire);                 // тушим огонь от попадания в двигатель
                extinguishFire(s.fuseFire);                   // и огонь запала баков, если успел загореться
                s.fuelLeak = false; s.fuelChecks = 0;         // протечка устранена: шанс взрыва сброшен
                s.fuelTimer = -1;                             // и запал, если уже горел, тоже отменяется
            }
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
           && sym(h, "_ZN5Piece15getKilledBySideEv", getKilledBySide)
           && sym(h, "_ZN5Piece12getKilledByIEv", getKilledByI)
           && sym(h, "_ZN7Vehicle24hasBecomeDisabledBecauseEh", hasBecomeDisabledBecause)
           && sym(h, "_ZN7Vehicle18getPointerToEngineEv", getPointerToEngine)
           && sym(h, "_ZN6Engine15getSteeringTypeEv", getSteeringType)
           && sym(h, "_ZN7Vehicle20getMassOfVehicleInKGEv", getMassKG)
           && sym(h, "_ZN4Game66destroyVehicleAndEvacuateCrewmen_KillerSide_KillerI_ByMethod_HitOnEP7VehiclehhhPKc", destroyVehicle);
    // необязательные символы для красивого взрыва: если не найдутся, взрыв будет просто ванильный
    sym(h, "_ZN4Game55createExplosionAt_Type_VisibleOnlyTo_PoundsOfExplosivesE8POSITIONhhf", createExplosionAt);
    sym(h, "_ZN4Game105effectOfExplosionAt_Type_Sound_PoundsOfExplosives_HitRoof_ShooterSide_ShooterI_DamagedByMethod_MakeCraterE6VECTORhPKcfbhhhb", effectOfExplosionAt);
    sym(h, "_ZN4Game32createSparksFromExplodingVehicleEP5Piece", createSparks);
    sym(h, "_ZN4Game38createFireAt_SmokeSize_Type_TimeToLiveE8POSITIONfhf", createFireAt);

    void* penTarget = dlsym(h, "_ZN4Game180vehicleIsPenetrated_KillerSide_KillerI_ByMethod_Shot_IsHitOnTurret_IsHitOnSuperstructure_IsHitOnMantlet_IsHitOnFace_IsHitOnHighOrLow_IsHitOnFrontOrBack_OverPenetrationInMillimetresEP7VehiclehhhP4Shotbbbiiif");
    void* notPenTarget = dlsym(h, "_ZN4Game95radioThatVehicleIsHitButNotPenetrated_IsTurret_Structure_HighOrLow_Face_Mantlet_ByShot_ByMethodEP5PiecebiiibP4Shoth");
    void* destroyTarget = dlsym(h, "_ZN4Game66destroyVehicleAndEvacuateCrewmen_KillerSide_KillerI_ByMethod_HitOnEP7VehiclehhhPKc");
    void* tickBrewTarget = dlsym(h, "_ZN7Vehicle15timerTickBrewUpEf");
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
    if (destroyTarget)
        shadowhook_hook_func_addr(destroyTarget, (void*)hook_destroy, (void**)&orig_destroy);
    if (tickBrewTarget && orig_destroy)       // подмена взрыва имеет смысл только вместе с хуком destroy
        shadowhook_hook_func_addr(tickBrewTarget, (void*)hook_tickbrew, (void**)&orig_tickbrew);
    LOGI("ammo hooks: destroy=%p tickbrew=%p", destroyTarget, tickBrewTarget);
    LOGI("hook installed, radio=%p notpen=%p pen=%p", radioTarget, notPenTarget, penTarget);
    return JNI_VERSION_1_6;
}
