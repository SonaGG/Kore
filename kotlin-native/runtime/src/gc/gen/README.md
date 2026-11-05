 Generational mark & sweep GC (`gc=gen`)

A non-moving, generational collector for the Kotlin/Native runtime. Enable it with `-Xbinary=gc=gen`.

## Why non-moving

Kotlin/Native objects cannot move. StableRefs, cinterop pointers, ObjC bridging and pinned objects expose raw object
addresses to native code. So the young generation is not a separate space that survivors are copied out of. It is a
*state* of an object: generations use **sticky mark bits**.

- **Young** means unmarked. Every new object is unmarked: `GC::ObjectData` is zero.
- **Old** means marked. Marks are *not* cleared by the sweeper ("sticky"). An object that survives one collection
  becomes old in place.

## Collections

### Minor (young) collection

Runs with the world stopped:

1. Collect roots: thread stacks, TLS, globals and StableRefs. Old objects are already marked, so marking skips them and
   stops tracing there.
2. Process the remembered sets (below). Every old→young edge created since the last collection is treated as a root.
3. Mark transitively. Only young objects get marked; marking never enters the old generation.
4. Process weak references.
5. Sweep only pages that were allocated into since the last collection (the allocator's `used` pages). Pages that
   weren't touched contain only old objects. Unmarked objects are freed, and marked ones stay marked, so they become
   old.

The cost is proportional to live young objects, plus remembered-set entries, plus pages touched since the last
collection. It does not depend on the size of the old generation.

A page that is reused for allocation is swept in full by the next young collection, including the old objects it
holds. So once young collections are running, the custom allocator does not hand out a partly-filled page for new
objects if more than half of it was live after its last sweep. Such pages are set aside ("parked") until the next full
collection.

The young generation size (the allocation volume between young collections) defaults to 64 MiB. Set it in MiB with
`-Xbinary=gcYoungGenerationSize=<n>`.

### Major (full) collection

1. Clear every mark (one linear pass over the heap) and drop the remembered sets.
2. Run an ordinary full mark from the roots and process weak references.
3. Sweep every page. Survivors stay marked, so the whole live heap is old afterwards.

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

One word before each heap object, as in the stop-the-world collector:

| Value                                       | Meaning                                       |
|---------------------------------------------|-----------------------------------------------|
| `0`                                         | young (unmarked)                              |
| non-zero                                    | marked/old: `1`, or a mark-queue link pointer |
| bit 1 (`kRemembered`) set on a marked value | old and already logged in an object buffer    |

Queue links are 8-byte aligned, so bit 1 is always free.
