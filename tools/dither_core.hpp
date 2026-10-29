#pragma once

// Shared dithering core: palette handling, the mixing-plan lookup table, and the 8x8
// Bayer matrix. Deliberately dependency-free (no Hyprland, no cairo, no PNG) so the
// generator, the preview tool, the standalone tests and — later — the plugin all compute
// IDENTICAL numbers. If this file and the generated .frag ever disagree, the shader and
// the CPU texture path would dither differently, which is the one bug this design must
// not have.
//
// The algorithm is Yliluoma's arbitrary-palette ordered dithering ("Algorithm 1" with the
// refinements from the article in ../../dithering/):
//
//   For each pixel: pick one of two palette colours whose MIX approximates the input.
//   A BAYER threshold decides which of the two to draw, so a flat area shows the two
//   colours in a stable pattern instead of crawling noise.
//
// The expensive part (N^2/2 candidate pairs, each with a solved mix ratio) depends only
// on the INPUT COLOUR, so it is precomputed into a lookup table keyed by a quantised
// colour, and the per-pixel cost becomes one table lookup plus one comparison.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Hyprdecor::Dither
{
    using RGB = std::array<uint8_t, 3>;

    // ---------------------------------------------------------------- palette

    // A palette entry. Alpha is dropped on purpose: the screen shader matches on colour
    // and the input is opaque. Translucent palette slots are rejected by loadPalette().
    struct Palette
    {
        std::vector<RGB> colours;
    };

    // Drops fully transparent entries (the 14x1 strip's index 0 is a transparent
    // sentinel) and removes duplicates. Two identical hexes in the .aseprite palette
    // (#000000 at 0 and 5) would otherwise waste half the pair search and produce two
    // indistinguishable "different" colours.
    //
    // Order is PRESERVED: the first occurrence wins, so a palette file's ordering is
    // meaningful to anyone reading it, and indices stay stable across regenerations.
    inline Palette dedupe(std::vector<RGB> colours, std::vector<uint8_t> const &alpha = {})
    {
        Palette out;
        for (size_t i = 0; i < colours.size(); i++)
        {
            const bool OPAQUE = alpha.empty() || alpha[i] != 0;
            if (!OPAQUE)
                continue;

            bool seen = false;
            for (auto const &c : out.colours)
                if (c == colours[i])
                {
                    seen = true;
                    break;
                }

            if (!seen)
                out.colours.push_back(colours[i]);
        }
        return out;
    }

    // ----------------------------------------------------------- colour compare

    // sRGB -> linear light. Palette mixing is a physical average of emitted light, so a
    // mix computed on sRGB bytes is wrong: it makes dark pairs too bright and shifts
    // hues. The working AAP-64 generator mixed in linear light; so does this.
    inline double srgbToLinear(double c)
    {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }

    struct LinearRGB
    {
        double r = 0, g = 0, b = 0;
    };

    inline LinearRGB toLinear(RGB const &c)
    {
        return {srgbToLinear(c[0] / 255.0), srgbToLinear(c[1] / 255.0), srgbToLinear(c[2] / 255.0)};
    }

    // Oklab: a perceptually uniform space, used for the ERROR metric because euclidean RGB
    // distances do not match what the eye calls "far apart". This is the one deliberate
    // modernisation over the article's luminance-weighted RGB, carried over from the
    // AAP-64 generator that already works.
    struct Oklab
    {
        double L = 0, a = 0, b = 0;
    };

    inline Oklab linearToOklab(LinearRGB const &c)
    {
        const double L = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
        const double M = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
        const double S = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;

        const double L_ = std::cbrt(L), M_ = std::cbrt(M), S_ = std::cbrt(S);

        Oklab out;
        out.L = 0.2104542553 * L_ + 0.7936177850 * M_ - 0.0040720468 * S_;
        out.a = 1.9779984951 * L_ - 2.4285922050 * M_ + 0.4505937099 * S_;
        out.b = 0.0259040371 * L_ + 0.7827717662 * M_ - 0.8086757660 * S_;
        return out;
    }

    inline Oklab toOklab(RGB const &c)
    {
        return linearToOklab(toLinear(c));
    }

    // Squared Oklab distance. Squared because the penalty below is a distance added to it,
    // and dropping the square root does not move the minimum.
    inline double oklabDist2(Oklab const &x, Oklab const &y)
    {
        const double DL = x.L - y.L, DA = x.a - y.a, DB = x.b - y.b;
        return DL * DL + DA * DA + DB * DB;
    }

    // The article's psychovisual term, in Oklab:
    //     error = dist2(target, mix) + penalty * dist2(c1, c2) * (|ratio - 0.5| + 0.5)
    // It prefers a slightly less exact mix of two SIMILAR colours over an accurate mix of
    // two opposites, which is what stops a grey becoming a black/white stipple. Weighted
    // down when the mix is lopsided, because an 85/15 split is much less noisy than 50/50.
    inline double mixingError(Oklab const &want, Oklab const &mix, double pairDist2, double ratio, double psychovisual)
    {
        return oklabDist2(want, mix) + psychovisual * pairDist2 * (std::fabs(ratio - 0.5) + 0.5);
    }

    // ------------------------------------------------------------------- plans

    // One LUT record. `first`/`second` index into Palette::colours; a Bayer threshold
    // below `level` selects `second`, otherwise `first`.
    //
    // `level` is stored as a BAYER LEVEL, 0..RATIO_LEVELS (16), not as a 0..255 ratio:
    // an 8x8 Bayer matrix offers 64 thresholds, but 17 levels is visually ample and keeps
    // the record to one byte per field so the whole plan packs into one 32-bit word.
    struct Plan
    {
        uint8_t first  = 0;
        uint8_t second = 0;
        uint8_t level  = 0; // 0..RATIO_LEVELS, fraction of `second`
    };

    inline constexpr int RATIO_LEVELS = 16;

    // Packs a plan into one word: [ first:8 | second:8 | level:8 | reserved:8 ].
    // One 32-bit word per entry keeps a 32768-entry table at 128 KB of shader constants,
    // rather than the 256 KB three separate arrays would cost.
    inline uint32_t packPlan(Plan const &p)
    {
        return (uint32_t)p.first | ((uint32_t)p.second << 8) | ((uint32_t)p.level << 16);
    }

    inline Plan unpackPlan(uint32_t w)
    {
        return Plan{(uint8_t)(w & 0xFF), (uint8_t)((w >> 8) & 0xFF), (uint8_t)((w >> 16) & 0xFF)};
    }

    // Yliluoma's refined DeviseBestMixingPlan, in Oklab.
    //
    // For every unordered palette pair: project the target colour onto the segment between
    // the two entries (the "mathematical ratio" refinement, so there is no inner ratio
    // loop), quantise that to a level a Bayer comparison can express, compute what mixing
    // at that level REALLY produces in linear light, and score it with the psychovisual
    // error. O(N^2) per colour.
    inline Plan devisePlan(Palette const &pal, RGB const &want, double psychovisual)
    {
        const size_t N = pal.colours.size();

        // Per-colour data precomputed once, so the O(N^2) loop does no repeated maths.
        std::vector<Oklab>     OK(N);
        std::vector<LinearRGB> LIN(N);
        for (size_t i = 0; i < N; i++)
        {
            LIN[i] = toLinear(pal.colours[i]);
            OK[i]  = linearToOklab(LIN[i]);
        }
        const Oklab TARGET = toOklab(want);

        Plan   best;
        double leastPenalty = 1e99;

        for (size_t i = 0; i < N; i++)
        {
            for (size_t j = i; j < N; j++)
            {
                const Oklab &A = OK[i];
                const Oklab &B = OK[j];

                const double DR = B.L - A.L, DA = B.a - A.a, DB = B.b - A.b;
                const double PAIRDIST2 = DR * DR + DA * DA + DB * DB;

                double ratio = 0.0;
                if (PAIRDIST2 > 1e-12)
                {
                    const double NUM = (TARGET.L - A.L) * DR + (TARGET.a - A.a) * DA + (TARGET.b - A.b) * DB;
                    ratio            = NUM / PAIRDIST2;
                }
                if (ratio < 0.0)
                    ratio = 0.0;
                else if (ratio > 1.0)
                    ratio = 1.0;

                int level = (int)std::lround(ratio * RATIO_LEVELS);
                if (level < 0)
                    level = 0;
                else if (level > RATIO_LEVELS)
                    level = RATIO_LEVELS;

                // What the stipple actually averages to: a LINEAR-light blend of the pair.
                const double    RQ = (double)level / RATIO_LEVELS;
                const LinearRGB MIX{LIN[i].r * (1.0 - RQ) + LIN[j].r * RQ, LIN[i].g * (1.0 - RQ) + LIN[j].g * RQ,
                                    LIN[i].b * (1.0 - RQ) + LIN[j].b * RQ};
                const Oklab     MIXOK = linearToOklab(MIX);

                const double PENALTY = mixingError(TARGET, MIXOK, PAIRDIST2, RQ, psychovisual);
                if (PENALTY < leastPenalty)
                {
                    leastPenalty = PENALTY;
                    best         = Plan{(uint8_t)i, (uint8_t)j, (uint8_t)level};
                }
            }
        }
        return best;
    }

    // --------------------------------------------------------------------- LUT

    // Input key width. 5 bits per channel = 32768 entries, the agreed starting point.
    // Kept as a parameter everywhere so it can be raised once the shader is benchmarked.
    constexpr int DEFAULT_KEY_BITS = 5;

    inline size_t lutSize(int keyBits)
    {
        return (size_t)1 << (3 * keyBits);
    }

    // Quantises to the TOP `keyBits` bits of each channel. Truncation is used rather than
    // rounding so the shader can reproduce it with integer shifts, and float rounding
    // cannot make GPU and CPU disagree about which entry a colour belongs to.
    inline size_t keyFor(RGB const &c, int keyBits)
    {
        const int    SHIFT = 8 - keyBits;
        const size_t R = (size_t)(c[0] >> SHIFT);
        const size_t G = (size_t)(c[1] >> SHIFT);
        const size_t B = (size_t)(c[2] >> SHIFT);
        return (R << (2 * keyBits)) | (G << keyBits) | B;
    }

    // Representative colour of a key: the centre of its quantisation cell, so a key does
    // not systematically skew toward its lower corner.
    inline RGB colourForKey(size_t key, int keyBits)
    {
        const int    SHIFT = 8 - keyBits;
        const size_t MASK  = ((size_t)1 << keyBits) - 1;
        const int    STEP  = 1 << SHIFT;
        RGB          out;
        out[0] = (uint8_t)(((key >> (2 * keyBits)) & MASK) * STEP + STEP / 2);
        out[1] = (uint8_t)(((key >> keyBits) & MASK) * STEP + STEP / 2);
        out[2] = (uint8_t)((key & MASK) * STEP + STEP / 2);
        return out;
    }

    // Forces the key holding an exact palette colour to always draw that colour. Without
    // this, a cell centre can sit a hair off an entry and the stipple would visibly
    // "flicker" flat pixel art that is already using the palette.
    inline void preserveExactColours(std::vector<uint32_t> &lut, Palette const &pal, int keyBits)
    {
        for (size_t p = 0; p < pal.colours.size(); p++)
        {
            // Nearest OTHER entry, used as the never-drawn second colour.
            size_t                          other = p;
            double                          bestD = 1e99;
            const Oklab                     CP    = toOklab(pal.colours[p]);
            for (size_t q = 0; q < pal.colours.size(); q++)
            {
                if (q == p)
                    continue;
                const double D = oklabDist2(CP, toOklab(pal.colours[q]));
                if (D < bestD)
                {
                    bestD = D;
                    other = q;
                }
            }

            const size_t KEY = keyFor(pal.colours[p], keyBits);
            lut[KEY]         = packPlan(Plan{(uint8_t)p, (uint8_t)other, 0});
        }
    }

    inline std::vector<uint32_t> buildLut(Palette const &pal, int keyBits = DEFAULT_KEY_BITS, double psychovisual = 0.1)
    {
        std::vector<uint32_t> lut(lutSize(keyBits));
        for (size_t k = 0; k < lut.size(); k++)
            lut[k] = packPlan(devisePlan(pal, colourForKey(k, keyBits), psychovisual));

        preserveExactColours(lut, pal, keyBits);
        return lut;
    }

    // ---------------------------------------------------------------- bayer

    // 4x4 Bayer matrix, values 0..15, the same one the AAP-64 shader uses. It is paired
    // with RATIO_LEVELS = 16 so the compare is `BAYER4 < level`, with no scaling.
    constexpr std::array<uint8_t, 16> BAYER4 = {
        0, 8,  2,  10,
        12, 4, 14, 6,
        3, 11, 1,  9,
        15, 7, 13, 5,
    };

    // 8x8 Bayer matrix, values 0..63, the article's table. Kept available because the
    // agreed plan calls for 8x8; the ratio is then `level * 4` in the 0..63 space.
    constexpr std::array<uint8_t, 64> BAYER8 = {
        0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4,  36,
        14, 46, 6,  38, 60, 28, 52, 20, 62, 30, 54, 22, 3,  35, 11, 43, 1,  33, 9,  41,
        51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7,  39, 13, 45, 5,  37, 63, 31, 55, 23,
        61, 29, 53, 21,
    };

    // Picks the palette index to draw at (x, y) for one plan.
    //
    // `x`/`y` must be BLOCK coordinates (screen pixel divided by the block size), not
    // pixel coordinates: keying the pattern to the sampled pixel would repeat it inside
    // every block and show a grid instead of a dither.
    //
    // The comparison is done in the matrix's own 0..1 space rather than by shifting an
    // 8-bit ratio, so a 4x4 matrix (16 thresholds) and the 16 ratio levels stay exact:
    // `(B + 0.5) / 16 < level / 16` selects `second` for exactly `level` of the 16 cells.
    inline uint8_t ditherPixel(Plan const &plan, int x, int y)
    {
        const uint8_t B = BAYER4[(size_t)((y & 3) * 4 + (x & 3))];
        return ((int)B < plan.level) ? plan.second : plan.first;
    }
}
