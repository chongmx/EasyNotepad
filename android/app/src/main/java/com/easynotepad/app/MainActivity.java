package com.easynotepad.app;

import android.Manifest;
import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Insets;
import android.net.ConnectivityManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.DisplayCutout;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.concurrent.LinkedBlockingQueue;

/**
 * NativeActivity with the small Java helpers the native code calls through JNI:
 * soft-keyboard control, a queue of typed Unicode characters, the safe area, and large downloads.
 */
public class MainActivity extends NativeActivity {
    private final LinkedBlockingQueue<Integer> unicodeQueue = new LinkedBlockingQueue<>();

    // The parts of the app's view covered by the system: status and navigation bars, a notch, the
    // on-screen keyboard. Left, top, right, bottom in pixels; -1 until Android first reports them.
    private final int[] safeInsets = {-1, -1, -1, -1};
    private WindowInsets lastInsets;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        View decor = getWindow().getDecorView();
        decor.setOnApplyWindowInsetsListener((v, insets) -> {
            lastInsets = insets;
            updateSafeInsets();
            return v.onApplyWindowInsets(insets);
        });
        // The view the native surface fills can move or resize (rotation, keyboard): measure again.
        decor.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> updateSafeInsets());
        decor.requestApplyInsets();
    }

    // How much of each edge of the app's view the system covers. The insets are given for the whole
    // window; the app's view may already stop short of the bars (older Android) or run underneath
    // them (edge to edge, Android 15), so only the overlap with the view counts.
    private void updateSafeInsets() {
        WindowInsets insets = lastInsets;
        View decor = getWindow().getDecorView();
        View content = decor.findViewById(android.R.id.content);
        if (insets == null || content == null || content.getWidth() == 0)
            return;
        int left, top, right, bottom;
        if (Build.VERSION.SDK_INT >= 30) {
            Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout()
                                        | WindowInsets.Type.ime());
            left = i.left;
            top = i.top;
            right = i.right;
            bottom = i.bottom;
        } else {
            left = insets.getSystemWindowInsetLeft();
            top = insets.getSystemWindowInsetTop();
            right = insets.getSystemWindowInsetRight();
            bottom = insets.getSystemWindowInsetBottom();
            if (Build.VERSION.SDK_INT >= 28) {
                DisplayCutout cutout = insets.getDisplayCutout();
                if (cutout != null) {
                    left = Math.max(left, cutout.getSafeInsetLeft());
                    top = Math.max(top, cutout.getSafeInsetTop());
                    right = Math.max(right, cutout.getSafeInsetRight());
                    bottom = Math.max(bottom, cutout.getSafeInsetBottom());
                }
            }
        }
        int[] at = new int[2];
        content.getLocationInWindow(at);
        int windowW = decor.getWidth(), windowH = decor.getHeight();
        int x0 = at[0], y0 = at[1], x1 = x0 + content.getWidth(), y1 = y0 + content.getHeight();
        synchronized (safeInsets) {
            safeInsets[0] = Math.max(0, left - x0);
            safeInsets[1] = Math.max(0, top - y0);
            safeInsets[2] = Math.max(0, x1 - (windowW - right));
            safeInsets[3] = Math.max(0, y1 - (windowH - bottom));
        }
    }

    /** Left, top, right, bottom in pixels of the app's view; -1 values until known. */
    public int[] getSafeInsets() {
        synchronized (safeInsets) {
            return safeInsets.clone();
        }
    }

    public void showSoftInput() {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            imm.showSoftInput(getWindow().getDecorView(), 0);
        });
    }

    public void hideSoftInput() {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            imm.hideSoftInputFromWindow(getWindow().getDecorView().getWindowToken(), 0);
        });
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            int c = event.getUnicodeChar(event.getMetaState());
            if (c != 0)
                unicodeQueue.offer(c);
        }
        return super.dispatchKeyEvent(event);
    }

    public int pollUnicodeChar() {
        Integer c = unicodeQueue.poll();
        return c == null ? 0 : c;
    }

    // ---- Files outside the app's folders ---------------------------------------------------------
    // The editor opens, saves and lists files by path (a folder is a workspace), so it needs plain
    // file access to the shared storage: "All files access" (MANAGE_EXTERNAL_STORAGE) on Android 11
    // and later, which only the system settings can grant, and the storage permission before. The
    // Storage Access Framework hands out content:// documents instead of paths, which a workspace,
    // Find in Files or git cannot work in.

    private static final int STORAGE_PERMISSION = 4712;
    private static final String PREFERENCES = "easynotepad", ASK_AT_START = "askStorageAccessAtStart";
    // Asked at most once per run (the process): "Not now" holds until the app is started again.
    private static boolean askedAtStart;

    @Override
    protected void onResume() {
        super.onResume();
        askForStorageAccessAtStart();
    }

    // At start, without access: why it is needed, and the way to it. The Open dialog says the same
    // (with its own button) for the rest of the run.
    private void askForStorageAccessAtStart() {
        if (askedAtStart || hasStorageAccess())
            return;
        askedAtStart = true;
        final SharedPreferences preferences = getSharedPreferences(PREFERENCES, MODE_PRIVATE);
        if (!preferences.getBoolean(ASK_AT_START, true))
            return;
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Access to your files")
            .setMessage("EasyNotepad can only see its own files. To open files and folders elsewhere on the "
                        + "device (Download, Documents, an SD card), it needs access to all files.")
            .setPositiveButton("Allow", (d, w) -> requestStorageAccess())
            .setNegativeButton("Not now", null)
            .setNeutralButton("Don't ask again", (d, w) -> preferences.edit().putBoolean(ASK_AT_START, false).apply())
            .show();
    }

    /** True when the app may read and write the shared storage, not only its own folders. */
    public boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= 30)
            return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /** Opens the app's "All files access" page in the settings (Android 11+), or the permission prompt. */
    public void requestStorageAccess() {
        runOnUiThread(() -> {
            if (Build.VERSION.SDK_INT < 30) {
                requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                                                 Manifest.permission.WRITE_EXTERNAL_STORAGE},
                                   STORAGE_PERMISSION);
                return;
            }
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                         Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                // No page for one app on this device: the list of every app that may ask for it.
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (Exception ignored) {
                }
            }
        });
    }

    /** The shared storage's folder, "/storage/emulated/0" for the device's first user. */
    @SuppressWarnings("deprecation")
    public String sharedStoragePath() {
        return Environment.getExternalStorageDirectory().getAbsolutePath();
    }

    // ---- Large downloads (models): fetched by the app itself -----------------------------------
    // Android's DownloadManager gave no reason when it stalled ("waiting to retry"), and Hugging
    // Face answers with a redirect to a signed, expiring address. So the app downloads by itself:
    // each attempt asks the original URL again (a fresh signed address), resumes from the ".part"
    // file with an HTTP Range request, retries with growing pauses, and reports the actual error.
    // It runs while the app is alive (for a release: a user-initiated data transfer job, see plan 23).
    // A file the user picked in the system file picker is copied in the same way ("import").

    private static final int IDLE = 0, RUNNING = 1, RETRYING = 2, DONE = 3, FAILED = 4;
    private static final int ATTEMPTS = 8;
    private static final int PICK_FILE = 4711;

    private volatile int transferState = IDLE;
    private volatile long transferReceived, transferTotal;
    private volatile String transferMessage = "";
    private volatile boolean transferCancel;
    private Thread transferThread;
    private String importTarget; // where a picked file goes, while the picker is open

    private boolean transferBusy() {
        return transferThread != null && transferThread.isAlive();
    }

    /** Starts downloading `url` to the absolute `path` (via path + ".part"); false if one runs already. */
    public boolean startDownload(String url, String path, boolean allowMetered) {
        if (transferBusy())
            return false;
        transferCancel = false;
        transferState = RUNNING;
        transferMessage = "";
        transferReceived = 0;
        transferTotal = 0;
        transferThread = new Thread(() -> download(url, path), "model-download");
        transferThread.start();
        return true;
    }

    private void download(String url, String path) {
        File target = new File(path);
        File part = new File(path + ".part");
        File parent = target.getParentFile();
        if (parent != null)
            parent.mkdirs();
        for (int attempt = 1; attempt <= ATTEMPTS && !transferCancel; ++attempt) {
            HttpURLConnection c = null;
            try {
                long have = part.length();
                c = (HttpURLConnection) new URL(url).openConnection();
                c.setInstanceFollowRedirects(true);
                c.setConnectTimeout(20000);
                c.setReadTimeout(60000);
                c.setRequestProperty("User-Agent", "EasyNotepad");
                if (have > 0)
                    c.setRequestProperty("Range", "bytes=" + have + "-");
                int code = c.getResponseCode();
                if (code == 416 && have > 0) { // nothing left: the .part is the whole file
                    finish(part, target);
                    return;
                }
                boolean append = code == 206;
                if (code != 200 && code != 206)
                    throw new IOException("HTTP " + code + " " + c.getResponseMessage());
                if (!append)
                    have = 0; // the server sent it all again
                long length = c.getContentLengthLong();
                transferTotal = length > 0 ? have + length : 0;
                transferReceived = have;
                transferState = RUNNING;
                transferMessage = "";
                try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(part, append)) {
                    byte[] buffer = new byte[1 << 20];
                    for (int n; (n = in.read(buffer)) > 0;) {
                        if (transferCancel)
                            throw new IOException("stopped");
                        out.write(buffer, 0, n);
                        transferReceived += n;
                    }
                }
                if (transferTotal > 0 && part.length() < transferTotal)
                    throw new IOException("the connection closed early");
                finish(part, target);
                return;
            } catch (Exception e) {
                if (transferCancel) {
                    transferState = IDLE;
                    return;
                }
                String why = e.getClass().getSimpleName() + (e.getMessage() != null ? ": " + e.getMessage() : "");
                if (attempt == ATTEMPTS) {
                    transferMessage = why;
                    transferState = FAILED;
                    return;
                }
                int pause = Math.min(60, 2 << attempt); // 4, 8, 16, 32, 60 ... seconds
                transferMessage = "retrying in " + pause + " s (" + attempt + "/" + ATTEMPTS + "): " + why;
                transferState = RETRYING;
                try {
                    Thread.sleep(pause * 1000L);
                } catch (InterruptedException ignored) {
                    transferState = IDLE;
                    return;
                }
            } finally {
                if (c != null)
                    c.disconnect();
            }
        }
        if (transferCancel)
            transferState = IDLE;
    }

    private void finish(File part, File target) throws IOException {
        target.delete();
        if (!part.renameTo(target))
            throw new IOException("could not rename " + part.getName());
        transferReceived = target.length();
        transferTotal = transferReceived;
        transferState = DONE;
    }

    /** Opens the system file picker; the chosen file is copied to `path` (progress as a download). */
    public boolean importFile(String path) {
        if (transferBusy())
            return false;
        importTarget = path;
        runOnUiThread(() -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            try {
                startActivityForResult(pick, PICK_FILE);
            } catch (Exception e) {
                transferMessage = "no file picker: " + e.getMessage();
                transferState = FAILED;
            }
        });
        transferCancel = false;
        transferMessage = "choose the file";
        transferReceived = 0;
        transferTotal = 0;
        transferState = RUNNING;
        return true;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_FILE)
            return;
        final Uri uri = data != null && resultCode == RESULT_OK ? data.getData() : null;
        final String path = importTarget;
        if (uri == null || path == null) {
            transferMessage = "no file chosen";
            transferState = FAILED;
            return;
        }
        transferThread = new Thread(() -> copyIn(uri, path), "model-import");
        transferThread.start();
    }

    private void copyIn(Uri uri, String path) {
        File target = new File(path);
        File part = new File(path + ".part");
        File parent = target.getParentFile();
        if (parent != null)
            parent.mkdirs();
        transferMessage = "";
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0))
                transferTotal = c.getLong(0);
        } catch (Exception ignored) {
        }
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(part, false)) {
            if (in == null)
                throw new IOException("the file could not be opened");
            byte[] buffer = new byte[1 << 20];
            for (int n; (n = in.read(buffer)) > 0;) {
                if (transferCancel)
                    throw new IOException("stopped");
                out.write(buffer, 0, n);
                transferReceived += n;
            }
            out.close();
            finish(part, target);
        } catch (Exception e) {
            part.delete();
            if (transferCancel) {
                transferState = IDLE;
                return;
            }
            transferMessage = e.getClass().getSimpleName() + (e.getMessage() != null ? ": " + e.getMessage() : "");
            transferState = FAILED;
        }
    }

    /** {state, received bytes, total bytes, 0}: state 0 none, 1 running, 2 retrying, 3 done, 4 failed. */
    public long[] downloadStatus() {
        return new long[] {transferState, transferReceived, transferTotal, 0};
    }

    /** The current retry note, or why it failed. */
    public String getLastDownloadError() {
        return transferMessage;
    }

    /** True on a metered network (mobile data, a hotspot): the app asks before a large download. */
    public boolean isMeteredNetwork() {
        ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        return cm != null && cm.isActiveNetworkMetered();
    }

    /** Stops the transfer; a download's ".part" stays, so the next one resumes. */
    public void cancelDownload() {
        transferCancel = true;
        if (transferThread != null)
            transferThread.interrupt();
    }
}
