description = "Kotlin Library (KLIB) metadata manipulation library"

plugins {
    id("common-configuration")
    id("test-federation-convention")
    id("com.autonomousapps.dependency-analysis")
    kotlin("jvm")
}

group = "org.jetbrains.kotlinx"

val deployVersion = findProperty("kotlinxMetadataKlibDeployVersion") as String?
version = deployVersion ?: "0.0.1-SNAPSHOT"

sourceSets {
    "main" { projectDefault() }
    "test" { projectDefault() }
}

optInToK1Deprecation()

val embedded by configurations
embedded.isTransitive = false
configurations.getByName("compileOnly").extendsFrom(embedded)
configurations.getByName("testApi").extendsFrom(embedded)

dependencies {
    api(kotlinStdlib())
    embedded(project(":kotlin-metadata"))
    embedded(project(":core:compiler.common"))
    embedded(project(":core:names"))
    embedded(project(":core:deserialization"))
    embedded(project(":core:deserialization.common"))
    embedded(project(":compiler:serialization.common"))
    embedded(project(":compiler:serialization"))
    embedded(project(":kotlin-util-klib-metadata"))
    embedded(project(":kotlin-util-klib"))
    embedded(project(":kotlin-util-io"))
    embedded(protobufLite())
    testImplementation(kotlinTest("junit"))
    testImplementation(libs.junit4)
}

if (deployVersion != null) {
    publish()
}

runtimeJarWithRelocation {
    from(mainSourceSet.output)
    exclude("**/*.proto")
    // Kore: upstream relocates `org.jetbrains.kotlin` -> `kotlin.metadata.internal` here, embedding private copies of
    // core/compiler modules. In Kore those modules are stubs backed by kotlin-compiler.jar (nothing to embed), and this
    // jar is only consumed inside the Kotlin/Native compiler, where the original classes are on the classpath anyway.
    // So the relocation is disabled; the library binds to the compiler's own classes.
}

sourcesJar()

javadocJar()

kotlin {
    compilerOptions {
        freeCompilerArgs.add("-Xallow-kotlin-package")
    }
}
