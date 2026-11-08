// Kore: the `gg.sona.kore` settings plugin, which installs a Kore distribution and points Kotlin/Native builds at it.
// A standalone build: run it with `./gradlew -p gradle-plugin <task>` from the Kore root.

rootProject.name = "kore-gradle-plugin"

dependencyResolutionManagement {
    repositories {
        mavenCentral()
    }
}
