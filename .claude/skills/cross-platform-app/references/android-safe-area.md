# Android safe area (NativeActivity + C++)

## Why `contentRect` is not enough

With targetSdk 35 (Android 15) apps are edge to edge: the window extends under the status bar and
the 3-button navigation bar. NativeActivity reports `android_app::contentRect` from the visible
display frame, which does not shrink for the navigation bar in that mode, so a bottom bar ends up
under the system buttons. The window insets are the source of truth.

## Java (the activity subclass)

```java
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
    decor.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> updateSafeInsets());
    decor.requestApplyInsets();
}

// Insets are for the whole window; the app's view may stop short of the bars (older Android) or
// run under them (edge to edge): count only the overlap with the view.
private void updateSafeInsets() {
    WindowInsets insets = lastInsets;
    View decor = getWindow().getDecorView();
    View content = decor.findViewById(android.R.id.content);
    if (insets == null || content == null || content.getWidth() == 0) return;
    int left, top, right, bottom;
    if (Build.VERSION.SDK_INT >= 30) {
        Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout()
                                    | WindowInsets.Type.ime());
        left = i.left; top = i.top; right = i.right; bottom = i.bottom;
    } else {
        left = insets.getSystemWindowInsetLeft(); top = insets.getSystemWindowInsetTop();
        right = insets.getSystemWindowInsetRight(); bottom = insets.getSystemWindowInsetBottom();
        if (Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
            DisplayCutout c = insets.getDisplayCutout();
            left = Math.max(left, c.getSafeInsetLeft()); top = Math.max(top, c.getSafeInsetTop());
            right = Math.max(right, c.getSafeInsetRight()); bottom = Math.max(bottom, c.getSafeInsetBottom());
        }
    }
    int[] at = new int[2];
    content.getLocationInWindow(at);
    int w = decor.getWidth(), h = decor.getHeight();
    int x0 = at[0], y0 = at[1], x1 = x0 + content.getWidth(), y1 = y0 + content.getHeight();
    synchronized (safeInsets) {
        safeInsets[0] = Math.max(0, left - x0);
        safeInsets[1] = Math.max(0, top - y0);
        safeInsets[2] = Math.max(0, x1 - (w - right));
        safeInsets[3] = Math.max(0, y1 - (h - bottom));
    }
}

public int[] getSafeInsets() { synchronized (safeInsets) { return safeInsets.clone(); } }
```

Manifest: `android:windowSoftInputMode="adjustResize"` and `configChanges` including
`orientation|screenSize|screenLayout|density|keyboardHidden` so rotation resizes instead of
recreating the activity.

## Native (once every few frames)

```cpp
bool javaSafeInsets(android_app* app, int out[4])
{
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    bool ok = false;
    jobject activity = app->activity->clazz;
    if (jmethodID id = env->GetMethodID(env->GetObjectClass(activity), "getSafeInsets", "()[I")) {
        auto array = static_cast<jintArray>(env->CallObjectMethod(activity, id));
        if (array && env->GetArrayLength(array) == 4) {
            jint v[4];
            env->GetIntArrayRegion(array, 0, 4, v);
            ok = v[0] >= 0;
            for (int i = 0; i < 4; ++i) out[i] = v[i];
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    app->activity->vm->DetachCurrentThread();
    return ok;
}
// Every 8 frames: if javaSafeInsets() succeeded use it, else derive from app->contentRect.
```

The UI then places its root window at `viewport.pos + (left, top)` with size
`viewport.size - (left + right, top + bottom)`. Floating windows are clamped into the same rect.

## Rotation

Nothing special: the window resizes, the UI reads the new size every frame (ImGui's Android backend
does), the layout is re-chosen, the insets listener fires.

## Checks on a device

- 3-button navigation and gesture navigation, portrait and landscape (the bar moves to a side).
- A phone with a notch/cutout, landscape both ways.
- Keyboard open: the focused field stays visible.
