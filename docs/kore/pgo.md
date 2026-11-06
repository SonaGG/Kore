# Kore: profile-guided optimization (PGO)

Kore can optimize a binary with a profile of its own execution, like a JIT compiler does with the profiles it
collects at run time: with branch frequencies, call counts and the targets of virtual calls, LLVM inlines hot calls
(and leaves cold ones alone), promotes hot virtual and interface calls to direct (inlinable) calls, lays out hot code
together and moves cold code out of the way.

It is LLVM's instrumentation-based PGO, applied to the whole program (the module that the LTO pipelines optimize).

## Workflow

1. **Build an instrumented binary** with the binary option `pgoGenerate=true` (optimized builds only).
2. **Run it on a representative workload.** When the program exits, it writes a raw profile, by default
   `default_<id>.profraw` in the working directory. The `LLVM_PROFILE_FILE` environment variable overrides the path,
   e.g. `LLVM_PROFILE_FILE=game-%p.profraw` (`%p`: process id, `%m`: binary signature, which makes several runs
   merge into the same file). Instrumentation makes the program much slower (up to an order of magnitude when
   several threads run the same code, as they share the counters): train with a fixed amount of work (e.g. a number
   of frames), not with time limits.
3. **Merge the raw profiles** into an indexed profile with `llvm-profdata` (in the `bin` directory of the LLVM
   distribution under `~/.konan/dependencies`, version 21):

   ```
   llvm-profdata merge -o game.profdata default_*.profraw
   ```

4. **Rebuild with the profile:** binary option `pgoUse=<path to game.profdata>`. Use the same sources, compiler and
   compiler options (other than `pgoGenerate`) as for the instrumented build: functions that changed since get no
   profile, and LLVM warns about them (`function control flow change detected (hash mismatch)`). A stale profile
   is still safe to use, just less effective. Commit the `.profdata` file and refresh it now and then.

## Usage

Command line:

```
kotlinc-native main.kt -opt -Xbinary=pgoGenerate=true -o game-instrumented
kotlinc-native main.kt -opt -Xbinary=pgoUse=/path/to/game.profdata -o game
```

Gradle (Kotlin Gradle Plugin), for all binaries of a project:

```
./gradlew linkReleaseExecutableMingwX64 -Pkotlin.native.binary.pgoGenerate=true
./gradlew linkReleaseExecutableMingwX64 -Pkotlin.native.binary.pgoUse=/path/to/game.profdata
```

or per binary in the build script:

```kotlin
kotlin {
    mingwX64 {
        binaries.executable {
            binaryOption("pgoUse", project.file("pgo/game.profdata").absolutePath)
        }
    }
}
```

Use absolute paths: the compiler may run in a Gradle daemon with a different working directory.

## Notes

- The options apply to optimized (`-opt`, release) final binaries: executables and shared libraries. Debug builds
  ignore them. An instrumented static library needs the program it is linked into to link LLVM's profile runtime.
- With either option, the runtime is not precompiled (binary option `precompileRuntime`) but optimized together
  with the program, so the profile covers its hot paths too (allocation, safepoints, ...). This makes compilation
  slower when the precompiled runtime would have been cached.
- Instrumented builds are compiled to machine code without splitting into parallel partitions
  (`codegenPartitions`).
- Instrumented binaries link LLVM's profile runtime (`clang_rt.profile`). Supported targets: `mingw_x64`. The LLVM
  distribution Kore compiles with does not include compiler-rt; the compiler takes the profile runtime from the LLVM
  development distribution of the same version if it is in `~/.konan/dependencies`. If not, compile the instrumented
  build once with `-Xoverride-konan-properties=llvmHome.mingw_x64=<LLVM development distribution>` (the
  `llvm.mingw_x64.dev` entry of `konan.properties`), which downloads it. The Windows build of the profile runtime
  targets MSVC; the compiler links a small shim for the few C runtime functions MinGW does not have.
- The profile runtime is not thread-safe in a strict sense: counters are updated non-atomically, so counts from
  concurrently running threads can be slightly lower than they should be. This does not matter for optimization.

## Results

Trained on one run of the workload itself, min of interleaved runs, mingw_x64 (ms):

| Benchmark                          | Without PGO | With PGO |
|------------------------------------|-------------|----------|
| Game benchmark, steady frame mean  | 5.72        | 5.34     |
| Game benchmark, tick phase         | 1.68        | 1.37     |
| Game benchmark, mesh phase         | 1.79        | 1.70     |
| Microbenchmarks: hashMap           | 57.2        | 48.9     |
| Microbenchmarks: lambdas           | 70.4        | 61.7     |
| Microbenchmarks: strings           | 14.6        | 11.8     |
| Microbenchmarks: sortObjs          | 91.5        | 45.4     |
| Microbenchmarks: boxedList         | 12.2        | 13.7     |

The other microbenchmarks (virtual calls, matmul, sortInts) do not change.
