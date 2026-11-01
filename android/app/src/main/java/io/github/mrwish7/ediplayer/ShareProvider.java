package io.github.mrwish7.ediplayer;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.net.URLConnection;

/**
 * Lends the files MainActivity.shareImage() puts in cache/share to the app the user shares
 * them with (read only, through the permission the share intent grants).
 */
public class ShareProvider extends ContentProvider {
    static File dir(Context context) {
        return new File(context.getCacheDir(), "share");
    }

    static Uri uriFor(Context context, String name) {
        return Uri.parse("content://" + context.getPackageName() + ".share/" + Uri.encode(name));
    }

    private File file(Uri uri) {
        // only names directly in the folder
        return new File(dir(getContext()), new File(uri.getLastPathSegment()).getName());
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        return URLConnection.guessContentTypeFromName(uri.getLastPathSegment());
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    // name and size, which receiving apps ask for
    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File f = file(uri);
        MatrixCursor c = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
        if (f.exists()) c.addRow(new Object[] {f.getName(), f.length()});
        return c;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
