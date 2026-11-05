# Kore

Kore is a fork of the Kotlin/Native compiler and runtime. Its goal is runtime performance and memory use competitive
with C/C++, while remaining a drop-in replacement for Kotlin/Native. Kore is a project by Sona Interactive, Inc.

Kore is based on [JetBrains/kotlin](https://github.com/JetBrains/kotlin) tag `v2.4.21` and is licensed under
Apache-2.0. See [NOTICE.txt](NOTICE.txt). Kore is not affiliated with JetBrains or the Kotlin Foundation.

## Using Kore from a Gradle project

Build the distribution (below), then point a Kotlin Multiplatform project that uses KGP **2.4.21** at it in
`gradle.properties`:

```properties
kotlin.native.home=/path/to/Kore/kotlin-native/dist
```

KGP logs `A user-provided Kotlin/Native distribution configured: …` and stops auto-provisioning the upstream compiler.
Published third-party klibs, such as `kotlinx-coroutines-core`, resolve and link as usual.

## Building

Requirements: Windows, Linux or macOS host; internet access on the first build. Gradle provisions JDK 21 itself, and
the LLVM/MinGW toolchains are downloaded to `~/.konan/dependencies`.

Build the compiler, runtime and stdlib for the host:

```bash
./gradlew :kotlin-native:dist
```

Build the host's platform libraries (on Windows: posix, windows, opengl32, zlib, …):

```bash
./gradlew :kotlin-native:distPlatformLibs
```

The output goes to `kotlin-native/dist`. Use `:kotlin-native:crossDist` to add cross-compilation targets (Linux and
Android from any host). Apple targets need a macOS host with Xcode.
