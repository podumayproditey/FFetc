// libmod.so - авто-ремонт, взрывы, траки + буксировка ПТ-орудий, Firefight 13.0.2 (arm64)
// Хуки: ShadowHook (Bytedance).
#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <mutex>
#include <atomic>
#include <unordered_set>
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

static void towOnDestroy(void* v);   // буксировка: тягач уничтожен -> отцепить
// Любое уничтожение техники (пробитие, взрыв рядом, арта...). Если ваниль назначила взрыв БК
// (таймер >0, значит в технике есть ВВ-боеприпасы), решаем сами: сразу / с горением и искрами / без взрыва.
static void hook_destroy(void* game, void* v, unsigned char ks, unsigned char ki, unsigned char method, const char* hitOn) {
    g_game = game;
    towOnDestroy(v);                                       // если это тягач — отцепляем орудие
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

// ======================================================================
// ПЕРЕВОЗКА ПТ-ОРУДИЙ ТЕХНИКОЙ (буксировка)
// ----------------------------------------------------------------------
// Как пользоваться (интерфейс игры не меняется):
//   1. Подгоните технику к орудию.
//   2. Перетащите маркер (goto) расчёта орудия НА технику.
//   3. Расчёт цепляет орудие (TOW_HOOK_SECONDS), орудие «привязывается» к технике,
//      расчёт «привязывается» к орудию. Маркер расчёта едет вместе с техникой.
//   4. Чтобы отцепить: перетащите маркер расчёта в сторону от техники.
//      Орудие остаётся на месте, разворачивается стволом по ходу техники, расчёт идёт к новому маркеру.
// Орудия с <moveable>no</moveable> (HeavyWeapon::getIsMoveable()==0) цеплять нельзя.
// Физика: жёсткий дышловой прицеп. Точка прицепа сзади техники, орудие тянется за ней,
// угол орудия = направление от точки прицепа к орудию (ствол назад), поэтому на поворотах орудие
// «заносит» по дуге как настоящий прицеп. Ограничение излома — TOW_MAX_ARTIC_DEG.
// ======================================================================
static const bool  TOW_ENABLED          = true;
static const bool  TOW_DEBUG_RADIO      = false;
static const float TOW_SCAN_INTERVAL    = 0.25f;  // как часто техника ищет орудие, маркер которого стоит на ней (сек)
static const float TOW_HOOK_SECONDS     = 5.0f;   // сколько расчёт цепляет орудие (техника должна стоять)
static const float TOW_HOOK_MAX_WAIT    = 90.0f;  // если за это время не дотащили орудие до техники — отмена
static const float TOW_RANGE_MULT       = 3.0f;   // макс. расстояние орудие↔техника для начала = MULT * (длина техники + длина орудия)
static const float TOW_SNAP_MULT        = 1.0f;   // орудие должно быть не дальше MULT*(Lт+Lо) от точки прицепа, чтобы защёлкнуться
static const float TOW_PICK_MARGIN      = 12.0f;  // насколько близко к технике должен стоять маркер (сверх габарита)
static const float TOW_MAX_HOOK_SPEED   = 3.0f;   // пикс/с: быстрее — считаем, что техника едет, таймер прицепа сбрасывается
static const float TOW_GAP              = 2.0f;   // зазор между точкой прицепа и осью орудия
static const float TOW_HITCH_EXTRA      = 1.0f;   // вынос точки прицепа за корму
static const float TOW_MAX_ARTIC_DEG    = 85.0f;  // максимальный излом «тягач–орудие»
static const bool  TOW_BARREL_REAR      = true;   // в походе ствол смотрит назад (как у реальных ПТО), иначе — вперёд
static const bool  TOW_DEPLOY_FACE_FORWARD = true;// при отцепе повернуть орудие стволом по ходу техники
static const float TOW_MASS_RATIO_MIN   = 0.8f;   // масса техники должна быть >= RATIO * масса орудия (0 = не проверять)
static const float TOW_DRAG_EPS         = 30.0f;  // на сколько игрок должен сдвинуть маркер, чтобы это считалось «отцепить»
static const float TOW_MARK_STEP        = 8.0f;   // маркер перезаписываем, когда техника уехала от него дальше этого
static const float TOW_SNAP_SECONDS     = 1.5f;   // плавная «подтяжка» орудия и расчёта после защёлкивания
static const float TOW_CREW_SMOOTH      = 6.0f;   // скорость сглаживания посадки расчёта на орудие

static const float PI_F = 3.14159265358979f;

struct V2 { float x, y; };

using GetF_t         = float (*)(void*);
using GetPosAng_t    = void (*)(void* piece, POSITION* p, float* a);
using SetPosAng_t    = void (*)(void* piece, POSITION p, float a);
using GetPieceSI_t   = void* (*)(void* game, unsigned char side, unsigned char i);
using GetSquadSS_t   = void* (*)(void* game, unsigned char side, unsigned char squad);
using SetGoto_t      = void (*)(void* squad, POSITION p);
using Halt0_t        = void (*)(void* obj);
using GetMoveable_t  = unsigned char (*)(void* gun);
using ULFn2_t        = unsigned long (*)(void*, float);
using ATMove_t       = unsigned long (*)(void* gun, POSITION p, float dt);
using ManMove_t      = unsigned long (*)(void* man, POSITION p, unsigned char speed, void* world, float dt);
using Squash_t       = unsigned long (*)(void* game, void* veh);

static GetPosAng_t   getPosAngle;
static SetPosAng_t   setPosAngle;
static GetF_t        getSpeedPx, getAvoidLen, getAvoidWid, getHullLen, getGotoXf, getGotoYf, getGunMassKg;
static GetPieceSI_t  getPieceSideI;
static GetSquadSS_t  getSquadSideSquad;
static SetGoto_t     setSquadGoto;
static Halt0_t       manHalt, setGunDisassembled;
static GetMoveable_t getIsMoveable;
static void*         g_vtATGun, *g_vtMan;       // vptr объектов классов (vtable+16)
static ATMove_t      orig_atmove;
static ManMove_t     orig_manmove;
static ULFn2_t       orig_assemble;
static Squash_t      orig_squash;
static bool          g_towReady = false;

// ---- углы: ориентацию «угол → направление» игра нигде не документирует, определяем по движению техники ----
// Кандидаты: dir(a) = (cos(s*a + o), sin(s*a + o)), s = ±1, o = 0, π/2, π, 3π/2 (8 штук).
// Пока данных нет — берём «0 = вверх, по часовой»: dir = (sin a, -cos a).
static int   g_convS = 1;
static float g_convO = -PI_F / 2;
static bool  g_convReady = false, g_convFrozen = false;
static double g_convScore[8];
static int    g_convN = 0;
static std::unordered_map<void*, POSITION> g_convAnchor;

static V2 dirOf(float a) { float t = g_convS * a + g_convO; return V2{cosf(t), sinf(t)}; }
static float wrapPi(float a) {
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}
static float angleOf(V2 d) {
    float a = g_convS * (atan2f(d.y, d.x) - g_convO);
    a = fmodf(a, 2 * PI_F);
    if (a < 0) a += 2 * PI_F;
    return a;
}
static float dist2d(float ax, float ay, float bx, float by) { return hypotf(ax - bx, ay - by); }

static void convSample(void* v, POSITION p, float a) {
    if (g_convFrozen) return;
    auto it = g_convAnchor.find(v);
    if (it == g_convAnchor.end()) { g_convAnchor[v] = p; return; }
    float dx = p.x - it->second.x, dy = p.y - it->second.y, d = hypotf(dx, dy);
    if (d < 6.0f) return;
    it->second = p;
    if (d > 60.0f) return;                     // телепорт/рывок — не считаем
    float mx = dx / d, my = dy / d;
    for (int k = 0; k < 8; k++) {
        int s = (k & 1) ? -1 : 1; float o = (k >> 1) * (PI_F / 2);
        float t = s * a + o;
        g_convScore[k] += mx * cosf(t) + my * sinf(t);
    }
    g_convN++;
    if (g_convN < 8) return;
    int best = 0, second = -1;
    for (int k = 1; k < 8; k++) if (g_convScore[k] > g_convScore[best]) best = k;
    for (int k = 0; k < 8; k++) if (k != best && (second < 0 || g_convScore[k] > g_convScore[second])) second = k;
    double b = g_convScore[best] / g_convN, c = g_convScore[second] / g_convN;
    if (b >= 0.6 && b - c >= 0.3) {
        g_convS = (best & 1) ? -1 : 1; g_convO = (best >> 1) * (PI_F / 2);
        if (!g_convReady) LOGI("tow: angle convention found s=%d o=%.2f (score %.2f)", g_convS, g_convO, b);
        g_convReady = true;
    }
    if (g_convN >= 60 && g_convReady) g_convFrozen = true;   // дальше не меняем, чтобы не было скачков
}

// ---- состояние буксировки ----
struct Tow {
    void* veh = nullptr; void* gun = nullptr; void* sq = nullptr;
    unsigned char side = 0, squadI = 0;
    bool towed = false;                 // false = расчёт ещё цепляет
    float timer = 0, waited = 0, snapT = 0, haltT = 0;
    float markX = 0, markY = 0;         // положение маркера после нашей последней записи
    std::vector<void*> crew;
};
struct ScanState { float in = 0, msgCd = 0; };
static std::unordered_map<void*, Tow> g_tow;             // ключ — тягач
static std::unordered_set<void*> g_towGuns;             // орудия в любой фазе (цепляют/везут)
static std::unordered_set<void*> g_towedGuns;           // орудия, которые уже едут
static std::unordered_set<void*> g_towCrew;             // расчёт едущих орудий
static std::unordered_map<void*, ScanState> g_scan;
static std::atomic<int> g_towedCount{0};

enum { REL_CANCEL = 0, REL_DRAG = 1, REL_TRACTOR_LOST = 2, REL_GUN_LOST = 3 };

static bool isATGun(void* p) { return p && g_vtATGun && *(void**)p == g_vtATGun; }
static bool isMan(void* p)   { return p && g_vtMan && *(void**)p == g_vtMan; }

static float pieceLen(void* p, float fallback) {
    float l = getAvoidLen ? getAvoidLen(p) : 0.0f;
    if (!(l > 1.0f) || l > 500.0f) l = fallback;
    return l;
}
static float pieceWid(void* p, float fallback) {
    float w = getAvoidWid ? getAvoidWid(p) : 0.0f;
    if (!(w > 1.0f) || w > 500.0f) w = fallback;
    return w;
}
static float vehLen(void* v) {
    float l = getAvoidLen ? getAvoidLen(v) : 0.0f;
    if (!(l > 1.0f) || l > 500.0f) l = getHullLen ? getHullLen(v) : 0.0f;
    if (!(l > 1.0f) || l > 500.0f) l = 24.0f;
    return l;
}
static float gunLen(void* g) { return pieceLen(g, 12.0f); }

static void towMsg(void* gun, const char* fmt, unsigned char urg = URG_GREEN) {
    char buf[96];
    snprintf(buf, sizeof buf, fmt, (int)getSquadI(gun) + 1);
    sendMsg(gun, buf, urg);
}

static void squadGotoTo(void* sq, POSITION p, float& outX, float& outY) {
    setSquadGoto(sq, p);
    outX = getGotoXf(sq); outY = getGotoYf(sq);    // читаем обратно: игра могла подправить координаты
}

static void towRelease(void* v, int reason) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_tow.find(v);
    if (it == g_tow.end()) return;
    Tow t = it->second;
    g_tow.erase(it);
    g_towGuns.erase(t.gun);
    if (t.towed) {
        g_towedGuns.erase(t.gun);
        for (void* c : t.crew) g_towCrew.erase(c);
        g_towedCount--;
        if (g_towedCount < 0) g_towedCount = 0;
    }
    if (!t.towed) {
        LOGI("tow: hookup cancelled veh=%p gun=%p", v, t.gun);
        return;
    }
    if (getStatus(t.gun) != 0) return;
    POSITION gp; float ga; getPosAngle(t.gun, &gp, &ga);
    if (reason == REL_DRAG && TOW_DEPLOY_FACE_FORWARD && getStatus(v) == 0) {
        POSITION vp; float va; getPosAngle(v, &vp, &va);
        // Орудие в походе смотрит назад — при отцепе разворачиваем стволом по ходу техники
        setPosAngle(t.gun, gp, TOW_BARREL_REAR ? va : ga);
    }
    if (reason == REL_TRACTOR_LOST) {                // техника потеряна: расчёт остаётся у орудия, а не бежит за маркером
        float mx, my; squadGotoTo(t.sq, gp, mx, my);
    }
    if (reason != REL_GUN_LOST) towMsg(t.gun, "Squad %d: gun unhitched", URG_YELLOW);
    LOGI("tow: released veh=%p gun=%p reason=%d", v, t.gun, reason);
}

static void towOnDestroy(void* v) {      // из hook_destroy: тягач уничтожен — сразу отцепляем
    if (!g_towReady || !v) return;
    towRelease(v, REL_TRACTOR_LOST);
}

// Все живые люди этого расчёта + количество орудий в отряде
static void collectSquad(unsigned char side, unsigned char squadI, std::vector<void*>& crew, int& guns) {
    crew.clear(); guns = 0;
    for (int i = 0; i < 255; i++) {
        void* p = getPieceSideI(g_game, side, (unsigned char)i);
        if (!p) continue;
        if (getSide(p) != side || getSquadI(p) != squadI) continue;
        if (isATGun(p) && getStatus(p) == 0) guns++;
        else if (isMan(p) && getStatus(p) == 0) crew.push_back(p);
    }
}

static void towScan(void* v, POSITION vp, ScanState& sc) {
    if (isImmobilized(v)) return;
    if (getSpeedPx(v) > TOW_MAX_HOOK_SPEED) return;
    unsigned char side = getSide(v);
    float lt = vehLen(v);
    float pick = fmaxf(lt, pieceWid(v, 14.0f)) * 0.75f + TOW_PICK_MARGIN;

    void* best = nullptr; float bd = 1e30f; void* bestSq = nullptr;
    for (int i = 0; i < 255; i++) {
        void* g = getPieceSideI(g_game, side, (unsigned char)i);
        if (!isATGun(g) || getStatus(g) != 0 || getSide(g) != side || g_towGuns.count(g)) continue;
        void* sq = getSquadSideSquad(g_game, side, getSquadI(g));
        if (!sq) continue;
        float mx = getGotoXf(sq), my = getGotoYf(sq);
        if (dist2d(mx, my, vp.x, vp.y) > pick) continue;           // маркер расчёта не на этой технике
        POSITION gp = getPosition(g);
        float dg = dist2d(gp.x, gp.y, vp.x, vp.y);
        if (dg > TOW_RANGE_MULT * (lt + gunLen(g))) continue;      // орудие слишком далеко
        if (dg < bd) { bd = dg; best = g; bestSq = sq; }
    }
    if (!best) return;
    bool canMsg = sc.msgCd <= 0;
    auto refuse = [&](const char* text) { if (canMsg) { towMsg(best, text, URG_ORANGE); sc.msgCd = 20.0f; } };

    if (getIsMoveable(best) == 0)        { refuse("Squad %d: this gun is immovable, cannot tow"); return; }
    if (!g_convReady)                    { refuse("Squad %d: tow calibrating, drive a vehicle a little first"); return; }
    if (TOW_MASS_RATIO_MIN > 0 && getMassKG && getGunMassKg) {
        float mv = getMassKG(v), mg = getGunMassKg(best);
        if (mv > 0 && mg > 0 && mv < mg * TOW_MASS_RATIO_MIN) { refuse("Squad %d: vehicle too light for this gun"); return; }
    }
    unsigned char squadI = getSquadI(best);
    std::vector<void*> crew; int guns = 0;
    collectSquad(side, squadI, crew, guns);
    if (guns > 1)                        { refuse("Squad %d: can tow only one gun per squad"); return; }

    Tow t;
    t.veh = v; t.gun = best; t.sq = bestSq; t.side = side; t.squadI = squadI;
    t.timer = TOW_HOOK_SECONDS; t.markX = getGotoXf(bestSq); t.markY = getGotoYf(bestSq);
    g_tow[v] = t;
    g_towGuns.insert(best);
    towMsg(best, "Squad %d: hooking up gun", URG_YELLOW);
    LOGI("tow: hooking veh=%p gun=%p squad=%d", v, best, (int)squadI);
}

static void towAttach(Tow& t, POSITION vp) {
    int guns = 0;
    t.towed = true; t.snapT = 0; t.haltT = 0;
    collectSquad(t.side, t.squadI, t.crew, guns);
    g_towedGuns.insert(t.gun);
    for (void* c : t.crew) g_towCrew.insert(c);
    g_towedCount++;
    setGunDisassembled(t.gun);                       // орудие в походном положении
    haltCurrentOption(t.gun);
    for (void* c : t.crew) manHalt(c);
    squadGotoTo(t.sq, vp, t.markX, t.markY);         // маркер расчёта теперь едет с техникой
    towMsg(t.gun, "Squad %d: gun hitched", URG_GREEN);
    LOGI("tow: hitched veh=%p gun=%p crew=%d", t.veh, t.gun, (int)t.crew.size());
}

// Один шаг буксировки (вызывается из тика техники)
static void towStep(void* v, POSITION p, float a, float dt) {
    auto it = g_tow.find(v);
    if (it == g_tow.end()) return;
    Tow& t = it->second;
    if (getStatus(t.gun) != 0) { towRelease(v, REL_GUN_LOST); return; }

    POSITION gp; float ga; getPosAngle(t.gun, &gp, &ga);
    float mx = getGotoXf(t.sq), my = getGotoYf(t.sq);
    V2 fwd = dirOf(a);
    float lt = vehLen(v), lg = gunLen(t.gun);
    float D = lg * 0.5f + TOW_GAP;                                   // длина дышла: от точки прицепа до оси орудия
    float hd = lt * 0.5f + TOW_HITCH_EXTRA;
    POSITION H{p.x - fwd.x * hd, p.y - fwd.y * hd};                  // точка прицепа сзади техники

    if (!t.towed) {                                                  // ---- фаза «цепляют» ----
        float pick = fmaxf(lt, pieceWid(v, 14.0f)) * 0.75f + TOW_PICK_MARGIN;
        float range = TOW_RANGE_MULT * (lt + lg);
        if (dist2d(mx, my, p.x, p.y) > pick * 2.0f || dist2d(gp.x, gp.y, p.x, p.y) > range * 1.5f) {
            towRelease(v, REL_CANCEL); return;                       // маркер увели или техника уехала
        }
        t.waited += dt;
        if (getSpeedPx(v) > TOW_MAX_HOOK_SPEED * 2.0f) t.timer = TOW_HOOK_SECONDS;   // техника едет — прицеп идёт заново
        else if (t.timer > 0) t.timer -= dt;
        if (t.timer <= 0 && dist2d(gp.x, gp.y, H.x, H.y) <= (lt + lg) * TOW_SNAP_MULT) { towAttach(t, p); return; }
        if (t.waited > TOW_HOOK_MAX_WAIT) { towRelease(v, REL_CANCEL); return; }
        return;
    }

    // ---- фаза «едем» ----
    // Игрок сдвинул маркер расчёта от техники → отцепить
    if (dist2d(mx, my, t.markX, t.markY) > TOW_DRAG_EPS) { towRelease(v, REL_DRAG); return; }
    // Маркер едет вместе с техникой
    if (dist2d(p.x, p.y, t.markX, t.markY) > TOW_MARK_STEP) squadGotoTo(t.sq, p, t.markX, t.markY);

    // Физика прицепа: орудие остаётся на расстоянии D от НОВОЙ точки прицепа на линии «точка прицепа → прежнее положение орудия»
    V2 rear{-fwd.x, -fwd.y};
    V2 w{gp.x - H.x, gp.y - H.y};
    float wl = hypotf(w.x, w.y);
    if (wl < 1e-3f || wl > D * 4.0f) w = rear;                       // вырожденный случай/рывок: ставим строго сзади
    else { w.x /= wl; w.y /= wl; }
    float ang = atan2f(rear.x * w.y - rear.y * w.x, w.x * rear.x + w.y * rear.y);   // угол излома со знаком
    float maxA = TOW_MAX_ARTIC_DEG * PI_F / 180.0f;
    if (fabsf(ang) > maxA) {                                         // не даём сложиться «ножницами»
        ang = copysignf(maxA, ang);
        float c = cosf(ang), s = sinf(ang);
        w = V2{rear.x * c - rear.y * s, rear.x * s + rear.y * c};
    }
    POSITION target{H.x + w.x * D, H.y + w.y * D};
    V2 b = TOW_BARREL_REAR ? w : V2{-w.x, -w.y};                     // куда смотрит ствол
    float targetAngle = angleOf(b);

    float k = (t.snapT < TOW_SNAP_SECONDS) ? 1.0f - expf(-dt * 5.0f) : 1.0f;
    t.snapT += dt;
    POSITION np{gp.x + (target.x - gp.x) * k, gp.y + (target.y - gp.y) * k};
    float na = ga + wrapPi(targetAngle - ga) * k;
    setPosAngle(t.gun, np, na);
    setGunDisassembled(t.gun);

    // Расчёт сидит на орудии (слоты по бокам, ближе к станинам)
    V2 nv{-b.y, b.x};
    float kc = 1.0f - expf(-dt * TOW_CREW_SMOOTH);
    bool doHalt = (t.haltT -= dt) <= 0;
    if (doHalt) t.haltT = 1.0f;
    int idx = 0;
    for (void* c : t.crew) {
        if (getStatus(c) != 0) continue;
        float lat = ((idx & 1) ? 1.0f : -1.0f) * lg * 0.35f;
        float lon = -D * 0.3f - (idx / 2) * lg * 0.2f;
        POSITION cp = getPosition(c);
        POSITION ct{np.x + nv.x * lat + b.x * lon, np.y + nv.y * lat + b.y * lon};
        POSITION cn{cp.x + (ct.x - cp.x) * kc, cp.y + (ct.y - cp.y) * kc};
        setPosAngle(c, cn, a);
        if (doHalt) manHalt(c);
        idx++;
    }
}

static void towTick(void* v, float dt) {
    if (!TOW_ENABLED || !g_towReady || !g_game || !v) return;
    POSITION p; float a;
    if (getStatus(v) != 0) {                                         // техника уничтожена
        towRelease(v, REL_TRACTOR_LOST);
        g_convAnchor.erase(v); g_scan.erase(v);
        return;
    }
    getPosAngle(v, &p, &a);
    convSample(v, p, a);
    if (g_tow.count(v)) { towStep(v, p, a, dt); return; }
    ScanState& sc = g_scan[v];
    sc.msgCd -= dt; sc.in -= dt;
    if (sc.in > 0) return;
    sc.in = TOW_SCAN_INTERVAL;
    towScan(v, p, sc);
}

// ---- блокировка «самодеятельности» едущих орудий и расчёта ----
// Типы возврата у этих функций неизвестны, поэтому возвращаем/пробрасываем регистр x0 целиком (0 = «ничего не сделано»)
static bool towedGun(void* g)  { if (g_towedCount.load() <= 0) return false; std::lock_guard<std::recursive_mutex> lk(g_mu); return g_towedGuns.count(g) != 0; }
static bool towedCrew(void* m) { if (g_towedCount.load() <= 0) return false; std::lock_guard<std::recursive_mutex> lk(g_mu); return g_towCrew.count(m) != 0; }
static bool towingVeh(void* v) {
    if (g_towedCount.load() <= 0) return false;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_tow.find(v);
    return it != g_tow.end() && it->second.towed;
}
static unsigned long hook_atmove(void* gun, POSITION p, float dt) {
    if (towedGun(gun)) return 0;                     // везут — сам не ползёт
    return orig_atmove(gun, p, dt);
}
static unsigned long hook_manmove(void* man, POSITION p, unsigned char speed, void* world, float dt) {
    if (towedCrew(man)) return 0;                    // расчёт сидит на орудии, пешком не ходит
    return orig_manmove(man, p, speed, world, dt);
}
static unsigned long hook_assemble(void* gun, float dt) {
    if (towedGun(gun)) return 0;                     // в походе не разворачиваем
    return orig_assemble(gun, dt);
}
static unsigned long hook_squash(void* game, void* veh) {
    if (towingVeh(veh)) return 0;                    // тягач с прицепом не давит своих (и расчёт на орудии)
    return orig_squash(game, veh);
}

template <class T> static bool symOpt(void* h, const char* n, T& out) {
    out = reinterpret_cast<T>(dlsym(h, n));
    if (!out) LOGI("tow: symbol not found: %s", n);
    return out != nullptr;
}
static bool towResolve(void* h) {
    void* vtA = dlsym(h, "_ZTV5ATGun");
    void* vtM = dlsym(h, "_ZTV3Man");
    if (vtA) g_vtATGun = (char*)vtA + 16;            // vptr указывает на vtable+16 (Itanium ABI)
    if (vtM) g_vtMan   = (char*)vtM + 16;
    bool ok = vtA && vtM
        && symOpt(h, "_ZN5Piece24getPosition_AngleRadiansEP8POSITIONPf", getPosAngle)
        && symOpt(h, "_ZN5Piece46setPositionAndResetTimeSinceMoved_AngleRadiansE8POSITIONf", setPosAngle)
        && symOpt(h, "_ZN5Piece25getSpeedInPixelsPerSecondEv", getSpeedPx)
        && symOpt(h, "_ZN5Piece30getAvoidableLengthForThisPieceEv", getAvoidLen)
        && symOpt(h, "_ZN5Piece29getAvoidableWidthForThisPieceEv", getAvoidWid)
        && symOpt(h, "_ZN4Game23getPointerToPieceSide_IEhh", getPieceSideI)
        && symOpt(h, "_ZN4Game27getPointerToSquadSide_SquadEhh", getSquadSideSquad)
        && symOpt(h, "_ZN11SquadGround8getGotoXEv", getGotoXf)
        && symOpt(h, "_ZN11SquadGround8getGotoYEv", getGotoYf)
        && symOpt(h, "_ZN11SquadGround7setGotoE8POSITION", setSquadGoto)
        && symOpt(h, "_ZN3Man21actionInfantrymanHaltEv", manHalt)
        && symOpt(h, "_ZN11HeavyWeapon13getIsMoveableEv", getIsMoveable)
        && symOpt(h, "_ZN11HeavyWeapon20setFullyDisassembledEv", setGunDisassembled);
    symOpt(h, "_ZN7Vehicle13getHullLengthEv", getHullLen);          // необязательные
    symOpt(h, "_ZN11HeavyWeapon11getMassInKgEv", getGunMassKg);
    g_towReady = ok;
    LOGI("tow: resolve %s", ok ? "ok" : "FAILED (tow disabled)");
    return ok;
}

static unsigned char hook_breakdown(void* v, unsigned char grid, float dt) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    towTick(v, dt);                    // буксировка орудий (тик тягача)
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
    // ---- буксировка ПТ-орудий ----
    if (TOW_ENABLED && towResolve(h)) {
        void* atMoveT   = dlsym(h, "_ZN5ATGun29actionATGunMoveTo_TimeElapsedE8POSITIONf");
        void* manMoveT  = dlsym(h, "_ZN3Man55actionInfantrymanMoveTo_MovementSpeed_World_TimeElapsedE8POSITIONhP5Worldf");
        void* assembleT = dlsym(h, "_ZN11HeavyWeapon19assembleTimeElapsedEf");
        void* squashT   = dlsym(h, "_ZN4Game44checkIfVehiclesSquashInfantryAndHeavyWeaponsEP7Vehicle");
        if (atMoveT)   shadowhook_hook_func_addr(atMoveT,   (void*)hook_atmove,   (void**)&orig_atmove);
        if (manMoveT)  shadowhook_hook_func_addr(manMoveT,  (void*)hook_manmove,  (void**)&orig_manmove);
        if (assembleT) shadowhook_hook_func_addr(assembleT, (void*)hook_assemble, (void**)&orig_assemble);
        if (squashT)   shadowhook_hook_func_addr(squashT,   (void*)hook_squash,   (void**)&orig_squash);
        LOGI("tow hooks: atmove=%p manmove=%p assemble=%p squash=%p", atMoveT, manMoveT, assembleT, squashT);
    }
    LOGI("hook installed, radio=%p notpen=%p pen=%p", radioTarget, notPenTarget, penTarget);
    return JNI_VERSION_1_6;
}
