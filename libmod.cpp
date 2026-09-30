// libmod.so - авто-ремонт сломавшегося двигателя, Firefight 13.01 (arm64)
// НЕ СОБИРАЛОСЬ и не тестировалось: каркас. Нужен Dobby (https://github.com/jmpews/Dobby).
#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <unordered_map>
#include <mutex>
#include "dobby.h"

#define TAG "libmod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ---- настройки ----
static const float REPAIR_SECONDS = 45.0f;   // время ремонта
static const float GRACE_SECONDS  = 20.0f;   // иммунитет к новой поломке после ремонта
static const bool  TEST_ALL_BROKEN = true;   // ТЕСТ: каждая новая техника ломается при первом тике
static const bool  LOG_STATE_CHANGES = true; // лог смены isImmobilized/disabledBecause (найти другие виды поломок)

// ---- экспортированные функции libmain.so (Itanium mangling) ----
// Piece/Vehicle: this-указатель в x0, аргументы по ABI AArch64.
using GetStatus_t   = unsigned char (*)(void* piece);
using IsImmob_t     = unsigned char (*)(void* piece);
using SetImmob_t    = void (*)(void* piece, unsigned char v);
using GetDisabled_t = unsigned char (*)(void* vehicle);
using SetDisabled_t = void (*)(void* vehicle, unsigned char v);
using Halt_t        = void (*)(void* piece);
using Breakdown_t   = unsigned char (*)(void* vehicle, unsigned char gridType, float dt);

static GetStatus_t   getStatus;
static IsImmob_t     isImmobilized;
static SetImmob_t    setImmobilized;
static GetDisabled_t getDisabledBecause;
static SetDisabled_t setDisabledBecause;
static Halt_t        haltCurrentOption;
static Breakdown_t   orig_breakdown;

struct State { float repairLeft = -1; float grace = 0; bool seen = false; int lastImm = -1; int lastWhy = -1; };
static std::unordered_map<void*, State> g_state;
static std::mutex g_mu;

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

    unsigned char r = orig_breakdown(v, grid, dt);

    if (LOG_STATE_CHANGES) {
        int imm = isImmobilized(v), why = getDisabledBecause(v);
        if (imm != s.lastImm || why != s.lastWhy) {
            LOGI("vehicle %p: immobilized=%d why=%d status=%d", v, imm, why, getStatus(v));
            s.lastImm = imm; s.lastWhy = why;
        }
    }
    bool alive  = getStatus(v) == 0;
    bool broken = isImmobilized(v) != 0 && getDisabledBecause(v) == 1; // 1 = обычная поломка; 8 = brew up
    if (alive && broken) {
        if (s.repairLeft < 0) s.repairLeft = REPAIR_SECONDS;
        s.repairLeft -= dt;
        // TODO: проверять, что в экипаже есть живой (Man::getDisability), и что нет боя рядом
        if (s.repairLeft <= 0) {
            setImmobilized(v, 0);
            setDisabledBecause(v, 0);
            s.repairLeft = -1;
            s.grace = GRACE_SECONDS;
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

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
    void* h = dlopen("libmain.so", RTLD_NOW | RTLD_NOLOAD);   // libmain уже загружена SDLActivity
    if (!h) { LOGI("libmain.so not loaded"); return JNI_VERSION_1_6; }

    bool ok = sym(h, "_ZN5Piece9getStatusEv", getStatus)
           && sym(h, "_ZN5Piece13isImmobilizedEv", isImmobilized)
           && sym(h, "_ZN5Piece14setImmobilizedEh", setImmobilized)
           && sym(h, "_ZN7Vehicle18getDisabledBecauseEv", getDisabledBecause)
           && sym(h, "_ZN7Vehicle18setDisabledBecauseEh", setDisabledBecause)
           && sym(h, "_ZN5Piece17haltCurrentOptionEv", haltCurrentOption);
    void* target = dlsym(h, "_ZN7Vehicle50considerIfVehicleBreaksDownOverTerrain_TimeElapsedEhf");
    if (!ok || !target) { LOGI("init failed"); return JNI_VERSION_1_6; }

    DobbyHook(target, (void*)hook_breakdown, (void**)&orig_breakdown);
    LOGI("hook installed");
    return JNI_VERSION_1_6;
}
