# Kore: compressed 32-bit references and 4-byte object headers

Status: compressed references implemented (`-Xbinary=compressedRefs`, gc=gen with Immix, no nursery, 64-bit MinGW and
Linux), on by default for optimized MinGW executables, zero-based there, with 4-byte object headers in effect (compact headers). See "Implementation" below; the design that follows is the original plan, and
the implementation differs where noted. It changes the in-memory object layout, but not the klib format or the Kotlin
ABI.

## Implementation

- **Encoding.** `ref = (address - Kotlin_heapBase) >> 3`, 0 for null; decoding is `ref == 0 ? null : base + ref * 8` (a
  `test` and a `cmov` around one `lea`, and nothing more where the compiler knows the reference is not null). The base
  is not zero: Windows places ASLR images above 32 GiB, and permanent objects in the image must stay encodable. It is
  the 32 GiB-aligned address below the image, so that encoding is one shift (`address >> 3`, truncated: the base
  vanishes modulo 2^32, and null encodes to 0). The heap reservation (`HeapReservation`) is the largest free range of
  that 32 GiB span, at least 4 GiB above the base (see the stack scan below).
- **What is compressed.** Reference fields of objects (heap, stack and permanent) and `Array` elements. Static fields,
  thread locals and locals stay full pointers. Runtime: `HeapRefSlot` (`CompressedRefs.hpp`) and the slot-aware
  `DirectRefAccessor`; the `*HeapRef*` entry points take 32-bit slots, the new `*VolatileStaticRef` ones full ones.
  Compiler: `i32` field and element types (`heapRefType`), `loadHeapRef`/`decodeRef` for loads, stores through the
  runtime barriers (which encode).
- **Permanent objects.** Their references are relative to a base known at run time only: they are emitted as 0 into
  writable globals, and a module constructor calls `Kotlin_applyCompressedRefFixups` with the (slot, object) pairs.
- **Stack objects.** A stack address cannot be encoded. Escape analysis keeps on the heap every object that may be
  stored into a field (`nodesStoredIntoFields`: nodes whose drain is the drain of a field node), and treats a parameter
  stored into a field as escaping, since the field may belong to an object of a callee that its summary hides.
- **Conservative stack scan.** Compiled code may hold a reference across a call as a 32-bit value, or as its byte
  offset from the base (`ref * 8`, a common subexpression of decoding). The scan also tries both halves of each stack
  word as 32-bit references, and words below 32 GiB as offsets. With the heap 4 GiB or more above the base, 32-bit
  integers in either form never point into the heap.
- **Caches.** Compiler caches (of full-pointer code) are not used with compressed references.
- **Zero-based mode** (`-Xbinary=compressedRefsZeroBased`, MinGW executables, the default there): the executable is
  linked at a fixed address below 4 GiB (no ASLR for it), the heap reserved below 4 GiB too (both free ranges around the
  shared user data page at 2 GiB, about 4 GiB), and a reference is the low 32 bits of the address: decoding is a zero
  extension, with no null check. CPU-bound code runs as fast as with full pointers (pointer chasing and virtual calls
  8-10% faster), and objects are smaller. Heaps beyond 4 GiB need `-Xbinary=compressedRefsZeroBased=false`.
- **Results** (mingw_x64): the Compose huge-tree cold run peaks at 96-107 MiB (101 MiB upstream, 130-156 MiB without
  compressed references), with the first composition in 125 ms (221 ms without, 419 ms upstream). Objects with three
  references take 20 bytes instead of 32. CPU-bound microbenchmarks range from 8% faster (pointer chasing) to 20% slower
  (HashMap), the game benchmark is 5% slower: not the default yet.

## Why

On 64-bit targets every reference field, array element and object header word takes 8 bytes. Typical Kotlin heaps
(small objects, lists of references, maps) are dominated by those words. Halving them gives:

- about 30-45% less heap for reference-heavy data, so more of it fits in the caches;
- half the memory traffic for marking, sweeping and the young generation's remembered slots;
- twice as many references per cache line in `Array<T>` scans and in `ArrayList`/`HashMap` internals.

The JVM gets this from CompressedOops and compact object headers, and it is a large part of why the JVM beats Kore on
pointer-chasing code at equal algorithmic work.

## Design

### Heap reservation and encoding

- At startup, reserve one contiguous range of address space for the whole Kotlin heap, 32 GiB by default (an
  `-Xbinary=heapReserve=` option, and `KORE_HEAP_RESERVE` at run time). Commit memory on demand: `VirtualAlloc`
  `MEM_RESERVE` then `MEM_COMMIT` on MinGW, `mmap(PROT_NONE)` then `mprotect` on POSIX. Immix chunks, single-object
  pages and extra-object pages all come from this range. They no longer use `calloc`.
- A reference is `uint32_t ref = (address - heapBase) >> 3`. All heap objects are already 8-byte aligned, so 32 GiB is
  addressable. Null is 0; the first page of the reservation is never mapped, so no object has offset 0.
- `heapBase` is a constant global. It is loaded once per function and hoisted like the type info loads
  (`!invariant.load`). Decoding is `base + (zext(ref) << 3)`, which is one `lea`/address mode on x86-64:
  `[base + ref*8]`.

### What gets compressed

- Reference fields of heap objects and the elements of `Array<T>`. These are the bulk of the bytes.
- Not compressed: locals, arguments and return values, which stay full `ptr` in LLVM IR. This keeps conservative stack
  scanning unchanged and keeps every C/ObjC interop boundary unchanged. Stack-allocated objects' fields are compressed
  like heap ones, but a stack object lives outside the reservation, so it cannot be encoded. Escape analysis already
  guarantees that no heap field points at a stack object. A stack object's own fields hold heap references, which
  encode as usual.
- Globals (static fields) and thread locals stay full pointers: few in number, and the GC root scanning code stays
  simple.
- Permanent (static data) objects: place them in the reservation's first chunk at link time, which needs a dedicated
  section and a startup copy. Alternatively, give them a second base (`ref` tag bit 31 selecting the image base),
  decoded with a `cmov`. The first option is simpler and faster to decode; prefer it.

### 4-byte headers

Today's header is `TypeInfo*` (with 2 tag bits), and the generational GC adds an 8-byte `ObjectData` word before every
object.

- Replace the `TypeInfo*` with a 32-bit type id that indexes a global `TypeInfo*` table. Meta objects move to a side
  table keyed by object address, which is rare: weak references, associated objects, ObjC. The tag bits (stack,
  permanent) move to the type id's two low bits.
- Mark, old and remembered bits go to side bitmaps per Immix block (list item 7). Item 7 is a prerequisite: compact
  headers only pay off once the GC word is gone.
- A small object then costs 4 bytes of header (12-byte aligned payloads pack 3 `Int` fields in 16 bytes). Array
  headers become `type id + count` = 8 bytes.
- `loadTypeInfo` becomes `table[load32(obj) >> 2]`: one extra dependent load, but the table is hot and invariant,
  so LICM hoists it with the rest of the dispatch chain (see the invariant type info work).

### Compiler changes

- `LlvmTypes`: object field and array element type `i32` for references. Field and element GEPs go through
  `encodeRef`/`decodeRef` helpers in `CodeGenerator`, and so do the write barriers (`UpdateHeapRef*`) and the bulk copy
  intrinsics.
- `ClassLayoutBuilder`: reference fields are 4 bytes with 4-byte alignment. The existing sort by alignment then packs
  them with `Int` fields.
- `KotlinStaticData`: static object images use encoded references (needs the base at link time, see permanent objects
  above).
- Escape analysis and stack allocation: unchanged, apart from the rule above.
- Atomics on reference fields (`AtomicReference`, `@Volatile`): 32-bit `cmpxchg`, which works on every target.

### Runtime changes

- Every place that reads or writes a reference slot in the heap goes through `RefSlot` accessors: the mark loops, the
  remembered-slot recording, the sweep's object traversal (`traverseReferredObjects`), `ArrayHeader` element access,
  the C++ `ObjHeader` field accessors, and the bulk copy and fill of object arrays.
- Interop: `StableRef`, `CPointer` to object arrays (`refTo` on `Array<T>` is not allowed today, so no change),
  and ObjC export block/associated objects all pass full pointers, decoded at the boundary.
- The conservative stack scanner checks candidates against the reservation range first, which is cheaper than today's
  page registry lookup.

## Compatibility

- klibs carry IR, not layouts, so they are unaffected. The option must be uniform per binary: caches are keyed by it
  like other binary options (`NativeSecondStageCompilationConfig` cache suffix).
- C code that reads Kotlin object internals is unsupported. The runtime's own C++ is the only such code, and it goes
  through the accessors.
- Heaps larger than the reservation fail allocation with an `OutOfMemoryError` naming the option.

## Plan

1. Item 7 first: side mark bitmaps and array mark stacks, removing `ObjectData`.
2. Reservation-based heap behind `-Xbinary=heapReserve=`, still with 8-byte references. Measure that nothing
   regresses.
3. Compressed fields and array elements behind `-Xbinary=compressedRefs=true`: compiler, runtime accessors, static
   data.
4. Type-id headers.
5. Validate on the runtime tests (all configurations), box tests with the option on, and GC stress with threads and
   coroutines. Then measure the rtbench, the game benchmark and Calista. Make it the default only when it is faster
   and every test passes.

## Risks

- Every runtime reference access must be audited. A missed raw access corrupts memory silently. The `RefSlot`
  accessor type should make raw `ObjHeader**` heap slots fail to compile.
- Decoding costs an `add` on every reference load. This is usually hidden in the address mode, but it shows up in
  code that compares or hashes references (identity maps).
- 32 GiB reservations can fail under restrictive address space limits (32-bit processes, some containers). Fall back
  to a smaller reservation with a warning.

## Compact headers (implemented)

With zero-based compressed references, type infos and meta objects lie below 4 GiB, so the high half of an object's
header word is always zero. `-Xbinary=compactHeaders` (the default with zero-based compressed references) gives it to
the object's first 4-byte field instead of a type id table:

- The compiler lays out object bodies as `{i32 header, fields...}` (8 bytes at least, as the runtime reads the header
  word), and puts a 4-byte field first when the class inherits no fields. Fields sort by their stored size (4 bytes for
  compressed references). Arrays keep their 16-byte header.
- Type info loads mask the high half (`0xFFFFFFE0`). Permanent objects' headers are 32-bit relocations (the image is
  linked below 4 GiB), box caches use the compact box layout (8-byte boxes for values of 4 bytes or less).
- The runtime's C++ structures are unchanged: `ObjHeader::untagged` masks the high half, and installing or removing a
  meta object keeps it (`ObjHeader::nonAddressBits`). The GC's header bit operations already keep the other bits.

An object with three references takes 16 bytes (24 with compressed references alone, 32 originally). The Compose huge
tree peaks at 82-95 MiB (upstream: 101 MiB), with the first composition in 115 ms (upstream: 386 ms); CPU-bound
benchmarks are as fast as or faster than with full pointers (virtual calls 12% faster).
