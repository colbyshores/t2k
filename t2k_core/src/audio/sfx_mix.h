#pragma once

// Shared SFX gain constants -- NOT the combined mix formula. The two backends
// mix in genuinely different ways (desktop's SDL_mixer path clamps to an int
// 0..MIX_MAX_VOLUME with std::clamp truncating the product; the 3DS's ndsp
// path applies an unclamped float mix with its own, differently-ordered
// multiply chain at its ndspChnSetMix sites -- the clip-ceiling analysis that
// unclamped path forces is below, on VOICE_BOOST). Sharing the
// PRODUCT would force one backend's multiply order onto the other and could
// shift the result by an ULP or clip differently; sharing just these three
// named quantities keeps both backends' own mix expressions as the single
// source of truth for how they combine, while guaranteeing they can never
// silently drift on WHAT the shared numbers themselves are.

#include "../game/sfx.h"

namespace ts {

// Samples are raw full-scale digital audio with no per-sound authored
// headroom (unlike the original arcade-reference mix, which we can't inspect --
// its DSP driver is a closed binary blob). Cut a flat baseline so the user-facing
// slider has real headroom to work with in both directions.
constexpr float SFX_MASTER_GAIN = 0.30f;

// Loudness-match the spoken "voice" samples (Superzapper Recharge, Excellent,
// Yes, One Up) up toward the blast/explosion SFX. Pinned to 2.0 rather than
// the higher value that would exactly loudness-match them: on the 3DS's
// unclamped ndsp mix path, the max-slider product with the compressed voice
// gains (GROOVY 1.29 binding) hits 1.0 at VOICE_BOOST 2.58 -- past that it
// clipped GROOVY and YES at default volume. 2.0 (product 0.774) keeps margin.
// (Naming the fourth recipient does not move the ceiling: ONE_UP's baked gain
// is 1.21, so GROOVY's 1.29 stays binding.) See
// docs/known-issues/superzapper-recharge-inaudible.md.
constexpr float VOICE_BOOST = 2.0f;

// Per-sample loudness boost for the four voice samples; 1.0 (no boost) for
// everything else. SEXY_YES1/2 (the bonus-round gate-catch pair) are voice
// samples but are deliberately NOT in this list: unlike GROOVY/YES/SUPERZAP
// their bank data is raw (no AGC mastering pass), so their baked gains already
// carry the full loudness lift (2.19 / 2.50 -- YES2 at the normalizer's clamp
// ceiling). Boosting them 2.0x on top would put their per-voice ndsp mix at
// 0.30*2.50*2.0 = 1.50 > 1.0 -- constant clipping at default volume, past the
// ceiling this comment block exists to guard. Unboosted worst case (max
// slider, event volume 1.0): 0.30*2.19 = 0.657 and 0.30*2.50 = 0.750, both
// under GROOVY's blessed 0.774 binding. The catch pair OVERLAPS on two
// channels; worst-case instantaneous SUM at max slider with both event
// volumes 1.0 is peak-exact (79/127)*2.19*0.30 + (92/127)*2.50*0.30 = 0.952
// < 1.0, and at warp.cpp's alternating 0.9/0.7 push volumes it is 0.748 or
// 0.775 depending on which sample leads that catch (push_yes_pair, warp.cpp)
// -- the summed pair cannot clip the ndsp master even if both sample peaks
// aligned.
inline float sfxExtraGain(SfxId id) {
    switch (id) {
        case SfxId::SUPERZAP:
        case SfxId::GROOVY:
        case SfxId::YES:
        case SfxId::ONE_UP:
            return VOICE_BOOST;
        default:
            return 1.0f;
    }
}

} // namespace ts
