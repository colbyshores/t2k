// Auto-generated from the reference SFX asset dir -- see its README for provenance.
// Raw signed 8-bit PCM mono, arcade-reference (1994) SFX samples.
// Rates are calibrated (2.75x the raw Amiga-period-derived value -- see README.md
// 'Pitch calibration' section). gain is a per-sound loudness-compensation multiplier
// (target-RMS normalized, clamped [0.5,2.5] -- see README.md 'Loudness calibration').
// Regenerate with tools/gen_sfx_bank.py -- NEVER hand-edit.
#pragma once
#include <cstdint>
namespace ts { namespace sfxdata {
// Player Shot Normal (period 428, calibrated 22999.3 Hz, gain 1.98), 1856 bytes
extern const int8_t SFX_SHOOT1[1856];
// Powered Up Shot (period 428, calibrated 22999.3 Hz, gain 0.66), 6518 bytes
extern const int8_t SFX_SHOOT2[6518];
// Large Explosion (period 428, calibrated 22999.3 Hz, gain 0.61), 20674 bytes
extern const int8_t SFX_BOOM[20674];
// Tink For Spike (period 254, calibrated 14092.7 Hz, gain 0.63), 1038 bytes
extern const int8_t SFX_SPIKE[1038];
// Player Jump (period 428, calibrated 22999.3 Hz, gain 1.55), 6366 bytes
extern const int8_t SFX_CRAWL[6366];
// Warp (period 568, calibrated 17330.5 Hz, gain 0.79), 28360 bytes
extern const int8_t SFX_POWER[28360];
// Static or Pulsar (period 284, calibrated 34661.0 Hz, gain 0.74), 16356 bytes
extern const int8_t SFX_THUNDER[16356];
// Crackle (period 214, calibrated 45998.7 Hz, gain 0.86), 17812 bytes
extern const int8_t SFX_OUCH[17812];
// Extra Explosion (period 856, calibrated 11499.7 Hz, gain 0.93), 6346 bytes
extern const int8_t SFX_REFLECT[6346];
// Excellent (period 512, calibrated 19226.1 Hz, gain 1.29), 22902 bytes
extern const int8_t SFX_GROOVY[22902];
// yes (period 512, calibrated 19226.1 Hz, gain 1.05), 23148 bytes
extern const int8_t SFX_YES[23148];
// Superzapper Recharge (period 512, calibrated 19226.1 Hz, gain 0.85), 43352 bytes
extern const int8_t SFX_SUPERZAP[43352];
// Powered Up Shot (period 428, calibrated 22999.3 Hz, gain 0.66), 6518 bytes
extern const int8_t SFX_POWERUP_SPAWN[6518];
// oneup (period 512, calibrated 19226.1 Hz, gain 1.21), 17326 bytes
extern const int8_t SFX_ONEUP[17326];
// sexy yes 1 (period 512, calibrated 19226.1 Hz, gain 2.19), 11432 bytes
extern const int8_t SFX_SEXY_YES1[11432];
// sexy yes 2 (period 512, calibrated 19226.1 Hz, gain 2.50), 12854 bytes
extern const int8_t SFX_SEXY_YES2[12854];
// Bonus Pickup (period 1230, calibrated 8000.0 Hz, gain 1.47), 1267 bytes
extern const int8_t SFX_BONUS_PICKUP[1267];
// Bonus Lose (aww) (period 447, calibrated 8000.0 Hz, gain 2.44), 14070 bytes
extern const int8_t SFX_BONUS_LOSE[14070];
struct SfxInfo { const int8_t* data; int len; float rate_hz; float gain; };
extern const SfxInfo TABLE[18];
} } // namespace ts::sfxdata
