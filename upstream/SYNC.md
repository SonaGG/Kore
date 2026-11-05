# Syncing Kore with upstream Kotlin

Kore is a source snapshot of the Kotlin/Native parts of [JetBrains/kotlin](https://github.com/JetBrains/kotlin). It is
not a git-history fork. The snapshot commit for each upstream release is tagged `upstream-v<version>`:

- `upstream-v2.4.21` is the snapshot of upstream commit `9180ce6a7a7d3627f88b424717583f28ddca052d`.

## What is imported

[`paths.txt`](paths.txt) lists every upstream path that Kore carries. They are copied verbatim with the same directory
layout as upstream, which keeps klib-relative source paths identical (`-Xklib-relative-path-base`).

Monorepo modules that Kore does **not** carry are listed in [`stub-projects.txt`](stub-projects.txt). Each one becomes
an empty Gradle project with its upstream path, backed by the matching published artifact at
`kore.upstream.kotlin.version`.

## Moving to a newer upstream release

Use a vendor branch, so that Kore's own changes are kept by an ordinary three-way merge.

1. In a JetBrains/kotlin checkout that has the new tag (e.g. `v2.5.0`), export the same paths into a scratch directory:

   ```bash
   git -c core.longpaths=true archive --format=tar v2.5.0 $(sed 's|/$||' <Kore>/upstream/paths.txt) | tar -x -C /path/to/scratch
   ```

2. In Kore, create a branch from the previous snapshot and replace its contents with the new export:

   ```bash
   git switch -c upstream-import upstream-v2.4.21
   git rm -rq . && cp -r /path/to/scratch/. . && git add -A
   git commit -m "Import Kotlin/Native sources from JetBrains/kotlin v2.5.0"
   git tag upstream-v2.5.0
   ```

3. Merge into `main`: `git switch main && git merge upstream-import`. Resolve conflicts in files marked `Kore:`.

4. Bump `kore.upstream.kotlin.version` and `build.number` in `gradle.properties`.

5. Run `./gradlew help`. For every "Project with path ':x' could not be found" error, add `:x` to `stub-projects.txt`.
   Use the artifact column if it is a production dependency, or `none` if it is test-only.

6. Regenerate checksums for the new artifacts with `./gradlew --write-verification-metadata sha256 help`, then rebuild
   and re-run the verification in the README.

Note: `git archive` works on a checkout whose working tree is incomplete, for example on Windows without
`core.longpaths`. It reads straight from the object database.
