/*
 * Copyright 2010-2021 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "MarkAndSweepUtils.hpp"

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

} // namespace

void kotlin::gc::stopTheWorld(GCHandle gcHandle, const char* reason) noexcept {
    if (reportPauses()) {
        gPauseStartMicros = konan::getTimeMicros();
        gPauseReason = reason;
    }
    mm::RequestThreadsSuspension(reason);
    gcHandle.suspensionRequested();

    mm::WaitForThreadsSuspension();
    gcHandle.threadsAreSuspended();
}

void kotlin::gc::resumeTheWorld(kotlin::gc::GCHandle gcHandle) noexcept {
    mm::ResumeThreads();
    gcHandle.threadsAreResumed();
    if (reportPauses() && gPauseReason != nullptr) {
        konan::consoleErrorf("[kore-gc] pause %llu us (%s)\n",
                             static_cast<unsigned long long>(konan::getTimeMicros() - gPauseStartMicros), gPauseReason);
        gPauseReason = nullptr;
    }
}
