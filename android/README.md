# ZQuest Classic for Android (player)

This is a native Android player using the project's C++ runtime and Allegro's
Android backend. It does not include the desktop quest editor.

## Build

Install Android Studio with the Android SDK, NDK r27 or newer, and CMake 3.27 or
newer. The build also needs a Java 17 runtime and Gradle 8.9 or newer on `PATH`.

```sh
export ANDROID_HOME="$HOME/Android/Sdk" # adjust if needed
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/<installed-version>" # optional
./android/build.sh
```

`build.sh` uses the newest NDK it finds under `$ANDROID_HOME/ndk`. NDK r27 is the
first release whose `libc++_shared.so` is aligned for 16 KB memory pages, which
Android 15+ devices may use.

The debug APK is written to
`android/app/build/outputs/apk/debug/app-debug.apk`. The script first generates
Allegro's Android library; run it once before opening `android/` in Android
Studio so that its local AAR dependency is available.

## The quest library

Opening the app shows the library rather than jumping straight into a game:

- **On this device** lists quests already imported into the app. Tapping one
  starts it.
- **Choose folder** picks a folder once with the system folder picker (it opens
  in Downloads). Everything below **In \<folder name>** is then read from that
  folder, including sub-folders, and re-read every time the library appears. A
  `.qst` is imported as-is; a `.zip` is extracted into its own folder so the
  music and extra files a quest ships with are kept.
- **Import file** still does a one-off import of a single `.qst` or `.zip`.

Re-importing the same archive reopens the copy that is already there rather
than adding a second "name (2)" entry.

Everything is copied into the app's private storage, so quests run offline and
the app needs no broad storage permission.

## Playing

In game, the top-right bar has three buttons:

- **Home** stops the game and returns to the library.
- **Menu** opens ZC's own pause menu, the same one a hardware Escape key opens.
- **Pad** shows or hides the on-screen D-pad and buttons. The choice is
  remembered, and the bar stays visible either way.

Touching or clicking elsewhere in the gameplay area does not open the pause
menu. Use **Menu** or Escape to pause; ordinary touches can interact with the
game without interrupting it.

The on-screen controls send ordinary key events, so a hardware keyboard works
too. The default ZC controls are the arrow keys, `Z`/`X`, `Q`/`W`, `Enter`, and
`Space`.

The library and the game run in separate processes. That is deliberate: stopping
the game shuts down its process, and keeping the library elsewhere means the
quest list survives.

## Test status

Verified on a Pixel 10 Pro (Android 16) with the six quests in
`Download/quests`: `q1.zip`, `q2.zip`, `q321_FunintheSun8bitExtraordinare.zip`,
`q569_PassageThroughTime.zip`, `q845_TheOfficial7thQuest.zip` and
`q853_MetdroidPlanetXX32.zip`. All six import, start, and play with no crashes;
the touch controls, the pause menu, the pad toggle and the Home button were all
exercised. A touch in the middle of the game was also verified not to open the
pause menu, while the explicit Menu button still does. This is a smoke test, not
a full playthrough of any quest.

The APK passes Android's 16 KB page size compatibility check.

## Current limitations

- The APK includes `arm64-v8a` for phones and `x86_64` for Waydroid and other
  x86-64 Android environments.
- The build is debug-signed, so Android may show a compatibility notice on first
  launch. Pick "Don't Show Again".
- ZC draws its pause menu small in the top-left corner rather than filling the
  screen, and leaves part of the menu painted for a moment after it closes. It
  is usable with the on-screen D-pad, but it does not look right.
- Long folder listings are read on the UI thread, and there is no way yet to
  delete an imported quest once it is in the library.

`third_party/allegro5-android-teardown.patch` is applied to Allegro on Android
only. It guards two NULL dereferences that fire while the game shuts down,
because Allegro runs its destructors newest first and so destroys bitmaps after
the display they belong to. Without it, pressing Home intermittently killed the
game process with a "ZQuest Classic keeps stopping" dialog.
