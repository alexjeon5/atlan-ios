// SPDX-License-Identifier: GPL-2.0-or-later
package atlan.host;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.os.Build;
import android.os.Bundle;
import android.os.Looper;
import android.os.Process;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

// The window a guest session draws in: a full-screen SurfaceView whose surface the native side presents GL frames
// on, with the activity's touches, GPS fixes and network state passed to the guest. One session per process; it
// starts once the permissions are settled, the view has a size and the app's prepare() and dataRoot() agree, and it
// ends with the process. Subclasses (one per app) say where the app's data is.
public abstract class GuestActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("atlanhost");
    }

    private static final int PERMISSION_REQUEST = 1;
    private SurfaceView view;
    private int viewWidth, viewHeight;
    private boolean permissionsSettled, started;
    private LocationManager locations;
    private LocationListener locationListener;
    private ConnectivityManager connectivity;
    private ConnectivityManager.NetworkCallback networkCallback;

    // The folder the app's data lies in, or null when it cannot start (the subclass has told the user why).
    protected abstract String dataRoot();

    // Runtime permissions the app needs besides location.
    protected String[] extraPermissions() {
        return new String[0];
    }

    // Whether the session can start now. False when the subclass sent the user elsewhere first (asked again on
    // every resume).
    protected boolean prepare() {
        return true;
    }

    // Input bindings text (shell/input.hpp): DATA_ROOT/input.cfg, else the APK's assets/input.cfg, else none.
    protected String inputBindings(String root) {
        byte[] data = readFile(new File(root, "input.cfg"));
        if (data == null) data = readAsset("input.cfg");
        return data == null ? "" : new String(data, java.nio.charset.StandardCharsets.UTF_8);
    }

    // Native replacement signature file: DATA_ROOT/native.sig, else the APK's assets/native.sig, else none.
    protected String nativeSignatures(String root) {
        File file = new File(root, "native.sig");
        if (file.isFile()) return file.getPath();
        byte[] data = readAsset("native.sig");
        if (data == null) return "";
        File copy = new File(getFilesDir(), "native.sig");
        try (OutputStream out = new FileOutputStream(copy)) {
            out.write(data);
        } catch (Exception e) {
            return "";
        }
        return copy.getPath();
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON | WindowManager.LayoutParams.FLAG_FULLSCREEN);
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        view.setOnTouchListener((v, event) -> touch(event));
        setContentView(view);
        hideSystemBars();
        List<String> missing = new ArrayList<>();
        List<String> wanted = new ArrayList<>();
        wanted.add(Manifest.permission.ACCESS_FINE_LOCATION);
        for (String p : extraPermissions()) wanted.add(p);
        for (String p : wanted)
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
        if (missing.isEmpty()) settlePermissions();
        else requestPermissions(missing.toArray(new String[0]), PERMISSION_REQUEST);
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        if (request == PERMISSION_REQUEST) settlePermissions();
    }

    private void settlePermissions() {
        permissionsSettled = true;
        startLocation();
        tryStart();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        tryStart();
    }

    @Override
    public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) hideSystemBars();
    }

    private void tryStart() {
        if (started || !permissionsSettled || viewWidth == 0 || viewHeight == 0) return;
        if (!prepare()) return;
        String root = dataRoot();
        if (root == null) return;
        started = true;
        startNetwork();
        File logs = getExternalFilesDir("logs");
        nativeStart(this, root, logs == null ? "" : logs.getPath(), inputBindings(root), nativeSignatures(root), viewWidth, viewHeight);
    }

    // The guest chose to quit (its own exit dialog). Guest thread.
    void onGuestExit() {
        runOnUiThread(this::finish);
    }

    @Override
    protected void onDestroy() {
        stopLocation();
        stopNetwork();
        super.onDestroy();
        if (isFinishing()) {
            nativeStop();
            Process.killProcess(Process.myPid());  // a session cannot be restarted in the same process
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeSetWindow(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        viewWidth = width;
        viewHeight = height;
        nativeViewSize(width, height);
        nativeSetWindow(holder.getSurface());
        tryStart();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSetWindow(null);
    }

    private boolean touch(MotionEvent event) {
        final int action = event.getActionMasked();
        final long time = event.getEventTime();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                final int i = event.getActionIndex();
                final boolean down = action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN;
                nativeTouch(down ? 0 : 2, event.getPointerId(i), event.getX(i), event.getY(i), time);
                return true;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); ++i)
                    nativeTouch(1, event.getPointerId(i), event.getX(i), event.getY(i), time);
                return true;
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < event.getPointerCount(); ++i)
                    nativeTouch(3, event.getPointerId(i), event.getX(i), event.getY(i), time);
                return true;
            default:
                return false;
        }
    }

    private void startLocation() {
        if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) return;
        locations = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        if (locations == null) return;
        locationListener = new LocationListener() {
            @Override
            public void onLocationChanged(Location fix) {
                location(fix);
            }

            @Override
            public void onStatusChanged(String provider, int status, Bundle extras) {}

            @Override
            public void onProviderEnabled(String provider) {}

            @Override
            public void onProviderDisabled(String provider) {}
        };
        try {
            locations.requestLocationUpdates(LocationManager.GPS_PROVIDER, 0L, 0.0F, locationListener, Looper.getMainLooper());
        } catch (Exception e) {
            locationListener = null;
        }
    }

    private void stopLocation() {
        if (locations != null && locationListener != null) locations.removeUpdates(locationListener);
        locationListener = null;
    }

    private static void location(Location fix) {
        nativeLocation(fix.getLatitude(), fix.getLongitude(), fix.hasAltitude() ? fix.getAltitude() : 0.0,
                fix.hasSpeed() ? fix.getSpeed() : -1.0, fix.hasBearing() ? fix.getBearing() : -1.0,
                fix.hasAccuracy() ? fix.getAccuracy() : 0.0, fix.getTime());
    }

    private void startNetwork() {
        connectivity = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (connectivity == null) return;
        nativeNetwork(connectivity.getActiveNetwork() != null);
        networkCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                nativeNetwork(true);
            }

            @Override
            public void onLost(Network network) {
                nativeNetwork(connectivity.getActiveNetwork() != null && !network.equals(connectivity.getActiveNetwork()));
            }
        };
        try {
            connectivity.registerDefaultNetworkCallback(networkCallback);
        } catch (Exception e) {
            networkCallback = null;
        }
    }

    private void stopNetwork() {
        if (connectivity != null && networkCallback != null) connectivity.unregisterNetworkCallback(networkCallback);
        networkCallback = null;
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    protected byte[] readAsset(String name) {
        try (InputStream in = getAssets().open(name)) {
            return readAll(in);
        } catch (Exception e) {
            return null;
        }
    }

    protected static byte[] readFile(File file) {
        if (!file.isFile()) return null;
        try (InputStream in = new FileInputStream(file)) {
            return readAll(in);
        } catch (Exception e) {
            return null;
        }
    }

    private static byte[] readAll(InputStream in) throws java.io.IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[16384];
        for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
        return out.toByteArray();
    }

    private static native void nativeStart(GuestActivity activity, String dataRoot, String logDir, String bindings,
                                           String signatures, int width, int height);
    private static native void nativeStop();
    private static native void nativeSetWindow(Surface surface);
    private static native void nativeViewSize(int width, int height);
    private static native void nativeTouch(int phase, int pointer, float x, float y, long timeMs);
    private static native void nativeLocation(double latitude, double longitude, double altitude, double speed,
                                              double course, double accuracy, long timeMs);
    private static native void nativeNetwork(boolean available);
}
