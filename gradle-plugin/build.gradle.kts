import java.util.Properties

plugins {
    `java-gradle-plugin`
    `maven-publish`
}

// The plugin carries the version of Kore it installs by default, and the Kotlin version Kore is compatible with.
val koreProperties = Properties().apply { rootDir.resolve("../gradle.properties").reader().use { load(it) } }
val koreVersion = providers.gradleProperty("kore.version").orElse(koreProperties.getProperty("kore.version")).get()
val kotlinVersion = koreProperties.getProperty("kore.upstream.kotlin.version")
// Where the plugin downloads distributions from by default, and where `publish` sends it (`kore.maven.url` overrides the latter).
val defaultMavenUrl = "https://maven.cloverclient.com/releases"
val publishMavenUrl = providers.gradleProperty("kore.maven.url").orElse(defaultMavenUrl).get()

group = "gg.sona.kore"
version = koreVersion

tasks.withType<JavaCompile>().configureEach {
    options.release.set(17)
    options.compilerArgs.addAll(listOf("-Xlint:all", "-Werror"))
}

val generateKoreProperties by tasks.registering(WriteProperties::class) {
    destinationFile.set(layout.buildDirectory.file("generated/kore/resources/gg/sona/kore/gradle/kore.properties"))
    property("version", koreVersion)
    property("kotlinVersion", kotlinVersion)
    property("mavenUrl", defaultMavenUrl)
}

sourceSets.main {
    resources.srcDir(files(layout.buildDirectory.dir("generated/kore/resources")).builtBy(generateKoreProperties))
}

gradlePlugin {
    plugins {
        register("kore") {
            id = "gg.sona.kore"
            implementationClass = "gg.sona.kore.gradle.KorePlugin"
            displayName = "Kore"
            description = "Installs a Kore distribution and makes Kotlin/Native builds use it."
        }
    }
}

publishing {
    publications.withType<MavenPublication>().configureEach {
        pom {
            url.set("https://github.com/SonaGG/Kore")
            licenses {
                license {
                    name.set("The Apache License, Version 2.0")
                    url.set("https://www.apache.org/licenses/LICENSE-2.0.txt")
                }
            }
            organization {
                name.set("Sona Interactive, Inc.")
            }
        }
    }
    repositories {
        maven {
            name = "koreMaven"
            url = uri(publishMavenUrl)
            if (url.scheme == "https" || url.scheme == "http") credentials(PasswordCredentials::class)
        }
    }
}
