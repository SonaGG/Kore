/*
 * Copyright 2010-2021 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "MarkAndSweepUtils.hpp"

#include <cstdio>
#include <cstdlib>

#include "Porting.h"

namespace {

// Kore: with `KORE_GC_PAUSES` set in the environment, every stop-the-world pause is reported on stderr (to correlate
// stutters with collections without a special build).
bool reportPauses() noexcept {
    static const bool report = std::getenv("KORE_GC_PAUSES") != nullptr;
    return report;
}

uint64_t gPauseStartMicros = 0;
const char* gPauseReason = nullptr;
constexpr int kMaxPausePhases = 8;
const char* gPhaseNames[kMaxPausePhases];
uint64_t gPhaseEnds[kMaxPausePhases];
int gPhaseCount = 0;

} // namespace

void kotlin::gc::stopTheWorld(GCHandle gcHandle, const char* reason) noexcept {
    if (reportPauses()) {
        gPauseStartMicros = konan::getTimeMicros();
        gPauseReason = reason;
        gPhaseCount = 0;
    }
    mm::RequestThreadsSuspension(reason);
    gcHandle.suspensionRequested();

    mm::WaitForThreadsSuspension();
    gcHandle.threadsAreSuspended();
    pausePhase("suspend");
}

void kotlin::gc::resumeTheWorld(kotlin::gc::GCHandle gcHandle) noexcept {
    mm::ResumeThreads();
    gcHandle.threadsAreResumed();
    if (reportPauses() && gPauseReason != nullptr) {
        uint64_t end = konan::getTimeMicros();
        char phases[256];
        size_t length = 0;
        uint64_t previous = gPauseStartMicros;
        for (int i = 0; i < gPhaseCount && length + 40 < sizeof(phases); ++i) {
            length += std::snprintf(phases + length, sizeof(phases) - length, " %s=%llu", gPhaseNames[i],
                                    static_cast<unsigned long long>(gPhaseEnds[i] - previous));
            previous = gPhaseEnds[i];
        }
        phases[length] = 0;
        konan::consoleErrorf("[kore-gc] pause %llu us (%s)%s rest=%llu\n", static_cast<unsigned long long>(end - gPauseStartMicros),
                             gPauseReason, phases, static_cast<unsigned long long>(end - previous));
        gPauseReason = nullptr;
    }
}

void kotlin::gc::pausePhase(const char* name) noexcept {
    if (!reportPauses() || gPauseReason == nullptr || gPhaseCount >= kMaxPausePhases) return;
    gPhaseNames[gPhaseCount] = name;
    gPhaseEnds[gPhaseCount] = konan::getTimeMicros();
    ++gPhaseCount;
}
