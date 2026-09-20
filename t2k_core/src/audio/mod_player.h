#pragma once
// ============================================================================
// mod_player.h — portable ProTracker MOD replayer (clean-room port of the
// original Tube Shooter chiptune engine in the reference chiptune source:
// MOD2Mem / MODRenderer / ChannelMixer). GL-free, libctru-free, no STL in the
// hot loop — a fixed-size "C with classes" aggregate so it can run on the 3DS
// audio worker thread. Renders mono signed-16 PCM @ 44100 Hz.
//
// Ground truth: the reference chiptune source. The two songs ship embedded in mod_data.h
// (mymod1/mymod2 — untitled 4-channel MODs, uncompressed). Effect set, period
// table, LUTs (tabVolume[j,v]=j*v/63, tabPostProc=clamp(k+128)), the 300 Hz
// mixer cadence and the output FIR all mirror the the reference build exactly.
// ============================================================================
#include <cstdint>
#include <cstddef>

namespace ts {
namespace modmusic {

constexpr int MOD_SAMPLE_RATE = 44100;
constexpr int MOD_DMALEN      = 147;   // samples produced per 300 Hz mixer frame
constexpr int MOD_MAX_CH      = 16;
constexpr int MOD_FP_SHIFT    = 12;

class ModPlayer {
public:
    ModPlayer() = default;

    // Parse an embedded MOD byte stream (stays referenced — sample data is read
    // in place, so `data` must outlive the player; the static mod_data.h arrays
    // do). Returns false on a malformed header. Does not start playback.
    bool load(const uint8_t* data, size_t size);

    void play();          // (re)start from the top of the arrangement
    void setPlaying(bool p) { render_ = p; }   // pause / resume -- the ONLY
                          // playback gate any sink uses; a `stop()` that also
                          // reset the channel states, and a `playing()` reader,
                          // both existed here with no caller and are gone.

    // Render exactly MOD_DMALEN mono s16 samples (one 300 Hz frame). Silent
    // (zeros) when not loaded / not playing.
    void renderFrame(int16_t* out);

    // Convenience: render `frames` samples in MOD_DMALEN chunks (frames should
    // be a multiple of MOD_DMALEN; a partial tail is zero-filled).
    void render(int16_t* out, int frames);

private:
    void buildTables();
    void setChannelRate(int ch, int rate);
    int  getNote(int rate) const;
    void sequencerTick();       // MODRenderer: advance ticks/rows + effects
    void channelMix(int16_t* out);

    // ---- module data (parsed) ----
    const uint8_t* raw_ = nullptr;
    bool loaded_ = false;
    bool render_ = false;

    int numCh_ = 4;
    int numPat_ = 0;
    int songLen_ = 0;
    uint8_t arrangement_[128] = {};

    struct Instr { int length, finetune, defVol, loopStart, loopLen; const uint8_t* data; };
    Instr instr_[32] = {};      // 1..31 (0 unused)

    struct Note { uint16_t tone; uint8_t instr, effect, op; };
    // 64 patterns max × 64 rows × MOD_MAX_CH — matches AINCMOD's mod_pat bound.
    Note pat_[64][64][MOD_MAX_CH] = {};

    // ---- sequencer state ----
    int patRow_ = 0, momPat_ = 0, arrPos_ = 0;
    int bpm_ = 125, numTicks_ = 6, ticks_ = 0, timerCalls_ = 0;

    // ---- per-channel mixer + effect state (0..numCh_-1) ----
    int32_t sbAdder_[MOD_MAX_CH] = {};
    int32_t sbPos_[MOD_MAX_CH] = {}, sbEnd_[MOD_MAX_CH] = {};
    int32_t sbLoopStart_[MOD_MAX_CH] = {}, sbLoopEnd_[MOD_MAX_CH] = {};
    const uint8_t* sbStart_[MOD_MAX_CH] = {};
    uint8_t sbStatus_[MOD_MAX_CH] = {};     // stStop=1 stOnce=2 stLoop=3
    int32_t sbVolume_[MOD_MAX_CH] = {};

    int32_t sbVolSlide_[MOD_MAX_CH] = {};
    int32_t sbPortamento_[MOD_MAX_CH] = {}, sbPortaNote_[MOD_MAX_CH] = {};
    int32_t sbToneHeight_[MOD_MAX_CH] = {};
    int32_t sbVibPos_[MOD_MAX_CH] = {}, sbVib_[MOD_MAX_CH] = {}, sbVibDepth_[MOD_MAX_CH] = {};
    int32_t sbTremPos_[MOD_MAX_CH] = {}, sbTrem_[MOD_MAX_CH] = {}, sbTremDepth_[MOD_MAX_CH] = {}, sbTremVol_[MOD_MAX_CH] = {};
    int32_t sbArpPos_[MOD_MAX_CH] = {}, sbArp0_[MOD_MAX_CH] = {}, sbArp1_[MOD_MAX_CH] = {}, sbArp2_[MOD_MAX_CH] = {};

    // ---- lookup tables ----
    int8_t  tabVolume_[256][64];                 // [sample+128][vol 0..63]
    int16_t mixBuf_[MOD_DMALEN];
    uint8_t outBuf_[MOD_DMALEN];
};

} // namespace modmusic
} // namespace ts
