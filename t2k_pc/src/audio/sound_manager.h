#pragma once

#include <string>

#include "game/sfx.h"
#include "audio/sfx_data.h"   // sfxdata::SfxInfo, for the pitched-chunk helper

#ifdef TS_WITH_AUDIO
#include <SDL2/SDL_mixer.h>
#endif

namespace ts {

// =============================================================================
// Sound Manager
// =============================================================================

class SoundManager {
public:
    explicit SoundManager(int num_channels = 16);
    ~SoundManager();

    // Non-copyable
    SoundManager(const SoundManager&) = delete;
    SoundManager& operator=(const SoundManager&) = delete;

    /// Start playing a music file (MOD, XM, IT, S3M, OGG, MP3).
    void playMusic(const std::string& path, float volume = -1.0f);

    /// Stop the currently playing music.
    void stopMusic();

    /// Set SoundManager's own music level (0.0 - 1.0) -- SDL_mixer's Mix_Music
    /// path only. NOTE: this does NOT move the audible soundtrack. music_sdl.cpp
    /// installs Mix_HookMusic, and Mix_VolumeMusic does not apply to a hooked
    /// stream (see that file's "COEXISTENCE WITH SoundManager" note); the live
    /// music gain is modmusic::music_set_volume, which is what the shared menu's
    /// Music Volume row is wired to in main.cpp. Kept so this class's own
    /// Mix_Music path (playMusic, which nothing currently calls) stays
    /// self-consistent. Its SFX twin below IS load-bearing.
    void setMusicVolume(float volume);

    /// Set the global sound effects volume (0.0 - 1.0).
    void setSfxVolume(float volume);

    /// Decode the embedded arcade-reference SFX (audio/sfx_data.h) into Mix_Chunks,
    /// one per SfxId, so gameplay doesn't need on-disk WAV assets. Safe no-op
    /// if TS_WITH_AUDIO is off or the mixer failed to open.
    void loadEmbeddedSfx();

    /// Drain a frame's worth of SfxEvents from the game engine (see
    /// game/sfx.h) into SDL2_mixer. Must be called EVERY FRAME even when the
    /// queue is empty: the climb-out chant re-triggers from here (see the
    /// yesChant* members below), not only in response to an event.
    ///
    /// SfxEvent::pitch is honoured for one-shots too: they resolve through the
    /// quantised pitch cache below (chunkForPitch), which hands back the baked
    /// chunk at pitch 1.0 and falls back to it once the cache is full. The YES
    /// chant is the one voice that needs a FRESH chunk per utterance, because
    /// its pitch keeps climbing while it sounds and SDL_mixer cannot retune a
    /// playing chunk. The THUNDER loop is the only voice still started
    /// unpitched.
    void drainSfx(SfxQueue& queue);

    /// Silence across a transition. The desktop twin of sfx_3ds.cpp's
    /// stop_all/stop_loops -- see the comment on the flush helper in main.cpp.
    /// stopAllSfx()   screen/state change: halt every channel, in flight or not
    /// stopSfxLoops() level change: kill HELD loops, let one-shots finish
    ///
    /// NB stopAllSfx() does NOT drop the SfxQueue, and does not need to: this
    /// line used to say it did. drainSfx() consumes the queue synchronously
    /// within the frame that pushed it, so there is never a backlog here to
    /// drop -- unlike the 3DS, whose stop_all() must clear s_pending because
    /// its SFX worker is a separate thread that has not read it yet.
    void stopAllSfx();
    void stopSfxLoops();

private:
    float music_volume_ = 0.7f;
    float sfx_volume_ = 0.8f;
    bool mixer_initialized_ = false;

#ifdef TS_WITH_AUDIO
    Mix_Chunk* embedded_[(int)SfxId::COUNT] = {};
    int        thunder_channel_ = -1;   // active loop channel, or -1
    int        yes_channel_     = -1;   // climb-out "Yes!" chant channel, or -1

    // ---- Climb-out "Yes!" chant (desktop twin of sfx_3ds.cpp's YES branch) --
    // The 3DS retunes the LIVE looping voice with ndspChnSetRate every tick, so
    // the loop shortens as the glissando climbs and the utterances accelerate
    // on their own. SDL_mixer cannot retune a playing chunk at all, and the old
    // code answered that by making LOOP_PITCH a no-op -- which left the desktop
    // looping ONE chunk at 1.0x forever: a drone that never accelerates, which
    // is exactly the "it repeats the buffer" defect.
    //
    // The desktop reproduces the MECHANISM instead of the API: play each
    // utterance as a ONE-SHOT at the current pitch, and start the next one only
    // when that one has finished. Because the glissando has climbed in the
    // meantime, every utterance is shorter and hotter than the last, so the
    // acceleration is emergent here for the same reason it is on hardware. The
    // pitch is realised by rewriting the synthesized WAV header's sampleRate
    // before Mix_LoadWAV_RW resamples it -- SDL's own resampler doing exactly
    // what ndspChnSetRate does.
    bool       yesChantActive_    = false;  // utterances should keep coming
    bool       yesChantReleasing_ = false;  // finish this one, then stop
    float      yesChantPitch_     = 1.0f;   // latest LOOP_PITCH rate multiplier
    float      yesChantVolume_    = 1.0f;   // latest LOOP_PITCH volume (taper)
    Mix_Chunk* yesChantChunk_     = nullptr; // the pitched chunk now playing

    // ---- Mid-pass pitch compensation --------------------------------------
    // A fixed pitch per utterance is not quite the 3DS's behaviour: hardware
    // retunes the voice CONTINUOUSLY, so a pass speeds up while it is sounding
    // and finishes sooner than its start-pitch implies. Using the start pitch
    // here made every desktop utterance ~14% too long, which is small per word
    // but accumulates against engine.yes_beat_ms -- and those stamps are what
    // the YES! shatter signs appear on, so the sign would drift off the word.
    //
    // Fix: play each utterance at the pitch the glissando will average ACROSS
    // that pass -- p_eff = p0 + 0.5 * rate * duration, solved by one refinement
    // (duration itself depends on p_eff). `rate` is MEASURED from successive
    // LOOP_PITCH events rather than hardcoded, so retuning GLISS_PER_TICK in
    // game_step.cpp cannot silently desync this. Measured against the engine's
    // own stamp model: mean onset error 0.132s -> 0.036s, max 0.209s -> 0.071s.
    float      yesChantPrevPitch_ = 0.0f;   // last LOOP_PITCH value seen
    uint32_t   yesChantPrevMs_    = 0;      // ... and when (SDL_GetTicks)
    float      yesChantPitchRate_ = 0.0f;   // d(pitch)/dt, per second

    /// Build a Mix_Chunk for `info` resampled as if its source rate were
    /// rate_hz * pitch -- i.e. a pitch shift. Caller owns the result.
    /// Returns nullptr if the mixer is down or decoding fails.
    Mix_Chunk* makePitchedChunk(const sfxdata::SfxInfo& info, float pitch);

    // ---- One-shot pitch cache ---------------------------------------------
    // SfxEvent::pitch was DISCARDED for every one-shot on this backend while
    // the 3DS applied it to every voice (ndspChnSetRate). It is not decorative:
    // engine.cpp pitches the explosion BOOM by energy over 0.85..1.15, so big
    // explosions sound deeper than small ones on hardware and all explosions
    // sounded identical on PC. GROOVY also alternates two voices at 0.97/1.00.
    //
    // Chunks are built on demand and NEVER FREED while the app runs: SDL_mixer
    // gives no safe way to free a chunk that might still be mixing on some
    // channel, and the alternative (free-on-evict) is a use-after-free waiting
    // for a long sample and a busy frame. The set of distinct (id, pitch) pairs
    // the game actually produces is tiny, so a small fixed table covers it and
    // anything past the cap simply falls back to the unpitched chunk -- a
    // slightly wrong pitch, never a crash and never unbounded memory.
    static constexpr int PITCH_CACHE_MAX = 48;
    struct PitchedChunk { int id = -1; int q = 0; Mix_Chunk* chunk = nullptr; };
    PitchedChunk pitchCache_[PITCH_CACHE_MAX];
    int          pitchCacheUsed_ = 0;

    /// The chunk to play for `id` at `pitch`, or the unpitched one when pitch is
    /// ~1.0 or the cache is full. Never returns a chunk the caller owns.
    Mix_Chunk* chunkForPitch(SfxId id, float pitch);

    /// Start one chant utterance at the current pitch/volume, freeing the
    /// previous utterance's chunk (which has necessarily finished, since this
    /// is only called when the channel is idle).
    void startYesUtterance();
#endif
};

} // namespace ts
