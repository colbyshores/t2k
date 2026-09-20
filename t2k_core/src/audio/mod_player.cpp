// ============================================================================
// mod_player.cpp — clean-room ProTracker replayer, faithful to the reference chiptune source
// (MOD2Mem / MODRenderer / ChannelMixer). See mod_player.h.
// ============================================================================
#include "mod_player.h"

#include <cstring>

namespace ts {
namespace modmusic {

namespace {
constexpr int ST_STOP = 1, ST_ONCE = 2, ST_LOOP = 3;
constexpr int MOD_MAX_VOL = 16;   // nominal MOD volume ceiling
constexpr int MAX_VOLUME  = 63;   // LUT / effect volume range

// Amiga period table (AINCMOD mod_notes[0..59]).
const uint16_t kNotes[60] = {
    0x36,0x39,0x3C,0x40,0x43,0x47,0x4C,0x55,0x5A,0x5F,0x65,0x6B,
    0x71,0x78,0x7F,0x87,0x8F,0x97,0xA0,0xAA,0xB4,0xBE,0xCA,0xD6,
    0xE2,0xF0,0xFE,0x10D,0x11D,0x12E,0x140,0x153,0x168,0x17D,0x194,0x1AC,
    0x1C5,0x1E0,0x1FC,0x21A,0x23A,0x25C,0x280,0x2A6,0x2D0,0x2FA,0x328,0x358,
    0x386,0x3C1,0x3FA,0x436,0x477,0x4BB,0x503,0x54F,0x5A0,0x5F5,0x650,0x6B0 };

// ProTracker vibrato/tremolo sine table (AINCMOD vib_sine_table[0..31]).
const uint8_t kVibSine[32] = {
    0,24,49,74,97,120,141,161,180,197,212,224,235,244,250,253,
    255,253,250,244,235,224,212,197,180,161,141,120,97,74,49,24 };

inline uint16_t rd16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

void ModPlayer::buildTables() {
    // tabVolume[j+128][vol] = (int8_t)(j*vol / 63)  (AINCMOD StartSoundPlayer).
    for (int j = -128; j <= 127; ++j)
        for (int v = 0; v <= MAX_VOLUME; ++v)
            tabVolume_[j + 128][v] = (int8_t)((j * v) / MAX_VOLUME);
}

bool ModPlayer::load(const uint8_t* data, size_t size) {
    loaded_ = false; render_ = false;
    if (!data || size < 1084) return false;
    raw_ = data;
    buildTables();

    // ---- header (MOD2Mem) ----
    const uint8_t* h = data;
    // 20-byte song name, then 31 × 30-byte instrument headers.
    const uint8_t* ip = h + 20;
    for (int v = 1; v <= 31; ++v) {
        const uint8_t* e = ip + (v - 1) * 30;
        Instr& in = instr_[v];
        in.length    = rd16be(e + 22) * 2;   // Amiga words -> bytes
        in.finetune  = e[24] & 0x0F;
        in.defVol    = e[25]; if (in.defVol > MAX_VOLUME) in.defVol = MAX_VOLUME;
        in.loopStart = rd16be(e + 26) * 2;
        in.loopLen   = rd16be(e + 28) * 2;
        in.data      = nullptr;
    }
    const uint8_t* after = h + 20 + 31 * 30;   // = h + 950
    songLen_ = after[0];
    // after[1] = ciaa/restart (unused). arrangement follows.
    std::memcpy(arrangement_, after + 2, 128);
    const char* modtype = (const char*)(after + 2 + 128);   // 4 bytes @ 1080

    if      (!std::memcmp(modtype, "6CHN", 4)) numCh_ = 6;
    else if (!std::memcmp(modtype, "8CHN", 4)) numCh_ = 8;
    else if (!std::memcmp(modtype, "12CH", 4)) numCh_ = 12;
    else if (!std::memcmp(modtype, "16CH", 4)) numCh_ = 16;
    else numCh_ = 4;
    if (numCh_ > MOD_MAX_CH) numCh_ = MOD_MAX_CH;

    const int HDR = 1084;
    const int noteBytes = 4;
    // Pattern-area size = file - header - all sample data.
    long patArea = (long)size - HDR;
    for (int v = 1; v <= 31; ++v) patArea -= instr_[v].length;
    const int patStride = noteBytes * 64 * numCh_;
    if (patArea > 0 && (patArea % patStride) == 0) {
        numPat_ = (int)(patArea / patStride);
    } else {
        numPat_ = 0;
        for (int v = 0; v < songLen_; ++v)
            if (arrangement_[v] + 1 > numPat_) numPat_ = arrangement_[v] + 1;
    }
    if (numPat_ < 0) numPat_ = 0;
    if (numPat_ > 64) numPat_ = 64;

    // ---- patterns (standard 4-byte note unpack) ----
    const uint8_t* pp = data + HDR;
    for (int v = 0; v < numPat_; ++v)
        for (int r = 0; r < 64; ++r)
            for (int c = 0; c < numCh_; ++c) {
                const uint8_t* n = pp;
                pp += noteBytes;
                if (pp > data + size) return false;
                Note& nt = pat_[v][r][c];
                nt.instr  = (uint8_t)((n[0] & 0xF0) | (n[2] >> 4));
                nt.tone   = (uint16_t)(((n[0] & 0x0F) << 8) | n[1]);
                nt.effect = (uint8_t)(n[2] & 0x0F);
                nt.op     = n[3];
            }

    // ---- sample data pointers (read in place) ----
    const uint8_t* sp = pp;
    for (int v = 1; v <= 31; ++v) {
        if (instr_[v].length > 0) {
            if (sp + instr_[v].length > data + size) instr_[v].length = (int)(data + size - sp);
            instr_[v].data = (instr_[v].length > 0) ? sp : nullptr;
            sp += instr_[v].length;
        }
    }

    loaded_ = true;
    return true;
}

void ModPlayer::play() {
    if (!loaded_) return;
    arrPos_ = 0;
    momPat_ = arrangement_[0];
    patRow_ = 0;
    bpm_ = 125; numTicks_ = 6; ticks_ = 0; timerCalls_ = 0;
    for (int i = 0; i < MOD_MAX_CH; ++i) sbStatus_[i] = ST_STOP;
    render_ = true;
}

void ModPlayer::setChannelRate(int ch, int rate) {
    if (rate < kNotes[0]) rate = kNotes[0];
    else if (rate > kNotes[59]) rate = kNotes[59];
    sbAdder_[ch] = (int32_t)((((int64_t)(3579364 / rate)) << MOD_FP_SHIFT) / MOD_SAMPLE_RATE);
}

int ModPlayer::getNote(int rate) const {
    if (rate <= kNotes[0]) return 0;
    if (rate >= kNotes[59]) return 59;
    for (int v = 0; v <= 58; ++v)
        if (rate >= kNotes[v] && rate < kNotes[v + 1]) return v;
    return 0;
}

// MODRenderer — advance the tick/row sequencer by one 300 Hz timer call and
// apply per-tick + per-row effects (goto-oncemore loop -> while).
void ModPlayer::sequencerTick() {
    if (!render_) return;
    timerCalls_ += (((bpm_ * 2) / 5) << MOD_FP_SHIFT) / 300;

    while (timerCalls_ > (1 << MOD_FP_SHIFT)) {
        timerCalls_ -= (1 << MOD_FP_SHIFT);
        ticks_++;

        if (ticks_ < numTicks_) {                 // ---- per-tick effects ----
            for (int ch = 0; ch < numCh_; ++ch) {
                if (sbVolSlide_[ch] > 0) {
                    sbVolume_[ch] += sbVolSlide_[ch];
                    if (sbVolume_[ch] > MOD_MAX_VOL) { sbVolume_[ch] = MOD_MAX_VOL; sbVolSlide_[ch] = 0; }
                } else if (sbVolSlide_[ch] < 0) {
                    if (sbVolume_[ch] + sbVolSlide_[ch] < 0) { sbVolume_[ch] = 0; sbVolSlide_[ch] = 0; }
                    else sbVolume_[ch] += sbVolSlide_[ch];
                }
                if (sbPortamento_[ch] != 0) {
                    sbToneHeight_[ch] += sbPortamento_[ch];
                    if (sbPortaNote_[ch] > 0)
                        if ((sbToneHeight_[ch] <= sbPortaNote_[ch] && sbPortamento_[ch] < 0) ||
                            (sbToneHeight_[ch] >= sbPortaNote_[ch] && sbPortamento_[ch] > 0)) {
                            sbToneHeight_[ch] = sbPortaNote_[ch]; sbPortamento_[ch] = 0;
                        }
                    setChannelRate(ch, sbToneHeight_[ch]);
                }
                if (sbVib_[ch] > 0) {
                    sbVibPos_[ch] += sbVib_[ch];
                    if (sbVibPos_[ch] > 31) sbVibPos_[ch] -= 64;
                    if (sbVibPos_[ch] >= 0)
                        setChannelRate(ch, sbToneHeight_[ch] + (kVibSine[sbVibPos_[ch]] * sbVibDepth_[ch]) / 128);
                    else
                        setChannelRate(ch, sbToneHeight_[ch] - (kVibSine[sbVibPos_[ch] & 31] * sbVibDepth_[ch]) / 128);
                }
                if (sbTrem_[ch] > 0) {
                    sbTremPos_[ch] += sbTrem_[ch];
                    if (sbTremPos_[ch] > 31) sbTremPos_[ch] -= 64;
                    if (sbTremPos_[ch] >= 0) {
                        sbVolume_[ch] = sbTremVol_[ch] + (kVibSine[sbTremPos_[ch]] * sbTremDepth_[ch]) / 64;
                        if (sbVolume_[ch] > 63) sbVolume_[ch] = 63;
                    } else {
                        int d = sbTremVol_[ch] - (kVibSine[sbTremPos_[ch] & 31] * sbTremDepth_[ch]) / 64;
                        sbVolume_[ch] = d > 0 ? d : 0;
                    }
                }
                if (sbArpPos_[ch] > 0) {
                    switch (ticks_ % 3) {
                        case 0: setChannelRate(ch, sbArp0_[ch]); break;
                        case 1: setChannelRate(ch, sbArp1_[ch]); break;
                        case 2: setChannelRate(ch, sbArp2_[ch]); break;
                    }
                }
            }
            continue;
        }

        ticks_ = 0;
        bool cont = false;
        for (int ch = 0; ch < numCh_; ++ch) {     // ---- per-row triggers ----
            Note& nt = pat_[momPat_][patRow_][ch];
            int tone = nt.tone, effect = nt.effect, op = nt.op, insn = nt.instr;

            if (tone > 0 && effect != 3) {
                setChannelRate(ch, tone);
                sbToneHeight_[ch] = tone;
                if (effect != 5) { sbPortamento_[ch] = 0; sbPortaNote_[ch] = 0; }
                sbArpPos_[ch] = 0;
                if (effect != 6) sbVib_[ch] = 0;
                sbTrem_[ch] = 0;
                if (effect == 0 && op == 0) sbPos_[ch] = 0;
            }
            if (insn > 0 && insn <= 31 && instr_[insn].length > 0) {
                sbStart_[ch] = instr_[insn].data;
                sbPos_[ch]   = 0;
                sbEnd_[ch]   = instr_[insn].length << MOD_FP_SHIFT;
                if (instr_[insn].loopLen == 0) sbStatus_[ch] = ST_ONCE;
                else {
                    sbStatus_[ch]    = ST_LOOP;
                    sbLoopStart_[ch] = instr_[insn].loopStart << MOD_FP_SHIFT;
                    sbLoopEnd_[ch]   = (instr_[insn].loopLen + instr_[insn].loopStart) << MOD_FP_SHIFT;
                }
                sbVolume_[ch] = instr_[insn].defVol * MOD_MAX_VOL / MAX_VOLUME;
                if (effect != 5) { sbPortamento_[ch] = 0; sbPortaNote_[ch] = 0; }
                sbArpPos_[ch] = 0;
                if (effect != 6) sbVib_[ch] = 0;
                sbTrem_[ch] = 0;
                sbVolSlide_[ch] = 0;
            }

            switch (effect) {
                case 0:
                    if (op != 0) {
                        sbArpPos_[ch] = 1;
                        sbArp0_[ch] = sbToneHeight_[ch];
                        int g = getNote(sbToneHeight_[ch]);
                        sbArp1_[ch] = (g - (op >> 4)   > 0) ? kNotes[g - (op >> 4)]   : kNotes[0];
                        sbArp2_[ch] = (g - (op & 0x0F) > 0) ? kNotes[g - (op & 0x0F)] : kNotes[0];
                    }
                    break;
                case 1: sbPortamento_[ch] = -op; sbPortaNote_[ch] = 0; break;
                case 2: sbPortamento_[ch] =  op; sbPortaNote_[ch] = 0; break;
                case 3:
                    if (tone > 0) {
                        if (sbToneHeight_[ch] < tone) { sbPortamento_[ch] =  op; sbPortaNote_[ch] = tone; }
                        else                          { sbPortamento_[ch] = -op; sbPortaNote_[ch] = tone; }
                    }
                    break;
                case 4:
                    sbVibPos_[ch] = 0; sbVib_[ch] = op >> 4; sbVibDepth_[ch] = op & 0x0F;
                    break;
                case 5:
                    if (tone > 0) {
                        if (sbToneHeight_[ch] < tone) { sbPortamento_[ch] =  (sbPortamento_[ch] < 0 ? -sbPortamento_[ch] : sbPortamento_[ch]); sbPortaNote_[ch] = tone; }
                        else                          { sbPortamento_[ch] = -(sbPortamento_[ch] < 0 ? -sbPortamento_[ch] : sbPortamento_[ch]); sbPortaNote_[ch] = tone; }
                    }
                    sbVolSlide_[ch] = (op >> 4 == 0) ? -op : (op >> 4);
                    break;
                case 6:
                    sbVolSlide_[ch] = (op >> 4 == 0) ? -op : (op >> 4);
                    break;
                case 7:
                    sbTremVol_[ch] = sbVolume_[ch]; sbTremPos_[ch] = 0;
                    sbTrem_[ch] = op >> 4; sbTremDepth_[ch] = op & 0x0F;
                    break;
                case 9: sbPos_[ch] = (op << 8) << MOD_FP_SHIFT; break;
                case 10: sbVolSlide_[ch] = (op >> 4 == 0) ? -op : (op >> 4); break;
                case 11:
                    patRow_ = 0; arrPos_ = op;
                    if (arrPos_ >= songLen_) arrPos_ = 0;
                    momPat_ = arrangement_[arrPos_]; cont = true;
                    break;
                case 12: sbVolume_[ch] = op * MOD_MAX_VOL / MAX_VOLUME; break;
                case 13:
                    patRow_ = op; arrPos_++;
                    if (arrPos_ >= songLen_) arrPos_ = 0;
                    momPat_ = arrangement_[arrPos_]; cont = true;
                    break;
                case 15:
                    if (op <= 31) numTicks_ = op; else bpm_ = op;
                    break;
                case 14:
                    switch (op >> 4) {
                        case 1: sbToneHeight_[ch] -= op & 0x0F; setChannelRate(ch, sbToneHeight_[ch]); break;
                        case 2: sbToneHeight_[ch] += op & 0x0F; setChannelRate(ch, sbToneHeight_[ch]); break;
                        case 10:
                            sbVolume_[ch] += op & 0x0F;
                            if (sbVolume_[ch] > MOD_MAX_VOL) sbVolume_[ch] = MOD_MAX_VOL;
                            break;
                        case 11:
                            if (sbVolume_[ch] - (op & 0x0F) < 0) sbVolume_[ch] = 0;
                            else sbVolume_[ch] -= op & 0x0F;
                            break;
                    }
                    break;
            }
        }

        if (!cont) {
            patRow_++;
            if (patRow_ > 63) {
                patRow_ = 0; arrPos_++;
                if (arrPos_ >= songLen_) arrPos_ = 0;
                momPat_ = arrangement_[arrPos_];
            }
        }
    }
}

// ChannelMixer — mix all channels into 147 mono samples, post-process + FIR.
void ModPlayer::channelMix(int16_t* out) {
    for (int i = 0; i < MOD_DMALEN; ++i) mixBuf_[i] = 0;

    for (int ch = 0; ch < numCh_; ++ch) {
        if (sbStatus_[ch] == ST_STOP || !sbStart_[ch]) continue;
        int vol = clampi(sbVolume_[ch], 0, MAX_VOLUME);
        int32_t pos = sbPos_[ch], end = sbEnd_[ch], adder = sbAdder_[ch];
        const uint8_t* base = sbStart_[ch];
        for (int i = 0; i < MOD_DMALEN; ++i) {
            int8_t s = (int8_t)base[(uint32_t)pos >> MOD_FP_SHIFT];
            mixBuf_[i] = (int16_t)(mixBuf_[i] + tabVolume_[(int)s + 128][vol]);
            pos += adder;
            if (pos > end) {
                if (sbStatus_[ch] == ST_LOOP) { pos = sbLoopStart_[ch]; end = sbLoopEnd_[ch]; }
                else { sbStatus_[ch] = ST_STOP; break; }
            }
        }
        sbPos_[ch] = pos; sbEnd_[ch] = end;
    }

    for (int i = 0; i < MOD_DMALEN; ++i)
        outBuf_[i] = (uint8_t)clampi(mixBuf_[i] + 128, 0, 255);

    // Output FIR (AINCMOD tail): soft low-pass, recentre to signed 16.
    out[0] = (int16_t)((outBuf_[0] - 128) * 256);
    out[1] = (int16_t)((outBuf_[1] - 128) * 256);
    for (int i = 2; i <= MOD_DMALEN - 3; ++i)
        out[i] = (int16_t)((int)outBuf_[i] * 96 +
                           ((int)outBuf_[i + 1] + outBuf_[i - 1]) * 64 +
                           ((int)outBuf_[i + 2] + outBuf_[i - 2]) * 16 - 256 * 128);
    out[MOD_DMALEN - 2] = (int16_t)((outBuf_[MOD_DMALEN - 2] - 128) * 256);
    out[MOD_DMALEN - 1] = (int16_t)((outBuf_[MOD_DMALEN - 1] - 128) * 256);
}

void ModPlayer::renderFrame(int16_t* out) {
    if (!loaded_ || !render_) { std::memset(out, 0, MOD_DMALEN * sizeof(int16_t)); return; }
    sequencerTick();
    channelMix(out);
}

void ModPlayer::render(int16_t* out, int frames) {
    int done = 0;
    while (done + MOD_DMALEN <= frames) { renderFrame(out + done); done += MOD_DMALEN; }
    if (done < frames) std::memset(out + done, 0, (frames - done) * sizeof(int16_t));
}

} // namespace modmusic
} // namespace ts
