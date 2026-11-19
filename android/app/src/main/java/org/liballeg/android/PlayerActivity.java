package org.liballeg.android;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.app.AlertDialog;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;
import android.util.Log;

import java.util.HashMap;
import java.util.Map;

/** Allegro's Android surface with lightweight touch controls layered over it. */
public final class PlayerActivity extends AllegroActivity {
    private final Map<View, Integer> heldControls = new HashMap<>();
    private View controlsOverlay;
    /** The cog menu (scrim + pages) lives in its own overlay ABOVE the pad, so it
     *  always draws over the pad and survives a rotation pad rebuild. */
    private FrameLayout menuOverlay;
    /** Pad rebuild baseline: the decor size the controls were last laid out for.
     *  A layout-change to a different size (i.e. a completed rotation) triggers
     *  rebuildControls; set at the end of addTouchControls. */
    private int padBuiltW = -1, padBuiltH = -1;
    private View gamePad;
    private FrameLayout gamePadContainer;
    private LinearLayout centerRow;
    private StickView stick;
    private boolean menuHeld = false;
    private boolean returningToLibrary = false;
    private View settingsPanel;
    private View settingsSubPanel;
    private View engineMenuPanel;
    private final java.util.ArrayList<View> enginePages = new java.util.ArrayList<>();
    private int fontPickSlot = -1;   // which More-Options slot the page-12 chooser edits
    private View menuScrim;
    private float padScale = 0.65f;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private Runnable pendingMouseRelease;
    private EditText charInput;
    private boolean forwardingChars = false;
    private boolean textCapture = false;
    private boolean syntheticKey = false;

    /** Asks the running game to unwind and shut down, as the in-game Quit does. */
    private native void nativeRequestQuit();

    /** Holds an Allegro keycode down/up for the on-screen stick. */
    private native void nativeSetGameKey(int keycode, boolean down);

    /** Emits a real key event, which is what ZC's menus and dialogs read. */
    private native void nativeSendGameKeyEvent(int keycode, boolean down);

    /** Feeds ZC's mouse-driven GUI (menus, dialogs) with a pointer position and click. */
    private native void nativeSetGameMouse(int x, int y, boolean down);
    private native void nativeSaveGame();
    private native void nativeLoadGame();
    private native void nativeQuickSave();
    private native void nativeQuickLoad();
    private native String nativeCurrentQuestKey();
    private native void nativeMenuAction(int id);
    private native int nativeGameBottomPx();
    private native int nativeGameTopPx();
    private native void nativeSetPaused(boolean pause);
    private native void nativeConfigOp(int op);
    private native int nativeGetToggle(int id);
    private native int nativeGetGameKey(int btnId);
    private native void nativeBindGameKey(int btnId, int keycode);
    private native String nativeGetGameKeyName(int btnId);
    private native int nativeVolumeGet(int which);
    private native void nativeVolumeSet(int which, int value);
    private native int nativePanGet();
    private native void nativePanSet(int value);
    private native String nativeQuestInfo();
    private native int nativeFontCount();
    private native String nativeFontNameAt(int i);
    private native int nativeGetFontPref(int slot);
    private native void nativeSetFontPref(int slot, int val);

    /** Types one character into the engine, as a real keyboard would. */
    private native void nativeSendGameChar(int ch);

    static {
        loadAllegroLibrary("allegro");
        loadAllegroLibrary("allegro_primitives");
        loadAllegroLibrary("allegro_image");
        loadAllegroLibrary("allegro_font");
        loadAllegroLibrary("allegro_audio");
        loadAllegroLibrary("allegro_acodec");
        loadAllegroLibrary("allegro_memfile");
        loadAllegroLibrary("allegro_dialog");
        // Allegro opens the player with dlopen() from native code, which does not
        // register the library with the runtime's native method lookup. Loading
        // it here as well lets Java call into zplayer (the quit request).
        System.loadLibrary("zplayer");
    }

    private static void loadAllegroLibrary(String name) {
        try {
            System.loadLibrary(name + "-debug");
        } catch (UnsatisfiedLinkError debugBuildNotFound) {
            try {
                System.loadLibrary(name);
            } catch (UnsatisfiedLinkError releaseBuildNotFound) {
                releaseBuildNotFound.addSuppressed(debugBuildNotFound);
                Log.e("ZQuestClassic", "Could not load Allegro library " + name, releaseBuildNotFound);
                throw releaseBuildNotFound;
            }
        }
    }

    public PlayerActivity() {
        super("libzplayer.so");
    }

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().getDecorView().setKeepScreenOn(true);
        // On rotation the window resizes asynchronously and reading the display
        // metrics inside onConfigurationChanged gives a STALE size, which is what
        // used to leave L/the stick "stuck" mid-rotation. Rebuild the pad from a
        // layout-change instead: that fires only once the window has actually
        // settled at the new size, and it also re-centers an open cog menu page.
        getWindow().getDecorView().addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            int w = r - l, h = b - t;
            if (w == padBuiltW && h == padBuiltH) return;
            padBuiltW = w; padBuiltH = h;
            v.post(() -> {
                rebuildControls();
                for (View p : enginePages)
                    if (p != null && p.getVisibility() == View.VISIBLE) centerEnginePage(p);
            });
        });
    }

    // Allegro creates/replaces its SurfaceView asynchronously after onCreate.
    // This package-level override runs after that replacement, so the controls
    // stay on top of the native game surface.
    @Override
    void createSurface() {
        super.createSurface();
        addTouchControls();
    }

    // The Activity is NOT recreated on rotation (configChanges in the manifest),
    // so the on-screen controls must be re-laid-out for the new orientation here —
    // otherwise the pad stays in the old orientation's positions (cut off,
    // smushed at the bottom, wrong order). Rebuild on the next layout pass so the
    // rotated insets/size are current.
    @Override
    public void onConfigurationChanged(android.content.res.Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // The pad rebuild + menu re-center are driven by the layout-change listener
        // in onCreate, which runs only after the window settles at the rotated size
        // (reading the size here is too early and used to strand L/the stick).
    }

    private void addTouchControls() {
        ViewGroup decor = (ViewGroup) getWindow().getDecorView();
        if (controlsOverlay != null && controlsOverlay.getParent() instanceof ViewGroup) {
            ((ViewGroup) controlsOverlay.getParent()).removeView(controlsOverlay);
        }
        FrameLayout controls = new FrameLayout(this);
        controls.setFocusable(false);
        // Keep every control clear of the display cutout, rounded corners and the
        // gesture bar on real phones (Waydroid has none, so this only shows up on
        // hardware): the gear, the pad and the face buttons all inset off the edges.
        int[] si = safeInsets();
        controls.setPadding(Math.max(si[0], dp(12)), Math.max(si[1], dp(12)),
                            Math.max(si[2], dp(14)), Math.max(si[3], dp(20)));
        boolean portrait = getResources().getConfiguration().orientation
                == android.content.res.Configuration.ORIENTATION_PORTRAIT;

        // Anything the stick and the buttons do not claim becomes a pointer event
        // for ZC's own mouse-driven menus and dialogs. ViewGroup only consults this
        // listener for touches no child consumed, so the controls keep priority.
        // The release is deferred: the menu samples the button once per frame, and a
        // tap shorter than a frame would otherwise never be seen as a click.
        controls.setOnTouchListener((view, event) -> {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_POINTER_DOWN:
                case MotionEvent.ACTION_MOVE:
                    cancelPendingMouseRelease();
                    nativeSetGameMouse((int) event.getX(), (int) event.getY(), true);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_POINTER_UP:
                case MotionEvent.ACTION_CANCEL: {
                    final int x = (int) event.getX();
                    final int y = (int) event.getY();
                    cancelPendingMouseRelease();
                    pendingMouseRelease = () -> {
                        pendingMouseRelease = null;
                        nativeSetGameMouse(x, y, false);
                    };
                    mainHandler.postDelayed(pendingMouseRelease, 90);
                    return true;
                }
                default:
                    return false;
            }
        });

        // Settings card: one gear opens a menu holding every action that used to
        // sit on the top bar, so screen real estate goes to the game itself.
        padScale = getSharedPreferences("zquestclassic", MODE_PRIVATE).getInt("pad-scale", 65) / 100f;

        // The cog menu (gear opens it) and its scrim are built in ensureMenuOverlay()
        // and added in a separate overlay ABOVE the pad — so the menu always draws
        // on top of the pad and survives a rotation pad rebuild.

        // Soft-keyboard sink. It stays invisible and always empty: every character the
        // IME commits is forwarded to the engine and then wiped, so text only ever
        // appears in the game's own fields, never in the overlay.
        charInput = new EditText(this);
        charInput.setAlpha(0f);
        charInput.setSingleLine(true);
        charInput.setBackground(null);
        charInput.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        charInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) {
                if (forwardingChars || s.length() == 0) return;
                forwardingChars = true;
                for (int i = 0; i < s.length(); i++) nativeSendGameChar(s.charAt(i));
                s.clear();
                forwardingChars = false;
            }
        });
        charInput.setOnKeyListener((view, keyCode, event) -> {
            if (event.getAction() == KeyEvent.ACTION_DOWN && keyCode == KeyEvent.KEYCODE_DEL) {
                nativeSendGameChar(8);
                return true;
            }
            return false;
        });
        controls.addView(charInput, new FrameLayout.LayoutParams(dp(2), dp(2), Gravity.TOP | Gravity.START));

        // ZC draws its own menu into the top-left corner of the screen, so the
        // gear and menu sit top-right to stay clear of the menu and of the game
        // HUD, which lives inside the scaled 640x480 viewport in the middle.

        // Gamepad Container
        FrameLayout gamePadContainer = new FrameLayout(this);
        gamePadContainer.setFocusable(false);
        this.gamePadContainer = gamePadContainer;

        // Thumbstick instead of a button cross. ZC only reads four directions,
        // so the stick resolves an angle into the digital directions the engine
        // already understands; the whole lower-left box is the touch target and
        // the stick re-centres under the thumb, which is what makes it feel
        // forgiving compared to hitting four small buttons.
        // Shoulders and config pinned just below the game screen, on the black:
        // L up-left of the deck, config dead-centre, R up-right. Primary A/B, the
        // stick and Map/Start all live in the lower deck.
        padPortrait = portrait;
        padTopInset = si[1];
        // Fallback before the native render reports the true game bottom;
        // placeShoulderRow() corrects the row to sit just below the game.
        int ph_ = getResources().getDisplayMetrics().heightPixels;
        int pw_ = getResources().getDisplayMetrics().widthPixels;
        int gameBottom = portrait ? (int) (ph_ * 0.08f + pw_ * 0.75f) : dp(120);
        int deckTop = Math.max(0, gameBottom + dp(8) - padTopInset);

        lBtn = makeControl("L", KeyEvent.KEYCODE_Q, sc(78));
        FrameLayout.LayoutParams lTop = new FrameLayout.LayoutParams(sc(78), sc(78), Gravity.TOP | Gravity.START);
        rBtn = makeControl("R", KeyEvent.KEYCODE_W, sc(78));
        FrameLayout.LayoutParams rTop = new FrameLayout.LayoutParams(sc(78), sc(78), Gravity.TOP | Gravity.END);

        // The gear stays in the controls overlay (OUTSIDE gamePad) so hiding the pad
        // never strands you without the cog. In landscape it normally sits hidden and
        // the centre trio carries the visible cog; it reappears if the pad is hidden.
        cfgBtn = makeCogButton(sc(66));
        FrameLayout.LayoutParams cfgTop;

        // Movement stick, plus the primary face buttons.
        stick = new StickView();
        LinearLayout actionRow = new LinearLayout(this);
        actionRow.setGravity(Gravity.CENTER);
        actionRow.addView(makeControl("B", KeyEvent.KEYCODE_X, sc(98)));
        actionRow.addView(makeControl("A", KeyEvent.KEYCODE_Z, sc(98)));

        // Map/Start (and the gear too) live in one centered row.
        centerRow = new LinearLayout(this);
        centerRow.setFocusable(false);
        centerRow.setGravity(Gravity.CENTER);   // keep all three buttons on one vertical line
        centerRow.addView(makeControl("Map", KeyEvent.KEYCODE_SPACE, sc(70)));
        centerRow.addView(makeControl("Start", KeyEvent.KEYCODE_ENTER, sc(70)));

        FrameLayout.LayoutParams stickParams;
        FrameLayout.LayoutParams faceParams;
        FrameLayout.LayoutParams centerParams;

        if (portrait) {
            // Portrait layout: L/R below the game on the black deck; stick + A/B
            // raised and dead-level; the Map/Start/cog trio dead-centre in the deck.
            lTop.setMargins(dp(10), deckTop, 0, 0);
            rTop.setMargins(0, deckTop, dp(10), 0);
            stickParams = new FrameLayout.LayoutParams(sc(260), sc(260), Gravity.START | Gravity.BOTTOM);
            stickParams.setMargins(sc(6), 0, 0, sc(341));
            faceParams = new FrameLayout.LayoutParams(-2, -2, Gravity.END | Gravity.BOTTOM);
            faceParams.setMargins(0, 0, dp(22), dp(253));
            centerRow.addView(makeCogButton(sc(70)));   // same Map/Start/cog trio as landscape
            centerParams = new FrameLayout.LayoutParams(-2, sc(70) + dp(20), Gravity.CENTER_HORIZONTAL | Gravity.BOTTOM);
            centerParams.setMargins(0, 0, 0, dp(14));
            // Standby gear (bottom-right) shows only when the pad is hidden.
            cfgTop = new FrameLayout.LayoutParams(sc(66), sc(66), Gravity.END | Gravity.BOTTOM);
            cfgTop.setMargins(0, 0, dp(12), dp(14));
        } else {
            // Symmetric LANDSCAPE layout, positioned from the native-rendered game
            // top/bottom (placeLandscapeDeck) so it centres on ANY screen: L/R raised
            // into the top black band, stick + A/B vertically centred on mirrored
            // sides, Map/Start/cog dead-centre in the bottom black band.
            lTop.setMargins(dp(10), dp(10), 0, 0);
            rTop.setMargins(0, dp(10), dp(10), 0);
            stickParams = new FrameLayout.LayoutParams(sc(260), sc(260), Gravity.CENTER_VERTICAL | Gravity.START);
            stickParams.setMargins(sc(6), 0, 0, 0);
            faceParams = new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER_VERTICAL | Gravity.END);
            faceParams.setMargins(0, 0, dp(22), 0);
            centerRow.addView(makeCogButton(sc(70)));   // the gear is the right arm of the trio
            // Game fills the screen in landscape, so the trio clusters bottom-right.
            centerParams = new FrameLayout.LayoutParams(-2, sc(70) + dp(20), Gravity.END | Gravity.BOTTOM);
            centerParams.setMargins(0, 0, dp(20), dp(8));
            cfgTop = new FrameLayout.LayoutParams(sc(66), sc(66), Gravity.CENTER_HORIZONTAL | Gravity.BOTTOM);
            cfgTop.setMargins(0, 0, dp(6), dp(6));
        }

        gamePadContainer.addView(lBtn, lTop);
        gamePadContainer.addView(rBtn, rTop);
        gamePadContainer.addView(stick, stickParams);
        gamePadContainer.addView(actionRow, faceParams);
        gamePadContainer.addView(centerRow, centerParams);

        gamePad = gamePadContainer;
        
        // Restore visibility state
        boolean isVisible = getSharedPreferences("zquestclassic", MODE_PRIVATE).getBoolean("controls-visible", true);
        gamePad.setVisibility(isVisible ? View.VISIBLE : View.GONE);

        controls.addView(gamePad, new ViewGroup.LayoutParams(-1, -1));

        // Add the gear after the pad so it sits on top in z-order and stays
        // visible even when the pad is hidden (Show/Hide Pad).
        controls.addView(cfgBtn, cfgTop);
        // In both orientations the live cog lives in the center trio (inside the pad),
        // so the standalone gear is a standby for when the pad is hidden. Respect the
        // persisted pad state at launch so the menu stays reachable after a relaunch.
        boolean isVisible2 = getSharedPreferences("zquestclassic", MODE_PRIVATE)
                .getBoolean("controls-visible", true);
        cfgBtn.setVisibility(isVisible2 ? View.GONE : View.VISIBLE);

        // Keep nudging the shoulder row down onto the black until the native
        // render reports the real game bottom (it fills in shortly after launch).
        // Landscape parallels that: L/R and the trio track the native game bounds.
        for (int i = 1; i <= 12; i++) gamePad.postDelayed(
                padPortrait ? this::placeShoulderRow : this::placeLandscapeDeck, i * 250L);

        controlsOverlay = controls;

        // Record the decor size this pad was laid out for so the onCreate
        // layout-change listener doesn't rebuild on the very first layout pass.
        android.graphics.Point real = new android.graphics.Point();
        getWindowManager().getDefaultDisplay().getRealSize(real);
        padBuiltW = real.x; padBuiltH = real.y;
        decor.addView(controlsOverlay, new ViewGroup.LayoutParams(-1, -1));

        // Add the menu overlay ABOVE the pad so the cog always draws on top and is
        // not torn down when rebuildControls() recreates the pad on rotation.
        ensureMenuOverlay(decor);

        // Publish real control geometry once laid out. Coordinates measured off
        // screenshots go stale as soon as a pill or row is added, which turns automated
        // taps into silent misses; reading them from logcat is exact.
        controls.post(() -> logControlRects(controls, 0));
    }

    /** Builds (once) the cog-menu overlay and keeps it ABOVE the pad overlay. The
     *  scrim + menu pages live here so they always draw on top of the pad (bug fix)
     *  and are NOT torn down when rebuildControls() recreates the pad on rotation —
     *  an open menu survives and is re-centered for the new orientation. */
    private void ensureMenuOverlay(ViewGroup decor) {
        if (menuOverlay != null) {
            if (menuOverlay.getParent() instanceof ViewGroup)
                ((ViewGroup) menuOverlay.getParent()).removeView(menuOverlay);
            decor.addView(menuOverlay, new FrameLayout.LayoutParams(-1, -1));
            return;
        }
        FrameLayout ov = new FrameLayout(this);
        ov.setFocusable(false);
        menuOverlay = ov;

        // Legacy gear details card (kept for the older Settings sub-panel path).
        LinearLayout settingsPanelLayout = new LinearLayout(this);
        settingsPanelLayout.setOrientation(LinearLayout.VERTICAL);
        settingsPanelLayout.setFocusable(false);
        settingsPanelLayout.setVisibility(View.GONE);
        settingsPanelLayout.setBackground(cardBackground(false));
        settingsPanelLayout.addView(makeSettingsRow("Home", this::returnToLibrary));
        settingsPanelLayout.addView(makeSettingsRow("Save", this::saveGameNow));
        settingsPanelLayout.addView(makeSettingsRow("Load", this::loadGameNow));
        settingsPanelLayout.addView(makeSettingsRow("Menu", () -> showEngineMenu(true)));
        settingsPanelLayout.addView(makeSettingsRow("Quick Save", this::quickSaveNow));
        settingsPanelLayout.addView(makeSettingsRow("Quick Load", this::quickLoadNow));
        settingsPanelLayout.addView(makeSettingsRow("Pad", this::toggleGamePad));
        settingsPanelLayout.addView(makeSettingsRow("Settings", () -> showSettingsGrid(true)));
        settingsPanelLayout.addView(makeSettingsRow("Pause", () -> tapScancode(49)));   // KEY_F3
        settingsPanelLayout.addView(makeSettingsRow("Step", () -> tapScancode(50)));    // KEY_F4
        settingsPanelLayout.addView(makeSettingsRow("Keyboard", this::toggleTextCapture));
        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(dp(240), -2, Gravity.TOP | Gravity.END);
        panelParams.setMargins(0, dp(8 + 46 + 6), dp(8), 0);
        ov.addView(settingsPanelLayout, panelParams);
        settingsPanel = settingsPanelLayout;

        LinearLayout settingsSub = new LinearLayout(this);
        settingsSub.setOrientation(LinearLayout.VERTICAL);
        settingsSub.setFocusable(false);
        settingsSub.setVisibility(View.GONE);
        settingsSub.setBackground(cardBackground(false));
        settingsSub.addView(makeSettingsRow("\u2039 Back", () -> showSettingsGrid(false)));
        settingsSub.addView(makePadSizeRow());
        FrameLayout.LayoutParams subParams = new FrameLayout.LayoutParams(dp(240), -2, Gravity.TOP | Gravity.END);
        subParams.setMargins(0, dp(8 + 46 + 6), dp(8), 0);
        ov.addView(settingsSub, subParams);
        settingsSubPanel = settingsSub;

        // Tapping outside the open menu closes it; transparent, clickable, no dim.
        menuScrim = new View(this);
        menuScrim.setVisibility(View.GONE);
        menuScrim.setBackgroundColor(0x00000000);
        menuScrim.setClickable(true);
        menuScrim.setOnClickListener(v -> dismissCogMenu());
        ov.addView(menuScrim, new FrameLayout.LayoutParams(-1, -1));

        // Native replacement for the desktop ESC menu (page 0). buildEnginePage(0)
        // attaches it to menuOverlay (which is set to ov) so it lands on top of the scrim.
        engineMenuPanel = buildEnginePage(0);

        decor.addView(menuOverlay, new FrameLayout.LayoutParams(-1, -1));
    }

    private void logControlRects(View view, int depth) {
        if (view instanceof TextView) {
            TextView text = (TextView) view;
            if (text.getText() != null && text.getText().length() > 0) {
                int[] pos = new int[2];
                view.getLocationOnScreen(pos);
                Log.i("ZQCTL", text.getText() + " " + pos[0] + "," + pos[1]
                        + " " + view.getWidth() + "x" + view.getHeight()
                        + " center=" + (pos[0] + view.getWidth() / 2) + "," + (pos[1] + view.getHeight() / 2));
            }
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) logControlRects(group.getChildAt(i), depth + 1);
        }
    }

    /** Writes progress to the current slot via the engine, then confirms. */
    private void saveGameNow() {
        try {
            nativeSaveGame();
            Toast.makeText(this, "Saving…", Toast.LENGTH_SHORT).show();
        } catch (UnsatisfiedLinkError noSave) {
            // Library not linked yet (before the game loop starts); non-fatal.
            Toast.makeText(this, "Not in a game yet", Toast.LENGTH_SHORT).show();
        }
    }

    /** Returns to the save-slot selection so a saved game can be continued. */
    private void loadGameNow() {
        try {
            nativeLoadGame();
            Toast.makeText(this, "Opening save slots…", Toast.LENGTH_SHORT).show();
        } catch (UnsatisfiedLinkError noLoad) {
            Toast.makeText(this, "Not in a game yet", Toast.LENGTH_SHORT).show();
        }
    }

    /** Quick-save: one hidden state, auto-overwritten, apart from the named slots. */
    private void quickSaveNow() {
        try {
            nativeQuickSave();
            Toast.makeText(this, "Quick saved", Toast.LENGTH_SHORT).show();
        } catch (UnsatisfiedLinkError noQ) {
            Toast.makeText(this, "Not in a game yet", Toast.LENGTH_SHORT).show();
        }
    }

    /** Quick-load: restores the last quick save. */
    private void quickLoadNow() {
        try {
            nativeQuickLoad();
            Toast.makeText(this, "Quick load…", Toast.LENGTH_SHORT).show();
        } catch (UnsatisfiedLinkError noQ) {
            Toast.makeText(this, "Not in a game yet", Toast.LENGTH_SHORT).show();
        }
    }

    // --- Regular save states: up to 3 portable ZC .sav slots per quest ---
    // Each slot is a plain copy of the engine's standalone save file, so a slot
    // can be copied to a desktop ZC save folder and played there untouched.

    private String questKey() {
        try { String k = nativeCurrentQuestKey(); return (k == null || k.isEmpty()) ? "default" : k; }
        catch (UnsatisfiedLinkError e) { return "default"; }
    }
    /** The engine's live standalone save for the current quest. */
    private java.io.File standaloneSave() {
        return new java.io.File(new java.io.File(getFilesDir(), "saves"), "standalone-" + questKey() + ".sav");
    }
    private java.io.File stateDir() {
        java.io.File d = new java.io.File(getFilesDir(), "savestates" + java.io.File.separator + questKey());
        if (!d.exists()) d.mkdirs();
        return d;
    }
    private java.io.File stateSlotFile(int slot) { return new java.io.File(stateDir(), "slot" + (slot + 1) + ".sav"); }

    /** Save the full world state into one of the 3 slots. The engine writes the
     *  live snapshot (.state) on its own thread, so copy it into the slot a beat
     *  after. (The .sav beside it is only the progress blob — the full world —
     *  hero, enemies, screen buffers — lives in .state, which quick-load reads.) */
    private void saveStateNow(int slot) {
        try { nativeQuickSave(); }
        catch (UnsatisfiedLinkError noGame) {
            Toast.makeText(this, "Not in a game yet", Toast.LENGTH_SHORT).show(); return;
        }
        final java.io.File src = new java.io.File(new java.io.File(getFilesDir(), "quick"), questKey() + ".state");
        final java.io.File dst = stateSlotFile(slot);
        mainHandler.postDelayed(() -> {
            try {
                if (!src.exists()) { Toast.makeText(this, "Nothing to save yet", Toast.LENGTH_SHORT).show(); return; }
                copyFile(src, dst);
                Toast.makeText(this, "Saved state to slot " + (slot + 1), Toast.LENGTH_SHORT).show();
            } catch (java.io.IOException io) {
                Toast.makeText(this, "Couldn't save slot " + (slot + 1), Toast.LENGTH_SHORT).show();
            }
        }, 450);
    }

    /** Restore a full-world save state slot, then jump straight back into the game. */
    private void loadStateNow(int slot) {
        final java.io.File src = stateSlotFile(slot);
        final java.io.File dst = new java.io.File(new java.io.File(getFilesDir(), "quick"), questKey() + ".state");
        try {
            if (!src.exists()) { Toast.makeText(this, "Slot " + (slot + 1) + " is empty", Toast.LENGTH_SHORT).show(); return; }
            copyFile(src, dst);
            nativeQuickLoad();
            Toast.makeText(this, "Restored slot " + (slot + 1), Toast.LENGTH_SHORT).show();
        } catch (java.io.IOException io) {
            Toast.makeText(this, "Couldn't load slot " + (slot + 1), Toast.LENGTH_SHORT).show();
        } catch (UnsatisfiedLinkError e) {
            Toast.makeText(this, "Not in a game yet", Toast.LENGTH_SHORT).show();
        }
    }

    private void pickSaveSlot(boolean saving) {
        java.text.SimpleDateFormat fmt = new java.text.SimpleDateFormat("h:mm a", java.util.Locale.US);
        String[] items = new String[3];
        for (int i = 0; i < 3; i++) {
            java.io.File f = stateSlotFile(i);
            boolean has = f.exists() && f.length() > 0;
            String when = has ? " (" + fmt.format(new java.util.Date(f.lastModified())) + ")" : "";
            items[i] = (saving ? "Save to slot " : "Restore slot ") + (i + 1)
                    + (has ? " ✓" : " (empty)") + when;
        }
        new AlertDialog.Builder(this).setTitle(saving ? "Save State" : "Load State")
            .setItems(items, (d, w) -> { d.dismiss(); if (saving) saveStateNow(w); else loadStateNow(w); })
            .show();
    }

    private void toggleGamePad() {
        if (gamePad == null) return;
        // Toggle the PERSISTED preference and apply it directly (the pad is now
        // always visible, even under the open menu, so no modal special-casing).
        boolean newState = !getSharedPreferences("zquestclassic", MODE_PRIVATE)
                .getBoolean("controls-visible", true);
        getSharedPreferences("zquestclassic", MODE_PRIVATE)
                .edit()
                .putBoolean("controls-visible", newState)
                .apply();
        gamePad.setVisibility(newState ? View.VISIBLE : View.GONE);
        // In both orientations the visible cog lives in the center trio (inside the
        // pad), so the standalone gear is the standby: show it only once the pad is
        // hidden, to keep the menu reachable.
        if (cfgBtn != null) cfgBtn.setVisibility(newState ? View.GONE : View.VISIBLE);
    }

    // The library runs in its own process, so the game can shut its own process
    // down when it stops without taking the library with it. Asking the game to
    // stop and letting it finish itself are two separate steps: the native loop
    // has to unwind and shut Allegro down before the Activity may be destroyed.
    private void returnToLibrary() {
        if (returningToLibrary) return;
        returningToLibrary = true;
        releaseStick();
        nativeSetGameMouse(0, 0, false);
        for (View view : new java.util.ArrayList<>(heldControls.keySet())) releaseControl(view);
        releaseMenuKey();
        nativeRequestQuit();
    }

    private void releaseMenuKey() {
        if (menuHeld) {
            menuHeld = false;
            sendKey(KeyEvent.KEYCODE_ESCAPE, KeyEvent.ACTION_UP);
        }
    }

    /** Small pad button that holds a compat-layer scancode while pressed. */
    private TextView makeHeldScancode(String label, int scancode, int size) {
        TextView button = new TextView(this);
        button.setText(label);
        button.setTextColor(Color.WHITE);
        button.setTextSize(label.length() > 1 ? 13 : 18);
        button.setGravity(Gravity.CENTER);
        button.setFocusable(false);
        button.setBackground(background(false));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(size, size);
        params.setMargins(sc(11), sc(11), sc(11), sc(11));
        button.setLayoutParams(params);

        button.setOnTouchListener((view, event) -> {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                view.setBackground(background(true));
                nativeSetGameKey(scancode, true);
                return true;
            }
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                view.setBackground(background(false));
                nativeSetGameKey(scancode, false);
                return true;
            }
            return true;
        });
        return button;
    }

    /** Presses and releases a compat scancode, spaced so the game sees the edge. */
    private void tapScancode(int scancode) {
        nativeSendGameKeyEvent(scancode, true);
        mainHandler.postDelayed(() -> nativeSendGameKeyEvent(scancode, false), 90);
    }

    /** Round gear button that opens the engine menu, styled like the other pad
     *  buttons (My Boy keeps its fast-forward/config in the L-R top row). */
    private TextView makeCogButton(int size) {
        TextView b = new TextView(this);
        b.setText("\u2699");
        b.setTextColor(Color.WHITE);
        b.setTextSize(20);
        b.setGravity(Gravity.CENTER);
        b.setFocusable(false);
        b.setBackground(background(false));
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(size, size);
        p.setMargins(sc(11), sc(11), sc(11), sc(11));
        b.setLayoutParams(p);
        b.setOnTouchListener((view, event) -> {
            int a = event.getActionMasked();
            if (a == MotionEvent.ACTION_DOWN) view.setBackground(background(true));
            if (a == MotionEvent.ACTION_UP) {
                view.setBackground(background(false));
                toggleCog();
            }
            if (a == MotionEvent.ACTION_CANCEL) view.setBackground(background(false));
            return true;
        });
        return b;
    }

    /** Landscape: re-position L/R and the Map/Start/cog trio from the native-rendered
     *  game top/bottom so they land in the top and bottom black bands, dead-centre on
     *  ANY screen (no hardcoded pixel values). Runs after layout like placeShoulderRow. */
    private void placeLandscapeDeck() {
        if (padPortrait || gamePadContainer == null) return;
        int gb = nativeGameBottomPx();
        int gt = nativeGameTopPx();
        if (gb <= 0 || gt <= 0) return;            // not rendered yet; a later retry repositions
        int screenH = getResources().getDisplayMetrics().heightPixels;
        int frameTop = gamePadContainer.getTop();

        // L/R up into the top black band, vertically centred on it.
        int lh = lBtn != null ? lBtn.getHeight() : 0;
        if (lh > 0) {
            int top = Math.max(dp(6), gt / 2 - frameTop - lh / 2);
            FrameLayout.LayoutParams l = (FrameLayout.LayoutParams) lBtn.getLayoutParams();
            l.topMargin = top; lBtn.setLayoutParams(l);
            FrameLayout.LayoutParams r = (FrameLayout.LayoutParams) rBtn.getLayoutParams();
            r.topMargin = top; rBtn.setLayoutParams(r);
        }

        // Map/Start/cog stay where the layout put them (centred bottom in portrait,
        // bottom-right in landscape) — the game fills the screen, so no band math.
    }

    /** Moves the L/cog/R row to sit just below the actual scaled game (reported
     *  natively in pixels), so it lands on the black deck on any screen. */
    private void placeShoulderRow() {
        if (!padPortrait || lBtn == null) return;
        int gb = nativeGameBottomPx();
        if (gb <= 0) return;                       // game not rendered yet; a later retry repositions
        int top = Math.max(0, gb + dp(20) - padTopInset);
        FrameLayout.LayoutParams l = (FrameLayout.LayoutParams) lBtn.getLayoutParams();
        l.topMargin = top; lBtn.setLayoutParams(l);
        // cfgBtn (the gear) is fixed bottom-right, not part of this row.
        FrameLayout.LayoutParams r = (FrameLayout.LayoutParams) rBtn.getLayoutParams();
        r.topMargin = top; rBtn.setLayoutParams(r);
    }

    private TextView makeControl(String label, int keyCode, int size) {
        TextView button = new TextView(this);
        button.setText(label);
        button.setTextColor(Color.WHITE);
        button.setTextSize(label.length() > 1 ? 13 : 22);
        button.setGravity(Gravity.CENTER);
        button.setFocusable(false);
        button.setBackground(background(false));
        button.setOnTouchListener((view, event) -> {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                if (!heldControls.containsKey(view)) {
                    heldControls.put(view, keyCode);
                    // Both paths are needed: the key event drives the main game loop,
                    // while held state is what blocking screens (title, file select)
                    // actually read.
                    sendKey(keyCode, KeyEvent.ACTION_DOWN);
                    int compat = compatScancode(keyCode);
                    if (compat >= 0) nativeSetGameKey(compat, true);
                    view.setBackground(background(true));
                }
                return true;
            }
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL || action == MotionEvent.ACTION_POINTER_UP) {
                mainHandler.postDelayed(() -> releaseControl(view), 250);
                return true;
            }
            return true;
        });

        // Size is supplied by the parent for the d-pad. Face buttons use a
        // comfortably tappable square with a small gap between controls.
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(size, size);
        params.setMargins(sc(11), sc(11), sc(11), sc(11));
        button.setLayoutParams(params);
        return button;
    }

    private GradientDrawable background(boolean pressed) {
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.OVAL);
        shape.setColor(pressed ? Color.argb(190, 50, 120, 210) : Color.argb(120, 20, 24, 32));
        shape.setStroke(dp(1), Color.argb(170, 255, 255, 255));
        return shape;
    }

    private void sendKey(int keyCode, int action) {
        long now = System.currentTimeMillis();
        syntheticKey = true;
        try {
            dispatchKeyEvent(new KeyEvent(now, now, action, keyCode, 0));
        } finally {
            syntheticKey = false;
        }
    }

    /** ABC toggles typing mode: printable keys then go to the engine as characters. */
    private void toggleTextCapture() {
        textCapture = !textCapture;
        Log.i("ZQCTL", "textCapture=" + textCapture);
    }

    /**
     * In typing mode printable keys are sent to the engine as characters. The game
     * surface never accepts an input connection here, so a soft keyboard has nowhere
     * to commit; this is the emulator approach instead.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (textCapture && !syntheticKey && event.getAction() == KeyEvent.ACTION_DOWN) {
            int code = event.getKeyCode();
            if (code == KeyEvent.KEYCODE_DEL) {
                nativeSendGameChar(8);
                return true;
            }
            if (code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_NUMPAD_ENTER) {
                nativeSendGameChar(13);
                return true;
            }
            int ch = event.getUnicodeChar();
            if (ch >= 32 && ch < 127) {
                nativeSendGameChar(ch);
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    /** Android keycode -> the compat layer's scancode for the same key. */
    private int compatScancode(int androidKeyCode) {
        switch (androidKeyCode) {
            case KeyEvent.KEYCODE_Q: return 17;      // L
            case KeyEvent.KEYCODE_W: return 23;      // R
            case KeyEvent.KEYCODE_X: return 24;      // B
            case KeyEvent.KEYCODE_Z: return 26;      // A
            case KeyEvent.KEYCODE_SPACE: return 75;   // Map
            case KeyEvent.KEYCODE_ENTER: return 67;   // Start
            default: return -1;
        }
    }

    private void releaseControl(View view) {
        Integer keyCode = heldControls.remove(view);
        if (keyCode != null) {
            sendKey(keyCode, KeyEvent.ACTION_UP);
            int compat = compatScancode(keyCode);
            if (compat >= 0) nativeSetGameKey(compat, false);
            view.setBackground(background(false));
        }
    }

    /**
     * Floating 8-way thumbstick. Holds the D-pad key codes the engine already
     * consumes, so nothing on the native side changes: an angle inside the
     * deadzone releases everything, and outside it presses one direction, or
     * two when the angle lands in a diagonal window.
     */
    private final class StickView extends View {
        private static final float DEADZONE = 0.16f;      // fraction of travel
        private static final float DIAGONAL = 0.383f;     // sin(22.5 deg)

        private final int[] keyCodes = {
                84, 85, 82, 83};   // Allegro KEY_UP/DOWN/LEFT/RIGHT, per the control scheme
        private final boolean[] held = new boolean[4];

        private final Paint ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint knobPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF bounds = new RectF();

        private float centreX, centreY;   // current origin; moves when floating
        private float knobX, knobY;       // knob offset from the origin
        private int activePointer = -1;

        StickView() {
            super(PlayerActivity.this);
            setFocusable(false);
            setLayerType(LAYER_TYPE_SOFTWARE, null);   // no hardware layer for a 60fps overlay

            ringPaint.setStyle(Paint.Style.STROKE);
            ringPaint.setStrokeWidth(dp(2));
            ringPaint.setColor(Color.argb(150, 255, 255, 255));
            fillPaint.setStyle(Paint.Style.FILL);
            fillPaint.setColor(Color.argb(70, 20, 24, 32));
            knobPaint.setStyle(Paint.Style.FILL);
            knobPaint.setColor(Color.argb(170, 50, 120, 210));
        }

        @Override
        protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            bounds.set(0, 0, w, h);
            centreX = w / 2f;
            centreY = h / 2f;
            knobX = 0;
            knobY = 0;
        }

        private float radius() {
            return Math.min(bounds.width(), bounds.height()) / 2f - dp(8);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float r = radius();
            canvas.drawCircle(centreX, centreY, r, fillPaint);
            canvas.drawCircle(centreX, centreY, r, ringPaint);
            canvas.drawCircle(centreX + knobX, centreY + knobY, r * 0.34f, knobPaint);
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    activePointer = event.getPointerId(0);
                    // Fixed ring: the stick base stays where it is and only the
                    // knob follows the finger, so the whole thing never jumps.
                    update(event.getX(), event.getY());
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    int index = event.findPointerIndex(activePointer);
                    if (index < 0) return true;
                    update(event.getX(index), event.getY(index));
                    return true;
                }
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL: {
                    int index = event.findPointerIndex(activePointer);
                    if (index >= 0 && (event.getActionMasked() == MotionEvent.ACTION_UP
                            || event.getActionMasked() == MotionEvent.ACTION_CANCEL)) {
                        reset();
                    }
                    return true;
                }
                default:
                    return true;
            }
        }

        private void reset() {
            activePointer = -1;
            knobX = 0;
            knobY = 0;
            for (int i = 0; i < held.length; i++) setDirection(i, false);
            centreX = getWidth() / 2f;
            centreY = getHeight() / 2f;
            invalidate();
        }

        private void update(float x, float y) {
            float r = radius();
            float dx = x - centreX;
            float dy = y - centreY;
            float distance = (float) Math.hypot(dx, dy);
            float magnitude = distance / r;

            if (magnitude > 1f) {           // clamp the knob to the rim
                dx = dx / distance * r;
                dy = dy / distance * r;
                magnitude = 1f;
            }
            knobX = dx;
            knobY = dy;

            if (magnitude < DEADZONE || distance == 0f) {
                for (int i = 0; i < held.length; i++) setDirection(i, false);
            } else {
                float ux = dx / distance;
                float uy = dy / distance;
                setDirection(0, uy < -DIAGONAL);   // up
                setDirection(1, uy > DIAGONAL);    // down
                setDirection(2, ux < -DIAGONAL);   // left
                setDirection(3, ux > DIAGONAL);    // right
            }
            invalidate();
        }

        private void setDirection(int index, boolean pressed) {
            if (held[index] == pressed) return;
            held[index] = pressed;
            nativeSetGameKey(keyCodes[index], pressed);
            nativeSendGameKeyEvent(keyCodes[index], pressed);
        }
    }

    /** Releases every direction the stick is holding without waiting for touch-up. */
    private void releaseStick() {
        if (stick != null) stick.reset();
    }

    /** Drops a queued mouse release, so a new press takes effect immediately. */
    private void cancelPendingMouseRelease() {
        if (pendingMouseRelease != null) {
            mainHandler.removeCallbacks(pendingMouseRelease);
            pendingMouseRelease = null;
        }
    }

    /** Raises the soft keyboard so typed characters reach the engine's text fields. */
    private void openKeyboard() {
        if (charInput == null) return;
        charInput.requestFocus();
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        if (imm != null) imm.showSoftInput(charInput, InputMethodManager.SHOW_IMPLICIT);
    }

    /** Lowers the soft keyboard again. */
    private void closeKeyboard() {
        if (charInput == null) return;
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        if (imm != null) imm.hideSoftInputFromWindow(charInput.getWindowToken(), 0);
    }

    /** Converts a design dp to a screen value, scaled by the pad-size setting. */
    private int sc(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density * padScale);
    }

    private GradientDrawable cardBackground(boolean pressed) {
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.RECTANGLE);
        shape.setColor(pressed ? Color.argb(240, 0x28, 0x30, 0x3E) : Color.argb(240, 0x31, 0x39, 0x3A));
        shape.setStroke(dp(2), Color.rgb(0x42, 0x51, 0x52));
        shape.setCornerRadius(dp(4));
        return shape;
    }

    private View makeSettingsRow(String label, Runnable action) {
        return makeSettingsRow(label, action, -1, false);
    }

    /** Section label for the settings sheet (small, gray, uppercase). */
    private View makeSettingsHeader(String title) {
        TextView h = new TextView(this);
        h.setText(title.toUpperCase());
        h.setTextSize(12);
        h.setTypeface(null, android.graphics.Typeface.BOLD);
        h.setTextColor(Color.rgb(0x8A, 0x91, 0x9B));
        h.setGravity(Gravity.CENTER_VERTICAL);
        h.setPadding(dp(20), dp(16), dp(20), dp(8));
        h.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(40)));
        return h;
    }

    private View makeSettingsRow(String label, Runnable action, int widthDp) {
        return makeSettingsRow(label, action, widthDp, false);
    }

    /** drill=true rows open another menu page and show a "›" affordance;
     *  plain rows are terminal actions and stay clean. */
    private View makeSettingsRow(CharSequence label, Runnable action, int widthDp, boolean drill) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setFocusable(false);
        row.setClickable(true);
        row.setPadding(dp(20), 0, drill ? dp(12) : dp(20), 0);
        row.setMinimumHeight(dp(54));

        TextView title = new TextView(this);
        title.setText(label);
        title.setTextColor(Color.WHITE);
        title.setTextSize(16);
        title.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(title, new LinearLayout.LayoutParams(0, dp(54), 1f));

        if (drill) {
            TextView chev = new TextView(this);
            chev.setText("›");
            chev.setTextColor(Color.argb(150, 255, 255, 255));
            chev.setTextSize(26);
            chev.setGravity(Gravity.CENTER);
            row.addView(chev, new LinearLayout.LayoutParams(dp(24), dp(54)));
        }

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                widthDp > 0 ? dp(widthDp) : ViewGroup.LayoutParams.MATCH_PARENT, dp(54));
        row.setLayoutParams(lp);

        row.setOnTouchListener((view, event) -> {
            int a = event.getActionMasked();
            if (a == MotionEvent.ACTION_DOWN) view.setBackground(touchHighlight(true));
            if (a == MotionEvent.ACTION_UP) {
                view.setBackground(touchHighlight(false));
                if (action != null) action.run();
            }
            if (a == MotionEvent.ACTION_CANCEL) view.setBackground(touchHighlight(false));
            return true;
        });
        return row;
    }

    /** Flat row highlight for the settings sheet (no per-row pill/card). */
    private GradientDrawable touchHighlight(boolean pressed) {
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.RECTANGLE);
        shape.setCornerRadius(0);
        shape.setColor(pressed ? Color.argb(42, 255, 255, 255) : Color.argb(0, 0, 0, 0));
        return shape;
    }

    /** Opaque rounded sheet behind the whole settings list. */
    private GradientDrawable panelBackground() {
        GradientDrawable p = new GradientDrawable();
        p.setShape(GradientDrawable.RECTANGLE);
        p.setColor(Color.argb(250, 0x20, 0x26, 0x2F));
        p.setCornerRadius(dp(16));
        return p;
    }

    /** Sheet height, capped at 400dp so it never swallows a landscape screen. */
    private int panelHeightPx() {
        int screenH = getResources().getDisplayMetrics().heightPixels;
        return Math.max(dp(220), Math.min(dp(400), (int) (screenH * 0.75f)));
    }

    private LinearLayout makePadSizeRow() {
        int percent = getSharedPreferences("zquestclassic", MODE_PRIVATE).getInt("pad-scale", 65);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(20), 0, dp(16), 0);
        row.setMinimumHeight(dp(54));
        TextView label = new TextView(this);
        label.setText("Pad size");
        label.setTextColor(Color.WHITE);
        label.setTextSize(16);
        label.setPadding(0, 0, dp(8), 0);
        row.addView(label);
        TextView val = new TextView(this);
        val.setText(percent + "%");
        val.setTextColor(Color.WHITE);
        val.setTextSize(15);
        val.setGravity(Gravity.CENTER);
        val.setMinWidth(dp(48));
        val.setPadding(dp(4), 0, dp(8), 0);
        row.addView(val);
        SeekBar sb = new SeekBar(this);
        sb.setMax(110);                                  // 40%..150%
        sb.setProgress(percent - 40);
        LinearLayout.LayoutParams slp =
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        row.addView(sb, slp);
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean f) {
                val.setText((p + 40) + "%");
            }
            @Override public void onStartTrackingTouch(SeekBar s) { }
            @Override public void onStopTrackingTouch(SeekBar s) {
                int np = s.getProgress() + 40;
                getSharedPreferences("zquestclassic", MODE_PRIVATE)
                    .edit().putInt("pad-scale", np).apply();
                rebuildControls();
            }
        });
        return row;
    }

    private LinearLayout makeVolumeRow(String label, int which) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(20), 0, dp(16), 0);
        row.setMinimumHeight(dp(54));
        TextView lbl = new TextView(this);
        lbl.setText(label);
        lbl.setTextColor(Color.WHITE);
        lbl.setTextSize(16);
        lbl.setPadding(0, 0, dp(8), 0);
        row.addView(lbl);
        TextView val = new TextView(this);
        val.setText(nativeVolumeGet(which) + "");
        val.setTextColor(Color.WHITE);
        val.setTextSize(15);
        val.setGravity(Gravity.CENTER);
        val.setMinWidth(dp(48));
        val.setPadding(dp(4), 0, dp(8), 0);
        row.addView(val);
        SeekBar sb = new SeekBar(this);
        sb.setMax(255);
        sb.setProgress(nativeVolumeGet(which));
        LinearLayout.LayoutParams slp =
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        row.addView(sb, slp);
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean f) {
                val.setText(p + "");
                nativeVolumeSet(which, Math.max(0, Math.min(255, p)));
            }
            @Override public void onStartTrackingTouch(SeekBar s) { }
            @Override public void onStopTrackingTouch(SeekBar s) { }
        });
        return row;
    }

    private void pickPan() {
        String[] opts = {"Mono", "1/2", "3/4", "Full"};
        final int cur = nativePanGet();
        new AlertDialog.Builder(this)
            .setTitle("Pan")
            .setSingleChoiceItems(opts, (cur >= 0 && cur < 4) ? cur : 0, (d, w) -> { nativePanSet(w); d.dismiss(); refreshEnginePage(9); })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void changePadScale(int delta) {
        int cur = getSharedPreferences("zquestclassic", MODE_PRIVATE).getInt("pad-scale", 65);
        int next = Math.max(40, Math.min(150, cur + delta));
        getSharedPreferences("zquestclassic", MODE_PRIVATE).edit().putInt("pad-scale", next).apply();
        rebuildControls();
    }

    private void toggleSettings() {
        if (settingsPanel == null) return;
        boolean anyVisible = settingsPanel.getVisibility() == View.VISIBLE
                || (settingsSubPanel != null && settingsSubPanel.getVisibility() == View.VISIBLE)
                || anyEnginePageVisible();
        settingsPanel.setVisibility(anyVisible ? View.GONE : View.VISIBLE);
        if (settingsSubPanel != null) settingsSubPanel.setVisibility(View.GONE);
        hideEnginePages();
    }

    /** Swaps the gear menu between the main page and the Settings page. */
    private void showSettingsGrid(boolean settings) {
        if (settingsPanel == null || settingsSubPanel == null) return;
        settingsPanel.setVisibility(settings ? View.GONE : View.VISIBLE);
        settingsSubPanel.setVisibility(settings ? View.VISIBLE : View.GONE);
        hideEnginePages();
    }

    /** Swaps the gear menu between the main page and the native engine menu. */
    private void showEngineMenu(boolean show) {
        if (settingsPanel == null) return;
        hideEnginePages();
        settingsPanel.setVisibility(show ? View.GONE : View.VISIBLE);
        if (settingsSubPanel != null) settingsSubPanel.setVisibility(View.GONE);
        if (show) showEnginePage(0);
    }

    private boolean anyEnginePageVisible() {
        for (View v : enginePages) if (v != null && v.getVisibility() == View.VISIBLE) return true;
        return false;
    }

    private void hideEnginePages() {
        for (View v : enginePages) if (v != null) v.setVisibility(View.GONE);
        if (menuScrim != null) menuScrim.setVisibility(View.GONE);
    }

    /** Shows the info of the currently loaded quest in an AlertDialog. */
    private void showQuestInfo() {
        new AlertDialog.Builder(this)
            .setTitle("Quest Info")
            .setMessage(nativeQuestInfo())
            .setNegativeButton("Close", null)
            .show();
    }

    /** Which menu page the "‹ Back" row of page n should return to (its parent). */
    private int backPage(int n) {
        switch (n) {
            case 7: return 5;          // More Options -> Options
            case 8: return 1;          // Load Quest -> Game
            case 9: case 10: return 2; // Sound / Control Schemes -> Settings
            case 11: return 3;         // Configs -> Misc
            case 12: return 7;         // font/scale chooser -> More Options
            case 13: return 1;         // Saves -> Game
            default: return 0;         // Home
        }
    }

    /** Shows one native engine-menu page (0 = top, 1..7 = submenus). */
    private void showEnginePage(int n) {
        hideEnginePages();
        if (settingsPanel != null) settingsPanel.setVisibility(View.GONE);
        if (settingsSubPanel != null) settingsSubPanel.setVisibility(View.GONE);
        if (enginePages.size() <= n || enginePages.get(n) == null) buildEnginePage(n);
        View pv = enginePages.get(n);
        pv.setVisibility(View.VISIBLE);
        centerEnginePage(pv);
        if (menuScrim != null) menuScrim.setVisibility(View.VISIBLE);
    }

    /** Forces the modal sheet to the exact centre of its parent (the CENTER
     *  gravity mis-places it ~80px right on some layouts). */
    private void centerEnginePage(View pv) {
        pv.post(() -> {
            ViewGroup parent = (ViewGroup) pv.getParent();
            if (parent == null) return;
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) pv.getLayoutParams();
            int pl = parent.getPaddingLeft();
            int pt = parent.getPaddingTop();
            lp.gravity = Gravity.NO_GRAVITY;
            lp.leftMargin = Math.max(0, (parent.getWidth()  - pv.getWidth())  / 2 - pl);
            lp.topMargin  = Math.max(0, (parent.getHeight() - pv.getHeight()) / 2 - pt);
            lp.rightMargin = 0;
            lp.bottomMargin = 0;
            pv.setLayoutParams(lp);
        });
    }

    /** A Setting toggle: runs the engine action but keeps the cog menu OPEN so
     *  several can be flipped at once (the user taps the cog/outside to close). */
    private void eRow(LinearLayout l, String label, int id) {
        l.addView(makeSettingsRow(label, () -> nativeMenuAction(id)));
    }

    /**
     * Toggle row showing live ON / OFF and flipping the label IN PLACE — not by
     * rebuilding the page, which hid the sheet for a frame (the "flashes the first
     * screen" glitch). The engine flips settings asynchronously, so the label re-reads
     * its real value a beat after the tap.
     */
    private void addToggleRow(LinearLayout l, String label, int id) {
        final TextView state = new TextView(this);
        final Runnable render = () -> {
            boolean on = nativeGetToggle(id) != 0;
            state.setText(on ? "ON" : "OFF");
            state.setTextColor(on ? Color.rgb(0x3F, 0xC9, 0x6C) : Color.rgb(0x8A, 0x91, 0x9B));
        };
        l.addView(buildToggledRow(label, () -> {
            nativeMenuAction(id);
            mainHandler.postDelayed(render, 120);
        }, state));
        render.run();
    }

    /** Show/Hide Pad row — readout shows whether the on-screen pad is SHOWn or HIDden,
     *  updated in place when toggled. */
    private void addPadToggleRow(LinearLayout l) {
        final TextView state = new TextView(this);
        final Runnable render = () -> {
            boolean shown = getSharedPreferences("zquestclassic", MODE_PRIVATE)
                    .getBoolean("controls-visible", true);
            state.setText(shown ? "HIDE" : "SHOW");
            state.setTextColor(shown ? Color.rgb(0x3F, 0xC9, 0x6C) : Color.rgb(0x8A, 0x91, 0x9B));
        };
        l.addView(buildToggledRow("Show/Hide Pad", () -> {
            toggleGamePad();
            render.run();   // the pref cache updates synchronously
        }, state));
        render.run();
    }

    /** A settings row with a right-aligned ON/OFF-style state readout. */
    private LinearLayout buildToggledRow(String label, Runnable onTap, TextView state) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setFocusable(false);
        row.setClickable(true);
        row.setPadding(dp(20), 0, dp(20), 0);
        row.setMinimumHeight(dp(54));
        TextView title = new TextView(this);
        title.setText(label);
        title.setTextColor(Color.WHITE);
        title.setTextSize(16);
        title.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(title, new LinearLayout.LayoutParams(0, dp(54), 1f));
        state.setTextSize(14);
        state.setTypeface(null, android.graphics.Typeface.BOLD);
        state.setGravity(Gravity.CENTER);
        state.setPadding(0, 0, dp(8), 0);
        row.addView(state, new LinearLayout.LayoutParams(dp(64), dp(54)));
        row.setOnClickListener(v -> onTap.run());
        row.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(54)));
        return row;
    }

    /** More-Options row: shows the current font/scale value and opens the in-menu
     *  chooser (page 12) instead of a popup dialog. */
    private void addValueRow(LinearLayout l, String label, int slot) {
        final TextView state = new TextView(this);
        final Runnable render = () -> {
            state.setText(fontSlotValue(slot, nativeGetFontPref(slot)));
            state.setTextColor(Color.WHITE);
        };
        l.addView(buildToggledRow(label, () -> {
            fontPickSlot = slot;
            showEnginePage(12);
        }, state));
        render.run();
    }

    /** Current label for a More-Options slot: the font name, or "xN" for a scale. */
    private String fontSlotValue(int slot, int val) {
        if (slot % 2 == 1) return "x" + val;                       // scale slot
        int cnt = nativeFontCount();
        if (val >= 0 && val < cnt) {
            String n = nativeFontNameAt(val);
            if (n != null && !n.isEmpty()) return n;
        }
        return "default";
    }

    /** Small bottom toast, e.g. to confirm a Screenshot/Quick-Save landed (the engine
     *  itself pops its own error dialog if the operation actually failed). */
    private void showToast(String msg) {
        Toast t = Toast.makeText(this, msg, Toast.LENGTH_SHORT);
        t.setGravity(Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL, 0, dp(90));
        t.show();
    }

    // ---- Config slots: three named snapshots of the live config (zc.cfg). One slot is
    // ---- "active" (the config currently loaded); loading a slot switches active to it
    // ---- and the page relabels it "Active". Restore Defaults on the active slot resets
    // ---- the current config to factory defaults; on a saved slot it clears that snapshot. ----
    private java.io.File cfgFile() { return new java.io.File(getFilesDir(), "zc.cfg"); }
    private static final int NUM_CONFIG_SLOTS = 3;
    private int mActiveSlot = -1;

    private void copyFile(java.io.File from, java.io.File to) throws java.io.IOException {
        try (java.io.InputStream in = new java.io.FileInputStream(from);
             java.io.OutputStream out = new java.io.FileOutputStream(to)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    private java.io.File configsDir() {
        java.io.File d = new java.io.File(getFilesDir(), "configs");
        if (!d.exists()) d.mkdirs();
        return d;
    }
    private java.io.File slotFile(int slot) { return new java.io.File(configsDir(), "slot" + (slot + 1) + ".cfg"); }
    private java.io.File namesFile() { return new java.io.File(configsDir(), "names.txt"); }
    private java.io.File activeFile() { return new java.io.File(configsDir(), "active.txt"); }

    private int activeSlot() {
        if (mActiveSlot < 0) {
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(activeFile()))) {
                mActiveSlot = Integer.parseInt(r.readLine().trim());
            } catch (Exception e) { mActiveSlot = 0; }
            if (mActiveSlot < 0 || mActiveSlot >= NUM_CONFIG_SLOTS) mActiveSlot = 0;
        }
        return mActiveSlot;
    }
    private void setActiveSlot(int slot) {
        mActiveSlot = slot;
        try (java.io.FileOutputStream o = new java.io.FileOutputStream(activeFile())) {
            o.write((slot + "\n").getBytes("UTF-8"));
        } catch (java.io.IOException ignored) { }
    }

    private String[] readConfigNames() {
        String[] out = { "config1", "config2", "config3" };
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(namesFile()))) {
            String line; int i = 0;
            while ((line = r.readLine()) != null && i < NUM_CONFIG_SLOTS) { out[i] = line.trim(); i++; }
        } catch (java.io.IOException ignored) { }
        return out;
    }
    private void writeConfigNames(String[] names) throws java.io.IOException {
        StringBuilder sb = new StringBuilder();
        for (String n : names) sb.append(n).append('\n');
        try (java.io.FileOutputStream o = new java.io.FileOutputStream(namesFile())) {
            o.write(sb.toString().getBytes("UTF-8"));
        }
    }
    private boolean slotHasContent(int slot) {
        java.io.File f = slotFile(slot);
        return f.exists() && f.length() > 0;
    }

    private void saveToSlot(int slot) {
        nativeConfigOp(0);   // flush the current settings into zc.cfg first
        try {
            copyFile(cfgFile(), slotFile(slot));
            showToast("Saved \"" + readConfigNames()[slot] + "\"");
        } catch (java.io.IOException e) {
            showToast("Couldn't save config");
        }
    }
    private void loadFromSlot(int slot) {
        if (!slotHasContent(slot)) { showToast("Slot empty"); return; }
        try {
            copyFile(slotFile(slot), cfgFile());
            nativeConfigOp(1);   // reload the settings from the slot, live
            setActiveSlot(slot); // this slot is now the active config
            showToast("Loaded \"" + readConfigNames()[slot] + "\"");
        } catch (java.io.IOException e) {
            showToast("Couldn't load config");
        }
    }
    private void renameSlot(int slot, String newName) {
        if (newName == null || newName.isEmpty()) return;
        String[] names = readConfigNames();
        names[slot] = newName;
        try {
            writeConfigNames(names);
            showToast("Renamed to " + newName);
        } catch (java.io.IOException e) {
            showToast("Couldn't rename");
        }
    }
    private void resetSlot(int slot) {
        java.io.File f = slotFile(slot);
        if (f.exists()) f.delete();   // clear this saved snapshot back to empty
        refreshConfigs();
        showToast("Restored \"" + readConfigNames()[slot] + "\" to defaults");
    }
    private void configAction(int slot) {
        String base = readConfigNames()[slot];
        boolean isActive = (slot == activeSlot());
        new AlertDialog.Builder(this)
            .setTitle((isActive ? "Active \u2014 " : "") + base)
            .setItems(new String[] {
                "Save to \"" + base + "\"",
                "Load \"" + base + "\"",
                "Rename \"" + base + "\"",
                "Restore Defaults",
                "Cancel"
            }, (d, w) -> {
                if (w == 0) { saveToSlot(slot); refreshConfigs(); }
                else if (w == 1) { loadFromSlot(slot); refreshConfigs(); }
                else if (w == 2) renameDialog(slot);
                else if (w == 3) { if (isActive) resetConfig(); else resetSlot(slot); }
            })
            .show();
    }
    private void renameDialog(int slot) {
        EditText et = new EditText(this);
        et.setText(readConfigNames()[slot]);
        new AlertDialog.Builder(this)
            .setTitle("Rename config")
            .setView(et)
            .setPositiveButton("OK", (d, w) -> {
                renameSlot(slot, et.getText().toString().trim());
                refreshConfigs();
            })
            .setNegativeButton("Cancel", null)
            .show();
    }
    private void refreshConfigs() {
        if (enginePages != null && enginePages.size() > 11) {
            View old = enginePages.get(11);
            enginePages.set(11, null);
            if (old != null && menuOverlay != null && old.getParent() == menuOverlay)
                menuOverlay.removeView(old);   // drop the stale page so the rebuild adds only one
        }
        showEnginePage(11);
    }

    private void resetConfig() {
        try {
            writeConfig("");   // empty config file => the engine falls back to its built-in defaults
            nativeConfigOp(2);
            showToast("Defaults restored");
        } catch (java.io.IOException e) {
            showToast("Couldn't reset config");
        }
    }

    private void writeConfig(String content) throws java.io.IOException {
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(cfgFile())) {
            out.write(content.getBytes("UTF-8"));
        }
    }

    /** Builds a scrollable cog page mirroring a desktop ESC submenu. */
    private View buildEnginePage(int n) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setFocusable(false);
        if (n != 0) l.addView(makeSettingsRow("‹ Back", () -> showEnginePage(backPage(n))));
        switch (n) {
            case 0:
                l.addView(makeSettingsHeader("ZQuest Classic"));
                l.addView(makeSettingsRow("Home", () -> { dismissCogMenu(); returnToLibrary(); }));
                l.addView(makeSettingsRow("Game", () -> showEnginePage(1), -1, true));
                l.addView(makeSettingsRow("Settings", () -> showEnginePage(2), -1, true));
                l.addView(makeSettingsRow("Misc", () -> showEnginePage(3), -1, true));
                l.addView(makeSettingsRow("Cheats", () -> showEnginePage(4), -1, true));
                l.addView(makeSettingsRow("Options", () -> showEnginePage(5), -1, true));
                break;
            case 1:
                l.addView(makeSettingsHeader("Game"));
                l.addView(makeSettingsRow("Saves", () -> showEnginePage(13), -1, true));
                l.addView(makeSettingsRow("Load Quest", () -> showEnginePage(8), -1, true));
                confirmRow(l, "End Game", "End Game", "End the current game?", 2);
                confirmRow(l, "Reset", "Reset", "Reset the game system?", 3);
                confirmRow(l, "Quit", "Quit", "Quit ZQuest Classic?", 4);
                break;
            case 13:
                // All save/load actions live under a single "Saves" row to keep Game clean.
                l.addView(makeSettingsHeader("Saves"));
                l.addView(makeSettingsRow("Save Game", () -> { saveGameNow(); dismissCogMenu(); }));
                l.addView(makeSettingsRow("Load Game", () -> { loadGameNow(); dismissCogMenu(); }));
                l.addView(makeSettingsRow("Save State", () -> { pickSaveSlot(true); }));
                l.addView(makeSettingsRow("Load State", () -> { pickSaveSlot(false); }));
                l.addView(makeSettingsRow("Quick Save", () -> { quickSaveNow(); dismissCogMenu(); }));
                l.addView(makeSettingsRow("Quick Load", () -> { quickLoadNow(); dismissCogMenu(); }));
                break;
            case 2:
                l.addView(makeSettingsHeader("Settings"));
                l.addView(makeSettingsRow("Control Schemes", () -> showEnginePage(10), -1, true)); l.addView(makeSettingsRow("Sound", () -> showEnginePage(9), -1, true));                addToggleRow(l, "Show FPS", 9);
                addToggleRow(l, "Cap FPS", 10); addToggleRow(l, "Show Time", 11); addToggleRow(l, "Click to Freeze", 17);
                addToggleRow(l, "Heart Beep", 12); addToggleRow(l, "Trans. Layers", 13); addToggleRow(l, "Up+A+B Quit", 14);
                addToggleRow(l, "Volume Keys", 15); addToggleRow(l, "Save Indicator", 16);
                addPadToggleRow(l);
                l.addView(makePadSizeRow());
                break;
            case 3:
                l.addView(makeSettingsHeader("Misc"));
                eRow(l, "Fullscreen", 5);
                l.addView(makeSettingsRow("Snapshot", () -> {
                    nativeMenuAction(6);
                    showToast("Snapshot taken");
                }));
                l.addView(makeSettingsRow("Configs", () -> showEnginePage(11), -1, true));
                l.addView(makeSettingsRow("Quest Info", () -> showQuestInfo())); addToggleRow(l, "Stretch Game Area", 25);
                addToggleRow(l, "Lock Aspect Ratio", 26); addToggleRow(l, "Lock Integer Scale", 27);
                break;
            case 4:
                l.addView(makeSettingsHeader("Cheats"));
                addToggleRow(l, "Invincible", 18);
                addToggleRow(l, "Walk Thru Walls", 19);
                eRow(l, "Kill All Enemies", 20);
                eRow(l, "Refill Life", 21); eRow(l, "Refill Magic", 22);
                break;
            case 5:
                l.addView(makeSettingsHeader("Options"));
                addToggleRow(l, "Epilepsy Reduction", 28);
                addToggleRow(l, "Pause In Background", 29);
                l.addView(makeSettingsRow("More Options", () -> showEnginePage(7), -1, true));
                break;
            case 7:
                l.addView(makeSettingsHeader("More Options"));
                addValueRow(l, "Dialog Font", 0);
                addValueRow(l, "Dialog Scale", 1);
                addValueRow(l, "GUI Font", 2);
                addValueRow(l, "GUI Scale", 3);
                addValueRow(l, "Title Font", 4);
                addValueRow(l, "Title Scale", 5);
                break;
            case 12:
                l.addView(makeSettingsHeader("More Options"));
                {
                    int slot = fontPickSlot;
                    boolean scale = slot % 2 == 1;
                    l.addView(makeSettingsHeader(scale ? "Scale" : "Font"));
                    int cur = nativeGetFontPref(slot);
                    if (scale) {
                        for (int v = 1; v <= 3; v++) {
                            final int fv = v;
                            l.addView(makeSettingsRow(("x" + v) + (cur == v ? "   ✓" : ""),
                                () -> { nativeSetFontPref(slot, fv); refreshEnginePage(7); }));
                        }
                    } else {
                        int cnt = nativeFontCount();
                        for (int i = 0; i < cnt; i++) {
                            String fname = nativeFontNameAt(i);
                            if (fname == null || fname.isEmpty()) continue;
                            final int fi = i;
                            final boolean current = (cur == i);
                            l.addView(makeSettingsRow(fname + (current ? "   ✓" : ""),
                                () -> { nativeSetFontPref(slot, fi); refreshEnginePage(7); }));
                        }
                    }
                }
                break;
            case 9:
                l.addView(makeSettingsHeader("Sound"));
                l.addView(makeVolumeRow("Music", 0));
                l.addView(makeVolumeRow("SFX", 1));
                l.addView(makeVolumeRow("Ambience", 2));
                l.addView(makeSettingsRow("Pan", this::pickPan));
                break;
            case 8:
                l.addView(makeSettingsHeader("Load Quest"));
                for (java.io.File q : discoverQuests()) {
                    java.io.File fq = q;
                    l.addView(makeSettingsRow(stripQstSuffix(q.getName()), () -> switchToQuest(fq)));
                }
                if (discoverQuests().isEmpty())
                    l.addView(makeSettingsRow("No quests on this device yet", () -> {}));
                break;
            case 10:
                l.addView(makeSettingsHeader("Control Schemes"));
                int[] cbtns = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
                String[] clabels = { "Up", "Down", "Left", "Right", "A", "B",
                        "Start", "L", "R", "Map", "Ex1", "Ex2", "Ex3", "Ex4" };
                for (int i = 0; i < cbtns.length; i++)
                    l.addView(remapRow(cbtns[i], clabels[i]));
                break;
            case 11:
                l.addView(makeSettingsHeader("Configs"));
                String[] cfgNames = readConfigNames();
                int act = activeSlot();
                for (int i = 0; i < NUM_CONFIG_SLOTS; i++) {
                    final int slot = i;
                    boolean isAct = (i == act);
                    String lbl = (isAct ? "Active \u2014 " : "") + cfgNames[slot]
                               + (isAct ? "" : (slotHasContent(slot) ? "" : "  (empty)"));
                    l.addView(makeSettingsRow(lbl, () -> configAction(slot), -1, true));
                }
                break;
        }
        android.widget.ScrollView s = new android.widget.ScrollView(this);
        s.setFocusable(false);
        s.setVisibility(View.GONE);
        s.setBackground(panelBackground());
        s.setPadding(dp(6), dp(6), dp(6), dp(6));
        s.setElevation(0f);
        s.setVerticalScrollBarEnabled(false);
        s.setHorizontalScrollBarEnabled(false);
        s.addView(l);
        View old = (n < enginePages.size()) ? enginePages.get(n) : null;
        while (enginePages.size() <= n) enginePages.add(null);
        enginePages.set(n, s);
        if (menuOverlay != null) {
            // Drop any stale copy of this page first: rebuilds (config saves /
            // renames call refreshConfigs, which clears the entry and re-shows)
            // would otherwise stack duplicate menu views on top of each other.
            if (old != null && old.getParent() == menuOverlay) menuOverlay.removeView(old);
            FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(dp(300), panelHeightPx(), Gravity.CENTER);
            menuOverlay.addView(s, p);
        }
        return s;
    }

    /** One Control Schemes key-binding row: label + currently bound key
     *  name on the right; tapping it opens the key picker. */
    private View remapRow(final int btn, final String label) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setFocusable(false);
        row.setClickable(true);
        row.setPadding(dp(20), 0, dp(12), 0);
        row.setMinimumHeight(dp(54));
        TextView title = new TextView(this);
        title.setText(label);
        title.setTextColor(Color.WHITE);
        title.setTextSize(16);
        title.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(title, new LinearLayout.LayoutParams(0, dp(54), 1f));
        TextView sub = new TextView(this);
        String kn = nativeGetGameKeyName(btn);
        sub.setText(kn == null ? "—" : kn);
        sub.setTextColor(Color.argb(150, 255, 255, 255));
        sub.setTextSize(14);
        sub.setGravity(Gravity.CENTER_VERTICAL);
        row.addView(sub, new LinearLayout.LayoutParams(dp(120), dp(54)));
        row.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(54)));
        row.setOnTouchListener((view, event) -> {
            int a = event.getActionMasked();
            if (a == MotionEvent.ACTION_DOWN) view.setBackground(touchHighlight(true));
            if (a == MotionEvent.ACTION_UP) {
                view.setBackground(touchHighlight(false));
                pickGameKey(btn, label);
            }
            if (a == MotionEvent.ACTION_CANCEL) view.setBackground(touchHighlight(false));
            return true;
        });
        return row;
    }

    /** Centered single-choice picker of curated keys for one bind; on pick,
     *  writes the binding natively and rebuilds the Control Schemes page. */
    private void pickGameKey(final int btn, final String label) {
        String[] names = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K",
                "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
                "Y", "Z", "Up", "Down", "Left", "Right", "Space", "Enter", "Shift" };
        // Alphabetical keycodes (1..26) then allegro's arrows/space/enter/lshift.
        final int[] codes = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 84, 85, 82, 83, 75, 67, 115 };
        new AlertDialog.Builder(this)
            .setTitle(label)
            .setItems(names, (d, w) -> { nativeBindGameKey(btn, codes[w]); refreshEnginePage(10); })
            .setNegativeButton("Cancel", null)
            .show();
    }

    /** Runs an engine menu action on the game thread, then closes the menu. */
    private void engineAction(int id) {
        nativeMenuAction(id);
        dismissCogMenu();
    }

    /** Pad visibility before the cog modal opened, restored on close. */
    private boolean padWasVisible = true;
    /** True while the modal shows, so a paired F3 unfreezes on close. */
    private boolean cogMenuOpen = false;

    // Shoulder-row buttons; repositioned once the native render reports the real
    // game bottom, so they sit on the black below the game on any screen.
    private TextView lBtn, rBtn, cfgBtn;
    private boolean padPortrait;
    private int padTopInset;

    /** Opens the cog as a centered, dimmed modal: pad hidden, game paused. */
    private void openCogMenu() {
        if (cogMenuOpen) return;
        cogMenuOpen = true;
        // The pad stays visible underneath the menu (user preference); only pause.
        showEnginePage(0);
        nativeSetPaused(true);   // deterministic pause (not an F3 toggle)
    }

    /** Closes the modal: unfreezes the game (pad visibility is untouched). */
    private void dismissCogMenu() {
        hideEnginePages();
        if (cogMenuOpen) {
            cogMenuOpen = false;
            nativeSetPaused(false);   // deterministic resume
        }
    }

    /** The cog opens straight into the native engine menu as a full modal. */
    private void toggleCog() {
        if (anyEnginePageVisible()) dismissCogMenu();
        else openCogMenu();
    }

    /** Centered confirm/cancel box; runs the engine action only if confirmed. */
    private void confirmAction(String title, String msg, int id) {
        new AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(msg)
            .setCancelable(false)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Yes", (d, w) -> engineAction(id))
            .show();
    }

    private void confirmRow(LinearLayout l, String label, String title, String msg, int id) {
        l.addView(makeSettingsRow(label, () -> confirmAction(title, msg, id)));
    }

    /** The .qst/quest files on this device, matching what the library shows. */
    private java.util.List<java.io.File> discoverQuests() {
        java.util.List<java.io.File> out = new java.util.ArrayList<>();
        java.util.Set<String> bundled = bundledQuestEntries();
        java.io.File questsDir = new java.io.File(getFilesDir(), "quests");
        java.io.File[] children = questsDir.isDirectory() ? questsDir.listFiles() : null;
        if (children != null) {
            for (java.io.File child : children) {
                if (bundled.contains(child.getName())) continue;
                if (child.isFile() && child.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".qst")) {
                    out.add(child);
                } else if (child.isDirectory()) {
                    java.io.File q = findFirstQst(child);
                    if (q != null) out.add(q);
                }
            }
        }
        java.util.Collections.sort(out, java.util.Comparator.comparing(java.io.File::getName, String.CASE_INSENSITIVE_ORDER));
        return out;
    }

    private java.io.File findFirstQst(java.io.File dir) {
        java.io.File[] files = dir.listFiles();
        if (files == null) return null;
        for (java.io.File f : files)
            if (f.isFile() && f.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".qst")) return f;
        return null;
    }

    /** Top-level entries shipped inside the APK's resources/quests folder. */
    private java.util.Set<String> bundledQuestEntries() {
        java.util.Set<String> names = new java.util.HashSet<>();
        try {
            String[] children = getAssets().list("quests");
            if (children != null) java.util.Collections.addAll(names, children);
        } catch (Exception ignore) { /* same as an install that lost them */ }
        return names;
    }

    private static String stripQstSuffix(String name) {
        return name.replaceFirst("(?i)\\.qst$", "");
    }

    /** Pins the chosen quest for the next boot, then shuts the game down cleanly;
     *  the library (separate process) sees a marker FILE for the auto-relaunch —
     *  SharedPreferences are cached per-process and would not cross the boundary. */
    private void switchToQuest(java.io.File questFile) {
        try {
            java.io.File marker = new java.io.File(getFilesDir(), "selected-quest.txt");
            try (java.io.FileOutputStream output = new java.io.FileOutputStream(marker)) {
                output.write(questFile.getAbsolutePath().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                output.write('\n');
            }
            new java.io.File(getFilesDir(), "pending-quest.txt").createNewFile();
            dismissCogMenu();
            returnToLibrary();
        } catch (Exception e) {
            Toast.makeText(this, "Could not load quest: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    /** Rebuilds an engine-menu page (so values refresh) and shows it. */
    private void refreshEnginePage(int n) {
        // Remember where the page was scrolled so a toggle rebuild doesn't yank the
        // user back to the top of a long settings list.
        android.widget.ScrollView cur = (n < enginePages.size() && enginePages.get(n) instanceof android.widget.ScrollView)
                ? (android.widget.ScrollView) enginePages.get(n) : null;
        final int scrollY = (cur != null) ? cur.getScrollY() : -1;
        hideEnginePages();
        if (enginePages.size() > n) enginePages.set(n, null);
        showEnginePage(n);
        android.widget.ScrollView next = (n < enginePages.size() && enginePages.get(n) instanceof android.widget.ScrollView)
                ? (android.widget.ScrollView) enginePages.get(n) : null;
        if (next != null && scrollY > 0) next.post(() -> next.scrollTo(0, scrollY));
    }

    /** Centered single-choice picker for a More-Options font/scale slot. */
    private void pickFontOption(String label, int slot, boolean scale) {
        java.util.ArrayList<String> names = new java.util.ArrayList<>();
        java.util.ArrayList<Integer> vals = new java.util.ArrayList<>();
        if (scale) {
            for (int v = 1; v <= 3; v++) { names.add("x" + v); vals.add(v); }
        } else {
            int cnt = nativeFontCount();
            for (int i = 0; i < cnt; i++) {
                String n = nativeFontNameAt(i);
                if (n != null && n.length() > 0) { names.add(n); vals.add(i); }
            }
        }
        String[] arr = names.toArray(new String[0]);
        new AlertDialog.Builder(this)
            .setTitle(label)
            .setItems(arr, (d, w) -> { nativeSetFontPref(slot, vals.get(w)); refreshEnginePage(7); })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void rebuildControls() {
        releaseStick();
        nativeSetGameMouse(0, 0, false);
        if (controlsOverlay != null) {
            ViewGroup parent = (ViewGroup) controlsOverlay.getParent();
            if (parent != null) parent.removeView(controlsOverlay);
            controlsOverlay = null;
        }
        addTouchControls();
    }

    /** Left/top/right/bottom screen insets in px (cutout + status/gesture bars). */
    private int[] safeInsets() {
        int[] o = {0, 0, 0, 0};
        View decor = getWindow().getDecorView();
        WindowInsets ins = decor.getRootWindowInsets();
        if (ins == null) return o;
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            Insets bar = ins.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            o[0] = bar.left; o[1] = bar.top; o[2] = bar.right; o[3] = bar.bottom;
            return o;
        }
        if (ins.getDisplayCutout() != null) {
            DisplayCutout cut = ins.getDisplayCutout();
            o[0] = cut.getSafeInsetLeft(); o[1] = cut.getSafeInsetTop();
            o[2] = cut.getSafeInsetRight(); o[3] = cut.getSafeInsetBottom();
        }
        o[0] = Math.max(o[0], ins.getSystemWindowInsetLeft());
        o[1] = Math.max(o[1], ins.getSystemWindowInsetTop());
        o[2] = Math.max(o[2], ins.getSystemWindowInsetRight());
        o[3] = Math.max(o[3], ins.getSystemWindowInsetBottom());
        return o;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    public void onResume() {
        super.onResume();
        enterFullscreen();
    }

    /** Hides the status + gesture bars so the game area is the whole screen. */
    private void enterFullscreen() {
        View dec = getWindow().getDecorView();
        dec.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    @Override
    public void onPause() {
        releaseStick();
        // A dialog spins on gui_mouse_b(); a button left down would hang it.
        nativeSetGameMouse(0, 0, false);
        for (View view : new java.util.ArrayList<>(heldControls.keySet())) releaseControl(view);
        
        // Release menu key if held
        if (menuHeld) {
            menuHeld = false;
            sendKey(KeyEvent.KEYCODE_ESCAPE, KeyEvent.ACTION_UP);
        }
        
        super.onPause();
    }
}