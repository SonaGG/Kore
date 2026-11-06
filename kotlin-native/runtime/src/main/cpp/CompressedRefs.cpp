/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "CompressedRefs.hpp"

// Set by the heap reservation before any object is allocated (see `HeapReservation`), when references are compressed.
extern "C" uintptr_t Kotlin_heapBase = 0;
