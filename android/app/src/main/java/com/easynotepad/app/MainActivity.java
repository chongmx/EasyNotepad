package com.easynotepad.app;

import android.app.NativeActivity;
import android.content.Context;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.view.DisplayCutout;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;

import java.util.concurrent.LinkedBlockingQueue;

/**
 * NativeActivity with the small Java helpers the native code calls through JNI:
 * soft-keyboard control, a queue of typed Unicode characters, and the safe area.
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
}
