// libmod.so - авто-ремонт сломавшегося двигателя, Firefight 13.0.2 (arm64)
// Хуки: ShadowHook (Bytedance).
#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <unordered_map>
#include <vector>
#include <cmath>
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
// (в) пробитие БОКА КОРПУСА (не башни и не зоны двигателя) в технике, где ваниль БК не нашла (БК в корпусе):
//     с этим шансом считаем, что задет БК, и дальше те же варианты, что в (б)
static const float AMMO_HULL_CHANCE     = 0.35f;
// (б) техника УНИЧТОЖЕНА (любым способом) и в ней есть ВВ-боеприпасы — ваниль сама назначила бы взрыв,
//     теперь решаем мы: сразу / после горения с искрами / вообще без взрыва (остаётся остов)
static const float AMMO_KILL_INSTANT    = 0.25f; // взрыв сразу
static const float AMMO_KILL_BURN       = 0.45f; // горит AMMO_BURN_MIN..MAX сек с искрами, потом взрыв (остаток шанса = без взрыва)
static const float AMMO_BURN_MIN        = 10.0f; // горит до взрыва (сек), случайно в этом диапазоне
static const float AMMO_BURN_MAX        = 60.0f; // максимум минута с начала возгорания
static const float AMMO_SPARK_MIN       = 3.0f;  // редкие искры: интервал между ними (сек)
static const float AMMO_SPARK_MAX       = 9.0f;
static const float AMMO_BOOM_MULT       = 1.6f;  // взрыв БК мощнее взрыва баков
// мощность взрыва от массы (фунты ВВ, линейно, с ограничением)
static const float BOOM_LB_PER_KG = 0.004f;     // 26 т -> ~104 lb, 60 т -> ~240 lb
static const float BOOM_LB_MIN    = 8.0f;
static const float BOOM_LB_MAX    = 400.0f;
static const float BOOM_FIRE_TTL  = 45.0f;      // сколько горит остов после взрыва
// Ванильный взрыв техники (Game::doAServerTick...): effectOfExplosionAt(type=0x11, "explode_tank", 10 lb, method 40)
// + createSparksFromExplodingVehicle. createExplosionAt в ванили не вызывается вообще — им взрыв не получится.
static const unsigned char BOOM_TYPE = 0x11;    // тип эффекта «взрыв техники» (из дизассемблера)
static const float BOOM_Z_OFFSET = 24.375f;     // ваниль берёт высоту земли + это значение
static const float BOOM_LB_VANILLA = 10.0f;     // ванильная мощность; растёт с массой, но не ниже
static const float BOOM_LB_CAP     = 40.0f;     // верхний предел (влияет на радиус поражения соседей)
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
using GroundH_t     = float (*)(void* world, POSITION p);   // World::getHeightOfGroundInPixelsAtPosition
static GroundH_t     groundHeight;
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
static GetU8_t       getSide, getSquadI, getI, getKilledBySide, getKilledByI, getEngineMounting;
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

// Контекст текущего вызова vehicleIsPenetrated (хук destroy срабатывает ВНУТРИ него)
struct PenCtx { void* v = nullptr; bool hullSideNoEngine = false; };
static PenCtx g_pen;

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
    // Бок корпуса вне секции двигателя: не башня/надстройка/маска, борт, и секция (fob) не совпадает с положением двигателя.
    // Соответствие из дизассемблера vehicleIsPenetrated: fob 0/1/2 ↔ getEngineMounting 2/1/0
    g_pen = PenCtx();
    if (wasAlive && getEngineMounting && !turret && !superstr && !mantlet &&
        (face == FUEL_FACE_LEFT || face == FUEL_FACE_RIGHT) && fob >= 0 && fob <= 2) {
        g_pen.v = v;
        g_pen.hullSideNoEngine = ((int)getEngineMounting(v) != 2 - fob);
    }
    orig_pen(game, v, ks, ki, method, shot, turret, superstr, mantlet, face, hl, fob, over);
    g_pen = PenCtx();
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
static float groundZ(POSITION p) {
    void* world = g_game ? *(void**)((char*)g_game + 0x50) : nullptr;   // Game+0x50 = World* (как в ванили)
    return (groundHeight && world) ? groundHeight(world, p) : 0.0f;
}

// Ванильный визуальный взрыв техники в точке p (тот же вызов, что делает серверный тик по таймеру brew up)
static void vanillaBoom(POSITION p, float lb, unsigned char ks, unsigned char ki) {
    if (!effectOfExplosionAt) return;
    effectOfExplosionAt(g_game, VECTOR{p.x, p.y, groundZ(p) + BOOM_Z_OFFSET}, BOOM_TYPE, nullptr /* звук по типу */,
                        lb, false, ks, ki, 40, false /* без воронки */);
}

static void boomEffects(void* v, unsigned char ks, unsigned char ki, float lbMult, const char* why) {
    if (!g_game) return;
    float kg = getMassKG ? getMassKG(v) : 0.0f;
    if (!(kg > 0.0f) || kg > 1e6f) kg = 20000.0f;          // защита от мусора
    float lb = BOOM_LB_VANILLA * lbMult * sqrtf(kg / 20000.0f);   // 20 т = ваниль, тяжёлые чуть сильнее
    if (lb < BOOM_LB_VANILLA) lb = BOOM_LB_VANILLA;
    if (lb > BOOM_LB_CAP * lbMult) lb = BOOM_LB_CAP * lbMult;
    POSITION p = getPosition(v);
    vanillaBoom(p, lb, ks, ki);                            // сам взрыв + звук + ударная волна по соседям
    if (createSparks) createSparks(g_game, v);             // ванильные искры от взорвавшейся техники
    // догорающий остов: createFireAt, тип огня как раньше
    if (createFireAt) createFireAt(g_game, p, 1.0f + lb / 40.0f, BOOM_FIRE_TYPE, BOOM_FIRE_TTL);
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
    bool vanillaAmmo = brewTimer(v) > 0.0f;                // ваниль нашла ВВ-боеприпасы в компоненте (башня и т.п.)
    if (!vanillaAmmo) {
        // БК в корпусе: ваниль взрыва не назначила. Бок корпуса вне двигателя — шанс, что задет БК
        if (!(g_pen.v == v && g_pen.hullSideNoEngine) || rand01() >= AMMO_HULL_CHANCE) return;
        LOGI("ammo: hull side hit on %p, rack hit", v);
    }
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
            if (createSparks) createSparks(g_game, v);   // редкие ванильные искры
        }
    }
    return r;
}

// =====================================================================================
// БУКСИРОВКА ПТ-ОРУДИЙ
// Орудие (ATGun : HeavyWeapon : Piece) и расчёт (Man) — отдельные Piece в одном отряде. Буксировка не использует
// ванильную посадку: пока орудие «прицеплено», мы каждый тик сами ставим его позицию/угол за техникой
// (Piece::setPositionAndResetTimeSinceMoved_AngleRadians), а расчёт переносим на орудие. Орудие остаётся
// обычным юнитом: его можно подбить, расчёт при этом освобождается ваниль-кодом.
// Управление: отдельная кнопка поверх боевого экрана (SceneGame::draw + touch-хуки). Ванильный UI не меняется.
// =====================================================================================
static const bool  TOW_ENABLED       = true;
static const float TOW_RANGE         = 90.0f;   // макс. расстояние от техники до орудия для сцепки (мировые единицы)
static const float TOW_MASS_RATIO    = 1.5f;    // масса техники >= TOW_MASS_RATIO * масса орудия
static const float TOW_HITCH_DIST    = 18.0f;   // от центра техники до точки сцепки (назад)
static const float TOW_TRAIL_DEFAULT = 28.0f;   // от точки сцепки до центра орудия, если ATGun::getGunTrailLength не годится
static const float TOW_TURN_K        = 2.5f;    // как быстро орудие «подтягивается» за поворотом техники (1/сек)
static const float TOW_ANGLE_OFFSET  = 0.0f;    // КАЛИБРОВКА: если орудие едет сбоку/спереди — попробуйте 1.5708 (90°), 3.1416 (180°)
static const bool  TOW_BARREL_BACK   = true;    // ствол смотрит назад (как при реальной буксировке)
static const bool  TOW_DISASSEMBLE   = true;    // на время буксировки орудие «разобрано» (не стреляет)
static const float TOW_UI_W = 124.0f, TOW_UI_H = 26.0f;   // размер кнопки (в точках касания)
static const float TOW_UI_RIGHT = 150.0f, TOW_UI_TOP = 70.0f;   // отступ от правого и верхнего края (в точках)
static const bool  TOW_DEBUG     = true;    // ОТЛАДКА: в радио пишется, почему кнопки нет (выключить после настройки)
static const int   TOW_TEXT_MODE = 10;          // параметр выравнивания drawText — как у ванильных кнопок

struct UXPOINT { float x, y; };   // HFA из 2 float (s0,s1): подтверждено по SceneGame::touchBegan
struct UXSIZE  { float w, h; };

using GameU8_t      = unsigned char (*)(void* game);
using GamePtr_t     = void* (*)(void* game);
using PieceSideI_t  = void* (*)(void* game, unsigned char side, unsigned char i);
using SquadSideSq_t = void* (*)(void* game, unsigned char side, unsigned char squad);
using ObjU8_t       = unsigned char (*)(void* obj);
using ObjU16_t      = unsigned short (*)(void* obj);
using ObjF_t        = float (*)(void* obj);
using ObjVoid_t     = void (*)(void* obj);
using SquadMember_t = void* (*)(void* squad, unsigned char idx);
using SetPosAng_t   = void (*)(void* piece, POSITION p, float angle);
using CalcPos_t     = POSITION (*)(float dist, float angle, POSITION origin);   // Geometry (статическая)
using AppGame_t     = void* (*)(void* appDelegate);
using DrawText_t    = void (*)(void* font, void* renderer, const char* text, UXPOINT pos, float size, int mode,
                               unsigned char r, unsigned char g, unsigned char b, UXPOINT shadow);
struct SDLRect { int x, y, w, h; };
using SdlSetColor_t = int (*)(void* ren, unsigned char r, unsigned char g, unsigned char b, unsigned char a);
using SdlGetColor_t = int (*)(void* ren, unsigned char* r, unsigned char* g, unsigned char* b, unsigned char* a);
using SdlRect_t     = int (*)(void* ren, const SDLRect* rc);
using SdlSetBlend_t = int (*)(void* ren, int mode);
using SdlGetBlend_t = int (*)(void* ren, int* mode);
using SceneDraw_t   = void (*)(void* scene);
using SceneTouch_t  = void (*)(void* scene, UXPOINT p);
using SceneSize_t   = void (*)(void* scene, UXSIZE s);

static GameU8_t      gSidePlayingAs, gIsASquadSelected;
static GamePtr_t     gSelectedSquad;
static PieceSideI_t  gPieceSideI;
static SquadSideSq_t gSquadSideSq;
static ObjU8_t       gGetType, gIsHeavy, gSquadIsATGun, gHWMoveable;
static ObjU16_t      gHWMass;
static ObjF_t        gGunTrail, gGetAngle;
static ObjVoid_t     gHWDisassemble, gHaltCrew, gSquadHalt;
static SquadMember_t gSquadMember;
static SetPosAng_t   gSetPosAng;
static CalcPos_t     gCalcPos;
static AppGame_t     gAppGame;
static DrawText_t    gDrawText;
static SdlSetColor_t gSdlSetColor; static SdlGetColor_t gSdlGetColor;
static SdlRect_t     gSdlFill, gSdlDraw;
static SdlSetBlend_t gSdlSetBlend; static SdlGetBlend_t gSdlGetBlend;
static SceneDraw_t   orig_scene_draw;
static SceneTouch_t  orig_touch_began, orig_touch_moved, orig_touch_ended;
static SceneSize_t   orig_scene_size;
static bool          g_towOk = false;           // все нужные функции найдены
static float         g_scrW = 0, g_scrH = 0;    // размер сцены в точках (SceneGame::setSize)
static bool          g_swallowTouch = false;    // касание началось на нашей кнопке — игре его не отдаём

struct TowLink { void* veh = nullptr; void* gun = nullptr; unsigned char side = 0, squadI = 0;
                 float h = 0, trail = 0; std::vector<void*> crew; };
static std::vector<TowLink> g_tows;             // под g_mu
struct TowUi { bool visible = false, hitched = false; void* veh = nullptr; void* gun = nullptr;
               float x = 0, y = 0, w = TOW_UI_W, h = TOW_UI_H; };
static TowUi g_ui;

static const float PIf = 3.14159265f;
static float wrapPi(float a) { while (a > PIf) a -= 2 * PIf; while (a < -PIf) a += 2 * PIf; return a; }

static TowLink* towByVeh(void* v) { for (auto& l : g_tows) if (l.veh == v) return &l; return nullptr; }
static bool     gunIsTowed(void* g) { for (auto& l : g_tows) if (l.gun == g) return true; return false; }

// Счётчики причин отказа (для диагностики в радио)
struct TowDiag { int heavy = 0, atgun = 0, moveable = 0, mass = 0, inrange = 0; float nearest = -1; };
static TowDiag g_diag;

// Орудие, которое эта техника может прицепить: своё, подвижное (<moveable>), в радиусе, по массе.
static void* towFindGun(void* game, void* veh, unsigned char side) {
    POSITION vp = getPosition(veh);
    float vmass = getMassKG ? getMassKG(veh) : 0.0f;
    void* best = nullptr; float bd = TOW_RANGE * TOW_RANGE;
    g_diag = TowDiag();
    for (int i = 0; i < 255; i++) {
        void* p = gPieceSideI(game, side, (unsigned char)i);
        if (!p || p == veh || getStatus(p) != 0 || !gIsHeavy(p) || gunIsTowed(p)) continue;
        g_diag.heavy++;
        void* sq = gSquadSideSq(game, side, getSquadI(p));
        if (!sq || !gSquadIsATGun(sq)) continue;          // только ПТ-орудия и гаубицы
        g_diag.atgun++;
        if (!gHWMoveable(p)) continue;                     // <moveable>no</moveable> — не буксируется
        g_diag.moveable++;
        POSITION gp = getPosition(p);
        float dx = gp.x - vp.x, dy = gp.y - vp.y, d = dx * dx + dy * dy;
        if (g_diag.nearest < 0 || sqrtf(d) < g_diag.nearest) g_diag.nearest = sqrtf(d);
        if (vmass < TOW_MASS_RATIO * (float)gHWMass(p)) continue;
        g_diag.mass++;
        if (d >= bd) continue;
        g_diag.inrange++;
        bd = d; best = p;
    }
    return best;
}

static void towRelease(size_t idx, const char* why) {   // под g_mu
    TowLink l = g_tows[idx];
    g_tows.erase(g_tows.begin() + (long)idx);
    LOGI("tow released (%s): veh=%p gun=%p", why, l.veh, l.gun);
}

static void towHitch(void* game, void* veh, void* gun, unsigned char side) {   // под g_mu
    TowLink l; l.veh = veh; l.gun = gun; l.side = side; l.squadI = getSquadI(gun);
    float t = gGunTrail ? gGunTrail(gun) : 0.0f;
    l.trail = (t > 8.0f && t < 120.0f) ? t : TOW_TRAIL_DEFAULT;
    l.h = gGetAngle(veh) + TOW_ANGLE_OFFSET;
    for (int i = 0; i < 255; i++) {                       // расчёт = люди из того же отряда
        void* p = gPieceSideI(game, side, (unsigned char)i);
        if (p && p != gun && getSquadI(p) == l.squadI && gGetType(p) == 1 && getStatus(p) == 0) l.crew.push_back(p);
    }
    void* sq = gSquadSideSq(game, side, l.squadI);
    if (sq) { gHaltCrew(sq); gSquadHalt(sq); }            // расчёт бросает текущие действия
    if (TOW_DISASSEMBLE && gHWDisassemble) gHWDisassemble(gun);
    g_tows.push_back(l);
    sendMsg(veh, "Gun hitched", URG_GREEN);
    LOGI("tow hitched: veh=%p gun=%p crew=%d trail=%.1f", veh, gun, (int)l.crew.size(), l.trail);
}

// Каждый тик техники (вызывается из hook_breakdown, g_mu уже взят)
static void towTick(void* v, float dt) {
    for (size_t i = 0; i < g_tows.size();) {
        TowLink& l = g_tows[i];
        if (l.veh != v) { i++; continue; }
        if (getStatus(v) != 0) { towRelease(i, "vehicle destroyed"); continue; }
        if (getStatus(l.gun) != 0) { towRelease(i, "gun destroyed"); continue; }
        POSITION vp = getPosition(v);
        float va = gGetAngle(v) + TOW_ANGLE_OFFSET;
        float k = TOW_TURN_K * dt; if (k > 1.0f) k = 1.0f;
        l.h = wrapPi(l.h + wrapPi(va - l.h) * k);                    // орудие запаздывает при поворотах
        POSITION hitch = gCalcPos(-TOW_HITCH_DIST, va, vp);          // точка сцепки позади техники
        POSITION gp = gCalcPos(-l.trail, l.h, hitch);                // орудие тянется за ней
        float ga = l.h + (TOW_BARREL_BACK ? PIf : 0.0f);
        gSetPosAng(l.gun, gp, ga);
        int n = (int)l.crew.size(), k2 = 0;
        for (void* m : l.crew) {                                     // расчёт «сидит на орудии»
            if (getStatus(m) != 0) continue;
            POSITION mp = gCalcPos(4.0f * ((float)k2 - (float)(n - 1) * 0.5f), ga + PIf * 0.5f, gp);
            gSetPosAng(m, mp, ga);
            k2++;
        }
        i++;
    }
}

// ---- кнопка поверх боевого экрана ----
static char g_reason[160] = "";
static void towDebugSay(const char* text) {   // в радио, только при смене причины и не чаще раза в ~2 с
    static char last[160] = ""; static int frame = 0, lastFrame = -1000;
    frame++;
    if (!TOW_DEBUG || !g_game || !orig_radio) return;
    if (strcmp(last, text) == 0 || frame - lastFrame < 120) return;
    snprintf(last, sizeof last, "%s", text); lastFrame = frame;
    unsigned char side = gSidePlayingAs ? gSidePlayingAs(g_game) : 0;
    orig_radio(g_game, text, URG_WHITE, side, POSITION{0, 0}, side, 0, 0);
}

static void towRefreshUi(void* game) {
    g_ui.visible = false;
    if (!game) { snprintf(g_reason, sizeof g_reason, "TOW: no game"); return; }
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    for (size_t i = 0; i < g_tows.size();) {                          // чистим связи с погибшими
        if (getStatus(g_tows[i].veh) != 0) towRelease(i, "vehicle destroyed (ui)");
        else if (getStatus(g_tows[i].gun) != 0) towRelease(i, "gun destroyed (ui)");
        else i++;
    }
    if (!gIsASquadSelected(game)) { snprintf(g_reason, sizeof g_reason, "TOW: no squad selected"); return; }
    void* sq = gSelectedSquad(game);
    if (!sq) { snprintf(g_reason, sizeof g_reason, "TOW: selected squad null"); return; }
    unsigned char side = gSidePlayingAs(game);
    void* veh = gSquadMember(sq, 0);
    if (!veh) { snprintf(g_reason, sizeof g_reason, "TOW: squad has no member 0"); return; }
    int type = gGetType(veh);
    if (type != 2) { snprintf(g_reason, sizeof g_reason, "TOW: selected piece type=%d (not vehicle)", type); return; }
    if (getStatus(veh) != 0 || getSide(veh) != side) { snprintf(g_reason, sizeof g_reason, "TOW: vehicle dead or other side"); return; }
    TowLink* l = towByVeh(veh);
    g_ui.veh = veh; g_ui.hitched = (l != nullptr); g_ui.gun = l ? l->gun : nullptr;
    if (!l) {
        g_ui.gun = towFindGun(game, veh, side);
        if (!g_ui.gun) {
            snprintf(g_reason, sizeof g_reason, "TOW: no gun. heavy=%d atgun=%d moveable=%d mass=%d range=%d near=%.0f",
                     g_diag.heavy, g_diag.atgun, g_diag.moveable, g_diag.mass, g_diag.inrange, g_diag.nearest);
            return;
        }
    }
    float sw = g_scrW > 0 ? g_scrW : 568.0f;
    g_ui.w = TOW_UI_W; g_ui.h = TOW_UI_H;
    g_ui.x = sw - TOW_UI_RIGHT - TOW_UI_W; g_ui.y = TOW_UI_TOP;
    g_ui.visible = true;
    snprintf(g_reason, sizeof g_reason, "TOW: button shown at %.0f,%.0f scr=%.0fx%.0f", g_ui.x, g_ui.y, g_scrW, g_scrH);
}

static void towDrawButton(void* scene, const char* text, unsigned char r, unsigned char g, unsigned char b) {
    void* ren = *(void**)((char*)scene + 0x8);        // SDL_Renderer* (так читает ванильный SceneGame)
    void* font = *(void**)((char*)scene + 0x778);     // UXFont* ванильных кнопок
    if (!ren || !gSdlFill) return;
    unsigned char or_ = 0, og = 0, ob = 0, oa = 255; int ob_mode = 0;
    gSdlGetColor(ren, &or_, &og, &ob, &oa); gSdlGetBlend(ren, &ob_mode);
    gSdlSetBlend(ren, 1 /* SDL_BLENDMODE_BLEND */);
    // игра рисует в удвоенных координатах относительно точек касания (см. дизассемблер: fadd s0,s0,s0)
    SDLRect rc{ (int)(2 * g_ui.x), (int)(2 * g_ui.y), (int)(2 * g_ui.w), (int)(2 * g_ui.h) };
    gSdlSetColor(ren, 0, 0, 0, 175);  gSdlFill(ren, &rc);
    gSdlSetColor(ren, r, g, b, 255);  gSdlDraw(ren, &rc);
    if (font && gDrawText) {
        float size = 0.8f * g_ui.h;                                   // как у ванильных кнопок: 2h * 0.4
        UXPOINT pos{ 2 * (g_ui.x + g_ui.w * 0.5f), 2 * (g_ui.y + g_ui.h * 0.55f) };
        gDrawText(font, ren, text, pos, size, TOW_TEXT_MODE, 255, 255, 255, UXPOINT{ size * 0.1f, size * 0.1f });
    }
    gSdlSetColor(ren, or_, og, ob, oa); gSdlSetBlend(ren, ob_mode);   // возвращаем состояние рендерера
}

static void hook_scene_size(void* scene, UXSIZE s) {
    g_scrW = s.w; g_scrH = s.h;
    orig_scene_size(scene, s);
}

static void hook_scene_draw(void* scene) {
    orig_scene_draw(scene);
    if (!TOW_ENABLED || !g_towOk || !scene) return;
    void* app = *(void**)((char*)scene + 0x10);                       // AppDelegate* (как в touchBegan)
    void* game = app ? gAppGame(app) : nullptr;
    if (game) g_game = game;
    towRefreshUi(game);
    towDebugSay(g_reason);
    if (g_ui.visible)
        g_ui.hitched ? towDrawButton(scene, "Unhitch gun", 255, 191, 0) : towDrawButton(scene, "Hitch gun", 64, 255, 64);
}

static bool towHandleTouch(UXPOINT p) {
    if (!TOW_ENABLED || !g_towOk || !g_ui.visible) return false;
    if (p.x < g_ui.x || p.x > g_ui.x + g_ui.w || p.y < g_ui.y || p.y > g_ui.y + g_ui.h) return false;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (!g_game || !g_ui.veh) return true;
    unsigned char side = getSide(g_ui.veh);
    TowLink* l = towByVeh(g_ui.veh);
    if (l) { size_t idx = (size_t)(l - g_tows.data()); void* v = g_ui.veh; towRelease(idx, "by user"); sendMsg(v, "Gun unhitched", URG_GREEN); }
    else if (g_ui.gun && getStatus(g_ui.gun) == 0 && !gunIsTowed(g_ui.gun)) towHitch(g_game, g_ui.veh, g_ui.gun, side);
    g_ui.visible = false;
    return true;
}

static void hook_touch_began(void* scene, UXPOINT p) {
    if (towHandleTouch(p)) { g_swallowTouch = true; return; }   // касание принадлежит нашей кнопке
    g_swallowTouch = false;
    orig_touch_began(scene, p);
}
static void hook_touch_moved(void* scene, UXPOINT p) { if (g_swallowTouch) return; orig_touch_moved(scene, p); }
static void hook_touch_ended(void* scene, UXPOINT p) {
    if (g_swallowTouch) { g_swallowTouch = false; return; }
    orig_touch_ended(scene, p);
}

static unsigned char hook_breakdown(void* v, unsigned char grid, float dt) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (TOW_ENABLED && g_towOk && !g_tows.empty()) towTick(v, dt);   // буксируемое орудие едет за техникой
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
            if (createSparks) createSparks(g_game, v);   // редкие ванильные искры
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

template <class T> static bool symAny(void* h1, void* h2, const char* n, T& out) {   // сначала один handle, потом другой
    out = reinterpret_cast<T>(dlsym(h1, n));
    if (!out) out = reinterpret_cast<T>(dlsym(h2, n));
    if (!out) LOGI("symbol not found: %s", n);
    return out != nullptr;
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
           && sym(h, "_ZN7Vehicle17getEngineMountingEv", getEngineMounting)
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
    sym(h, "_ZN5World35getHeightOfGroundInPixelsAtPositionE8POSITION", groundHeight);
    sym(h, "_ZN4Game38createFireAt_SmokeSize_Type_TimeToLiveE8POSITIONfhf", createFireAt);

    // ---- буксировка: все символы обязательны, иначе фича тихо отключается (остальной мод работает) ----
    void* sdl = dlopen("libSDL2.so", RTLD_NOW | RTLD_NOLOAD);
    if (!sdl) sdl = RTLD_DEFAULT;
    bool tow = sym(h, "_ZN4Game16getSidePlayingAsEv", gSidePlayingAs)
            && sym(h, "_ZN4Game16isASquadSelectedEv", gIsASquadSelected)
            && sym(h, "_ZN4Game25getPointerToSelectedSquadEv", gSelectedSquad)
            && sym(h, "_ZN4Game23getPointerToPieceSide_IEhh", gPieceSideI)
            && sym(h, "_ZN4Game27getPointerToSquadSide_SquadEhh", gSquadSideSq)
            && sym(h, "_ZN5Piece7getTypeEv", gGetType)
            && sym(h, "_ZN5Piece23isPieceTypeAHeavyWeaponEv", gIsHeavy)
            && sym(h, "_ZN5Squad27getIsAnATGunOrHowitzerSquadEv", gSquadIsATGun)
            && sym(h, "_ZN11HeavyWeapon13getIsMoveableEv", gHWMoveable)
            && sym(h, "_ZN11HeavyWeapon11getMassInKgEv", gHWMass)
            && sym(h, "_ZN11HeavyWeapon20setFullyDisassembledEv", gHWDisassemble)
            && sym(h, "_ZN5ATGun17getGunTrailLengthEv", gGunTrail)
            && sym(h, "_ZN5Piece17getAngleInRadiansEv", gGetAngle)
            && sym(h, "_ZN5Squad19haltAllATGunCrewmenEv", gHaltCrew)
            && sym(h, "_ZN11SquadGround16orderSquadToHaltEv", gSquadHalt)
            && sym(h, "_ZN5Squad23getPointerToSquadMemberEh", gSquadMember)
            && sym(h, "_ZN5Piece46setPositionAndResetTimeSinceMoved_AngleRadiansE8POSITIONf", gSetPosAng)
            && sym(h, "_ZN8Geometry52calculatePositionOfDistance_AngleRadians_AboutOriginEff8POSITION", gCalcPos)
            && sym(h, "_ZN11AppDelegate16getPointerToGameEv", gAppGame)
            && sym(h, "_ZN6UXFont8drawTextEP12SDL_RendererPKc7UXPOINTfihhhS4_", gDrawText)
            && symAny(sdl, h, "SDL_SetRenderDrawColor", gSdlSetColor) && symAny(sdl, h, "SDL_GetRenderDrawColor", gSdlGetColor)
            && symAny(sdl, h, "SDL_RenderFillRect", gSdlFill) && symAny(sdl, h, "SDL_RenderDrawRect", gSdlDraw)
            && symAny(sdl, h, "SDL_SetRenderDrawBlendMode", gSdlSetBlend) && symAny(sdl, h, "SDL_GetRenderDrawBlendMode", gSdlGetBlend);
    void* drawTarget  = dlsym(h, "_ZN9SceneGame4drawEv");
    void* beganTarget = dlsym(h, "_ZN9SceneGame10touchBeganE7UXPOINT");
    void* movedTarget = dlsym(h, "_ZN9SceneGame10touchMovedE7UXPOINT");
    void* endedTarget = dlsym(h, "_ZN9SceneGame10touchEndedE7UXPOINT");
    void* sizeTarget  = dlsym(h, "_ZN9SceneGame7setSizeE6UXSIZE");
    g_towOk = tow && drawTarget && beganTarget && movedTarget && endedTarget && sizeTarget;
    LOGI("towing symbols: tow=%d draw=%p began=%p moved=%p ended=%p size=%p", (int)tow, drawTarget, beganTarget, movedTarget, endedTarget, sizeTarget);
    if (!g_towOk) LOGI("towing disabled: some symbols are missing");

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
    if (g_towOk && TOW_ENABLED) {
        bool a = shadowhook_hook_func_addr(sizeTarget,  (void*)hook_scene_size,  (void**)&orig_scene_size)  != nullptr;
        bool b = shadowhook_hook_func_addr(drawTarget,  (void*)hook_scene_draw,  (void**)&orig_scene_draw)  != nullptr;
        bool c = shadowhook_hook_func_addr(beganTarget, (void*)hook_touch_began, (void**)&orig_touch_began) != nullptr;
        bool d = shadowhook_hook_func_addr(movedTarget, (void*)hook_touch_moved, (void**)&orig_touch_moved) != nullptr;
        bool e = shadowhook_hook_func_addr(endedTarget, (void*)hook_touch_ended, (void**)&orig_touch_ended) != nullptr;
        if (!(a && b && c && d && e)) { g_towOk = false; LOGI("towing: hook install failed (%d%d%d%d%d)", a, b, c, d, e); }
        else LOGI("towing hooks installed");
    }
    LOGI("hook installed, radio=%p notpen=%p pen=%p", radioTarget, notPenTarget, penTarget);
    return JNI_VERSION_1_6;
}
