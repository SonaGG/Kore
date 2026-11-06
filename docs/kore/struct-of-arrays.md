# Kore: struct-of-arrays for arrays of value classes

Status: design only. This builds on value flattening (`optimizations/ValueFlattening.kt`), which stores the fields of
small immutable classes inline in the objects holding them. This item does the same for arrays.

## Why

`Array<Vec3>` (or a `List<Vec3>` backed by one) holds references to separately allocated objects. A loop over
`a[i].x` loads the reference, then the field: two dependent loads, an 8-byte reference plus a 24-byte object (and its
header and GC word) per element, scattered over the heap, and no vectorization.

A struct-of-arrays representation keeps one primitive array per field (`xs: DoubleArray`, `ys`, `zs`). The loop then
reads `xs[i]` directly: one load, dense memory, and loops that LLVM can vectorize. The JVM cannot do this today (even
Valhalla flattens into arrays of structs, not structs of arrays).

## Which arrays

Flattening rejects a value class V as soon as its values flow into a location of another static type, and
`Array<V>.set(i, v)` passes `v` as the type parameter `T`. The SoA pass therefore adds an array-specific rule to the
same flow checker:

- **SoA candidate.** An array A of static type `Array<V>`, created by `arrayOfNulls`, `Array(n) { ... }` or
  `arrayOf(...)` with a non-null V. Every use of A must be one of:
  - `A[i]` (get) and `A[i] = v` (set) with `v: V`;
  - `A.size`, and `A.indices`/`A.lastIndex` (which inline to size);
  - for-in loops over A, which `KonanForLoopsLowering` lowers to index loops;
  - stores into and loads from local variables or fields whose type is exactly `Array<V>` and that are themselves
    candidates (the analysis is a fixpoint over these locations, like the holder fields of flattening).
- Any other use rejects A and every location it flows into: passing it to a function (other than the inline ones
  above, after inlining), `copyOf`, `asList`, `sort`, identity comparisons, casts, `Array<Any?>` parameters, vararg
  spreads, and anything unknown.
- V itself must satisfy flattening's value class conditions, except for the flows into these candidate arrays.
- Nullable elements (`Array<V?>`) and arrays whose elements may be read before being set (e.g. `arrayOfNulls<V>(n)`
  read before every index is written) are rejected in a first version. Supporting them later needs a presence
  bitmap: one bit per element, set by stores, checked by loads (throwing the `NullPointerException` a `!!` would have
  thrown, or producing `null`).

## Rewriting

For a candidate location L of type `Array<V>` with fields `f1..fk` (primitive types `T1..Tk`), replace L by k
locations `L$f1: T1Array .. L$fk: TkArray`. A field `Holder.points: Array<Vec3>` becomes the fields `points$x`,
`points$y`, `points$z`. A local `val a = Array(n) { ... }` becomes three locals.

- `Array<V>(n) { init }` → allocate k primitive arrays of size n, then run the init loop and store each component of
  the produced value (the constructor call is decomposed into its arguments, as in flattening's
  `constructorArguments`; otherwise a temporary is read field by field, which escape analysis removes).
- `A[i]` → `V(L$f1[i], .., L$fk[i])`, with the constructor inlined by the pre-codegen inliner. Escape analysis then
  removes the object where only its fields are read, so `a[i].x` becomes `L$x[i]`.
- `A[i] = v` → `L$f1[i] = v.f1; ..` (or the constructor arguments directly when `v` is a constructor call).
- `A.size` → `L$f1.size`. All component arrays always have the same size.
- Bounds checks: each component access does its own check. All are equal, and LLVM folds them after the first, so
  `IndexOutOfBoundsException` is still thrown where it was before.

## Semantics

- **Identity.** Elements are created on every read, as with flattening. Allowed only because identity is not
  observable for V, which the flattening conditions guarantee.
- **Atomicity.** A racing reader can see an element half-written (some components old, some new). Flattening already
  accepts this for fields. For arrays it is the same trade-off, and the doc of the option must state it.
- **Exceptions.** A store that throws halfway cannot happen: component stores are plain array stores after the bounds
  check. Allocation of the component arrays can throw `OutOfMemoryError` after some of them were allocated, which is
  unobservable (they are not stored anywhere yet).
- **Interop and reflection.** Candidates never reach code outside the analysis (external functions, `Any`
  parameters), so no outside code can see the representation.

## Composition with flattening

- Run SoA in the same pass as flattening and with the same flow checker, so a value class is accepted only if all of
  its flows are either flattened fields or SoA arrays.
- A holder field of type `Array<V>` is one of the "locations"; flattening of `V` fields and SoA of `Array<V>` fields
  can coexist in one class.
- `ArrayList<V>` is out of scope: its backing array is `Array<Any?>`. A later step could specialize `ArrayList` for V
  through `GenericSpecialization` with an SoA backing store, but that touches Agent B's area (`ArrayList` internals).

## Plan

1. Extend `ValueFlattening.FlowChecker` with an `ArrayOfValue` use kind and a fixpoint over `Array<V>` locations.
   Log candidates and rejection reasons with `KORE_VALUE_FLATTENING_LOG`.
2. Rewrite candidates as above. Gate it behind `-Xbinary=structOfArrays=true` (default off).
3. Tests:
   - semantic tests for gets, sets, size, loops and fixpoint flows through fields and locals;
   - rejection cases (passing the array to a function, `copyOf`, nullable elements);
   - the exceptions out-of-bounds accesses throw;
   - and a multi-threaded reader and writer to document tearing.
4. Measure on a particle-style microbenchmark (`Array<Vec3>` position and velocity updates) and on the game benchmark
   if it uses such arrays. Make it the default only if it is faster and passes all tests.
