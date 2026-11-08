/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package gg.sona.kore.gradle;

import org.gradle.api.GradleException;
import org.gradle.api.Plugin;
import org.gradle.api.Project;
import org.gradle.api.initialization.Settings;
import org.gradle.api.logging.Logger;
import org.gradle.api.logging.Logging;
import org.gradle.api.provider.ProviderFactory;

import java.io.IOException;
import java.io.InputStream;
import java.io.UncheckedIOException;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Properties;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The `gg.sona.kore` settings plugin. It installs the Kore distribution for this host (once, under
 * `~/.konan/kore/<version>`) and sets `kotlin.native.home` on every project, so the Kotlin Gradle plugin compiles with
 * Kore instead of downloading the upstream Kotlin/Native compiler.
 *
 * <p>Gradle properties:
 * <ul>
 *   <li>`kore.version`: the Kore version to use (by default, the plugin's own version);</li>
 *   <li>`kore.enabled=false`: leaves the build on upstream Kotlin/Native;</li>
 *   <li>`kore.maven.url`: the Maven repository holding the distributions;</li>
 *   <li>`kore.distribution=regular`: the distribution without prebuilt platform libraries;</li>
 *   <li>`kore.install.dir`: where distributions are installed (by default `$KONAN_DATA_DIR` or `~/.konan`, under `kore/`);</li>
 *   <li>`koreMavenUsername`, `koreMavenPassword`: credentials, for a repository that needs them;</li>
 *   <li>`kotlin.native.home`: if set, it wins and nothing is installed.</li>
 * </ul>
 */
public final class KorePlugin implements Plugin<Settings> {
    static final String NATIVE_HOME = "kotlin.native.home";

    private static final Logger LOGGER = Logging.getLogger(KorePlugin.class);

    @Override
    public void apply(Settings settings) {
        ProviderFactory providers = settings.getProviders();
        if ("false".equalsIgnoreCase(providers.gradleProperty("kore.enabled").getOrElse("true"))) {
            LOGGER.info("Kore: disabled (kore.enabled=false)");
            return;
        }
        // The Kotlin Gradle plugin ignores `kotlin.native.home` when `konan.data.dir` is set: the build would silently use
        // upstream Kotlin/Native.
        if (providers.gradleProperty("konan.data.dir").isPresent()) {
            throw new GradleException("Kore: `konan.data.dir` is set, which makes the Kotlin Gradle plugin ignore Kore. Remove it "
                    + "(use `kore.install.dir` to choose where Kore is installed), or set `kore.enabled=false`.");
        }
        String userHome = providers.gradleProperty(NATIVE_HOME).getOrNull();
        if (userHome != null) {
            LOGGER.lifecycle("Kore: using {}={} instead of installing Kore", NATIVE_HOME, userHome);
            return;
        }

        Properties defaults = loadDefaults();
        String version = providers.gradleProperty("kore.version").getOrElse(defaults.getProperty("version"));
        String kotlinVersion = defaults.getProperty("kotlinVersion");
        boolean prebuilt = !"regular".equalsIgnoreCase(providers.gradleProperty("kore.distribution").getOrElse("prebuilt"));

        KoreInstaller installer = new KoreInstaller(
                providers.gradleProperty("kore.maven.url").getOrElse(defaults.getProperty("mavenUrl")),
                providers.gradleProperty("koreMavenUsername").getOrNull(),
                providers.gradleProperty("koreMavenPassword").getOrNull(),
                dataDirectory(providers),
                settings.getStartParameter().isOffline(),
                LOGGER);
        Path home = installer.install(version, prebuilt);
        String homePath = home.toString();
        LOGGER.info("Kore: {} {} at {}", prebuilt ? "kore-native-prebuilt" : "kore-native", version, homePath);

        AtomicBoolean warned = new AtomicBoolean();
        settings.getGradle().beforeProject(project -> {
            project.getExtensions().getExtraProperties().set(NATIVE_HOME, homePath);
            project.getPluginManager().withPlugin("org.jetbrains.kotlin.multiplatform",
                    plugin -> checkKotlinVersion(project, kotlinVersion, version, warned));
        });
    }

    private static Properties loadDefaults() {
        Properties properties = new Properties();
        try (InputStream input = KorePlugin.class.getResourceAsStream("kore.properties")) {
            if (input == null) throw new GradleException("Kore: the plugin is missing its kore.properties");
            properties.load(input);
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
        return properties;
    }

    // `kore.install.dir`, else beside upstream Kotlin/Native: `KONAN_DATA_DIR`, then `~/.konan`.
    private static Path dataDirectory(ProviderFactory providers) {
        String configured = providers.gradleProperty("kore.install.dir").getOrNull();
        if (configured != null) return Paths.get(configured);
        String konanData = providers.environmentVariable("KONAN_DATA_DIR").getOrNull();
        if (konanData != null) return Paths.get(konanData);
        return Paths.get(providers.systemProperty("user.home").get(), ".konan");
    }

    // Kore reports the Kotlin version it is based on; another Kotlin Gradle plugin version may not work with it.
    private static void checkKotlinVersion(Project project, String kotlinVersion, String koreVersion, AtomicBoolean warned) {
        String pluginVersion;
        try {
            Class<?> wrapper = Class.forName("org.jetbrains.kotlin.gradle.plugin.KotlinPluginWrapperKt", false,
                    project.getPlugins().getPlugin("org.jetbrains.kotlin.multiplatform").getClass().getClassLoader());
            pluginVersion = (String) wrapper.getMethod("getKotlinPluginVersion", Project.class).invoke(null, project);
        } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
            return;
        }
        if (!kotlinVersion.equals(pluginVersion) && warned.compareAndSet(false, true)) {
            LOGGER.warn("Kore {} is built for the Kotlin Gradle plugin {}, but this build uses {}", koreVersion, kotlinVersion,
                    pluginVersion);
        }
    }
}
