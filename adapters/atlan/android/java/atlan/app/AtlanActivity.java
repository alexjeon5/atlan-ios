// SPDX-License-Identifier: GPL-2.0-or-later
package atlan.app;

import android.Manifest;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.widget.Toast;
import atlan.host.GuestActivity;
import atlan.runtime.R;
import java.io.File;

// The Atlan app on Android. Its data is the ATLAN folder the original app keeps at the top of the device's shared
// storage (or, without the all-files permission, in this app's own external folder). Back twice within two seconds
// leaves the app, as the original's back handler does.
public final class AtlanActivity extends GuestActivity {
    private static final long BACK_WINDOW_MS = 2000;
    private long lastBack;
    private Toast backToast;
    private boolean askedForStorage;

    // The app's own external folder needs no permission; shared storage needs all-files access (Android 11+) or
    // the storage permission (Android 8-10).
    private File ownRoot() {
        return getExternalFilesDir(null);
    }

    private static boolean hasAtlan(File root) {
        return root != null && new File(root, "ATLAN/APP").isDirectory();
    }

    private boolean canReadShared() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    @Override
    protected String[] extraPermissions() {
        if (Build.VERSION.SDK_INT >= 30) return new String[0];
        return new String[] {Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE};
    }

    @Override
    protected boolean prepare() {
        if (hasAtlan(ownRoot()) || canReadShared()) return true;
        if (Build.VERSION.SDK_INT >= 30 && !askedForStorage) {
            askedForStorage = true;
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
            return false;
        }
        return true;  // dataRoot() reports the missing folder
    }

    @Override
    protected String dataRoot() {
        if (hasAtlan(ownRoot())) return ownRoot().getPath();
        File shared = Environment.getExternalStorageDirectory();
        if (canReadShared() && hasAtlan(shared)) return shared.getPath();
        File own = ownRoot();
        new AlertDialog.Builder(this)
                .setMessage(getString(R.string.missing_data, shared.getPath() + "/ATLAN", (own == null ? "" : own.getPath()) + "/ATLAN"))
                .setCancelable(false)
                .setPositiveButton(android.R.string.ok, (dialog, which) -> finish())
                .show();
        return null;
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        final long now = System.currentTimeMillis();
        if (now - lastBack < BACK_WINDOW_MS) {
            if (backToast != null) backToast.cancel();
            finish();
            return;
        }
        lastBack = now;
        backToast = Toast.makeText(this, R.string.back_again, Toast.LENGTH_SHORT);
        backToast.show();
    }
}
