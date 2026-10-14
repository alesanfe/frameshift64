package com.jrickey.battleship;

import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;

import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

import org.libsdl.app.SDLActivity;

/**
 * BattleShipActivity — entry point for the SSB64 PC port on Android.
 *
 * Subclasses SDLActivity (vendored at android/app/src/main/java/org/libsdl/app/
 * from SDL release-2.32.10). SDLActivity:
 *   1. Calls getLibraries() to know which .so files to System.loadLibrary
 *      (in order). The last one is treated as "the main library" and SDL
 *      dlopens it + dlsym's the entry function.
 *   2. Looks up getMainSharedObject() — defaults to libmain.so in our
 *      nativeLibraryDir, which is exactly what we ship.
 *   3. Looks up getMainFunction() — defaults to "SDL_main", which our
 *      port.cpp now exports thanks to SDL_main.h's `#define main SDL_main`
 *      on Android.
 *   4. Invokes nativeRunMain(library, function, argv) on a dedicated thread.
 *
 * That's it. All the GLES surface, audio, controller, IME, and lifecycle
 * plumbing is handled by SDLActivity itself.
 */
public class BattleShipActivity extends SDLActivity {

    @Override
    protected String[] getLibraries() {
        // Order matters: SDL2 first (so its symbols are visible when libmain
        // resolves DT_NEEDED), then libmain (the one with SDL_main).
        return new String[] {
            "SDL2",
            "main",
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Orientation is forced landscape via setRequestedOrientation() below,
        // which intercepts SDLActivity's own request — no explicit call here.
        super.onCreate(savedInstanceState);
        // Lay the touch overlay on top of SDL's GLES surface. Has to come
        // after super.onCreate so SDLActivity has already installed its
        // surface as the root content view.
        TouchOverlay.install(this);
    }

    /**
     * Lock the game to landscape.
     *
     * The manifest pins this activity to {@code landscape}, but SDLActivity
     * re-asserts orientation at runtime from {@code setOrientationBis()}. Our
     * SDL window is resizable (for wide-aspect presentation), and SSB64 sets no
     * {@code SDL_HINT_ORIENTATIONS}, so SDL hits the "resizable + no hint" branch
     * and requests {@link ActivityInfo#SCREEN_ORIENTATION_FULL_USER} — which lets
     * the device's physical rotation win and overrides the manifest. On a
     * portrait-held device the game then renders portrait with the landscape
     * touch overlay squished.
     *
     * SSB64 is landscape-only, so coerce every orientation request (SDL's and
     * any system call) to sensor-landscape. SENSOR_LANDSCAPE keeps the window
     * wide (preserving the wide-aspect/letterbox behavior) while allowing either
     * landscape facing.
     */
    @Override
    public void setRequestedOrientation(int requestedOrientation) {
        super.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    /**
     * Route the hardware buttons that should open/close the in-game (LUS)
     * menu to {@link TouchOverlay#toggleMenu()}.
     *
     * With the on-screen hamburger auto-hidden whenever a physical controller
     * is active (see TouchOverlay.refreshOverlayVisibility), a handheld like
     * the Odin 2 otherwise has no way into the menu — this port binds no
     * gamepad button to the menu by default. We wire two:
     *
     *   - Android Back button   (KEYCODE_BACK)
     *   - Gamepad Select button (KEYCODE_BUTTON_SELECT)
     *
     * dispatchKeyEvent is the Activity's earliest key hook, so we see these
     * before SDL/LUS. We toggle on the initial ACTION_DOWN (repeatCount 0)
     * and swallow both DOWN and UP so the press isn't also delivered to the
     * game. toggleMenu() handles open and close, so the same button dismisses
     * the menu.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        final int keyCode = event.getKeyCode();
        final boolean isMenuKey =
               keyCode == KeyEvent.KEYCODE_BACK
            || keyCode == KeyEvent.KEYCODE_BUTTON_SELECT;

        // Diagnostic: log gamepad button-downs so an unexpected Select
        // mapping (pads vary) can be identified from `adb logcat -s ssb64.keys`.
        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0
            && (isMenuKey
                || (event.getSource() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)) {
            Log.i("ssb64.keys", "keyDown code=" + keyCode
                + " (" + KeyEvent.keyCodeToString(keyCode) + ") menuKey=" + isMenuKey);
        }

        if (isMenuKey) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                try {
                    TouchOverlay.toggleMenu();
                } catch (UnsatisfiedLinkError e) {
                    // Native side not up yet (pre-SDL_main) — ignore this press.
                }
            }
            return true; // consume DOWN and UP
        }
        return super.dispatchKeyEvent(event);
    }

    /**
     * Blocking HTTP(S) GET for the native shader-pack downloader
     * (src/platform/http_download.cpp calls in over JNI). Runs on the
     * caller's worker thread — never the UI thread. Android apps can't
     * exec curl, so the platform HTTP stack is the conforming backend;
     * the desktop builds shell out to `curl` instead.
     *
     * Returns true only on a 2xx response fully written to destPath.
     * A failed attempt deletes the partial file so callers never see a
     * torn payload — same contract as the curl `-f` path on desktop.
     */
    public boolean httpDownloadToFile(String url, String destPath) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setInstanceFollowRedirects(true);
            conn.setConnectTimeout(15_000);
            conn.setReadTimeout(60_000);
            conn.setRequestProperty("User-Agent", "BattleShip-Android");
            final int code = conn.getResponseCode();
            if (code < 200 || code >= 300) {
                Log.w("ssb64.http", "download " + url + " -> HTTP " + code);
                return false;
            }
            try (InputStream in = conn.getInputStream();
                 FileOutputStream out = new FileOutputStream(destPath)) {
                byte[] buf = new byte[64 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            }
            return true;
        } catch (Exception e) {
            Log.w("ssb64.http", "download failed: " + url, e);
            new java.io.File(destPath).delete();
            return false;
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    @Override
    protected void onDestroy() {
        // Unregister the InputManager listener and stop the menu poller —
        // both hold strong references into this Activity's view hierarchy
        // and would leak it (and keep ticking) across recreations.
        TouchOverlay.uninstall();
        super.onDestroy();
    }
}
