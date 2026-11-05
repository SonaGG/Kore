buildscript {
    // Same security overrides as upstream (CVE-2024-26308, CVE-2024-25710, CVE-2023-42503, CVE-2025-48924).
    configurations.all {
        resolutionStrategy.eachDependency {
            if (requested.group == "org.apache.commons" && requested.name == "commons-compress") {
                useVersion(libs.versions.commons.compress.get())
            }
            if (requested.group == "commons-io" && requested.name == "commons-io") {
                useVersion(libs.versions.commons.io.get())
            }
            if (requested.group == "org.apache.commons" && requested.name == "commons-lang3") {
                useVersion(libs.versions.commons.lang.get())
            }
        }
    }
}

plugins {
    base
    id("org.jetbrains.kotlin.jvm") apply false
    id("org.jetbrains.kotlin.plugin.serialization") apply false
    id("kotlin.native.build-tools-conventions") apply false
    `jvm-toolchains`
    id("com.autonomousapps.dependency-analysis")
    id("project-tests-convention") apply false
    id("test-federation-convention") apply false
    // Puts buildsrc-compat helpers on the script classpath; applied below once `extra` is populated.
    id("common-configuration") apply false
}

findProperty("deployVersion")?.let {
    assert(findProperty("build.number") != null) { "`build.number` parameter is expected to be explicitly set with the `deployVersion`" }
}

val kotlinLanguageVersion: String by extra

extra["kotlin_root"] = rootDir

val distDir by extra("$rootDir/dist")
val distKotlinHomeDir by extra("$distDir/kotlinc")
val distLibDir = "$distKotlinHomeDir/lib"
val commonLocalDataDir = "$rootDir/local"

extra["distLibDir"] = project.file(distLibDir)
extra["commonLocalDataDir"] = project.file(commonLocalDataDir)
extra["ideaSandboxDir"] = project.file("$commonLocalDataDir/ideaSandbox")
extra["ideaPluginDir"] = project.file("$distDir/artifacts/ideaPlugin/Kotlin")

rootProject.apply {
    from(rootProject.file("gradle/versions.gradle.kts"))
}

/** The upstream JetBrains/kotlin release that Kore is based on. All stub projects resolve to artifacts of this version. */
val koreUpstreamKotlinVersion: String = providers.gradleProperty("kore.upstream.kotlin.version").get()

if (!project.hasProperty("versions.kotlin-native")) {
    extra["versions.kotlin-native"] = koreUpstreamKotlinVersion
}

val coreLibProjects by extra { listOf(":kotlin-stdlib", ":kotlin-stdlib-jdk8", ":kotlin-test") }
val mppProjects by extra { listOf(":kotlin-stdlib", ":kotlin-test") }
val projectsWithOptInToUnsafeCastFunctionsFromAddToStdLib by extra { listOf(":native:kotlin-klib-commonizer") }

// The root project applies common configuration explicitly here, after its `extra` is populated.
pluginManager.apply("common-configuration")

tasks.register("createIdeaHomeForTests") {
    val ideaBuildNumberFileForTests = ideaBuildNumberFileForTests()
    val intellijSdkVersion = kotlinBuildProperties.versionsProperty("intellijSdk").get()
    outputs.dir(ideaHomePathForTests())
    doFirst {
        with(ideaBuildNumberFileForTests.get().asFile) {
            parentFile.mkdirs()
            writeText("IC-$intellijSdkVersion")
        }
    }
}

tasks.named<Delete>("clean") {
    delete(distDir)
}

// ---------------------------------------------------------------------------------------------------------------------
// Stub projects: see upstream/stub-projects.txt and settings.gradle.kts.
// ---------------------------------------------------------------------------------------------------------------------

data class KoreStub(val path: String, val artifact: String?)

val koreStubs: List<KoreStub> = file("upstream/stub-projects.txt").readLines()
    .map { it.substringBefore("#").trim() }
    .filter { it.isNotEmpty() }
    .map { line ->
        val parts = line.split(Regex("\\s+"))
        val artifact = parts.getOrNull(1) ?: "kotlin-compiler"
        KoreStub(parts[0], artifact.takeUnless { it == "none" })
    }

/** Named configurations that upstream scripts request explicitly from `:kotlin-test` (see `kotlinTest()` helper). */
val kotlinTestFlavorConfigurations = mapOf(
    "jvmRuntimeElements" to "kotlin-test",
    "jvmJUnitRuntimeElements" to "kotlin-test-junit",
    "jvmJUnit5RuntimeElements" to "kotlin-test-junit5",
    "jvmTestNGRuntimeElements" to "kotlin-test-testng",
)

koreStubs.forEach { stub ->
    project(stub.path) {
        val upstreamModule = stub.artifact?.let { "org.jetbrains.kotlin:$it:$koreUpstreamKotlinVersion" }

        if (stub.artifact == "kotlin-gradle-plugins-bom") {
            apply(plugin = "java-platform")
            extensions.configure<JavaPlatformExtension> { allowDependencies() }
            dependencies { "api"(platform(upstreamModule!!)) }
            return@project
        }

        apply(plugin = "java-library")
        apply(plugin = "java-test-fixtures")
        // Stubs contain no code; advertise the lowest JVM target so any consumer (some native modules target JVM 8)
        // can resolve them. The real bytecode level is whatever the published artifact has.
        extensions.configure<JavaPluginExtension> {
            sourceCompatibility = JavaVersion.VERSION_1_8
            targetCompatibility = JavaVersion.VERSION_1_8
        }
        if (upstreamModule != null) {
            dependencies { "api"(upstreamModule) }
        }

        // `projectTests(...)` helper asks for this configuration.
        configurations.consumable("tests-jar")

        if (stub.path == ":kotlin-test") {
            kotlinTestFlavorConfigurations.forEach { (configurationName, artifact) ->
                val scope = configurations.dependencyScope("${configurationName}Dependencies")
                dependencies { scope.name("org.jetbrains.kotlin:$artifact:$koreUpstreamKotlinVersion") }
                configurations.consumable(configurationName) { extendsFrom(scope.get()) }
            }
        }
    }
}
