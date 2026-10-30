package com.easynotepad.app;

import android.app.NativeActivity;
import android.content.Context;
import android.view.KeyEvent;
import android.view.inputmethod.InputMethodManager;

import java.util.concurrent.LinkedBlockingQueue;

/**
 * NativeActivity with the small Java helpers the native code calls through JNI:
 * soft-keyboard control and a queue of typed Unicode characters.
 */
public class MainActivity extends NativeActivity {
    private final LinkedBlockingQueue<Integer> unicodeQueue = new LinkedBlockingQueue<>();

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
