package io.github.mrwish7.ediplayer;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.DisplayCutout;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.window.OnBackInvokedDispatcher;

import java.nio.charset.StandardCharsets;

/**
 * The app's activity: a NativeActivity running libediplayer.so (src/gui/main_android.cpp),
 * plus what native code can't do on its own: text entry, the clipboard, the system bar
 * insets and the Back gesture. Strings to and from native code are UTF-8 byte arrays.
 */
public class MainActivity extends NativeActivity {
    static {
        // NativeActivity loads it too, but this makes the native methods below resolvable
        System.loadLibrary("ediplayer");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Draw behind the system bars on every version (Android 15 enforces it); the
        // native UI keeps clear of them using the insets
        Window w = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
        } else {
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
        w.getDecorView().setOnApplyWindowInsetsListener((v, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                nativeInsets(i.left, i.top, i.right, i.bottom);
            } else {
                int l = insets.getSystemWindowInsetLeft(), t = insets.getSystemWindowInsetTop();
                int r = insets.getSystemWindowInsetRight(), b = insets.getSystemWindowInsetBottom();
                DisplayCutout c = insets.getDisplayCutout();
                if (c != null) {
                    l = Math.max(l, c.getSafeInsetLeft());
                    t = Math.max(t, c.getSafeInsetTop());
                    r = Math.max(r, c.getSafeInsetRight());
                    b = Math.max(b, c.getSafeInsetBottom());
                }
                nativeInsets(l, t, r, b);
            }
            return insets;
        });

        // Back: the native UI closes a popup, or else sends the app to the background
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, MainActivity::nativeBack);
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {   // Android 10 to 12
        nativeBack();
    }

    // ---- called from native code (android_main's thread)

    /** Ask for a line of text; the result goes to nativeTextResult() unless cancelled. */
    public void showTextInput(byte[] key, byte[] title, byte[] hint, byte[] text) {
        runOnUiThread(() -> {
            EditText edit = new EditText(this);
            edit.setSingleLine();
            edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
            edit.setImeOptions(EditorInfo.IME_ACTION_DONE);
            edit.setHint(utf8(hint));
            edit.setText(utf8(text));
            edit.setSelection(edit.length());
            FrameLayout frame = new FrameLayout(this);
            frame.setPadding(dp(20), dp(8), dp(20), 0);
            frame.addView(edit);

            AlertDialog dialog = new AlertDialog.Builder(this)
                    .setTitle(utf8(title))
                    .setView(frame)
                    .setPositiveButton(android.R.string.ok, (d, which) ->
                            nativeTextResult(key, edit.getText().toString().getBytes(StandardCharsets.UTF_8)))
                    .setNegativeButton(android.R.string.cancel, null)
                    .create();
            dialog.setCanceledOnTouchOutside(false);   // a stray tap shouldn't lose the typing
            edit.setOnEditorActionListener((v, actionId, event) -> {
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
                return true;
            });
            dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            dialog.show();
            edit.requestFocus();
        });
    }

    public void setClipboardText(byte[] text) {
        runOnUiThread(() -> getSystemService(ClipboardManager.class)
                .setPrimaryClip(ClipData.newPlainText("EDIplayer", utf8(text))));
    }

    public void goToBackground() {
        runOnUiThread(() -> moveTaskToBack(true));
    }

    private static String utf8(byte[] b) {
        return new String(b, StandardCharsets.UTF_8);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private static native void nativeTextResult(byte[] key, byte[] text);
    private static native void nativeInsets(int left, int top, int right, int bottom);
    private static native void nativeBack();
}
