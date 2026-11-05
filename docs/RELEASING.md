# Releasing EasyNotepad

## Where the version lives

One file: **`VERSION`** at the repository root, `MAJOR.MINOR.PATCH` (semantic versioning: a
new feature raises MINOR, a fix alone raises PATCH, a change that breaks how people use the app
raises MAJOR). Everything else is derived from it:

| Where | How |
|---|---|
| Desktop and web builds | `CMakeLists.txt` reads `VERSION` into `project(... VERSION ...)` |
| Title bar, About, the MCP connector | `enp/Version.h`, written at build time by `cmake/Version.cmake` |
| Windows `EasyNotepad.exe` (Properties > Details) | `cmake/EasyNotepad.rc.in` |
| Android `versionName` and `versionCode` | `android/app/build.gradle` reads `VERSION`; `versionCode` is `MAJOR * 10000 + MINOR * 100 + PATCH` |
| Release notes | the version's section in `CHANGELOG.md` |

Never write a version number anywhere else.

## Release and development builds

`VERSION` holds the release being worked towards, so between two releases it already names the
next one. A build is a **release build** only when it is built from the tag `v<VERSION>` (with
no changes in the tree), or when CI builds that tag (`-DENP_RELEASE_TAG=v<VERSION>`). Every
other build is a **development build**:

| | Title bar | About and bug reports (`ENP_VERSION_FULL`) |
|---|---|---|
| Release | `v1.1.0` | `1.1.0` |
| Development | `v1.1.0-dev` | `1.1.0-dev+0b958ac` (`.dirty` with uncommitted changes) |

A tag that does not match `VERSION` stops the build.

## What CI checks (`.github/workflows/build.yml`)

On every push and pull request, the **version** job checks that `VERSION` is `MAJOR.MINOR.PATCH`
and that `CHANGELOG.md` has a section for it. On a tag `v*`, it also checks that the tag is
`v` + `VERSION`. The builds name their artifacts `EasyNotepad-<version>-<platform>`.

On a tag `v*`, once every build has passed, the **release** job publishes a GitHub Release named
after the tag: one zip per platform (the app and its resources, without the test programs), the
web build and the Android APKs (the debug one, and the signed release one when the key is set,
below), with the version's `CHANGELOG.md` section as its notes.

## Making a release

1. On a branch: set `VERSION` (if it does not already name this release), and in
   `CHANGELOG.md` replace the section's `unreleased` with today's date (`YYYY-MM-DD`).
2. Open a pull request to `main` and merge it once CI passes.
3. Tag the merged commit on `main` and push the tag:

   ```sh
   git checkout main && git pull
   git tag -a v1.1.0 -m "EasyNotepad 1.1.0"
   git push origin v1.1.0
   ```

4. CI builds the tag and publishes the release. Check the release page and its downloads.
5. Start the next version: on a branch, raise `VERSION` (for example to `1.2.0`) and add a
   `## [1.2.0] - unreleased` section at the top of `CHANGELOG.md`.

## The Android release key

Android installs only signed APKs, and an update only when it is signed with the same key as the
installed app. The key is our own, self-signed (as every Android key is): create it once with

```sh
scripts/android-release-key.sh ~/keys/easynotepad-release.jks
```

and store the four secrets it lists (`ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`,
`ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`) in the repository's Actions secrets. With them, CI
builds `EasyNotepad-release.apk` signed with the key on every run, and a release attaches it next
to the debug APK. Without them CI builds the debug APK only. The keystore never enters the
repository (`*.jks` is ignored); back it up with its password: if it is lost, the installed app
can never be updated, only uninstalled and installed again.
