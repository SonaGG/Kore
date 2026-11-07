# Kore: a copying young generation in the Immix heap

Status: on by default where supported (generational GC, Immix, no GC word); `-Xbinary=immixNursery=false` turns it off. Runtime and compiler options only: klib format and ABI do
not change.

## Why

The default collector (sticky-mark Immix, `memory-v2.md`) never moves objects. Every allocation records its start in
its block's start bitmap (a read-modify-write per object), young objects fill holes between old ones, and every block
that received allocations is swept object by object. The JVM's young collections cost what survives: a thread bumps a
pointer through its own buffer, survivors are copied out, and the buffers are reused whole.

The older nursery (`gcNurserySize`, `Nursery.cpp`) is a separate reservation outside the compressed-reference heap: it
needs a GC word per object for forwarding, so it turns compressed references and compact headers off (objects double
in size). This one lives in the Immix heap instead, and forwards through the header as evacuation does
(`gc-redesign.md`, phase 5), so references stay 32-bit and headers 4 bytes.

## Design

- **Nursery blocks.** A mutator allocates small objects by bumping through whole free Immix blocks, flagged
  `ImmixBlock::Nursery()`. Objects are laid out back to back from `Begin()`; the block records where they end
  (`SetNurseryTop`) when its thread moves to another block and when a collection starts (`CustomAllocator::SealNursery`).
  Medium objects keep the overflow blocks (with start bits); the GC's evacuation allocator never uses nursery blocks.
- **No start bits on the fast path.** Nursery allocation does not set start bits. A young collection copies every
  survivor of a nursery block (`alloc::canEvacuate`), so the sweep of a block without start bits finds no object and
  frees it whole.
- **Objects that stay where they are get a start bit** (`alloc::nurseryKeepInPlace`), so that the sweep keeps them (and
  frees the dead ones around them):
  - roots marked during the root phase (`gNurseryRootPhase`): thread-local and global roots;
  - everything the stacks may point to: looking an address up in a nursery block (`Heap::ForEachHeapSlot`) first parses
    the block's objects and sets all their start bits (`EnsureNurseryStarts`; a copied object's size is its copy's);
  - pinned objects (identity hash codes, stable references, weak reference implementations), objects with extra object
    data, objects with finalizers, and objects whose copy could not be allocated (`markInPlace`).
- **Full collections** mark concurrently and do not move objects: when one starts, every nursery block gets its start
  bits, and mutators set start bits on the fast path until the next young collection (`gNurseryStartBits`).

## Measurements

See the commit messages and `bench-suites/micro` runs. Phase A (copying survivors out of whole fresh blocks, with start
bits still set): neutral on the micro suite. Skipping the start bits on the fast path (unsafe experiment): about 20%
faster on binary_trees.
