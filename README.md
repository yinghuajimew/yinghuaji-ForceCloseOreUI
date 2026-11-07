<div align="center">

<h1 align="center">ForceCloseOreUI — Dependency (preloader)</h1>

<p>Force Minecraft Bedrock on Android to drop OreUI and go back to the classic JSON-UI,<br>
using the <code>libpreloader.so</code> loader.</p>

<p>
<b>English</b> · <a href="./README.zh-CN.md">中文</a>
</p>

</div>

<br/>

> [!IMPORTANT]
> **This is the dependency variant. Most people want the standalone one, or just the app.**
> - Easiest: the one-tap app — **<https://github.com/yinghuajimew/yhjmew.no.ore.ui>**
> - No `libpreloader.so` needed: **[ForceCloseOreUI-android](https://github.com/yinghuajimew/ForceCloseOreUI-android)**

## What this is

An `arm64-v8a` library that hooks Minecraft Bedrock's OreUI config initializer and forces every
`OreUIConfig` entry to `false`, making the game fall back to JSON-UI.

Unlike the standalone build, this one is loaded through the **`libpreloader.so`** loader, which
must be present in the APK alongside it. That loader was the original approach in this project and
works on a wider range of Android versions, including older ones.

## Requirements

| Item | Requirement |
|---|---|
| ABI | `arm64-v8a` only |
| Android | Any Android version supported by `libpreloader.so` |
| Injection | Both `libpreloader.so` **and** this library |

> [!NOTE]
> If you are on Android 11+ and don't specifically need the preloader path, prefer the
> **standalone** build — fewer moving parts, one less `.so` to get wrong.

## How to use

Inside a repackaged Minecraft APK you need both files:

```
lib/arm64-v8a/libpreloader.so            # the loader
lib/arm64-v8a/libForceCloseOreUI.so      # this library
```

**Recommended: use the one-tap app** — <https://github.com/yinghuajimew/yhjmew.no.ore.ui>
It lets you pick the SO type, injects `libpreloader.so` plus the matching library, re-signs and
installs. In the app's *SO settings*, choose **依賴版 SO (dependency)**.

If you repackage by hand: remember that a different signing key means you must uninstall the
official build first, and uninstalling deletes your world data.

Config and logs land in:

```
/storage/emulated/0/Android/data/com.mojang.minecraftpe/mods/ForceCloseOreUI/
├── config.json        # what to disable; created on first successful hook
├── debug.log          # session log (file channel, added in this fork)
├── crash_native.log   # native crash dump, with the game's own logcat snapshot
└── readme.txt         # folder note written on first run
```

If that path is not writable it falls back to `/storage/emulated/0/games/com.mojang/ForceCloseOreUI/`.

## What changed in this build

This fork is **not** the upstream dependency build. The hook mechanism is unchanged (it still uses
the `pl::hook` static registrar), but the diagnostics and signature coverage were brought up to the
level of the standalone version.

### 1. Real diagnostics instead of logcat-only

Upstream only wrote to logcat, which is gone the moment the process dies — useless for diagnosing a
crash. This build adds:

- **`debug.log` file channel** — survives a crash, so you can read what happened afterwards
- **session header** with manufacturer, model, Android version, SDK, ABI, page size and build
  fingerprint, so a bug report carries its own context
- **`crash_native.log`** — signal, fault address, and the full register dump at the moment of the
  crash
- **logcat snapshot thread** — captures the game's own logcat into the crash file (incremental,
  up to 50 minutes), because the interesting part is usually what the *game* logged right before
  dying

### 2. Signature coverage extended to 1.26.60

The built-in pattern list now covers **1.21.90 ~ 1.26.60**, tried newest-first:

| Version |
|---|
| 1.26.60 |
| 1.26.50 |
| 1.26.40 |
| 1.26.30 |
| 1.26.20 |

Adding a new game version means adding one line to the pattern list in `src/ForceCloseOreUI.cpp`.

### 3. Android-only repository

The upstream project also carried a Windows build. This fork is Android-only — the Windows DLL
lives separately at
**[ForceCloseOreUI-windows](https://github.com/yinghuajimew/ForceCloseOreUI-windows)**.

## Known limitation

On some newer Android versions (reported on Android 17 / SDK 37, vivo devices) the preloader fails
while parsing the game's ELF, logging `Gloss Elfinfo init...` and then aborting with `SIGABRT`
before any signature scan starts. That is a problem in the `libpreloader.so` loader, **not** in this
library, and it is not fixed here.

If you hit this, use the **standalone** build instead — it does not go through the preloader at all.

## Building

```sh
xmake f -p android -a arm64-v8a -m release -y
xmake
# -> build/android/arm64-v8a/release/libForceCloseOreUI.so
```

CI (`.github/workflows/build.yml`) builds and publishes the artifact.

## Credits

Based on the original [ForceCloseOreUI](https://github.com/QYCottage/forceCloseOreUI) by QYCottage
and contributors. The `pl::hook` static registrar and preloader come from that project.
