#include "sound_manager.h"
#include "audio/sfx_data.h"
#include "audio/sfx_mix.h"

#ifdef TS_WITH_AUDIO
#include <SDL2/SDL_timer.h>   // SDL_GetTicks -- the chant's glissando slope clock
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace ts {

// Both sinks index TABLE by (int)SfxId -- enum order IS bank order. Catch a
// drifted enum or a regenerated bank missing an id at compile time.
static_assert(sizeof(sfxdata::TABLE) / sizeof(sfxdata::TABLE[0]) == (size_t)SfxId::COUNT,
              "SfxId order/count must match the generated sfx_data bank TABLE");

SoundManager::SoundManager(int num_channels) {
#ifdef TS_WITH_AUDIO
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 512) < 0) {
        std::fprintf(stderr, "Warning: Could not initialize audio: %s\n", Mix_GetError());
        return;
    }
    mixer_initialized_ = true;
    Mix_AllocateChannels(num_channels);
#else
    (void)num_channels;
#endif
}

SoundManager::~SoundManager() {
#ifdef TS_WITH_AUDIO
    // Halt before freeing: the chant chunk is owned by this object and may
    // still be mixing when the game quits mid-climb-out.
    if (mixer_initialized_) Mix_HaltChannel(-1);
    if (yesChantChunk_) { Mix_FreeChunk(yesChantChunk_); yesChantChunk_ = nullptr; }

    for (auto* chunk : embedded_) {
        if (chunk) Mix_FreeChunk(chunk);
    }

    if (mixer_initialized_) {
        Mix_HaltMusic();
        Mix_CloseAudio();
    }
#endif
}

void SoundManager::playMusic(const std::string& path, float volume) {
#ifdef TS_WITH_AUDIO
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "Warning: Music file not found: %s\n", path.c_str());
        return;
    }

    Mix_Music* music = Mix_LoadMUS(path.c_str());
    if (!music) {
        std::fprintf(stderr, "Warning: Could not play music %s: %s\n",
                     path.c_str(), Mix_GetError());
        return;
    }

    float vol = (volume >= 0.0f) ? volume : music_volume_;
    Mix_VolumeMusic(static_cast<int>(vol * MIX_MAX_VOLUME));
    Mix_PlayMusic(music, -1);
#else
    (void)path; (void)volume;
#endif
}

void SoundManager::stopMusic() {
#ifdef TS_WITH_AUDIO
    Mix_HaltMusic();
#endif
}

void SoundManager::setMusicVolume(float volume) {
    music_volume_ = std::clamp(volume, 0.0f, 1.0f);
#ifdef TS_WITH_AUDIO
    Mix_VolumeMusic(static_cast<int>(music_volume_ * MIX_MAX_VOLUME));
#endif
}

void SoundManager::setSfxVolume(float volume) {
    sfx_volume_ = std::clamp(volume, 0.0f, 1.0f);
}

#ifdef TS_WITH_AUDIO
// Build one Mix_Chunk from an embedded sample, declaring its source rate as
// rate_hz * pitch. SDL_mixer resamples source-rate -> device-rate at load, so
// declaring a HIGHER source rate makes the sample play FASTER and higher --
// which is precisely what ndspChnSetRate does on the 3DS. pitch 1.0 is the
// baked rate, i.e. the original behaviour.
Mix_Chunk* SoundManager::makePitchedChunk(const sfxdata::SfxInfo& info, float pitch) {
    if (!mixer_initialized_) return nullptr;
    if (!(pitch > 0.01f)) pitch = 1.0f;

    // Wrap the raw signed-8-bit mono samples in an in-memory 8-bit WAV so
    // SDL_mixer resamples for us. WAV's 8-bit convention is UNSIGNED
    // (silence=128), so re-bias our already-signed source by +128 -- exact, no
    // upconversion needed.
    std::vector<uint8_t> pcm8(info.len);
    for (int s = 0; s < info.len; s++) pcm8[s] = (uint8_t)(info.data[s] + 128);

    struct __attribute__((packed)) WavHeader {
        char riff[4]; uint32_t riffLen; char wave[4];
        char fmt[4];  uint32_t fmtLen; uint16_t audioFmt, numChan;
        uint32_t sampleRate, byteRate; uint16_t blockAlign, bitsPerSample;
        char data[4]; uint32_t dataLen;
    } hdr;
    uint32_t dataBytes = (uint32_t)pcm8.size();
    std::memcpy(hdr.riff, "RIFF", 4);
    hdr.riffLen = 36 + dataBytes;
    std::memcpy(hdr.wave, "WAVE", 4);
    std::memcpy(hdr.fmt, "fmt ", 4);
    hdr.fmtLen = 16; hdr.audioFmt = 1; hdr.numChan = 1;
    hdr.sampleRate = (uint32_t)(info.rate_hz * pitch);   // <-- the pitch shift
    if (hdr.sampleRate < 1000) hdr.sampleRate = 1000;    // SDL rejects absurd rates
    hdr.bitsPerSample = 8;
    hdr.blockAlign = hdr.numChan * hdr.bitsPerSample / 8;
    hdr.byteRate = hdr.sampleRate * hdr.blockAlign;
    std::memcpy(hdr.data, "data", 4);
    hdr.dataLen = dataBytes;

    std::vector<uint8_t> wav(sizeof(hdr) + dataBytes);
    std::memcpy(wav.data(), &hdr, sizeof(hdr));
    std::memcpy(wav.data() + sizeof(hdr), pcm8.data(), dataBytes);

    SDL_RWops* rw = SDL_RWFromConstMem(wav.data(), (int)wav.size());
    return rw ? Mix_LoadWAV_RW(rw, 1) : nullptr;
}

// Pitch quantum for the one-shot cache: 1/64, i.e. ~27 cents. Finer than the
// game ever asks for (BOOM spans 0.85..1.15 across 300 energy units) and coarse
// enough that repeated explosions of similar size share one chunk.
static inline int quantizePitch(float p) { return (int)(p * 64.0f + 0.5f); }

Mix_Chunk* SoundManager::chunkForPitch(SfxId id, float pitch) {
    const int idx = (int)id;
    if (idx < 0 || idx >= (int)SfxId::COUNT || !embedded_[idx]) return nullptr;
    const int q = quantizePitch(pitch);
    if (q == 64) return embedded_[idx];          // pitch 1.0 -- the baked chunk

    for (int i = 0; i < pitchCacheUsed_; ++i)
        if (pitchCache_[i].id == idx && pitchCache_[i].q == q)
            return pitchCache_[i].chunk ? pitchCache_[i].chunk : embedded_[idx];

    if (pitchCacheUsed_ >= PITCH_CACHE_MAX) return embedded_[idx];   // graceful
    Mix_Chunk* c = makePitchedChunk(sfxdata::TABLE[idx], (float)q / 64.0f);
    if (!c) return embedded_[idx];
    pitchCache_[pitchCacheUsed_++] = { idx, q, c };
    return c;
}

// One utterance of the chant, at whatever pitch the glissando has reached.
// Deliberately NON-looping: the next one is started by drainSfx once this has
// finished, which is what makes the chant accelerate instead of drone.
void SoundManager::startYesUtterance() {
    const int idx = (int)SfxId::YES;
    if (!embedded_[idx]) return;
    const sfxdata::SfxInfo& info = sfxdata::TABLE[idx];

    // Safe to free here: this is only reached when the channel is idle, so the
    // previous utterance's chunk is no longer being mixed.
    if (yesChantChunk_) { Mix_FreeChunk(yesChantChunk_); yesChantChunk_ = nullptr; }

    // Mid-pass pitch (see the header): the hardware voice keeps accelerating
    // WHILE the word sounds, so a start-pitch one-shot runs long. Aim at the
    // pitch this pass will average. duration = loopSec / pitch, and pitch
    // depends on duration, so refine twice -- it converges immediately.
    const float loopSec = (info.rate_hz > 1.0f) ? ((float)info.len / info.rate_hz) : 1.0f;
    float pEff = yesChantPitch_;
    if (yesChantPitchRate_ > 0.0f && yesChantPitch_ > 0.01f) {
        float dur = loopSec / yesChantPitch_;
        for (int it = 0; it < 2; ++it) {
            pEff = yesChantPitch_ + 0.5f * yesChantPitchRate_ * dur;
            if (pEff < 0.01f) pEff = 0.01f;
            dur = loopSec / pEff;
        }
    }

    yesChantChunk_ = makePitchedChunk(info, pEff);
    if (!yesChantChunk_) { yesChantActive_ = false; return; }

    const int v = std::clamp((int)(yesChantVolume_ * sfx_volume_ * SFX_MASTER_GAIN
                                   * info.gain * sfxExtraGain(SfxId::YES) * MIX_MAX_VOLUME),
                             0, MIX_MAX_VOLUME);
    yes_channel_ = Mix_PlayChannel(-1, yesChantChunk_, 0);   // 0 = play ONCE
    // PER CHANNEL, never Mix_VolumeChunk: chunk volume is shared by every voice
    // playing that chunk, which is the wrong scope for a per-event level.
    if (yes_channel_ >= 0) Mix_Volume(yes_channel_, v);
}
#endif

void SoundManager::loadEmbeddedSfx() {
#ifdef TS_WITH_AUDIO
    if (!mixer_initialized_) return;
    for (int i = 0; i < (int)SfxId::COUNT; i++) {
        const sfxdata::SfxInfo& info = sfxdata::TABLE[i];

        // Wrap the raw signed-8-bit mono samples in an in-memory 8-bit WAV so
        // SDL_mixer resamples info.rate_hz -> the device rate at load time.
        // WAV's 8-bit convention is UNSIGNED (silence=128), so re-bias our
        // already-signed source by +128 -- exact, no upconversion needed.
        std::vector<uint8_t> pcm8(info.len);
        for (int s = 0; s < info.len; s++) pcm8[s] = (uint8_t)(info.data[s] + 128);

        struct __attribute__((packed)) WavHeader {
            char riff[4]; uint32_t riffLen; char wave[4];
            char fmt[4];  uint32_t fmtLen; uint16_t audioFmt, numChan;
            uint32_t sampleRate, byteRate; uint16_t blockAlign, bitsPerSample;
            char data[4]; uint32_t dataLen;
        } hdr;
        uint32_t dataBytes = (uint32_t)pcm8.size();
        std::memcpy(hdr.riff, "RIFF", 4);
        hdr.riffLen = 36 + dataBytes;
        std::memcpy(hdr.wave, "WAVE", 4);
        std::memcpy(hdr.fmt, "fmt ", 4);
        hdr.fmtLen = 16; hdr.audioFmt = 1; hdr.numChan = 1;
        hdr.sampleRate = (uint32_t)info.rate_hz;
        hdr.bitsPerSample = 8;
        hdr.blockAlign = hdr.numChan * hdr.bitsPerSample / 8;
        hdr.byteRate = hdr.sampleRate * hdr.blockAlign;
        std::memcpy(hdr.data, "data", 4);
        hdr.dataLen = dataBytes;

        std::vector<uint8_t> wav(sizeof(hdr) + dataBytes);
        std::memcpy(wav.data(), &hdr, sizeof(hdr));
        std::memcpy(wav.data() + sizeof(hdr), pcm8.data(), dataBytes);

        SDL_RWops* rw = SDL_RWFromConstMem(wav.data(), (int)wav.size());
        Mix_Chunk* chunk = rw ? Mix_LoadWAV_RW(rw, 1) : nullptr;
        if (!chunk) {
            std::fprintf(stderr, "Warning: could not decode embedded SFX %d: %s\n",
                         i, Mix_GetError());
        }
        embedded_[i] = chunk;
    }
#endif
}

// ---- SILENCE ACROSS TRANSITIONS (twin of sfx_3ds.cpp stop_all/stop_loops) ---
// The HELD loops are what strand: THUNDER loops until an explicit stop, and the
// climb-out chant keeps re-arming itself from yesChantActive_, so any
// transition that interrupts the thing driving them leaves the sound running on
// the next screen.
void SoundManager::stopSfxLoops() {
#ifdef TS_WITH_AUDIO
    if (!mixer_initialized_) return;
    if (thunder_channel_ >= 0) { Mix_HaltChannel(thunder_channel_); thunder_channel_ = -1; }
    if (yes_channel_ >= 0)     { Mix_HaltChannel(yes_channel_);     yes_channel_ = -1; }
    // Clearing the chant's own state matters as much as halting the channel:
    // leaving yesChantActive_ set would simply start the next utterance.
    yesChantActive_    = false;
    yesChantReleasing_ = false;
    yesChantPrevPitch_ = 0.0f;
    yesChantPitchRate_ = 0.0f;
#endif
}

void SoundManager::stopAllSfx() {
#ifdef TS_WITH_AUDIO
    if (!mixer_initialized_) return;
    stopSfxLoops();
    Mix_HaltChannel(-1);   // everything in flight belonged to the screen we left
#endif
}

void SoundManager::drainSfx(SfxQueue& queue) {
#ifdef TS_WITH_AUDIO
    // SFX_MASTER_GAIN / VOICE_BOOST / sfxExtraGain are shared with sfx_3ds.cpp
    // via sfx_mix.h -- the two backends' MIX FORMULAS still differ (this one
    // clamps to an int 0..MIX_MAX_VOLUME; the 3DS's ndsp mix is float and
    // unclamped) but now can never disagree on what these three numbers are.
    for (int i = 0; i < queue.count; i++) {
        const SfxEvent& e = queue.events[i];
        int idx = (int)e.id;
        if (idx < 0 || idx >= (int)SfxId::COUNT || !embedded_[idx]) continue;

        if (e.id == SfxId::THUNDER && e.action == SfxAction::LOOP_STOP) {
            if (thunder_channel_ >= 0) Mix_HaltChannel(thunder_channel_);
            thunder_channel_ = -1;
            continue;
        }
        const sfxdata::SfxInfo& info = sfxdata::TABLE[idx];
        if (e.id == SfxId::THUNDER && e.action == SfxAction::LOOP_START) {
            if (thunder_channel_ >= 0 && Mix_Playing(thunder_channel_)) continue; // already looping
            // sfxExtraGain is spelled out here for FORMULA PARITY with the
            // one-shot path below and with sfx_3ds.cpp's playOn(), so the gain
            // chain can never drift between sites. It does not move the beam's
            // level: THUNDER is not one of the four voice samples sfx_mix.h
            // boosts, so this factor is exactly 1.0 and un-boosted IS the beam's
            // correct level. (The real defect fixed in this block was
            // Mix_VolumeChunk -> Mix_Volume -- see the note at the one-shot path
            // below.)
            int vol = std::clamp((int)(e.volume * sfx_volume_ * SFX_MASTER_GAIN * info.gain
                                       * sfxExtraGain(e.id) * MIX_MAX_VOLUME), 0, MIX_MAX_VOLUME);
            thunder_channel_ = Mix_PlayChannel(-1, embedded_[idx], -1);
            if (thunder_channel_ >= 0) Mix_Volume(thunder_channel_, vol);
            continue;
        }

        // ---- Climb-out "Yes!" chant -- the desktop twin of sfx_3ds.cpp's YES
        // branch. See sound_manager.h for why this plays a SEQUENCE OF ONE-SHOTS
        // rather than one looping chunk: SDL_mixer cannot retune a playing
        // chunk, so a loop here can only ever drone at 1.0x and never
        // accelerate. Re-pitching per utterance reproduces the MECHANISM the
        // 3DS gets from ndspChnSetRate, and the acceleration is emergent for
        // the same reason -- each pass is shorter than the last.
        if (e.id == SfxId::YES && e.action != SfxAction::ONE_SHOT) {
            if (e.action == SfxAction::LOOP_STOP) {
                if (yes_channel_ >= 0) Mix_HaltChannel(yes_channel_);
                yes_channel_ = -1;
                yesChantActive_ = yesChantReleasing_ = false;
                if (yesChantChunk_) { Mix_FreeChunk(yesChantChunk_); yesChantChunk_ = nullptr; }
            } else if (e.action == SfxAction::LOOP_RELEASE) {
                // "Finish the current pass, then stop" -- the same deliberate
                // no-chop-mid-word behaviour the 3DS gets by re-queueing the
                // sample's remainder non-looping. Here the utterance is ALREADY
                // non-looping, so releasing is simply: stop starting new ones.
                yesChantReleasing_ = true;
            } else if (e.action == SfxAction::LOOP_START) {
                yesChantActive_    = true;
                yesChantReleasing_ = false;
                yesChantPitch_     = (e.pitch > 0.01f) ? e.pitch : 1.0f;
                yesChantVolume_    = e.volume;
                // No slope known yet -- the first utterance plays at its start
                // pitch, which is correct: the glissando begins here.
                yesChantPrevPitch_ = 0.0f;
                yesChantPitchRate_ = 0.0f;
                if (yes_channel_ >= 0) Mix_HaltChannel(yes_channel_);
                yes_channel_ = -1;
                startYesUtterance();
            } else {   // LOOP_PITCH -- retune the NEXT utterance, and re-level this one
                const float p  = (e.pitch > 0.01f) ? e.pitch : 1.0f;
                const uint32_t nowMs = SDL_GetTicks();
                // Measure the glissando's slope rather than assuming it.
                if (yesChantPrevPitch_ > 0.0f && nowMs > yesChantPrevMs_) {
                    const float dt = (float)(nowMs - yesChantPrevMs_) * 0.001f;
                    const float r  = (p - yesChantPrevPitch_) / dt;
                    // Low-pass: LOOP_PITCH arrives per FRAME but the pitch moves
                    // per 16ms TICK, so a frame that stepped zero ticks reports a
                    // slope of 0 and would otherwise knock the estimate about.
                    if (r > 0.0f)
                        yesChantPitchRate_ = (yesChantPitchRate_ > 0.0f)
                                           ? (yesChantPitchRate_ * 0.75f + r * 0.25f) : r;
                }
                yesChantPrevPitch_ = p;
                yesChantPrevMs_    = nowMs;
                yesChantPitch_  = p;
                yesChantVolume_ = e.volume;
                // Volume CAN be applied live (unlike pitch), so the release
                // taper still fades the utterance that is currently sounding.
                if (yesChantChunk_ && yes_channel_ >= 0) {
                    const int v = std::clamp((int)(yesChantVolume_ * sfx_volume_ * SFX_MASTER_GAIN
                                                   * info.gain * sfxExtraGain(e.id) * MIX_MAX_VOLUME),
                                             0, MIX_MAX_VOLUME);
                    Mix_Volume(yes_channel_, v);
                }
            }
            continue;
        }

        // PITCHED (3DS parity -- explosions deepen with energy), and volume set
        // PER CHANNEL rather than per chunk. Mix_VolumeChunk mutates the chunk
        // itself, which every other channel already playing that same chunk
        // shares -- so a second, quieter instance retroactively ducked the one
        // already sounding. Mix_Volume is per-voice, which is what the 3DS's
        // ndspChnSetMix does.
        const int vol = std::clamp((int)(e.volume * sfx_volume_ * SFX_MASTER_GAIN
                                         * info.gain * sfxExtraGain(e.id) * MIX_MAX_VOLUME),
                                   0, MIX_MAX_VOLUME);
        Mix_Chunk* chunk = chunkForPitch(e.id, e.pitch);
        if (!chunk) chunk = embedded_[idx];
        const int ch = Mix_PlayChannel(-1, chunk, 0);
        if (ch >= 0) Mix_Volume(ch, vol);
    }

    // ---- Chant re-trigger. THIS is the acceleration ------------------------
    // Runs every frame, not just when an event arrived. When the current
    // utterance has finished, start the next one AT THE PITCH THE GLISSANDO HAS
    // REACHED BY NOW -- so each is shorter than the last and the next one comes
    // sooner. Same emergent speed-up the 3DS gets from retuning a live loop;
    // the engine's own yes_beat_ms stamps (game_step.cpp) predict these onsets
    // from the same 512/yes_period, so the YES! shatter signs stay in sync.
    //
    // Releasing means "no new utterances" -- the one still sounding plays out
    // to its natural end rather than being chopped mid-word.
    if (yesChantActive_ && !yesChantReleasing_) {
        const bool idle = (yes_channel_ < 0) || !Mix_Playing(yes_channel_);
        if (idle) startYesUtterance();
    } else if (yesChantReleasing_ && yes_channel_ >= 0 && !Mix_Playing(yes_channel_)) {
        yesChantActive_ = yesChantReleasing_ = false;
        yes_channel_ = -1;
        if (yesChantChunk_) { Mix_FreeChunk(yesChantChunk_); yesChantChunk_ = nullptr; }
    }
#endif
    queue.clear();
}

} // namespace ts
