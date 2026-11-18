<p align="center">
  <img style="image-rendering: pixelated" width=400px src="./.github/logo-mobile.png" alt="ZQuest Classic Mobile logo"/>
</p>

# ZQuest Classic Mobile

An unofficial Android port of the [ZQuest Classic](https://github.com/ZQuestClassic/ZQuestClassic) player. Very early — a proof of concept, built almost entirely by an AI agent, that is slowly becoming a playable mobile app. Expect rough edges.

[Preview builds](https://github.com/reeseroyster/zquestclassic-android/releases) · [Upstream project](https://github.com/ZQuestClassic/ZQuestClassic) · [Port patch](./android-port.patch)

## Status

Early preview. Quests load and play on a phone with touch controls, but nothing here is polished or guaranteed. This is not an official release and is not affiliated with the ZQuest Classic team — all engine credit belongs to them.

## Install

Grab `app-debug.apk` from the [latest preview release](https://github.com/reeseroyster/zquestclassic-android/releases) and sideload it, or track this repo with [Obtainium](https://github.com/ImranR98/Obtainium) (enable *Include prereleases*) to get updates automatically. Then grant a quest folder or import a `.qst` and tap a quest to play.

## What's in this repo

- **[Releases](https://github.com/reeseroyster/zquestclassic-android/releases)** — prebuilt preview APKs (`arm64-v8a` + `x86_64`, Android 6+).
- **[`android-port.patch`](./android-port.patch)** — the Android shell and mobile changes, applied against upstream ZQuest Classic.

The engine itself is upstream's C++ core; this repo only adds the Android layer and distributes preview builds.

## Images

| Library | Gameplay |
| --- | --- |
| ![Library](./.github/screenshots/library.png) | ![Gameplay](./.github/screenshots/gameplay.png) |

## License

GPLv3, same as upstream. See [LICENSE](https://github.com/ZQuestClassic/ZQuestClassic/blob/main/LICENSE).