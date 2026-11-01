package com.easynotepad.app;

import android.app.DownloadManager;
import android.app.NativeActivity;
import android.content.Context;
import android.database.Cursor;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.DisplayCutout;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;

import java.io.File;
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

    // ---- Large downloads (models) through the system's DownloadManager ----------------------
    // It keeps going in the background and across app restarts; a running transfer to the same
    // file is picked up again instead of starting a second one.

    private long downloadId = -1;

    /** Starts (or rejoins) a download of `url` to the absolute `path`; returns false on failure. */
    public boolean startDownload(String url, String path, boolean allowMetered) {
        try {
            DownloadManager dm = (DownloadManager) getSystemService(Context.DOWNLOAD_SERVICE);
            Uri target = Uri.fromFile(new File(path));
            long existing = findDownload(dm, target);
            if (existing >= 0) {
                downloadId = existing;
                return true;
            }
            File file = new File(path);
            file.getParentFile().mkdirs();
            file.delete(); // DownloadManager does not resume a file it did not start
            DownloadManager.Request request = new DownloadManager.Request(Uri.parse(url))
                    .setDestinationUri(target)
                    .setTitle(file.getName())
                    .setDescription("EasyNotepad engine lab")
                    .setAllowedOverMetered(allowMetered)
                    .setAllowedOverRoaming(false)
                    .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE);
            downloadId = dm.enqueue(request);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private static long findDownload(DownloadManager dm, Uri target) {
        DownloadManager.Query query = new DownloadManager.Query().setFilterByStatus(
                DownloadManager.STATUS_PENDING | DownloadManager.STATUS_RUNNING | DownloadManager.STATUS_PAUSED);
        try (Cursor c = dm.query(query)) {
            while (c != null && c.moveToNext()) {
                String local = c.getString(c.getColumnIndexOrThrow(DownloadManager.COLUMN_LOCAL_URI));
                if (target.toString().equals(local))
                    return c.getLong(c.getColumnIndexOrThrow(DownloadManager.COLUMN_ID));
            }
        }
        return -1;
    }

    /**
     * {state, received bytes, total bytes, reason}: state 0 none, 1 running (2 when paused, e.g.
     * waiting for Wi-Fi), 3 done, 4 failed. The reason is DownloadManager's code.
     */
    public long[] downloadStatus() {
        long[] result = {0, 0, 0, 0};
        if (downloadId < 0)
            return result;
        DownloadManager dm = (DownloadManager) getSystemService(Context.DOWNLOAD_SERVICE);
        try (Cursor c = dm.query(new DownloadManager.Query().setFilterById(downloadId))) {
            if (c == null || !c.moveToFirst())
                return result;
            int status = c.getInt(c.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS));
            result[1] = c.getLong(c.getColumnIndexOrThrow(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR));
            result[2] = c.getLong(c.getColumnIndexOrThrow(DownloadManager.COLUMN_TOTAL_SIZE_BYTES));
            result[3] = c.getInt(c.getColumnIndexOrThrow(DownloadManager.COLUMN_REASON));
            switch (status) {
                case DownloadManager.STATUS_PAUSED: result[0] = 2; break;
                case DownloadManager.STATUS_SUCCESSFUL: result[0] = 3; break;
                case DownloadManager.STATUS_FAILED: result[0] = 4; break;
                default: result[0] = 1; break;
            }
        } catch (Exception e) {
            // keep the zeros
        }
        return result;
    }

    public void cancelDownload() {
        if (downloadId < 0)
            return;
        DownloadManager dm = (DownloadManager) getSystemService(Context.DOWNLOAD_SERVICE);
        dm.remove(downloadId);
        downloadId = -1;
    }
}
