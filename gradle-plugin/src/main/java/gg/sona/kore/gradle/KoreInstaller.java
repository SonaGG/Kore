/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

package gg.sona.kore.gradle;

import org.gradle.api.GradleException;
import org.gradle.api.logging.Logger;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.UncheckedIOException;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Comparator;
import java.util.HexFormat;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import java.util.stream.Stream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Installs Kore distributions under `<konan data dir>/kore/<version>/<artifact>-<os-arch>/`: downloads the bundle from
 * the Maven repository, checks its SHA-256, unpacks it beside the final directory and moves it in place. A file lock
 * makes concurrent builds wait for one another instead of installing twice.
 */
final class KoreInstaller {
    private static final String GROUP_PATH = "gg/sona/kore";
    private static final String INSTALLED_MARKER = ".kore-installed";

    private final String repositoryUrl;
    private final String username;
    private final String password;
    private final Path dataDirectory;
    private final boolean offline;
    private final Logger logger;

    KoreInstaller(String repositoryUrl, String username, String password, Path dataDirectory, boolean offline, Logger logger) {
        this.repositoryUrl = repositoryUrl.endsWith("/") ? repositoryUrl : repositoryUrl + "/";
        this.username = username;
        this.password = password;
        this.dataDirectory = dataDirectory;
        this.offline = offline;
        this.logger = logger;
    }

    /** The distribution's root (the directory holding `bin/`, `klib/` and `konan/`), installing it if needed. */
    Path install(String version, boolean prebuilt) {
        String platform = hostPlatform();
        String artifact = prebuilt ? "kore-native-prebuilt" : "kore-native";
        Path versionDirectory = dataDirectory.resolve("kore").resolve(version);
        Path installation = versionDirectory.resolve(artifact + "-" + platform);
        try {
            Path installed = installedRoot(installation);
            if (installed != null) return installed;
            if (offline) {
                throw new GradleException("Kore " + version + " is not installed in " + installation
                        + ", and Gradle runs offline. Run the build online once to install it.");
            }
            Files.createDirectories(versionDirectory);
            Path lockFile = versionDirectory.resolve(artifact + "-" + platform + ".lock");
            try (FileChannel channel = FileChannel.open(lockFile, StandardOpenOption.CREATE, StandardOpenOption.WRITE)) {
                FileLock lock = channel.lock();
                try {
                    installed = installedRoot(installation);
                    if (installed != null) return installed;
                    return download(version, platform, artifact, versionDirectory, installation);
                } finally {
                    lock.release();
                }
            }
        } catch (IOException e) {
            throw new UncheckedIOException("Kore: could not install " + artifact + " " + version + " in " + installation, e);
        }
    }

    private Path download(String version, String platform, String artifact, Path versionDirectory, Path installation)
            throws IOException {
        String extension = platform.startsWith("windows-") ? "zip" : "tar.gz";
        String fileName = artifact + "-" + version + "-" + platform + "." + extension;
        String url = repositoryUrl + GROUP_PATH + "/" + artifact + "/" + version + "/" + fileName;
        Path archive = versionDirectory.resolve(fileName + ".part");
        Path unpacked = versionDirectory.resolve(artifact + "-" + platform + ".unpacking");
        deleteRecursively(unpacked);
        try {
            logger.lifecycle("Kore: downloading {}", url);
            fetch(url, archive);
            verifyChecksum(url, archive);
            Files.createDirectories(unpacked);
            if (extension.equals("zip")) unzip(archive, unpacked);
            else untar(archive, unpacked);
            Path root = singleChildDirectory(unpacked);
            deleteRecursively(installation);
            Files.move(unpacked, installation, StandardCopyOption.ATOMIC_MOVE);
            String rootName = root.getFileName().toString();
            Files.writeString(installation.resolve(INSTALLED_MARKER), rootName, StandardCharsets.UTF_8);
            logger.lifecycle("Kore: installed {} {} in {}", artifact, version, installation);
            return installation.resolve(rootName);
        } finally {
            Files.deleteIfExists(archive);
            deleteRecursively(unpacked);
        }
    }

    private static Path installedRoot(Path installation) throws IOException {
        Path marker = installation.resolve(INSTALLED_MARKER);
        if (!Files.isRegularFile(marker)) return null;
        Path root = installation.resolve(Files.readString(marker, StandardCharsets.UTF_8).trim());
        return Files.isDirectory(root.resolve("konan")) ? root : null;
    }

    private void fetch(String url, Path target) throws IOException {
        URI uri = URI.create(url);
        if ("file".equals(uri.getScheme())) {
            try {
                Files.copy(Paths.get(uri), target, StandardCopyOption.REPLACE_EXISTING);
            } catch (NoSuchFileException e) {
                throw notFound(url);
            }
            return;
        }
        HttpResponse<Path> response = send(uri, HttpResponse.BodyHandlers.ofFile(target));
        if (response.statusCode() == 404) throw notFound(url);
        if (response.statusCode() != 200) throw new IOException("HTTP " + response.statusCode() + " for " + url);
    }

    private static GradleException notFound(String url) {
        return new GradleException("Kore: no distribution at " + url + ". Check `kore.version` (is that version published "
                + "for this host?) and `kore.maven.url`.");
    }

    // Gradle publishes `.sha1` files beside each artifact (and `.sha256` ones to some repositories); the strongest one
    // present is checked. Without any (another publisher), the download is trusted.
    private void verifyChecksum(String url, Path archive) throws IOException {
        for (String algorithm : List.of("SHA-256", "SHA-1")) {
            String extension = "." + algorithm.replace("-", "").toLowerCase(Locale.ROOT);
            String expected = readChecksum(URI.create(url + extension));
            if (expected == null) continue;
            String actual = digest(archive, algorithm);
            if (!actual.equals(expected)) {
                throw new GradleException("Kore: the " + algorithm + " of " + url + " is " + actual + ", expected " + expected);
            }
            return;
        }
        logger.warn("Kore: no checksum beside {}", url);
    }

    private String readChecksum(URI uri) throws IOException {
        String text;
        if ("file".equals(uri.getScheme())) {
            Path checksum = Paths.get(uri);
            if (!Files.isRegularFile(checksum)) return null;
            text = Files.readString(checksum, StandardCharsets.UTF_8);
        } else {
            HttpResponse<String> response = send(uri, HttpResponse.BodyHandlers.ofString());
            if (response.statusCode() != 200) return null;
            text = response.body();
        }
        String[] words = text.trim().split("\s+");
        return words.length == 0 || words[0].isEmpty() ? null : words[0].toLowerCase(Locale.ROOT);
    }

    private <T> HttpResponse<T> send(URI uri, HttpResponse.BodyHandler<T> handler) throws IOException {
        HttpClient client = HttpClient.newBuilder()
                .followRedirects(HttpClient.Redirect.NORMAL)
                .connectTimeout(Duration.ofSeconds(30))
                .build();
        HttpRequest.Builder request = HttpRequest.newBuilder(uri).GET();
        if (username != null && password != null) {
            String token = Base64.getEncoder().encodeToString((username + ":" + password).getBytes(StandardCharsets.UTF_8));
            request.header("Authorization", "Basic " + token);
        }
        try {
            return client.send(request.build(), handler);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("Interrupted while downloading " + uri, e);
        }
    }

    private static String digest(Path file, String algorithm) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance(algorithm);
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
        byte[] buffer = new byte[1 << 16];
        try (InputStream input = Files.newInputStream(file)) {
            for (int read; (read = input.read(buffer)) > 0; ) digest.update(buffer, 0, read);
        }
        return HexFormat.of().formatHex(digest.digest());
    }

    private static void unzip(Path archive, Path target) throws IOException {
        Path normalizedTarget = target.toAbsolutePath().normalize();
        try (ZipInputStream zip = new ZipInputStream(Files.newInputStream(archive))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null; ) {
                Path path = normalizedTarget.resolve(entry.getName()).normalize();
                if (!path.startsWith(normalizedTarget)) throw new IOException("Bad entry in " + archive + ": " + entry.getName());
                if (entry.isDirectory()) {
                    Files.createDirectories(path);
                } else {
                    Files.createDirectories(path.getParent());
                    try (OutputStream output = Files.newOutputStream(path)) {
                        zip.transferTo(output);
                    }
                }
            }
        }
    }

    // `tar` keeps the executable bits and symbolic links of the Linux and macOS bundles.
    private static void untar(Path archive, Path target) throws IOException {
        List<String> command = new ArrayList<>(List.of("tar", "-xzf", archive.toString(), "-C", target.toString()));
        Process process = new ProcessBuilder(command).redirectErrorStream(true).start();
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        try {
            if (!process.waitFor(30, TimeUnit.MINUTES) || process.exitValue() != 0) {
                throw new IOException("tar failed for " + archive + ": " + output);
            }
        } catch (InterruptedException e) {
            process.destroy();
            Thread.currentThread().interrupt();
            throw new IOException("Interrupted while unpacking " + archive, e);
        }
    }

    private static Path singleChildDirectory(Path directory) throws IOException {
        List<Path> children = new ArrayList<>();
        try (DirectoryStream<Path> stream = Files.newDirectoryStream(directory)) {
            stream.forEach(children::add);
        }
        if (children.size() != 1 || !Files.isDirectory(children.get(0))) {
            throw new IOException("Expected one directory in the Kore bundle, found " + children);
        }
        return children.get(0);
    }

    private static void deleteRecursively(Path path) throws IOException {
        if (!Files.exists(path)) return;
        try (Stream<Path> paths = Files.walk(path)) {
            for (Path p : (Iterable<Path>) paths.sorted(Comparator.reverseOrder())::iterator) Files.delete(p);
        }
    }

    static String hostPlatform() {
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        String arch = System.getProperty("os.arch").toLowerCase(Locale.ROOT);
        String family;
        if (os.startsWith("windows")) family = "windows";
        else if (os.startsWith("mac") || os.startsWith("darwin")) family = "macos";
        else if (os.startsWith("linux")) family = "linux";
        else throw new GradleException("Kore: unsupported host OS " + os);
        String architecture;
        if (arch.equals("amd64") || arch.equals("x86_64")) architecture = "x86_64";
        else if (arch.equals("aarch64") || arch.equals("arm64")) architecture = "aarch64";
        else throw new GradleException("Kore: unsupported host architecture " + arch);
        return family + "-" + architecture;
    }
}
