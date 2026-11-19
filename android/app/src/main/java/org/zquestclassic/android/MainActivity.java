package org.zquestclassic.android;

import android.app.AlertDialog;
import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.liballeg.android.PlayerActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Lets the user select a .qst or a .zip containing a quest. */
public final class MainActivity extends Activity {
    private static final int REQUEST_QUEST = 1;
    private static final int REQUEST_FOLDER = 2;
    private static final int REQUEST_EXPORT = 3;
    private static final int REQUEST_IMPORT = 4;
    private static final long MAX_QUEST_BYTES = 1024L * 1024L * 1024L;

    private final ExecutorService importer = Executors.newSingleThreadExecutor();
    private Button chooseButton;
    private View welcomeOverlay;
    private ProgressBar progress;
    private TextView status;

    private LinearLayout content;
    private LinearLayout deviceList;
    private TextView folderHeader;
    private LinearLayout folderList;
    private Uri folderUri;
    private String currentDocumentId;
    // Document ids of folders we descended into; a null entry means the tree root.
    // ArrayList rather than ArrayDeque because null is a valid entry here.
    private final java.util.ArrayList<String> folderHistory = new java.util.ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        showLibraryScreen();

        String storedUri = getSharedPreferences("zquestclassic", MODE_PRIVATE).getString("folder-uri", null);
        if (storedUri != null) {
            try {
                folderUri = Uri.parse(storedUri);
            } catch (Exception e) {
                folderUri = null;
            }
        }

        Uri incoming = getIntent() == null ? null : getIntent().getData();
        if (incoming != null) {
            importQuest(incoming);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // A quest-switch from the in-game cog menu wrote selected-quest.txt and a
        // companion marker before the game shut down; boot that quest immediately
        // instead of pausing on the list. Marker is a file because the library and
        // the game run in separate processes (SharedPreferences would be stale).
        if (new java.io.File(getFilesDir(), "pending-quest.txt").exists()) {
            new java.io.File(getFilesDir(), "pending-quest.txt").delete();
            startActivity(new Intent(this, PlayerActivity.class));
            return;
        }
        refreshLibrary();
    }

    private void showLibraryScreen() {
        ScrollView scroll = new ScrollView(this);
        // Android 15+ (targetSdk 35) draws edge-to-edge by default, which would
        // put the header under the notification bar. Push content below it here.
        scroll.setOnApplyWindowInsetsListener((v, insets) -> {
            int top = insets.getSystemWindowInsetTop();
            scroll.setPadding(0, top, 0, 0);
            return insets;
        });
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = dp(28);
        content.setPadding(padding, padding, padding, padding);
        scroll.addView(content, new ScrollView.LayoutParams(-1, -2));

        TextView title = new TextView(this);
        title.setText("ZQuest Classic Mobile");
        title.setTextSize(30);
        title.setTextColor(getColor(R.color.zc_cyan));
        title.setTypeface(android.graphics.Typeface.MONOSPACE, android.graphics.Typeface.BOLD);
        title.setLetterSpacing(0.08f);
        title.setGravity(Gravity.CENTER);
        content.addView(title, new LinearLayout.LayoutParams(-1, -2));

        TextView deviceHeader = new TextView(this);
        deviceHeader.setText("QUESTS");
        deviceHeader.setTextSize(16);
        deviceHeader.setTextColor(getColor(R.color.zc_gold));
        deviceHeader.setTypeface(android.graphics.Typeface.MONOSPACE, android.graphics.Typeface.BOLD);
        deviceHeader.setLetterSpacing(0.12f);
        LinearLayout.LayoutParams deviceHeaderParams = new LinearLayout.LayoutParams(-1, -2);
        deviceHeaderParams.topMargin = dp(24);
        content.addView(deviceHeader, deviceHeaderParams);

        deviceList = new LinearLayout(this);
        deviceList.setOrientation(LinearLayout.VERTICAL);
        content.addView(deviceList, new LinearLayout.LayoutParams(-1, -2));

        folderHeader = new TextView(this);
        folderHeader.setTextSize(16);
        folderHeader.setTextColor(getColor(R.color.zc_gold));
        folderHeader.setTypeface(android.graphics.Typeface.MONOSPACE, android.graphics.Typeface.BOLD);
        folderHeader.setLetterSpacing(0.12f);
        folderHeader.setVisibility(View.GONE);   // folder pane removed from the simplified library
        LinearLayout.LayoutParams folderHeaderParams = new LinearLayout.LayoutParams(-1, -2);
        folderHeaderParams.topMargin = dp(24);
        content.addView(folderHeader, folderHeaderParams);

        folderList = new LinearLayout(this);
        folderList.setOrientation(LinearLayout.VERTICAL);
        folderList.setVisibility(View.GONE);   // folder pane removed
        content.addView(folderList, new LinearLayout.LayoutParams(-1, -2));

        // "Import file" is intentionally not shown; the field stays assigned so the
        // external open-with import path (importQuest) can keep toggling it safely.
        chooseButton = new Button(this);
        chooseButton.setText("Import file");

        // ZC's own title screen is where save slots, save names and per-quest saves
        // live; the library otherwise boots straight into one pinned quest. A marker
        // file of "-" tells the engine not to pin a quest, so it shows the title.
        Button titleButton = new Button(this);
        titleButton.setText("Title screen");
        styleZcButton(titleButton);
        titleButton.setOnClickListener(view -> {
            try {
                File marker = new File(getFilesDir(), "selected-quest.txt");
                try (FileOutputStream output = new FileOutputStream(marker)) {
                    output.write("-\n".getBytes(StandardCharsets.UTF_8));
                }
                startActivity(new Intent(this, PlayerActivity.class));
            } catch (Exception e) {
                Toast.makeText(this, "Could not open title screen: " + e.getMessage(), Toast.LENGTH_LONG).show();
            }
        });

        // The library's three primary actions in one even row: load a save state,
        // ZQuest's title/save-slot screen, or the remaining library tools.
        Button loadGameButton = new Button(this);
        loadGameButton.setText("Load game");
        styleZcButton(loadGameButton);
        loadGameButton.setOnClickListener(view -> showLoadGame());

        Button optionsButton = new Button(this);
        optionsButton.setText("Options");
        styleZcButton(optionsButton);
        optionsButton.setOnClickListener(view -> showLibraryOptions());

        addButtonRow(loadGameButton, titleButton, optionsButton);

        progress = new ProgressBar(this);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressParams = new LinearLayout.LayoutParams(-2, -2);
        progressParams.topMargin = dp(12);
        content.addView(progress, progressParams);

        status = new TextView(this);
        status.setGravity(Gravity.CENTER);
        status.setTextColor(getColor(R.color.zc_text_dim));
        status.setTypeface(android.graphics.Typeface.MONOSPACE);
        status.setTextSize(13);
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(-1, -2);
        statusParams.topMargin = dp(12);
        content.addView(status, statusParams);

        // Full-screen layout: the scrollable library below, and a first-run
        // welcome overlay drawn on top until at least one quest is added.
        FrameLayout root = new FrameLayout(this);
        root.addView(scroll, new FrameLayout.LayoutParams(-1, -1));
        welcomeOverlay = buildWelcome();
        root.addView(welcomeOverlay, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);

        // If the user already picked a quest folder, auto-import any quests that
        // were added to it since the last time ("drop a game in, play it").
        importNewQuestsFromSavedFolder();
    }

    private void refreshLibrary() {
        if (content == null) return;

        deviceList.removeAllViews();

        // Quests already copied into app storage. A directly imported .qst sits
        // in quests/ as a file; an imported .zip is extracted into a directory
        // next to it, so both shapes have to be handled.
        File questsDir = new File(getFilesDir(), "quests");
        File[] children = questsDir.isDirectory() ? questsDir.listFiles() : null;
        if (children != null) {
            // The packaged resources are copied into files/ as well, and they
            // include example stubs that are not playable quests. Only offer
            // what was actually imported.
            java.util.Set<String> bundled = bundledQuestEntries();
            java.util.List<File> rows = new java.util.ArrayList<>();
            for (File child : children) {
                if (bundled.contains(child.getName())) continue;
                if (child.isFile() && child.getName().toLowerCase(Locale.ROOT).endsWith(".qst")) {
                    rows.add(child);
                } else if (child.isDirectory()) {
                    File qstFile = findFirstQst(child);
                    if (qstFile != null) rows.add(qstFile);
                }
            }
            java.util.Collections.sort(rows, java.util.Comparator.comparing(File::getName, String.CASE_INSENSITIVE_ORDER));
            for (File qstFile : rows) {
                String subtitle = qstFile.getParentFile() != null && !qstFile.getParentFile().equals(questsDir)
                        ? qstFile.getParentFile().getName()
                        : "Imported file";
                addQuestRow(deviceList, stripQstSuffix(qstFile.getName()), subtitle, qstFile);
            }
        }

        // First-run / empty library: show the full-screen welcome so adding quests
        // is the obvious first step. Once quests exist, hide it.
        if (deviceList.getChildCount() == 0) showWelcome(); else hideWelcome();

        // (The old "browse a quest folder" pane was removed; the library now shows
        // only the imported quests under QUESTS, plus the action buttons.)
    }

    private void showWelcome() { if (welcomeOverlay != null) welcomeOverlay.setVisibility(View.VISIBLE); }
    private void hideWelcome() { if (welcomeOverlay != null) welcomeOverlay.setVisibility(View.GONE); }

    /** Full-screen first-run panel: explains adding quests, hero action = Select folder. */
    private View buildWelcome() {
        LinearLayout w = new LinearLayout(this);
        w.setOrientation(LinearLayout.VERTICAL);
        w.setGravity(Gravity.CENTER);
        w.setBackgroundColor(getColor(R.color.zc_background));
        int pad = dp(28);
        w.setPadding(pad, pad, pad, pad);

        TextView app = new TextView(this);
        app.setText("ZQuest Classic Mobile");
        app.setTextSize(16);
        app.setGravity(Gravity.CENTER);
        app.setTextColor(getColor(R.color.zc_gold));
        app.setTypeface(android.graphics.Typeface.MONOSPACE, android.graphics.Typeface.BOLD);
        app.setLetterSpacing(0.1f);
        w.addView(app, new LinearLayout.LayoutParams(-1, -2));

        TextView heading = new TextView(this);
        heading.setText("Welcome!\nLet's add your quests so you can play.");
        heading.setTextSize(24);
        heading.setGravity(Gravity.CENTER);
        heading.setTextColor(getColor(R.color.zc_cyan));
        heading.setTypeface(android.graphics.Typeface.MONOSPACE, android.graphics.Typeface.BOLD);
        LinearLayout.LayoutParams headingP = new LinearLayout.LayoutParams(-1, -2);
        headingP.topMargin = dp(24);
        w.addView(heading, headingP);

        TextView sub = new TextView(this);
        sub.setText("Quest files are .qst (or a .zip containing one).\nPick the folder where you keep them and they'll be added to your library.");
        sub.setTextSize(15);
        sub.setGravity(Gravity.CENTER);
        sub.setTextColor(getColor(R.color.zc_text));
        sub.setTypeface(android.graphics.Typeface.MONOSPACE);
        LinearLayout.LayoutParams subP = new LinearLayout.LayoutParams(-1, -2);
        subP.topMargin = dp(16);
        w.addView(sub, subP);

        Button folderBtn = new Button(this);
        folderBtn.setText("Select folder");
        styleZcButton(folderBtn);
        folderBtn.setOnClickListener(view -> openFolderPicker());
        LinearLayout.LayoutParams folderP = new LinearLayout.LayoutParams(-1, -2);
        folderP.topMargin = dp(28);
        w.addView(folderBtn, folderP);

        Button singleBtn = new Button(this);
        singleBtn.setText("Pick quests from a folder…");
        styleZcButton(singleBtn);
        singleBtn.setOnClickListener(view -> openDocumentPicker());
        LinearLayout.LayoutParams singleP = new LinearLayout.LayoutParams(-1, -2);
        singleP.topMargin = dp(10);
        w.addView(singleBtn, singleP);

        TextView later = new TextView(this);
        later.setText("I'll add my quests later");
        later.setTextSize(14);
        later.setGravity(Gravity.CENTER);
        later.setTextColor(getColor(R.color.zc_text_dim));
        later.setPadding(dp(12), dp(12), dp(12), dp(12));
        later.setOnClickListener(view -> hideWelcome());
        LinearLayout.LayoutParams laterP = new LinearLayout.LayoutParams(-1, -2);
        laterP.topMargin = dp(20);
        w.addView(later, laterP);

        return w;
    }

    /** Top-level entry names that ship inside the APK's resources/quests folder. */
    private java.util.Set<String> bundledQuestEntries() {
        java.util.Set<String> names = new java.util.HashSet<>();
        try {
            String[] children = getAssets().list("quests");
            if (children != null) Collections.addAll(names, children);
        } catch (Exception noAssets) {
            // Without the asset list nothing is filtered out; the same behaviour
            // as an install that somehow lost them.
        }
        return names;
    }

    private static String stripQstSuffix(String name) {
        return name.replaceFirst("(?i)\\.qst$", "");
    }

    private String folderDisplayName() {
        String id = folderUri == null ? null : DocumentsContract.getTreeDocumentId(folderUri);
        if (id != null) {
            String tail = id.contains("/") ? id.substring(id.lastIndexOf('/') + 1) : id;
            if (!tail.isEmpty()) return tail;
        }
        return "your quest folder";
    }

    private File findFirstQst(File dir) {
        File[] files = dir.listFiles();
        if (files == null) return null;
        for (File file : files) {
            if (file.isFile() && file.getName().toLowerCase(Locale.ROOT).endsWith(".qst")) {
                return file;
            }
        }
        return null;
    }

    private void addQuestRow(LinearLayout parent, String title, String subtitle, final File questFile) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(dp(12), dp(12), dp(12), dp(12));
        row.setBackgroundResource(R.drawable.zc_row);
        row.setOnClickListener(view -> {
            importer.execute(() -> {
                try {
                    File marker = new File(getFilesDir(), "selected-quest.txt");
                    try (FileOutputStream output = new FileOutputStream(marker)) {
                        output.write(questFile.getAbsolutePath().getBytes(StandardCharsets.UTF_8));
                        output.write('\n');
                    }
                    runOnUiThread(() -> {
                        startActivity(new Intent(this, PlayerActivity.class));
                    });
                } catch (Exception e) {
                    runOnUiThread(() -> {
                        Toast.makeText(this, "Could not start quest: " + e.getMessage(), Toast.LENGTH_LONG).show();
                    });
                }
            });
        });

        TextView titleView = new TextView(this);
        titleView.setText(title);
        titleView.setTextSize(16);
        titleView.setTextColor(getColor(R.color.zc_text));
        row.addView(titleView, new LinearLayout.LayoutParams(-1, -2));

        TextView subtitleView = new TextView(this);
        subtitleView.setText(subtitle);
        subtitleView.setTextSize(12);
        subtitleView.setTextColor(getColor(R.color.zc_text_dim));
        row.addView(subtitleView, new LinearLayout.LayoutParams(-1, -2));

        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(-1, -2);
        rowParams.bottomMargin = dp(6);
        parent.addView(row, rowParams);
    }

    private void openFolderPicker() {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        // NO EXTRA_INITIAL_URI: providing one makes DocumentsUI on this line show the
        // target folder as "Can't use this folder" + empty (a blocked grant state),
        // whether we point at top-level Download or at the quests subfolder. Let the
        // stock browser open at its root list so the user can navigate to Download /
        // Quests and select it normally (Android then allows the subfolder grant).
        startActivityForResult(picker, REQUEST_FOLDER);
    }

    // ---- Save-state export / import via the SAE tree ----
    // Copies the real ZC .sav files (files/saves) and the per-quest save states
    // (files/savestates/<quest>/slotN.sav) into a user-picked folder, and back.

    private static final String DCMIME_DIR = DocumentsContract.Document.MIME_TYPE_DIR;

    private void openSaveTreePicker(int req) {
        // Same rationale as openFolderPicker: no target uri, let the picker open at
        // its root so the user can choose a flash drive / Downloads folder cleanly.
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), req);
    }

    // ---- Library "Load game": pick a save state, boot straight into it. ----
    // Scoped strictly per quest: each slot is stored under its quest's key, so
    // choosing a slot always loads into THAT quest (never into whatever is
    // currently pinned).

    private void showLibraryOptions() {
        String[] items = { "Add quest", "Select folder", "Export saves", "Import saves" };
        new AlertDialog.Builder(this).setTitle("Options")
            .setItems(items, (d, w) -> {
                d.dismiss();
                if (w == 0) openDocumentPicker();
                else if (w == 1) openFolderPicker();
                else if (w == 2) openSaveTreePicker(REQUEST_EXPORT);
                else if (w == 3) openSaveTreePicker(REQUEST_IMPORT);
            })
            .show();
    }

    private void showLoadGame() {
        java.io.File statesRoot = new java.io.File(getFilesDir(), "savestates");
        java.io.File[] questDirs = statesRoot.listFiles(File::isDirectory);
        List<String> labels = new ArrayList<>();
        List<String> keys = new ArrayList<>();
        List<File> files = new ArrayList<>();
        java.text.SimpleDateFormat fmt = new java.text.SimpleDateFormat("h:mm a", java.util.Locale.US);
        if (questDirs != null) for (java.io.File qd : questDirs) {
            File[] slots = qd.listFiles((d, n) -> n.endsWith(".sav"));
            if (slots == null) continue;
            for (File s : slots) {
                keys.add(qd.getName());
                files.add(s);
                String slotNo = s.getName().replaceFirst("slot", "").replaceFirst("\\.sav$", "");
                labels.add(qd.getName() + " — slot " + slotNo
                        + " (" + fmt.format(new java.util.Date(s.lastModified())) + ")");
            }
        }
        if (labels.isEmpty()) {
            Toast.makeText(this, "No save states yet — make one in-game (Saves › Save State).", Toast.LENGTH_LONG).show();
            return;
        }
        String[] items = labels.toArray(new String[0]);
        new AlertDialog.Builder(this).setTitle("Load game")
            .setItems(items, (d, w) -> launchSaveState(keys.get(w), files.get(w)))
            .show();
    }

    private void launchSaveState(String questKey, File slotFile) {
        try {
            // 1) Stage this slot as the quest's live full-world state (.state) —
            //    the file quick-load actually reads.
            File quickDir = new File(getFilesDir(), "quick");
            if (!quickDir.exists()) quickDir.mkdirs();
            copyStream(slotFile, new File(quickDir, questKey + ".state"));
            // 2) Find and pin the matching quest.
            File quest = findQuestFile(questKey);
            if (quest == null) {
                Toast.makeText(this, "Quest file for \"" + questKey + "\" not found", Toast.LENGTH_LONG).show();
                return;
            }
            File marker = new File(getFilesDir(), "selected-quest.txt");
            try (FileOutputStream o = new FileOutputStream(marker)) {
                o.write(quest.getAbsolutePath().getBytes(StandardCharsets.UTF_8));
            }
            // 3) Engine marker: on boot, load this saved state on the first frame.
            new File(getFilesDir(), "pending-quickload.txt").createNewFile();
            startActivity(new Intent(this, PlayerActivity.class));
        } catch (Exception e) {
            Toast.makeText(this, "Couldn't load: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void copyStream(File from, File to) throws java.io.IOException {
        try (FileInputStream in = new FileInputStream(from); FileOutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[8192]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    /** Finds the .qst whose filename (minus .qst) equals key, anywhere under files/quests. */
    private File findQuestFile(String key) {
        java.util.ArrayDeque<File> stack = new java.util.ArrayDeque<>();
        stack.push(new File(getFilesDir(), "quests"));
        while (!stack.isEmpty()) {
            File d = stack.pop();
            File[] kids = d.listFiles();
            if (kids == null) continue;
            for (File k : kids) {
                if (k.isDirectory()) stack.push(k);
                else if (k.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".qst")) {
                    String base = k.getName();
                    base = base.substring(0, base.length() - 4);
                    if (base.equals(key)) return k;
                }
            }
        }
        return null;
    }

    /** One child of <dirId> in <tree>; {documentId, displayName, mime}. */
    private List<String[]> treeChildren(ContentResolver cr, Uri tree, String dirId) {
        List<String[]> out = new ArrayList<>();
        Uri q = DocumentsContract.buildChildDocumentsUriUsingTree(tree, dirId);
        try (Cursor c = cr.query(q, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (c != null && c.moveToNext()) {
                out.add(new String[]{ c.getString(0), c.getString(1), c.getString(2) });
            }
        } catch (Exception ignored) { }
        return out;
    }

    /** Returns the document Uri of the named directory inside <dirId>, or null. */
    private Uri findTreeDir(ContentResolver cr, Uri tree, String dirId, String name) {
        for (String[] child : treeChildren(cr, tree, dirId)) {
            if (name.equals(child[1]) && DCMIME_DIR.equals(child[2]))
                return DocumentsContract.buildDocumentUriUsingTree(tree, child[0]);
        }
        return null;
    }

    /** The document Uri of <name> inside <dirId>, creating the directory if needed. */
    private Uri ensureTreeDir(ContentResolver cr, Uri tree, String dirId, String name) throws Exception {
        Uri exist = findTreeDir(cr, tree, dirId, name);
        if (exist != null) return exist;
        Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, dirId);
        Uri created = DocumentsContract.createDocument(cr, parent, DCMIME_DIR, name);
        if (created == null) throw new java.io.IOException("create folder " + name);
        return created;
    }

    /** Writes a local File into the tree as <name> inside <dirId>, replacing it. */
    private void putFileToTree(ContentResolver cr, Uri tree, String dirId, String name, File src) throws Exception {
        Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, dirId);
        Uri doc = DocumentsContract.createDocument(cr, parent, "application/octet-stream", name);
        if (doc == null) doc = DocumentsContract.createDocument(cr, parent, "application/octet-stream", name);
        if (doc == null) throw new java.io.IOException("create file " + name);
        try (InputStream in = new FileInputStream(src);
             OutputStream out = cr.openOutputStream(doc, "w")) {
            byte[] buf = new byte[8192]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    private void exportSaves(Uri tree) {
        status.setText("Exporting saves…");
        importer.execute(() -> {
            int count = 0;
            try {
                ContentResolver cr = getContentResolver();
                String rootId = DocumentsContract.getTreeDocumentId(tree);
                Uri top = ensureTreeDir(cr, tree, rootId, "ZQuest-Saves");
                String topId = DocumentsContract.getDocumentId(top);

                File savesLocal = new File(getFilesDir(), "saves");
                Uri savesDoc = ensureTreeDir(cr, tree, topId, "saves");
                String savesId = DocumentsContract.getDocumentId(savesDoc);
                File[] savFiles = savesLocal.listFiles((d, n) -> n.endsWith(".sav") || "order.txt".equals(n));
                if (savFiles != null) for (File f : savFiles) { putFileToTree(cr, tree, savesId, f.getName(), f); count++; }

                File statesLocal = new File(getFilesDir(), "savestates");
                Uri statesDoc = ensureTreeDir(cr, tree, topId, "savestates");
                String statesId = DocumentsContract.getDocumentId(statesDoc);
                File[] questDirs = statesLocal.listFiles(File::isDirectory);
                if (questDirs != null) for (File qd : questDirs) {
                    Uri qdDoc = ensureTreeDir(cr, tree, statesId, qd.getName());
                    String qdId = DocumentsContract.getDocumentId(qdDoc);
                    File[] slots = qd.listFiles((d, n) -> n.endsWith(".sav"));
                    if (slots != null) for (File s : slots) { putFileToTree(cr, tree, qdId, s.getName(), s); count++; }
                }
                final int done = count;
                runOnUiThread(() -> {
                    status.setText("");
                    Toast.makeText(this, "Exported " + done + " save(s) to ZQuest-Saves", Toast.LENGTH_LONG).show();
                });
            } catch (Exception e) {
                runOnUiThread(() -> Toast.makeText(this, "Export failed: " + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        });
    }

    // Select folder = import every quest stored in the picked folder (recursively).
    private void importFolder(Uri tree) {
        status.setText("Importing quests from folder…");
        importer.execute(() -> {
            final int[] count = {0};
            String err = null;
            try {
                count[0] = walkAndImport(getContentResolver(), tree, DocumentsContract.getTreeDocumentId(tree));
            } catch (Exception e) { err = e.getMessage(); }
            final String ferr = err;
            runOnUiThread(() -> {
                status.setText("");
                if (ferr != null) Toast.makeText(this, "Import failed: " + ferr, Toast.LENGTH_LONG).show();
                else Toast.makeText(this, "Imported " + count[0] + " quest(s) from folder.", Toast.LENGTH_LONG).show();
                refreshLibrary();
            });
        });
    }

    /** Recursively imports every .qst/.zip under a SAF tree directory; returns how many succeeded. */
    private int walkAndImport(ContentResolver cr, Uri tree, String dirId) {
        int n = 0;
        for (String[] child : treeChildren(cr, tree, dirId)) {
            if (DCMIME_DIR.equals(child[2])) {
                n += walkAndImport(cr, tree, child[0]);
            } else {
                String name = child[1] == null ? "" : child[1].toLowerCase(Locale.ROOT);
                if (name.endsWith(".qst") || name.endsWith(".zip")) {
                    Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, child[0]);
                    try { copyQuestIntoLibrary(doc); n++; } catch (Exception ignored) { }
                }
            }
        }
        return n;
    }

    // On launch, rescan the saved quest folder and import anything added since —
    // "drop a quest in the folder, reopen the app, it's there to play."
    private void importNewQuestsFromSavedFolder() {
        String saved = getSharedPreferences("zquestclassic", MODE_PRIVATE).getString("folder-uri", null);
        if (saved == null) return;
        final Uri tree = Uri.parse(saved);
        boolean granted = false;
        for (android.content.UriPermission p : getContentResolver().getPersistedUriPermissions()) {
            if (p.isReadPermission() && saved.equals(p.getUri().toString())) { granted = true; break; }
        }
        if (!granted) return;
        final java.util.Set<String> present = existingQuestNames();
        importer.execute(() -> {
            try {
                final int added = walkAndImportNew(getContentResolver(), tree, DocumentsContract.getTreeDocumentId(tree), present);
                if (added > 0) runOnUiThread(() -> {
                    Toast.makeText(this, "Added " + added + " new quest(s) from your folder.", Toast.LENGTH_LONG).show();
                    refreshLibrary();
                });
            } catch (Exception ignored) { }
        });
    }

    /** Names currently in the library (quest file basenames, lowercased) — used to skip re-imports. */
    private java.util.Set<String> existingQuestNames() {
        java.util.Set<String> names = new java.util.HashSet<>();
        File questsDir = new File(getFilesDir(), "quests");
        File[] children = questsDir.isDirectory() ? questsDir.listFiles() : null;
        if (children != null) for (File c : children) names.add(c.getName().toLowerCase(Locale.ROOT));
        return names;
    }

    /** Like walkAndImport, but skips any quest whose source basename is already in the library. */
    private int walkAndImportNew(ContentResolver cr, Uri tree, String dirId, java.util.Set<String> present) {
        int n = 0;
        for (String[] child : treeChildren(cr, tree, dirId)) {
            if (DCMIME_DIR.equals(child[2])) {
                n += walkAndImportNew(cr, tree, child[0], present);
            } else {
                String base = child[1] == null ? "" : child[1].trim();
                String lower = base.toLowerCase(Locale.ROOT);
                if (lower.endsWith(".qst") || lower.endsWith(".zip")) {
                    if (present.contains(lower)) continue;   // already imported
                    Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, child[0]);
                    try { copyQuestIntoLibrary(doc); present.add(lower); n++; } catch (Exception ignored) { }
                }
            }
        }
        return n;
    }


    /** Copies or extracts one quest into files/quests/ (no launch, no UI). Returns the quest file. */
    private File copyQuestIntoLibrary(Uri uri) throws Exception {
        String name = displayName(uri);
        if (name == null || name.isEmpty()) throw new IllegalArgumentException("no name");
        String lowerName = name.toLowerCase(Locale.ROOT);
        copyResourcesIfNeeded();
        File quests = new File(getFilesDir(), "quests");
        if (!quests.isDirectory() && !quests.mkdirs()) throw new IllegalStateException("cannot create quests dir");
        if (lowerName.endsWith(".qst")) {
            File selectedQuest = new File(quests, safeBaseName(name));
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IllegalStateException("cannot open file");
                copyLimited(input, new FileOutputStream(selectedQuest));
            }
            return selectedQuest;
        }
        if (lowerName.endsWith(".zip")) return importQuestArchive(uri, quests, name);
        throw new IllegalArgumentException("not a quest file");
    }

    private void importSaves(Uri tree) {
        status.setText("Importing saves…");
        importer.execute(() -> {
            int count = 0;
            try {
                ContentResolver cr = getContentResolver();
                String rootId = DocumentsContract.getTreeDocumentId(tree);
                // Accept the picked folder directly, or a "ZQuest-Saves" subfolder.
                String baseId = rootId;
                Uri zqs = findTreeDir(cr, tree, rootId, "ZQuest-Saves");
                if (zqs != null) baseId = DocumentsContract.getDocumentId(zqs);

                Uri statesDoc = findTreeDir(cr, tree, baseId, "savestates");
                if (statesDoc != null) {
                    String statesId = DocumentsContract.getDocumentId(statesDoc);
                    for (String[] q : treeChildren(cr, tree, statesId)) {
                        if (!DCMIME_DIR.equals(q[2])) continue;
                        File qd = new File(new File(getFilesDir(), "savestates"), q[1]);
                        if (!qd.exists() && !qd.mkdirs()) continue;
                        String qdId = q[0];
                        for (String[] slot : treeChildren(cr, tree, qdId)) {
                            if (!slot[1].endsWith(".sav")) continue;
                            Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, slot[0]);
                            try (InputStream in = cr.openInputStream(doc);
                                 OutputStream out = new FileOutputStream(new File(qd, slot[1]))) {
                                byte[] buf = new byte[8192]; int n;
                                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                            }
                            count++;
                        }
                    }
                }

                Uri savesDoc = findTreeDir(cr, tree, baseId, "saves");
                if (savesDoc != null) {
                    String savesId = DocumentsContract.getDocumentId(savesDoc);
                    File savesLocal = new File(getFilesDir(), "saves");
                    for (String[] f : treeChildren(cr, tree, savesId)) {
                        if (!f[1].endsWith(".sav") && !"order.txt".equals(f[1])) continue;
                        Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, f[0]);
                        try (InputStream in = cr.openInputStream(doc);
                             OutputStream out = new FileOutputStream(new File(savesLocal, f[1]))) {
                            byte[] buf = new byte[8192]; int n;
                            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                        }
                        count++;
                    }
                }
                final int done = count;
                runOnUiThread(() -> {
                    status.setText("");
                    Toast.makeText(this, "Imported " + done + " save(s).", Toast.LENGTH_LONG).show();
                });
            } catch (Exception e) {
                runOnUiThread(() -> Toast.makeText(this, "Import failed: " + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_QUEST && resultCode == RESULT_OK && data != null && data.getData() != null) {
            if (data.getClipData() != null && data.getClipData().getItemCount() > 0) {
                // Multiple quests picked together: import them all into the library
                // without auto-launching the game (so you can add everything up front).
                status.setText("Importing quests…");
                final android.net.Uri[] uris = new android.net.Uri[data.getClipData().getItemCount()];
                for (int i = 0; i < uris.length; i++) uris[i] = data.getClipData().getItemAt(i).getUri();
                importer.execute(() -> {
                    int ok = 0;
                    for (android.net.Uri u : uris) {
                        try { copyQuestIntoLibrary(u); ok++; } catch (Exception ignored) { }
                    }
                    final int n = ok;
                    runOnUiThread(() -> {
                        status.setText("");
                        Toast.makeText(this, "Added " + n + " quest" + (n == 1 ? "" : "s") + ".", Toast.LENGTH_LONG).show();
                        refreshLibrary();
                    });
                });
            } else {
                importQuest(data.getData());
            }
        } else if (requestCode == REQUEST_EXPORT && resultCode == RESULT_OK && data != null && data.getData() != null) {
            exportSaves(data.getData());
        } else if (requestCode == REQUEST_IMPORT && resultCode == RESULT_OK && data != null && data.getData() != null) {
            importSaves(data.getData());
        } else if (requestCode == REQUEST_FOLDER && resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException e) {
                // Ignore
            }
            // Remember this folder: from now on the app rescans it on launch and
            // imports any quests dropped in, so "put a quest here, play it" stays true.
            getSharedPreferences("zquestclassic", MODE_PRIVATE).edit().putString("folder-uri", uri.toString()).apply();
            importFolder(uri);
        }
    }

    private void listFolderContents() {
        if (folderUri == null) return;

        // currentDocumentId is null at the tree root and holds the document id of
        // the subdirectory being viewed otherwise.
        String listId = currentDocumentId != null ? currentDocumentId : DocumentsContract.getTreeDocumentId(folderUri);
        try {
            Uri treeUri = folderUri;
            Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, listId);

            Cursor cursor = getContentResolver().query(childrenUri,
                    new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE},
                    null, null, null);

            if (cursor == null) {
                folderList.addView(createMessageRow("Could not load folder contents."));
                return;
            }

            List<FolderItem> items = new ArrayList<>();
            try {
                while (cursor.moveToNext()) {
                    String docId = cursor.getString(0);
                    String name = cursor.getString(1);
                    String mime = cursor.getString(2);
                    if (docId == null || name == null) continue;

                    if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                        items.add(new FolderItem(true, name, docId));
                    } else if (name.toLowerCase(Locale.ROOT).endsWith(".qst")
                            || name.toLowerCase(Locale.ROOT).endsWith(".zip")) {
                        items.add(new FolderItem(false, name, docId));
                    }
                }
            } finally {
                cursor.close();
            }

            // Directories first, then files, each alphabetically case-insensitively.
            Collections.sort(items, (a, b) -> {
                if (a.isDir != b.isDir) return a.isDir ? -1 : 1;
                return a.name.compareToIgnoreCase(b.name);
            });

            if (currentDocumentId != null) addNavigationRow(folderList, ".. (up)");

            for (FolderItem item : items) {
                if (item.isDir) {
                    addFolderRow(folderList, item.name, true, item.docId);
                } else {
                    addFolderRow(folderList, item.name, false,
                            DocumentsContract.buildDocumentUriUsingTree(treeUri, item.docId));
                }
            }

            if (items.isEmpty()) {
                folderList.addView(createMessageRow(
                        currentDocumentId == null
                                ? "No .qst or .zip files in this folder yet."
                                : "Nothing to import here."));
            }
        } catch (Exception e) {
            // Most likely the persisted permission was revoked. Ask for the
            // folder again rather than leaving a screen that can never load.
            folderList.removeAllViews();
            folderHeader.setText("Choose a quest folder");
            folderUri = null;
            currentDocumentId = null;
            folderHistory.clear();
            getSharedPreferences("zquestclassic", MODE_PRIVATE).edit().remove("folder-uri").apply();
        }
    }

    private void addNavigationRow(LinearLayout parent, String title) {
        TextView row = new TextView(this);
        row.setText(title);
        row.setTextSize(16);
        row.setTextColor(getColor(R.color.zc_cyan));
        row.setPadding(dp(12), dp(12), dp(12), dp(12));
        row.setOnClickListener(view -> navigateUp());
        parent.addView(row, new LinearLayout.LayoutParams(-1, -2));
    }

    private void addFolderRow(LinearLayout parent, String title, boolean isDir, Object target) {
        TextView row = new TextView(this);
        row.setText(title);
        row.setTextSize(16);
        row.setTextColor(getColor(R.color.zc_text));
        row.setPadding(dp(12), dp(12), dp(12), dp(12));
        row.setBackgroundResource(R.drawable.zc_row);

        if (isDir) {
            final String docId = (String) target;
            row.setOnClickListener(view -> {
                folderHistory.add(currentDocumentId);
                currentDocumentId = docId;
                refreshLibrary();
            });
        } else {
            row.setOnClickListener(view -> importQuest((Uri) target));
        }

        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(-1, -2);
        rowParams.bottomMargin = dp(6);
        parent.addView(row, rowParams);
    }

    // Document ids look like "primary:Download/quests", so the parent of a nested
    // directory is its id with the last path segment removed.
    private void navigateUp() {
        if (currentDocumentId == null) return;
        if (!folderHistory.isEmpty()) {
            currentDocumentId = folderHistory.remove(folderHistory.size() - 1);
        } else if (currentDocumentId.contains("/")) {
            currentDocumentId = currentDocumentId.substring(0, currentDocumentId.lastIndexOf('/'));
        } else {
            currentDocumentId = null;
        }
        refreshLibrary();
    }

    private TextView createMessageRow(String message) {
        TextView view = new TextView(this);
        view.setText(message);
        view.setTextColor(getColor(R.color.zc_text_dim));
        view.setPadding(dp(12), dp(12), dp(12), dp(12));
        view.setAlpha(0.7f);
        return view;
    }

    private void openDocumentPicker() {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("*/*");
        // Allow selecting several quests at once. Android blocks *folder* access
        // under Downloads, so bulk import must use multi-file selection instead
        // (works anywhere, including Downloads).
        picker.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(picker, REQUEST_QUEST);
    }

    private void importQuest(Uri uri) {
        chooseButton.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        status.setText("Importing quest…");

        importer.execute(() -> {
            try {
                String name = displayName(uri);
                if (name == null || name.isEmpty()) {
                    throw new IllegalArgumentException("That file has no usable name.");
                }
                String lowerName = name.toLowerCase(Locale.ROOT);
                copyResourcesIfNeeded();
                File quests = new File(getFilesDir(), "quests");
                if (!quests.isDirectory() && !quests.mkdirs()) {
                    throw new IllegalStateException("Could not create the quest folder.");
                }

                File selectedQuest;
                if (lowerName.endsWith(".qst")) {
                    selectedQuest = new File(quests, safeBaseName(name));
                    try (InputStream input = getContentResolver().openInputStream(uri)) {
                        if (input == null) throw new IllegalStateException("Could not open the selected file.");
                        copyLimited(input, new FileOutputStream(selectedQuest));
                    }
                } else if (lowerName.endsWith(".zip")) {
                    selectedQuest = importQuestArchive(uri, quests, name);
                } else {
                    throw new IllegalArgumentException("Select a .qst file or a .zip containing a .qst file.");
                }

                File marker = new File(getFilesDir(), "selected-quest.txt");
                try (FileOutputStream output = new FileOutputStream(marker)) {
                    output.write(selectedQuest.getAbsolutePath().getBytes(StandardCharsets.UTF_8));
                    output.write('\n');
                }

                runOnUiThread(() -> {
                    progress.setVisibility(View.GONE);
                    chooseButton.setEnabled(true);
                    status.setText("Starting " + selectedQuest.getName());
                    startActivity(new Intent(this, PlayerActivity.class));
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    progress.setVisibility(View.GONE);
                    chooseButton.setEnabled(true);
                    status.setText(error.getMessage() == null ? "Could not import this quest." : error.getMessage());
                    Toast.makeText(this, status.getText(), Toast.LENGTH_LONG).show();
                });
            }
        });
    }

    private File importQuestArchive(Uri uri, File destination, String archiveName) throws Exception {
        String folderName = safeFileComponent(archiveName.replaceFirst("(?i)\\.zip$", ""));
        File questFolder = new File(destination, folderName);

        // Re-importing the same archive should just reopen it. Without this the
        // library fills up with "name (2)", "name (3)" copies every time the
        // same quest is tapped.
        File reused = reuseExistingImport(destination, folderName, uri, archiveName);
        if (reused != null) return reused;

        for (int suffix = 2; questFolder.exists(); suffix++) {
            questFolder = new File(destination, folderName + " (" + suffix + ")");
        }
        if (!questFolder.mkdirs()) throw new IllegalStateException("Could not create the quest folder.");

        try (InputStream source = getContentResolver().openInputStream(uri)) {
            if (source == null) throw new IllegalStateException("Could not open the selected ZIP file.");
            try (ZipInputStream zip = new ZipInputStream(source)) {
                File selectedQuest = null;
                long totalBytes = 0;
                ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) {
                    File output = resolveArchiveEntry(questFolder, entry.getName());
                    if (entry.isDirectory()) {
                        if (!output.isDirectory() && !output.mkdirs()) {
                            throw new IllegalStateException("Could not create a folder from the quest archive.");
                        }
                    } else {
                        File parent = output.getParentFile();
                        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                            throw new IllegalStateException("Could not create a folder from the quest archive.");
                        }
                        totalBytes += copyLimited(zip, new FileOutputStream(output), MAX_QUEST_BYTES - totalBytes);
                        if (selectedQuest == null && output.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".qst")) {
                            selectedQuest = output;
                        }
                    }
                    zip.closeEntry();
                }
                if (selectedQuest != null) {
                    writeImportMarker(questFolder, uri, archiveName);
                    return selectedQuest;
                }
            }
        }
        throw new IllegalArgumentException("The ZIP archive does not contain a .qst quest file.");
    }

    private static final String IMPORT_MARKER = ".import-source";

    /**
     * Returns the already-imported quest when this folder came from the same
     * archive, or null when the caller should import it afresh. Folders without
     * a marker came from an older build, so they are left alone.
     */
    private File reuseExistingImport(File destination, String folderName, Uri uri, String archiveName) {
        String expected = describeSource(uri, archiveName);
        // Check the base folder and every numbered copy, so re-importing still
        // finds the match when an earlier build already made the first copy.
        for (int suffix = 1; suffix < 1000; suffix++) {
            File folder = suffix == 1
                    ? new File(destination, folderName)
                    : new File(destination, folderName + " (" + suffix + ")");
            File marker = new File(folder, IMPORT_MARKER);
            if (!folder.isDirectory() || !marker.isFile()) continue;
            try (InputStream input = new FileInputStream(marker)) {
                if (!expected.equals(readAll(input).trim())) continue;
            } catch (Exception unreadable) {
                continue;
            }
            File quest = findFirstQst(folder);
            if (quest != null && quest.isFile()) return quest;
        }
        return null;
    }

    /** InputStream.readAllBytes() needs API 33; this works on every supported release. */
    private static String readAll(InputStream input) throws java.io.IOException {
        java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
        byte[] chunk = new byte[4096];
        int read;
        while ((read = input.read(chunk)) != -1) buffer.write(chunk, 0, read);
        return new String(buffer.toByteArray(), StandardCharsets.UTF_8);
    }

    private void writeImportMarker(File questFolder, Uri uri, String archiveName) throws Exception {
        try (FileOutputStream output = new FileOutputStream(new File(questFolder, IMPORT_MARKER))) {
            output.write(describeSource(uri, archiveName).getBytes(StandardCharsets.UTF_8));
        }
    }

    /** Identifies the source archive well enough to tell two downloads apart. */
    private String describeSource(Uri uri, String archiveName) {
        long size = -1;
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (index >= 0 && !cursor.isNull(index)) size = cursor.getLong(index);
            }
        } catch (Exception ignored) {
            // A missing size only makes the match slightly weaker.
        }
        return archiveName + "|" + size;
    }

    private void copyResourcesIfNeeded() throws Exception {
        File marker = new File(getFilesDir(), ".resources-installed-v2");
        if (marker.isFile()) return;

        copyAssetTree("", getFilesDir());
        if (!marker.createNewFile() && !marker.isFile()) {
            throw new IllegalStateException("Could not finish installing game resources.");
        }
    }

    private void copyAssetTree(String assetPath, File destination) throws Exception {
        String[] children = getAssets().list(assetPath);
        if (children != null && children.length > 0) {
            if (!destination.isDirectory() && !destination.mkdirs()) {
                throw new IllegalStateException("Could not create the game resources folder.");
            }
            for (String child : children) {
                String childPath = assetPath.isEmpty() ? child : assetPath + "/" + child;
                copyAssetTree(childPath, new File(destination, child));
            }
            return;
        }

        File parent = destination.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IllegalStateException("Could not create a resources folder.");
        }
        try (InputStream input = getAssets().open(assetPath)) {
            copyLimited(input, new FileOutputStream(destination));
        }
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) return cursor.getString(index);
            }
        }
        String path = uri.getLastPathSegment();
        return path == null ? "quest.qst" : path;
    }

    private static String safeBaseName(String name) {
        String base = safeFileComponent(name);
        if (!base.toLowerCase(java.util.Locale.ROOT).endsWith(".qst")) base += ".qst";
        return base;
    }

    private static String safeFileComponent(String name) {
        String base = new File(name).getName().replaceAll("[^A-Za-z0-9._ -]", "_").trim();
        if (base.isEmpty() || base.equals(".") || base.equals("..")) return "quest";
        return base;
    }

    private static void copyLimited(InputStream input, OutputStream output) throws Exception {
        copyLimited(input, output, MAX_QUEST_BYTES);
    }

    private static File resolveArchiveEntry(File root, String entryName) throws Exception {
        String normalizedName = entryName.replace('\\', '/');
        if (normalizedName.startsWith("/") || normalizedName.matches("^[A-Za-z]:.*")) {
            throw new IllegalArgumentException("The ZIP archive contains an unsafe path.");
        }
        if (normalizedName.isEmpty() || normalizedName.equals(".") || normalizedName.equals("./")) return root;
        File output = new File(root, normalizedName);
        String rootPath = root.getCanonicalPath() + File.separator;
        String outputPath = output.getCanonicalPath();
        if (!outputPath.startsWith(rootPath)) {
            throw new IllegalArgumentException("The ZIP archive contains an unsafe path.");
        }
        return output;
    }

    private static long copyLimited(InputStream input, OutputStream output, long maxBytes) throws Exception {
        try (OutputStream destination = output) {
            byte[] buffer = new byte[64 * 1024];
            long total = 0;
            int count;
            while ((count = input.read(buffer)) != -1) {
                total += count;
                if (total > maxBytes) throw new IllegalArgumentException("The quest archive is too large (1 GiB limit).");
                destination.write(buffer, 0, count);
            }
            return total;
        }
    }

    // Adds two or more buttons side-by-side on one even row of the library grid.
    private void addButtonRow(Button... buttons) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        // Fixed row height; children match it so every button is exactly the same
        // size regardless of its label length — no one sinks or loses a border.
        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(-1, dp(56));
        rowParams.topMargin = dp(10);
        content.addView(row, rowParams);
        for (int i = 0; i < buttons.length; i++) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -1, 1f);
            if (i > 0) lp.leftMargin = dp(8);
            row.addView(buttons[i], lp);
        }
    }

    // ZQuest's dialog buttons: flat panel, hard cyan border, cyan label.
    private void styleZcButton(Button button) {
        button.setBackgroundResource(R.drawable.zc_button);
        button.setTextColor(getColor(R.color.zc_cyan));
        button.setTypeface(android.graphics.Typeface.MONOSPACE, android.graphics.Typeface.BOLD);
        button.setAllCaps(false);
        button.setMinHeight(dp(48));
        button.setPadding(dp(12), dp(8), dp(12), dp(8));
        button.setLetterSpacing(0.04f);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onDestroy() {
        importer.shutdownNow();
        super.onDestroy();
    }

    private static class FolderItem {
        final boolean isDir;
        final String name;
        final String docId;

        FolderItem(boolean isDir, String name, String docId) {
            this.isDir = isDir;
            this.name = name;
            this.docId = docId;
        }
    }
}