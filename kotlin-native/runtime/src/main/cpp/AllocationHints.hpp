/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#pragma once

#include <cstddef>

namespace kotlin::alloc {

// Kore: the number of bytes at the start of the elements of the next array this thread allocates that the caller
// overwrites right away (e.g. the copied elements of a grown primitive array), so that they need not be zeroed first.
// Only for arrays whose elements the GC never reads. Allocators may ignore it; the caller resets it to 0 after the
// allocation.
extern thread_local size_t tlsArrayOverwrittenPrefixBytes;

} // namespace kotlin::alloc
