package io.github.mrwish7.ediplayer;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.MediaStore;
import android.provider.Settings;
import android.text.InputType;
import android.util.Log;
import android.view.DisplayCutout;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.webkit.MimeTypeMap;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.Toast;
import android.window.OnBackInvokedDispatcher;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * The app's activity: a NativeActivity running libediplayer.so (src/gui/main_android.cpp),
 * plus what native code can't do on its own: text entry, the clipboard, the system bar
 * insets, the Back gesture, and passing what is playing on to PlaybackService. Strings to
 * and from native code are UTF-8 byte arrays.
 */
public class MainActivity extends NativeActivity {
    private static final String TAG = "EDIplayer";

    static {
        // NativeActivity loads it too, but this makes the native methods below resolvable
        System.loadLibrary("ediplayer");
    }

    private boolean resumed;   // in front, so it may show dialogs
    private Bitmap artwork;    // the last artwork from native code

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

        // recordings left in the app's folder when it was closed while moving them
        final File recordings = new File(getExternalFilesDir(null), "EDIplayer_audio");
        final Context app = getApplicationContext();
        new Thread(() -> {
            File[] left = recordings.listFiles();
            if (left != null) for (File f : left) moveToDownloads(app, f);
        }).start();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {   // Android 10 to 12
        nativeBack();
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
    }

    @Override
    protected void onPause() {
        resumed = false;
        super.onPause();
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

    /** Share an image (file contents) through the share sheet, or save it to Pictures/EDIplayer */
    public void shareImage(byte[] data, byte[] name, boolean save) {
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(data, 0, data.length, o);
        final String type = o.outMimeType != null ? o.outMimeType : "image/jpeg";
        final String file = utf8(name).replaceAll("[\\\\/:*?\"<>|]", "_") + "."
                + MimeTypeMap.getSingleton().getExtensionFromMimeType(type);
        final Context app = getApplicationContext();
        new Thread(() -> {
            try {
                if (save) {
                    saveToMediaStore(app, MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY),
                            Environment.DIRECTORY_PICTURES + "/EDIplayer", file, type, new ByteArrayInputStream(data));
                    toast(app, "Saved to Pictures/EDIplayer");
                    return;
                }
                File dir = ShareProvider.dir(app);
                File[] old = dir.listFiles();
                if (old != null) for (File f : old) f.delete();   // keep only the latest
                dir.mkdirs();
                try (FileOutputStream out = new FileOutputStream(new File(dir, file))) {
                    out.write(data);
                }
                Uri uri = ShareProvider.uriFor(app, file);
                Intent send = new Intent(Intent.ACTION_SEND)
                        .setType(type)
                        .putExtra(Intent.EXTRA_STREAM, uri)
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                send.setClipData(ClipData.newRawUri(file, uri));   // the share sheet's preview
                runOnUiThread(() -> startActivity(Intent.createChooser(send, null)));
            } catch (IOException e) {
                Log.w(TAG, "image: " + e);
                toast(app, save ? "Cannot save the image" : "Cannot share the image");
            }
        }).start();
    }

    /** A recording has finished: move it from the app's folder to Download/EDIplayer */
    public void publishRecording(byte[] path) {
        final File f = new File(utf8(path));
        final Context app = getApplicationContext();
        new Thread(() -> {
            if (moveToDownloads(app, f)) toast(app, "Recording saved to Download/EDIplayer");
        }).start();
    }

    private static boolean moveToDownloads(Context context, File f) {
        String name = f.getName();
        String ext = name.substring(name.lastIndexOf('.') + 1).toLowerCase(java.util.Locale.ROOT);
        String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
        try (InputStream in = new FileInputStream(f)) {
            saveToMediaStore(context, MediaStore.Downloads.EXTERNAL_CONTENT_URI,
                    Environment.DIRECTORY_DOWNLOADS + "/EDIplayer", f.getName(),
                    type != null ? type : "application/octet-stream", in);
        } catch (IOException e) {
            Log.w(TAG, "recording: cannot move " + f + " to Download: " + e);
            return false;
        }
        f.delete();
        return true;
    }

    /**
     * Copy into a MediaStore collection, in a folder of the shared storage. The type must
     * be the one Android maps the name's extension to, or it renames the file.
     */
    private static void saveToMediaStore(Context context, Uri collection, String folder, String name, String type,
                                         InputStream in) throws IOException {
        ContentResolver resolver = context.getContentResolver();
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        v.put(MediaStore.MediaColumns.MIME_TYPE, type);
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, folder);
        v.put(MediaStore.MediaColumns.IS_PENDING, 1);   // hidden until complete
        Uri item = resolver.insert(collection, v);
        if (item == null) throw new IOException("cannot create " + folder + "/" + name);
        try (OutputStream out = resolver.openOutputStream(item)) {
            if (out == null) throw new IOException("cannot write " + name);
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        } catch (IOException e) {
            resolver.delete(item, null, null);
            throw e;
        }
        v.clear();
        v.put(MediaStore.MediaColumns.IS_PENDING, 0);
        resolver.update(item, v, null, null);
    }

    private static void toast(Context context, String text) {
        new Handler(Looper.getMainLooper()).post(() -> Toast.makeText(context, text, Toast.LENGTH_SHORT).show());
    }

    /** What is playing changed: pass it on to PlaybackService. art: null if unchanged. */
    public void updateMedia(boolean playing, boolean multicast, byte[] station, byte[] title, byte[] artist,
                            byte[] album, byte[] art) {
        final Bitmap bitmap = art == null ? null : art.length > 0 ? BitmapFactory.decodeByteArray(art, 0, art.length) : null;
        runOnUiThread(() -> {
            PlaybackService.State s = new PlaybackService.State();
            s.playing = playing;
            s.multicast = multicast;
            s.station = utf8(station);
            s.title = utf8(title);
            s.artist = utf8(artist);
            s.album = utf8(album);
            if (art != null) artwork = bitmap;
            s.art = artwork;
            PlaybackService.update(this, s);
            if (playing && resumed) askBatteryOnce();
        });
    }

    /**
     * Once, the first time something plays: ask to be left out of battery optimisation,
     * which on some phones stops playback in the background despite the foreground service.
     */
    private void askBatteryOnce() {
        SharedPreferences prefs = getPreferences(MODE_PRIVATE);
        if (prefs.getBoolean("batteryAsked", false)
                || getSystemService(PowerManager.class).isIgnoringBatteryOptimizations(getPackageName())) {
            return;
        }
        prefs.edit().putBoolean("batteryAsked", true).apply();
        new AlertDialog.Builder(this)
                .setTitle("Playing in the background")
                .setMessage("Some phones stop apps playing in the background to save battery. To keep "
                        + "EDIplayer playing reliably with the screen off or while you use other apps, "
                        + "allow it to run without battery restrictions.")
                .setPositiveButton("Allow", (d, which) -> {
                    try {
                        startActivity(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                Uri.parse("package:" + getPackageName())));
                    } catch (ActivityNotFoundException e) {
                        startActivity(new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
                    }
                })
                .setNegativeButton("Not now", null)
                .show();
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
    /** A PlaybackService.CMD_* command for the player */
    static native void nativeMediaCommand(int cmd);
}
