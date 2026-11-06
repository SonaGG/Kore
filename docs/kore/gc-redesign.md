# Kore: memory management v3 (compressed heap, full Immix, low-pause GC)

Status: plan. This replaces the incremental tuning of the generational mark-sweep collector (`gc/gen`) with a
redesign in phases. Each phase is selectable with a binary option and is benchmarked against the previous default
before it becomes the default. The phases build on `compressed-refs.md`, `gc-word.md` and `memory-v2.md`, which they
supersede where they differ.

## Goals and how they are measured

| Goal | Target | Measured by |
|---|---|---|
| Memory overhead | Peak RSS at or below upstream Kotlin/Native and the JVM (G1, default flags) on the same workload; live heap at most 0.6x today's for reference-heavy data | Compose huge tree (cold peak RSS, live heap after full GC), gamebench, rtbench, Calista |
| Mutator cost | No measurable slowdown against a build with no GC barriers (within run-to-run noise, about 1%) | rtbench, kostya and mlab suites, gamebench steady frame |
| Reclamation | Garbage of any size and age is reclaimed within one young cycle or one concurrent cycle; free memory goes back to the system | heap stats (`KORE_HEAP_STATS`), RSS over time in Calista and the Compose bench |
| Pauses | Stop-the-world pauses under 1 ms at the 99th percentile on 100 MiB heaps; no pause grows with heap size | `KORE_GC_PAUSES` histograms in Calista, gamebench and the Compose bench |

## Where we are (v2)

- Allocation: Immix blocks (32 KiB, 256-byte lines) for objects up to 8 KiB, bump allocation inlined in compiled
  code; larger objects get a `SingleObjectPage` each. Blocks are never returned to the system.
- Objects: 8-byte header (`TypeInfo*` plus tag bits 0-1 and GC bits 2-4: mark, old, remembered), 8-byte references,
  no GC word (`gc-word.md`).
- Collection: generational sticky-mark (non-moving). Young collections stop the world and mark in parallel from
  stacks (conservatively), globals and the remembered set (holder-logging barrier). Full collections mark
  concurrently with a snapshot-at-the-beginning (SATB) barrier, then sweep concurrently.
- Known problems:
  - Old garbage, large arrays in particular, is reclaimed only by full collections. On the Compose huge tree this is
    most of the 50 MiB peak gap to upstream: about 100 MiB of promoted-then-dropped arrays at the peak against 38 MiB
    live.
  - No defragmentation: sparse blocks stay sparse until all their objects die.
  - Memory is never returned to the system.
  - Every reference and header costs 8 bytes: a boxed `Int` takes 16 bytes and a 3-field node 32.

## Design (v3)

The design follows LXR (Zhao, Blackburn and McKinley, PLDI 2022), which builds on Immix (Blackburn and McKinley,
PLDI 2008) and RC Immix (Shahriyar et al., OOPSLA 2013), adapted to Kore's constraints: conservative stack scanning,
interop pinning, and no read barriers.

### 1. Compressed heap

See `compressed-refs.md` for the encoding.

- One reserved address range for the whole Kotlin heap (32 GiB by default), committed on demand. All heap memory
  comes from it: Immix blocks, large objects and meta objects. `calloc` is no longer used for the heap.
- References in heap objects and in `Array<T>` elements are 32 bits: `(address - base) >> 3`. Locals, arguments,
  globals and interop values stay full pointers.
- Object headers are 4 bytes: a 30-bit type id indexing the type info table, plus the 2 tag bits (stack and permanent
  objects). A boxed `Int` then takes 8 bytes instead of 16, and a node with three references 16 bytes instead of 32.
- GC state moves out of the header into **side metadata** at fixed offsets from the heap base. Chunks are 4 MiB, and
  the address of a metadata bit is computed with a shift and an add, with no lookup:
  - mark bitmap: 1 bit per 8 bytes, 1.6%;
  - log bits for the barrier: 1 bit per 32-bit reference slot, 3.1% of the reference fields only, laid out per
    8-byte granule, so 1.6% of the heap;
  - RC counts: 2 bits per 8 bytes, 3.1%;
  - line and block tables: 1 byte per 256-byte line, 0.4%.
  The total, about 7%, is less than the 8 bytes per object that the 4-byte header and the compressed references save.
- Conservative stack scanning first checks the candidate against the reservation (two compares), then finds the
  object through the object-start bitmap. The page registry lookup goes away.
- Meta objects (`ExtraObjectData`: weak references, associated objects) move to a side hash table keyed by object
  address, with a "has meta" bit in the side metadata.

### 2. Full Immix

- Lines of 128 bytes. The current 256-byte lines suit 16-byte-header objects; with 4-byte headers most objects fit in
  16-32 bytes. Blocks stay 32 KiB.
- Mark-time line marking (not sweep-time): the tracer marks the object's lines when it scans the object, using
  conservative line marking for small objects (one line mark, with the allocator skipping the first line of each hole).
  This removes the sweep's object walk: a block's free lines are known when marking ends.
- Lazy sweeping: free lines are discovered by the allocator while it takes holes, so there is no sweep phase for
  Immix blocks. Only objects with finalizers or meta objects need visiting, and they are tracked in a side list.
- Overflow allocation for medium objects (already present) and recycling with F = 1.
- **Opportunistic evacuation** (defragmentation), folded into the trace: at the start of a collection that
  defragments, pick candidate blocks from the mark and available histograms, as in the paper (§3.2.1). The tracer
  copies unpinned objects out of candidate blocks and leaves forwarding pointers in the old copies, then updates the
  reference it came through. Pinned objects are marked in place:
  - objects referenced conservatively from stacks;
  - objects with stable references, `pin`, or C pointers into their payload;
  - objects reached while target space is exhausted.
  Headroom: 2.5% of the heap in free blocks is kept for evacuation.
- Evacuation only happens in stop-the-world phases (see 3), so the mutator never sees a forwarded object and needs no
  read barrier.

### 3. Collection: LXR-style RC plus concurrent tracing

**Young objects** are bump-allocated into free blocks (or recycled lines) and are not counted.

**Epoch pause (stop the world, the only regular one), every N MiB of allocation (pause-target controlled):**
- Scan stacks conservatively; the objects found are pinned for this epoch.
- Apply the logged increments: the barrier logged the old values of modified fields, so the new values get +1 and
  the old values get −1. Decrements are buffered for concurrent processing.
- Trace young survivors from the roots and the increments, and evacuate them (copying, so the young space is
  compacted) into mature Immix blocks, or mark them in place when pinned. Their fields' referents get +1.
- Young lines with no survivors are free right away; there is no sweep.
- Cost: proportional to survivors plus logged fields, as in today's young collections, bounded by the controller.

**Concurrently with the mutators, after each epoch:**
- Process the decrements. An object reaching 0 is freed: its lines' live counts drop, it recursively decrements its
  children (lazily, with a work list), and finalizers are queued. Large objects (8 KiB and up) reaching 0 are
  unmapped or returned to the large-block cache at once. This fixes the promoted-and-dropped array problem.
- RC counts saturate at 3 ("stuck"). Stuck objects and cycles are reclaimed by the backup trace.

**Backup trace (concurrent SATB mark, the existing `ConcurrentMark`), triggered by heap growth or stuck-count
volume:**
- Marks live mature objects concurrently. The SATB barrier is the same field-logging barrier, since it already
  records old values.
- At its end, in the next epoch pause, it resets stuck counts to the traced counts (or to "marked" only), frees
  unmarked mature objects (concurrently), and selects mature defragmentation candidates. Remembered sets for
  candidate blocks are built during the trace (references into candidates), so the next pause can evacuate them and
  fix the references.

**Barrier** (the one barrier for RC, SATB and the generational boundary): a coalescing field-logging write barrier.
- Fast path, inlined: load the slot's log bit; if the slot is already logged in this epoch, store and continue.
- Slow path, out of line: set the bit, push (slot address, old value) to the thread-local log buffer.
- Young objects are allocated with their log bits set ("already logged"), so stores into fresh objects, the
  overwhelming majority, take the fast path, like today's young-holder check.
- Bulk array copies log once per destination range (card-style), like `copyHeapRefsInObject` today.
- Same instruction count on the fast path as the current holder barrier: one load, one test, one branch.

**Pause-time controller:** sizes the epoch (allocation volume between pauses) from the measured cost per surviving
byte and per log entry, aiming at a target pause (default 1 ms, `-Xbinary=gcPauseTargetMicros=`).

**Returning memory:** free blocks beyond a headroom (the larger of 10% of the live heap and 8 MiB) are decommitted
(`MEM_DECOMMIT` or `madvise(MADV_DONTNEED)`) by the GC thread after a concurrent cycle, oldest first. Their address
space stays reserved, and committing again is lazy.

### 4. No-cost guarantees for the mutator

- Allocation stays an inlined bump with one compare. Young objects need no header GC bits; the log bits for a fresh
  block are set in bulk when the block is taken.
- No read barrier, ever. Objects move only in pauses, and every reference to a moved object is fixed before the
  mutators resume: roots by the stack scan (pinned objects don't move), heap references by the trace or the
  remembered sets of candidate blocks.
- Type info loads stay invariant and hoistable (`table[id]` with an `!invariant.load` table base).
- Reference loads decode as `[base + ref*8]`, with the base in a register hoisted out of loops.
- Safepoint polls are unchanged.

## Phases

Each phase ends with:
- the runtime tests, all GC configurations;
- the GC stress programs with `KORE_GC_VERIFY*`;
- the suites (`run-all.sh`), rtbench, gamebench and the Compose bench (time and peak RSS);
- a commit.

Defaults change only when a phase is no slower on any benchmark beyond noise and passes everything.

1. **Heap reservation** (`-Xbinary=heapReserve=`): all heap memory from one reserved range, with chunked side
   metadata. Still 8-byte references and headers. Conservative lookups through the range check and the start bitmap.
   Decommit of free blocks. Measure: no regression; RSS drops on the Compose bench.
2. **Side GC bits:** mark, old and remembered bits move from the header to the side bitmaps, and the barrier checks
   side bits. The header word holds only the type info and tags. Measure: no regression.
3. **Compressed references** (`-Xbinary=compressedRefs=`): compiler (`LlvmTypes`, field and element GEPs,
   `encodeRef`/`decodeRef`, barriers, bulk copies, static data in the reservation's first chunk), runtime (`RefSlot`
   accessors everywhere a heap slot is read or written, enforced by type), interop boundaries.
4. **4-byte headers:** type ids, the type info table, and meta objects in a side table. Then make 3 and 4 the default
   once box tests and stress pass with them on.
5. **Full Immix:** 128-byte lines, mark-time line marking, lazy sweeping, opportunistic evacuation in full
   collections (stop the world in the first version), pinning. Benchmark it against v2 sticky-mark Immix.
6. **LXR:** field-logging barrier, epoch pauses with young evacuation, concurrent decrements, immediate reclamation of
   large objects, backup trace with mature evacuation sets, pause-time controller. Benchmark it against phase 5's
   collector, then pick the default.

## Risks

- Phase 3 touches every heap slot access in the runtime, and a missed raw access corrupts memory silently. Mitigate
  with the `RefSlot` type: raw `ObjHeader**` heap slots fail to compile.
- Moving objects with conservative stacks: anything a stack word may point to is pinned. Interior pointers in
  registers (C interop into array payloads) must pin too; `usePinned` and `refTo` already pin explicitly, so mark
  those objects as pinned.
- RC adds work proportional to pointer mutations of old objects. LXR shows this is cheaper than tracing for typical
  workloads, but mutation-heavy old graphs (large hash maps being rebuilt) need measuring. The fallback is the
  phase 5 collector, kept as an option.
- The 32 GiB reservation can fail under address-space limits. Then fall back to smaller reservations, and as a last
  resort to uncompressed references (a different binary).
