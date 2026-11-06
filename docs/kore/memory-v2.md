# Kore memory v2: Immix blocks in the custom allocator

Status: design, in progress. Runtime-only: object headers, klib format and ABI do not change.

## Why

The current heap combines the upstream free-list allocator (fixed-block, next-fit and single-object pages), a moving
nursery (`Nursery.cpp`) that copies survivors into those pages, and a sticky-mark generational GC (`gc/gen`). Allocation
from free lists scatters objects (bad locality, e.g. game entity ticks), the nursery's copying needs pinning and
forwarding protocols around conservative stack scanning, and young pauses grow with every survivor copied.

## Design

Immix (Blackburn & McKinley, PLDI 2008) with sticky mark bits for generations, inside `alloc/custom`:

- **Blocks and lines.** Small and medium objects (up to `kImmixMaxObjectBytes` = 8 KiB) live in 32 KiB `ImmixBlock`s,
  divided into 128 lines of 256 bytes. Larger objects keep using `SingleObjectPage`s. Extra object data keeps its pages.
- **Block metadata.** Per block: a line-occupancy byte map (128 bytes) and an object-start bitmap (1 bit per 8 bytes,
  512 bytes), set at allocation. All objects are 8-byte aligned (`alignas(8)` everywhere; `CustomHeapObject::size()`
  is not rounded: always step by `AllocationSize::bytesAtLeast(size)`).
- **Allocation.** Per thread (`CustomAllocator`): a bump cursor and limit inside the current hole (a run of free lines
  of the current block). Fast path: `cursor + size <= limit` then bump. Slow path: next hole in the block, else the
  next recyclable block (partly free, from the heap), else a free block. Medium objects (> 1 line) that do not fit the
  hole use a separate per-thread overflow block (Immix "overflow allocation") instead of skipping holes. Holes are
  zeroed when they are taken (objects must start zeroed).
- **Liveness and sweeping.** The GC marks objects exactly as today (gen GC `ObjectData` word, sticky: old objects stay
  marked between young collections). The sweep (existing concurrent sweep phase, per block, in parallel) walks the
  start bitmap: dead objects are finalized/released as today (`ObjectSweepTraits::trySweepElement` semantics: extra
  object data, finalizers), their start bits cleared; live objects mark the lines they cover. Free lines (with the
  usual "conservative line marking": a line after a live object's last line is also considered used if the object
  may extend into it) become holes. Blocks are then free (no live line), recyclable (some free lines) or full.
  Young collections only sweep blocks that received allocations since the previous collection (as `PrepareForYoungGC`
  does with `used_` pages today).
- **Conservative roots and slots.** `Heap::ForEachHeapSlot` resolves an address inside an Immix block through the start
  bitmap: the closest start bit at or before the address, then a bounds check with the object's allocated size.
- **No moving in the first version.** The nursery is not used with Immix (`gcNurserySize` is ignored). Optional later
  step: opportunistic evacuation of sparse blocks (Immix defragmentation), skipping objects referenced conservatively.
- **Selection.** Binary option `-Xbinary=immix=true` (runtime constant `Kotlin_immix`, `compiler::immix()`), off by
  default until it beats the current allocator on Calista and the benchmarks; then it becomes the default and the
  fixed-block/next-fit pages and the nursery are removed.

## Latency

- Every hot loop reaches a safepoint poll within a few microseconds (no loop without a poll when no callee is
  guaranteed to poll); long runtime operations poll or run in the native state.
- Young collections: stop-the-world parallel marking (GC helper threads), pause target controller sizing the young
  generation (`HeapGrowthController`) from measured pauses.

## Compiler

- Profile-guided optimization: binary options to build an instrumented binary and to use an LLVM profile.
