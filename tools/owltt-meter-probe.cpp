/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * DRIVE REAL AUDIO THROUGH A OWLTT AND PRINT PER-BAND GAIN REDUCTION.
 *
 * WHY IT EXISTS. It was reported that the bars did not respond to any audio, and the first two diagnoses were
 * guesses that moved the problem rather than finding it. This harness settled it in one run: the
 * meters work, the callback fires, and the compression itself is inverted across bands.
 *
 * WHAT IT FOUND, and the numbers are the point rather than the tool:
 *
 *   220 Hz, ratio 0.2..0.7 -> the MID band reports exactly 0.000 dB at every ratio, while being the
 *                             LOUDEST band at -2.5 dB.
 *   900 Hz, ratio 0.7      -> GR [68.014, 0.006, 0.000] against input [-41.5, -8.5, -2.5]:
 *                             the QUIETEST band reports 68 dB of reduction and the LOUDEST reports
 *                             none.
 *
 * The band with signal does not compress; the band with none runs away. That is either an inverted
 * up/down decision, a band index mismatch between the compressor and the meters, or a threshold in
 * the wrong domain -- and it is the reason "the bars move with ratio but not with audio", because
 * what moves is upward compression acting on near-silence.
 *
 * THIS IS ALSO THE ASSEMBLY TEST THE BRIEF ASKED FOR. `tests/test_fx_owltt.cpp` drives one band and
 * asserts the others stay near unity; this prints the numbers so a human can see WHICH way it is
 * wrong rather than only that it is.
 */
#include "fx/owltt/owltt_types.h"
#include "fx/owltt/owltt.h"
#include <array>
#include <cstdio>
#include <cmath>
static float kProbeHz = 220.0f;
int main() {
    using namespace feathers::fx::owltt;
    alignas(Owltt) static unsigned char storage[sizeof(Owltt)];
    auto* d = new (storage) Owltt();
    d->prepare(48000.0);
    d->setSlot(0);

    std::array<float,3> gr{}, lvl{};
    int fired = 0;
    std::array<float,3> outLvl{};
    bool clipped = false;
    d->setMeterCallback([&](size_t, const std::array<float,3>& g, const std::array<float,3>& l,
                            const std::array<float,3>& o, bool clip){
        gr = g; lvl = l; outLvl = o; clipped = clip; ++fired;
    });

    for (float ratio : {0.2f, 0.3f, 0.4f, 0.5f, 0.7f}) {
        Row row{};
        for (size_t i = 0; i < row.size(); ++i) row[i] = static_cast<float>(kParamDescs[i].def);
        row[static_cast<size_t>(Param::Enabled)] = 1.0f;
        row[static_cast<size_t>(Param::Ratio)]   = ratio;
        d->setTarget(stateOf(row));

        float L[512], R[512];
        for (int blk = 0; blk < 40; ++blk) {
            for (int i = 0; i < 512; ++i) { L[i] = R[i] = 0.7f * std::sin(6.2831853f * kProbeHz * (blk*512+i) / 48000.0f); }
            d->processStereo(L, R, 512);
        }
        std::printf("ratio %.1f -> GR [%.2f %.2f %.2f]  in [%.1f %.1f %.1f]  out [%.1f %.1f %.1f] clip=%d\n",
                    ratio, gr[0], gr[1], gr[2], lvl[0], lvl[1], lvl[2], outLvl[0], outLvl[1], outLvl[2], clipped ? 1 : 0);
    }

    // DOES THE MID BAND EVER COMPRESS? Drive a tone squarely between the crossovers.
    kProbeHz = 900.0f;
    {
        Row row{};
        for (size_t i = 0; i < row.size(); ++i) row[i] = static_cast<float>(kParamDescs[i].def);
        row[static_cast<size_t>(Param::Enabled)] = 1.0f;
        row[static_cast<size_t>(Param::Ratio)]   = 0.7f;
        d->setTarget(stateOf(row));
        float L[512], R[512];
        for (int blk = 0; blk < 60; ++blk) {
            for (int i = 0; i < 512; ++i) { L[i] = R[i] = 0.7f * std::sin(6.2831853f * kProbeHz * (blk*512+i) / 48000.0f); }
            d->processStereo(L, R, 512);
        }
        std::printf("\n900 Hz @ 0.7 -> GR [%.2f %.2f %.2f]  in [%.1f %.1f %.1f]  out [%.1f %.1f %.1f] clip=%d\n",
                    gr[0], gr[1], gr[2], lvl[0], lvl[1], lvl[2], outLvl[0], outLvl[1], outLvl[2], clipped ? 1 : 0);
    }
    d->~Owltt();
    return 0;
}
