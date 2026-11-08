# Kore

Kore is a fork of the Kotlin/Native compiler and runtime. Its goal is runtime performance and memory use competitive
with C/C++, while remaining a drop-in replacement for Kotlin/Native. Kore is a project by Sona Interactive, Inc.

Kore is based on [JetBrains/kotlin](https://github.com/JetBrains/kotlin) tag `v2.4.21` and is licensed under
Apache-2.0. See [NOTICE.txt](NOTICE.txt). Kore is not affiliated with JetBrains or the Kotlin Foundation.

## Using Kore from a Gradle project

Apply the `gg.sona.kore` settings plugin in a Kotlin Multiplatform project that uses KGP **2.4.21**
(`settings.gradle.kts`):

```kotlin
pluginManagement {
    repositories {
        maven("https://maven.cloverclient.com/releases") { content { includeGroupAndSubgroups("gg.sona.kore") } }
        gradlePluginPortal()
    }
}

plugins {
    id("gg.sona.kore") version "0.1.0"
}
```

On the first build, the plugin downloads the Kore distribution for the host (`gg.sona.kore:kore-native-prebuilt`),
checks it and unpacks it under `~/.konan/kore/<version>`. Then it sets `kotlin.native.home` on every project, so KGP logs
`A user-provided Kotlin/Native distribution configured: …` and compiles with Kore. Later builds, offline ones included,
reuse the installation. The plugin's version is the Kore version it installs. Published third-party klibs, such as
`kotlinx-coroutines-core`, resolve and link as usual.

Gradle properties of the plugin:

| Property                                     | Effect                                                                    |
|----------------------------------------------|---------------------------------------------------------------------------|
| `kore.version`                               | Kore version to install, instead of the plugin's own.                     |
| `kore.enabled=false`                         | Leaves the build on upstream Kotlin/Native.                               |
| `kotlin.native.home`                         | Uses that distribution (e.g. a local Kore build) and installs nothing.    |
| `kore.distribution=regular`                  | The distribution without prebuilt platform libraries.                     |
| `kore.install.dir`                           | Where distributions are installed (default `$KONAN_DATA_DIR` or `~/.konan`). |
| `kore.maven.url`                             | The Maven repository holding the distributions.                           |
| `koreMavenUsername`, `koreMavenPassword`     | Credentials, for a repository that needs them.                            |

`konan.data.dir` makes KGP ignore `kotlin.native.home`, so the plugin fails the build when it is set.

To use a local build of Kore instead (see below), set `kotlin.native.home=/path/to/Kore/kotlin-native/dist` in
`gradle.properties`, with or without the plugin.

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

## Releasing

Pushing a `v<version>` tag runs the Release workflow (`.github/workflows/release.yml`). It builds the bundles on
Windows, Linux and macOS, then publishes `gg.sona.kore:kore-native`, `gg.sona.kore:kore-native-prebuilt` (classifier
`<os>-<arch>`) and the `gg.sona.kore` Gradle plugin, all at that version, to the Kore Maven repository. The workflow
needs the repository secrets `KORE_MAVEN_USERNAME` and `KORE_MAVEN_PASSWORD`.

To publish this host's bundles and the plugin by hand, with `koreMavenUsername` and `koreMavenPassword` in
`~/.gradle/gradle.properties`:

```bash
./gradlew :kotlin-native:publishKoreToMavenRepository -Pkore.version=<version>
./gradlew -p gradle-plugin publish -Pkore.version=<version>
```
