 Generational mark & sweep GC (`gc=gen`)

A non-moving, generational collector for the Kotlin/Native runtime. Enable it with `-Xbinary=gc=gen`.

## Why non-moving

Kotlin/Native objects cannot move. StableRefs, cinterop pointers, ObjC bridging and pinned objects expose raw object
addresses to native code. So the young generation is not a separate space that survivors are copied out of. It is a
*state* of an object, kept in its GC word (`GC::ObjectData`, below):

- **Young** means not old. Every new object is young.
- **Old** is a sticky bit, set by the sweeper on every object that survives a collection: an object that survives one
  collection becomes old in place.

The mark state is separate from the old bit and is reset by every sweep, so a full collection can start marking without
first clearing marks.

## Collections

### Minor (young) collection

Marking runs with the world stopped, on the GC thread:

1. Collect roots: thread stacks, TLS, globals and StableRefs. Marking skips old objects and stops tracing there.
2. Process the remembered sets (below). Every old→young edge created since the last collection is treated as a root.
3. Mark transitively. Only young objects get marked; marking never enters the old generation.
4. Process weak references.
5. Resume the world, then sweep concurrently (mutators sweep the pages they need themselves). Only pages that were
   allocated into since the last collection (the allocator's `used` pages) are swept; untouched pages contain only old
   objects. Unmarked young objects are freed, marked ones become old, unmarked old objects are kept.

The cost is proportional to live young objects, plus remembered-set entries, plus pages touched since the last
collection. It does not depend on the size of the old generation.

A page that is reused for allocation is swept in full by the next young collection, including the old objects it
holds. So once young collections are running, the custom allocator does not hand out a partly-filled page for new
objects if more than half of it was live after its last sweep. Such pages are set aside ("parked") until the next full
collection.

The young generation size (the allocation volume between young collections) defaults to 64 MiB. Set it in MiB with
`-Xbinary=gcYoungGenerationSize=<n>`.

### Major (full) collection

Uses the `cms` collector's concurrent marker (snapshot-at-the-beginning deletion barrier, objects allocated during
marking are marked, mutator-local mark queues flushed at safepoints):

1. Stop the world, collect the mutators' roots, enable the barriers, resume.
2. Mark the whole heap concurrently with the mutators, then process weak references (with a weak-reference read
   barrier). If marking keeps finding new work, it finishes with the world stopped.
3. Stop the world briefly: disable the barriers, keep the remembered-set entries of live objects for the next young
   collection, resume.
4. Sweep every page concurrently. Survivors become old, unmarked objects are freed.

`GC.collect()`, `GC::Schedule()` and the timer always request a major collection. Allocation pressure requests a young
collection through `GC::ScheduleYoung()`. The collector upgrades that to a major one when the heap after the last young
collection exceeds the major threshold. That threshold is the live heap after the last major collection divided by
`targetHeapUtilization`, i.e. the old generation doubled.

## Write barrier and remembered sets

A minor collection must find every reference stored into an old object that points to a young object.

- **Object buffer (holder-aware path, the common case).** Compiled code stores into instance fields through
  `UpdateHeapRefInObject(holder, slot, value)`; the runtime's array helpers (`Array.set`, `fill`, `copyInto`) do the
  same. Its barrier, `gc::beforeHeapRefUpdateInObject`, works like this:
  - A store into a young holder returns after one load of the holder's GC word. This covers object initialization and
    most stores in allocation-heavy code; nothing is recorded.
  - The first time an old holder receives a young value, the holder is logged in a thread-local buffer, and a spare bit
    in `GC::ObjectData` (`kRemembered`) records that it was logged. A minor collection rescans each logged holder.
  - `gc::afterReferenceStoreInObject(holder, value)` applies the same logging to stores that bypass the store
    functions. Attaching a weak-reference implementation object to an object's `ExtraObjectData` is the one such store.
- **Slot buffer (generic path).** The remaining stores reach `gc::beforeHeapRefUpdate(slot, value)` without a holder:
  volatile and atomic fields, atomic array elements, globals and thread-locals. When `value` is a young heap object,
  the slot address is appended to a thread-local buffer.
  - A minor collection resolves the buffered slots against the heap's pages: it sorts them, finds the containing page,
    then the containing object.
  - A slot inside an old object is treated as a root. Other slots are ignored: young objects get traced anyway, and
    stack, global and TLS slots are roots already.

Each buffer is per thread. When a thread exits, its buffers are handed over to the collector. That happens while the
thread registry lock is held, so nothing is lost.

## `GC::ObjectData`

One word before each heap object:

| Bits | Meaning |
|---|---|
| bit 0 (`kOld`) | the object survived a collection. Set by the sweeper; cleared only when the object is freed |
| bit 1 (`kRemembered`) | the (old) object is logged in a remembered set since the last young collection |
| bits 2+ | mark state: `0` unmarked; `4` marked; otherwise a mark queue link (8-byte aligned pointer) |

All updates are atomic read-modify-writes, because mutators set `kRemembered` while markers and sweepers update the rest.
