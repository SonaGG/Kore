import java.util.*

pluginManagement {
    includeBuild("repo/kotlin-build-helpers")
    includeBuild("repo/gradle-settings-conventions")

    repositories {
        maven {
            url = uri("https://packages.jetbrains.team/maven/p/ij/intellij-dependencies")
            content {
                includeGroupByRegex("org\\.jetbrains\\.intellij\\.deps(\\..+)?")
                includeGroupByRegex("com.intellij.platform.*")
                includeGroupByRegex("org.jetbrains.jps.*")
            }
        }
        maven {
            url = uri("https://redirector.kotlinlang.org/maven/kotlin-dependencies")
            content {
                includeGroup("org.jetbrains.kotlin")
                includeGroup("org.spdx")
                includeGroup("org.jetbrains.kotlinx")
                includeGroup("org.jetbrains.kotlinx.benchmark")
            }
        }
        google {
            url = uri("https://cache-redirector.jetbrains.com/dl.google.com/dl/android/maven2")
            content {
                includeGroup("com.android.tools")
            }
        }

        mavenCentral {
            url = uri("https://cache-redirector.jetbrains.com/maven-central")
        }
        gradlePluginPortal()
    }

    plugins {
        id("de.undercouch.download") version "5.1.0"
        id("com.autonomousapps.dependency-analysis") version "3.6.1"
    }
}

plugins {
    id("internal-gradle-setup") // it's recommended to apply this plugin at first, as it changes the local.properties file
    id("kotlin-build-helpers")
    id("kotlin-bootstrap")
    id("develocity")
    id("jvm-toolchain-provisioning")
    id("kotlin-daemon-config")
    id("cache-redirector")
}

/*
 * Kore: monorepo projects that the native build references but that are NOT part of Kore.
 * Each is registered as an empty "stub" project with its upstream path, and the root build script wires it to the
 * matching published artifact at the pinned upstream version (`kore.upstream.kotlin.version` in gradle.properties).
 * This keeps upstream build scripts unmodified, which makes merging later upstream releases straightforward.
 */
val koreStubProjects: Set<String> = file("upstream/stub-projects.txt").readLines()
    .map { it.substringBefore("#").trim().substringBefore(" ") }
    .filter { it.isNotEmpty() }
    .toSet()

run {
    // Local copy: the isolated lifecycle action must not capture the settings script object.
    val stubPaths = koreStubProjects.toSet()
    gradle.lifecycle.afterProject {
        // Kore: stub projects (see below) intentionally don't apply common-configuration.
        if (path !in stubPaths && plugins.hasPlugin("java-base") && !plugins.hasPlugin("common-configuration")) {
            throw GradleException("common-configuration plugin should be applied to all projects")
        }
    }
}

dependencyResolutionManagement {
    val buildProperties = getKotlinBuildPropertiesForSettings(settings)

    components {
        withModule("com.google.code.gson:gson") {
            allVariants {
                withDependencies {
                    add("com.google.code.gson:gson") {
                        version {
                            require(buildProperties.versionsProperty("gson").get())
                        }
                    }
                }
            }
        }

        withModule("org.apache.commons:commons-compress") {
            allVariants {
                withDependencies {
                    add("org.apache.commons:commons-compress") {
                        version {
                            require(buildProperties.versionsProperty("commons-compress").get())
                        }
                    }
                }
            }
        }

        withModule("commons-io:commons-io") {
            allVariants {
                withDependencies {
                    add("commons-io:commons-io") {
                        version {
                            require(buildProperties.versionsProperty("commons-io").get())
                        }
                    }
                }
            }
        }
    }

    repositories {
        intellijRepository(buildProperties.versionsProperty("intellijSdk").get())
        intellijDependencies()
        kotlinDependencies()
        teamcityRepository()
        googleAndroidRepository()
        gradleLibsReleases()
        gradlePluginPortalRepository()
        litmuskt()
        kotlinIdePluginDependencies()
        mozillaReleases()
        kotlinFileDependenciesJsc()
        githubRelease("WasmEdge", "WasmEdge", revisionPrefix = "", groupAlias = "org.wasmedge")
        githubRelease("bytecodealliance", "wasmtime", groupAlias = "dev.wasmtime")
        githubCommit("webassembly", "testsuite")
        githubRelease("webassembly", "wabt", revisionPrefix = "")
        githubTag("google", "breakpad")
        githubCommit("google", "googletest")
        nodeJs()
        yarnDistributions()
        binaryenDistributions()
        d8Distributions()
        androidRepository()
        androidSystemImages()
        val mirrorRepo: String? = settings.providers.systemProperty("maven.repository.mirror").orNull
        mirrorRepo?.let(::maven)
        mavenCentral()
    }
    repositoriesMode = RepositoriesMode.FAIL_ON_PROJECT_REPOS
}

val buildProperties = getKotlinBuildPropertiesForSettings(settings)

includeBuild("repo/gradle-build-conventions")


// ---------------------------------------------------------------------------------------------------------------------
// Kore: real (source) projects. Paths and project directories are identical to upstream JetBrains/kotlin.
// ---------------------------------------------------------------------------------------------------------------------

include(
    // Native-specific compiler modules (upstream also bundles copies of these in kotlin-compiler.jar; Kore's win).
    ":compiler:ir.backend.native",
    ":compiler:ir.serialization.native",
    ":compiler:ir.objcinterop",
    ":compiler:fir:fir-native",
    ":compiler:fir:checkers:checkers.native",
    ":compiler:cli:cli-native-klib",
    ":core:compiler.common.native",

    // native/
    ":native:base",
    ":native:binary-options",
    ":native:cinterop.deserialization",
    ":native:cli-native",
    ":native:executors",
    ":native:frontend.native",
    ":native:kotlin-klib-commonizer",
    ":native:kotlin-klib-commonizer-api",
    ":native:kotlin-klib-commonizer-embeddable",
    ":native:kotlin-native-utils",
    ":native:kotlin-test-native-xctest",
    ":native:native.config",
    ":native:objcexport-header-generator",
    ":native:objcexport-header-generator-k1",
    ":native:unsafe-mem",

    // Klib metadata tooling not published to Maven Central.
    ":kotlin-metadata",
    ":kotlinx-metadata-klib",
    ":tools:kotlinp",
    ":tools:kotlinp-klib",

    ":prepare:build.version",
)

project(":compiler:ir.backend.native").projectDir = file("compiler/ir/backend.native")
project(":compiler:ir.serialization.native").projectDir = file("compiler/ir/serialization.native")
project(":compiler:ir.objcinterop").projectDir = file("compiler/ir/ir.objcinterop")
project(":native:kotlin-native-utils").projectDir = file("native/utils")
project(":native:frontend.native").projectDir = file("native/frontend")
project(":native:kotlin-klib-commonizer").projectDir = file("native/commonizer")
project(":native:kotlin-klib-commonizer-api").projectDir = file("native/commonizer-api")
project(":native:kotlin-klib-commonizer-embeddable").projectDir = file("native/commonizer-embeddable")
project(":native:objcexport-header-generator-k1").projectDir = file("native/objcexport-header-generator/impl/k1")
project(":kotlin-metadata").projectDir = file("libraries/kotlinx-metadata")
project(":kotlinx-metadata-klib").projectDir = file("libraries/kotlinx-metadata/klib")
project(":tools:kotlinp").projectDir = file("libraries/tools/kotlinp")
project(":tools:kotlinp-klib").projectDir = file("libraries/tools/kotlinp/klib")

includeBuild("kotlin-native/build-tools") {
    name = "native-build-tools"
}

include(":kotlin-native:dependencies")
include(":kotlin-native:endorsedLibraries:kotlinx.cli")
include(":kotlin-native:Interop:StubGenerator")
include(":kotlin-native:Interop:StubGeneratorConsistencyCheck")
include(":kotlin-native:backend.native")
project(":kotlin-native:backend.native").projectDir = file("kotlin-native/backend.native/compiler/ir/backend.native")
include(":kotlin-native:Interop:Runtime")
include(":kotlin-native:Interop:Indexer")
include(":kotlin-native:utilities:cli-runner")
include(":kotlin-native:klib")
include(":kotlin-native:common")
include(":kotlin-native:common:env")
include(":kotlin-native:common:files")
include(":kotlin-native:runtime")
include(":kotlin-native:libllvmext")
include(":kotlin-native:llvmDebugInfoC")
include(":kotlin-native:utilities")
include(":kotlin-native:tools:kdumputil")
include(":kotlin-native:platformLibs")
include(":kotlin-native:libclangext")
include(":kotlin-native:llvmInterop")
include(":kotlin-native:libclangInterop")
include(":kotlin-native:prepare:kotlin-native-compiler-embeddable")
include(":kotlin-native:tools:compiler-cache-invalidator")
include(":kotlin-native:tools:minidump-analyzer")

// ---------------------------------------------------------------------------------------------------------------------
// Kore: stub projects (see `koreStubProjects` above). They have no build file of their own; they are configured from
// the root build script. They keep their upstream project directory (e.g. `:kotlin-stdlib` -> libraries/stdlib), so
// `project(":kotlin-stdlib").file(...)` source lookups in upstream scripts keep working and real child projects
// (e.g. `:compiler:cli:cli-native-klib` under stub `:compiler:cli`) keep their default directories.
// ---------------------------------------------------------------------------------------------------------------------

val koreStubProjectDirs = mapOf(
    ":kotlin-stdlib" to "libraries/stdlib",
    ":kotlin-test" to "libraries/kotlin.test",
)

koreStubProjects.forEach { path ->
    include(path)
    project(path).apply {
        koreStubProjectDirs[path]?.let { projectDir = file(it) }
        buildFileName = "kore-stub-has-no-build-file.gradle.kts"
    }
}

// Gradle requires every project directory to exist. Stubs (and implicitly created parent projects such as
// `:compiler:tests-mutes`) whose upstream directory is not part of Kore get an empty placeholder directory instead.
// Children keep the directory they were assigned on `include`, so relocating a parent does not move real projects.
fun relocateMissingProjectDirs(descriptor: ProjectDescriptor) {
    if (!descriptor.projectDir.isDirectory) {
        val placeholder = file("build/kore-stubs/${descriptor.path.removePrefix(":").replace(':', '/')}")
        placeholder.mkdirs()
        descriptor.projectDir = placeholder
    }
    descriptor.children.forEach(::relocateMissingProjectDirs)
}
rootProject.children.forEach(::relocateMissingProjectDirs)
