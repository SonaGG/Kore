# Kore: removing the per-object GC word

Status: design. Prerequisite for compressed references (`compressed-refs.md`). The aim is to close the remaining
gaps to the JVM on code dominated by small objects (boxed `Int`s, 24-byte nodes).

## Today

The generational GC (`gc/gen`) puts one word, `GC::ObjectData` (`gc/gen/cpp/ObjectData.hpp`), before every heap
object through `alloc::HeapObject<HeapHeader>`. The word holds:

- bit 0 `kOld` (sticky; during nursery evacuation it marks a forwarded object instead);
- bit 1 `kRemembered`;
- bits 2+: the mark state, which is 0, `kMarkedNoQueue`, or the link of the intrusive mark queue
  (`intrusive_forward_list<ObjectData>` in `gc/gen/cpp/ConcurrentMark.hpp` and `ParallelProcessor.hpp`);
- the forwarding pointer, during the evacuation of the moving nursery.

A boxed `Int` therefore takes 24 bytes: 8 GC word + 8 type info + 4 value + 4 padding. Without the word it takes 16,
which is 33% less allocation and twice as many boxes per cache line as today's 24 bytes.

## Options

**A. Side bitmaps per Immix block.** Blocks already keep per-granule start bitmaps (`ImmixBlock.hpp`). Add mark, old
and remembered bitmaps: 3 × 512 bytes per 32 KiB block, about 4.7% of heap, against 33% for boxes today.
`ObjectData` becomes a view (block, bit index) computed from the object address. Large objects
(`SingleObjectPage`) keep the bits in the page header. Nursery blocks need their own bitmaps, and the forwarding
pointer moves into the evacuated object's first payload word (allowed: the source object is dead after copying, and
every object has at least one word after its header except `Any()`, whose copy can be found through a small side
table).

**B. GC bits in the header word.** Align type infos and meta objects to 32 bytes. That frees bits 2–4 of the
`typeInfoOrMeta_` word for marked, old and remembered.
- Header reads: `ObjHeader::type_info()` and the compiler's `loadTypeInfo` (`CodeGenerator.kt`) mask with `~31`
  instead of `~3`.
- Meta object installation (`createMetaObject`) must CAS on the full word and keep the bits.
- Mutators set the remembered bit with an atomic OR on the header. That is the same cache line as the type info
  load, so it costs less than today's separate word.
- Needs no side tables, and works for every allocator (Immix, large pages, nursery), with the forwarding pointer as
  in A.
- It changes how compiled code reads type infos, so it needs a compiler/runtime version pairing. The klib ABI is
  unaffected.

**Recommendation: B.** It is fewer moving parts, has no per-block bitmap lookups on the barrier fast path, and works
for all object kinds. A is the fallback if 32-byte type info alignment turns out to be a problem (e.g. ObjC export
type infos created at run time: they come from the runtime's allocator, which can align them).

## Common work (both options)

1. **Array-based mark stacks.** Replace `intrusive_forward_list<ObjectData>` in `ConcurrentMark` and
   `ParallelProcessor` with per-worker chunked stacks of `ObjHeader*` (512 entries per chunk), shared through a
   lock-free chunk pool for work stealing. Marking becomes "test-and-set the mark bit, push if newly marked". The
   queue link no longer needs to live in the object. This step is independently testable, and should be neutral or
   faster: no CAS on the link, better locality.
2. **Header size zero for `gen`.** `HeapObject<HeapHeader>` with an empty header type for the generational GC:
   - `type_layout::Composite` must support a zero-size first field;
   - `alloc::objectForObjectData` and `objectDataForObject` become identity-like conversions;
   - the compiler's inline bump allocation ("Inline the bump allocation of objects into compiled code") stops
     reserving and clearing the word;
   - static data (permanent objects) and stack objects need no GC word either.
3. **Barriers** (`gc/gen/cpp/Barriers.*`, `rememberOldHolderIfYoungValue`): read old and remembered from the new
   place. Keep the inlined young-holder fast path one load and one test.
4. **Sweep** (`ImmixBlock::Sweep`, `SingleObjectPage`, nursery release): clear the mark state and set old, in the new
   place.
5. **Verification:** `KORE_GC_VERIFY_YOUNG` and `KORE_GC_VERIFY`. Their checks of old-to-young edges and released
   nursery blocks use the same accessors.

## Staging and validation

- Behind `-Xbinary=gcWord=false` (runtime constant `Kotlin_gcWord`), default on until validated. The cms, pmcs and
  stms collectors keep their `ObjectData` untouched.
- Runtime C++ tests in all configurations, plus new tests for the mark stack (work stealing, termination with array
  slices) and for the header bits (concurrent remember versus meta object install).
- GC stress:
  - allocation-heavy multithreaded programs, with and without `KORE_GC_VERIFY_YOUNG=1`;
  - kotlinx.coroutines with `Dispatchers.Default`;
  - weak references and `identityHashCode` (meta objects) racing with marking.
- Measure boxedList, lambdas, hashMap, strings, sortObjs, the game benchmark (pause phases with `KORE_GC_PAUSES=1`)
  and Calista. Expected: allocation volume about −25% on box-heavy code, and shorter young pauses (fewer cache misses
  per traced object).

## Effort

Several days. The mark stack rework (1) and the zero-size header plumbing (2) are each a day or more, then barriers,
sweep and nursery forwarding, then a stress campaign. It should land as separate commits in that order, each passing
the runtime tests.
