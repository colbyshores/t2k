#pragma once
// ============================================================================
// audio_thread.h — the tiny platform shim the SHARED audio code needs
// (audio_analysis.cpp and music_core.cpp), and NOTHING ELSE. Header-only,
// allocation-free at steady state, no vtables.
//
// WHY THIS EXISTS: `audio_analysis.cpp` is ~92% pure DSP maths wearing a
// libctru jacket. Under DOCTRINE.md "TARGET PARITY IS POLICY" the analyzer has
// to be ONE file both targets compile, so the five platform constructs it
// actually used (<3ds.h>, LightLock, LightEvent, Thread, svcGetSystemTick)
// move here and everything that decides how the visuals FEEL — bands,
// envelopes, flux/onset/beat, the ring, the timeline — stays shared.
//
// RULES FOR THIS FILE:
//   * Threading and timing ONLY. No constant, coefficient, band boundary or
//     threshold may ever live here. A second copy of any analyzer maths on
//     one side of the #ifdef is a parity bug by construction.
//   * The 3DS path stays PURE libctru. No <thread>, no <mutex>, no
//     <condition_variable> is ever reachable under __3DS__ — that build is
//     -fno-exceptions -fno-rtti and keeps the priority + core pinning it
//     already had (core 1 syscore at prio+1, falling back to no-preference).
//   * Semantics are identical on both sides: a STICKY event (signal latches
//     until cleared), a plain non-recursive mutex, a joinable worker.
//     Priority and affinity are QUALITY-OF-SERVICE only — the ring is
//     lossy-by-design and every window is anchored to the ring's absolute
//     sample index, so arbitrary desktop scheduling can drop samples but can
//     never desync the timeline.
// ============================================================================

#include <stdint.h>
#include <stddef.h>

#if defined(__3DS__)
#include <3ds.h>

// VFPU setup (R1) for shared-audio workers (analyzer, music core): per-thread
// FPSCR FZ|DN so DSP maths decaying toward zero never traps on denormals.
// 3DS-only; the desktop branch below never sees this helper.
static inline void vfp_thread_fz_dn() {
    unsigned fpscr;
    __asm__ volatile ("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);  // FZ | DN
    __asm__ volatile ("vmsr fpscr, %0" :: "r"(fpscr));
}
#else
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// Desktop no-op, so a SHARED worker can call this unconditionally at thread
// entry and no call site needs an #ifdef. The 3DS branch above is the one
// sanctioned platform fork in t2k_core (DOCTRINE.md: "only audio/audio_thread.h,
// the OS-thread seam, carries an #ifdef __3DS__"), and this keeps it that way:
// the fork stays here rather than spreading to every worker that has to obey
// R1. x86-64 SSE flushes denormals under its own MXCSR and needs nothing.
static inline void vfp_thread_fz_dn() {}
#endif

namespace ts {
namespace audiofx {
namespace sys {

// ---------------------------------------------------------------------------
// Mutex — non-recursive, leaf. Held only for a memcpy; never taken while
// holding another lock.
// ---------------------------------------------------------------------------
struct Mutex {
#if defined(__3DS__)
    LightLock l;
    inline void init()   { LightLock_Init(&l); }
    inline void lock()   { LightLock_Lock(&l); }
    inline void unlock() { LightLock_Unlock(&l); }
#else
    std::mutex m;
    inline void init()   {}                 // constructed at static-init time
    inline void lock()   { m.lock(); }
    inline void unlock() { m.unlock(); }
#endif
};

// ---------------------------------------------------------------------------
// Event — STICKY: signal() latches, wait() returns immediately while latched,
// clear() unlatches. Matches LightEvent(RESET_STICKY) exactly, including the
// benign race where a signal landing between wait-return and clear() is lost:
// the wait always has a timeout, so the loop just re-polls.
// ---------------------------------------------------------------------------
struct Event {
#if defined(__3DS__)
    LightEvent e;
    inline void init()   { LightEvent_Init(&e, RESET_STICKY); }
    inline void signal() { LightEvent_Signal(&e); }
    inline void clear()  { LightEvent_Clear(&e); }
    inline void wait_timeout_ns(int64_t ns) { LightEvent_WaitTimeout(&e, ns); }
#else
    std::mutex              m;
    std::condition_variable cv;
    bool                    flag = false;
    inline void init() {}
    inline void signal() {
        { std::lock_guard<std::mutex> g(m); flag = true; }
        cv.notify_all();
    }
    inline void clear() {
        std::lock_guard<std::mutex> g(m);
        flag = false;
    }
    inline void wait_timeout_ns(int64_t ns) {
        std::unique_lock<std::mutex> g(m);
        if (flag) return;
        cv.wait_for(g, std::chrono::nanoseconds(ns), [this] { return flag; });
    }
#endif
};

// ---------------------------------------------------------------------------
// Thread — one joinable worker.
//
// `prioDelta` and `coreHint` are honoured on the 3DS and IGNORED elsewhere.
// On 3DS that reproduces the original call site byte for byte: read the main
// thread's priority, run at prio + delta (libctru: higher number == LOWER
// priority, so the analyzer stays below the music thread and can never glitch
// playback), 16 KB stack, try `coreHint` then fall back to -2 (no preference,
// read from the Exheader).
// ---------------------------------------------------------------------------
struct Thread {
#if defined(__3DS__)
    ::Thread h = nullptr;

    inline bool start(void (*fn)(void*), void* arg, size_t stackBytes,
                      int prioDelta, int coreHint) {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        h = threadCreate(fn, arg, stackBytes, prio + prioDelta, coreHint, false);
        if (!h)
            h = threadCreate(fn, arg, stackBytes, prio + prioDelta, -2, false);
        return h != nullptr;
    }

    inline void join() {
        if (!h) return;
        threadJoin(h, U64_MAX);
        threadFree(h);
        h = nullptr;
    }
#else
    std::thread t;

    inline bool start(void (*fn)(void*), void* arg, size_t stackBytes,
                      int prioDelta, int coreHint) {
        (void)stackBytes; (void)prioDelta; (void)coreHint;
        t = std::thread(fn, arg);
        return true;
    }

    inline void join() {
        if (t.joinable()) t.join();
    }
#endif
};

// ---------------------------------------------------------------------------
// Monotonic ticks. Deliberately split into "raw ticks" + "delta to micros" so
// the 3DS keeps its EXACT original arithmetic
// ((tick1 - tick0) / (SYSCLOCK_ARM11 / 1000000)) rather than converting each
// endpoint and subtracting. Feeds analysis_fft_micros(), an as-yet unconsumed
// diagnostic with zero influence on any feature value.
// ---------------------------------------------------------------------------
inline uint64_t ticks_now() {
#if defined(__3DS__)
    return (uint64_t)svcGetSystemTick();
#else
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

inline uint32_t ticks_delta_us(uint64_t delta) {
#if defined(__3DS__)
    return (uint32_t)(delta / (SYSCLOCK_ARM11 / 1000000));
#else
    return (uint32_t)(delta / 1000u);       // ticks_now() is nanoseconds here
#endif
}

// Milliseconds for the music core's request SETTLE window (music_core.cpp).
// Only DIFFERENCES are ever used, so the epoch is irrelevant — but the 3DS side
// stays literally osGetTime(), which is what that code has always called, so
// the settle behaviour on the handheld is unchanged. 64-bit on both, so a long
// session cannot wrap it the way ticks_delta_us()'s uint32 would.
inline int64_t wall_ms() {
#if defined(__3DS__)
    return (int64_t)osGetTime();
#else
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// ---------------------------------------------------------------------------
// The two release/acquire accesses the timeline write index needs. GCC and
// Clang builtins — devkitARM is GCC and the desktop build only enables its
// flags for GNU|Clang, so this is the same codegen the 3DS already shipped
// (no libatomic dependency, no std::atomic ABI question on ARMv6K).
// ---------------------------------------------------------------------------
inline void atomic_store_release_u32(volatile uint32_t* p, uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
#else
    *p = v;
#endif
}

inline uint32_t atomic_load_acquire_u32(volatile uint32_t* p) {
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#else
    return *p;
#endif
}

} // namespace sys
} // namespace audiofx
} // namespace ts
