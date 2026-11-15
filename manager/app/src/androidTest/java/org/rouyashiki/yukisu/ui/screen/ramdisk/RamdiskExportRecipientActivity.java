package org.rouyashiki.yukisu.ui.screen.ramdisk;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/** Separate test-APK UID; uses only platform classes outside the instrumentation classloader. */
public class RamdiskExportRecipientActivity extends Activity {
    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Uri uri = getIntent().getData();
        byte[] bytes = null;
        try (InputStream input = getContentResolver().openInputStream(uri)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            bytes = output.toByteArray();
        } catch (Exception ignored) { }
        boolean writeDenied = false;
        try (OutputStream ignored = getContentResolver().openOutputStream(uri, "w")) {
            // A successful open is a test failure, even without writing anything.
        } catch (Exception expected) { writeDenied = true; }
        Intent result = new Intent("org.rouyashiki.yukisu.ui.screen.ramdisk.EXPORT_RECEIVED");
        result.setPackage(getIntent().getStringExtra("resultPackage"));
        result.putExtra("bytes", bytes);
        result.putExtra("writeDenied", writeDenied);
        result.putExtra("uid", android.os.Process.myUid());
        sendBroadcast(result);
        finish();
    }
}
