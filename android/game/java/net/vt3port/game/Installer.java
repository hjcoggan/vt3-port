package net.vt3port.game;

import android.app.Activity;
import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Launcher. On first run the player picks their PC disc image and the v1.0.1 VT3.exe; this unpacks
 * the game data and builds image.bin in the app's files dir (the VT3_ANDROID_ROOT the game reads),
 * then writes install.ok. Every later launch goes straight to the game.
 */
public class Installer extends Activity {
    static { System.loadLibrary("vt3install"); }

    static native int extractIso(int fd, String outDir);
    static native int progress(); /* permille of cabinet files done */
    static native byte[] flattenPe(byte[] exe);

    /* SHA-256 of the image built from the v1.0.1 VT3.exe the port is translated from. */
    static final String IMAGE_SHA256 = "52be3cda5edb9aa536394fed656e22270d688d74e86db8602fd39a7512fc80bf";
    static final int PICK_ISO = 1, PICK_ZIP = 2;

    File root;
    Uri iso, zip;
    Button isoButton, zipButton, installButton;
    TextView status;
    final Handler ui = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        root = getExternalFilesDir(null);
        if (new File(root, "install.ok").exists()) {
            launch();
            return;
        }
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setPadding(48, 48, 48, 48);
        TextView title = new TextView(this);
        title.setText("Virtua Tennis 3 setup");
        title.setTextSize(24);
        box.addView(title);
        isoButton = button(box, "1. Select PC disc image (.iso)", PICK_ISO);
        zipButton = button(box, "2. Select v1.0.1 VT3.exe (or a .zip holding it)", PICK_ZIP);
        installButton = new Button(this);
        installButton.setText("Install");
        installButton.setEnabled(false);
        installButton.setOnClickListener(v -> install());
        box.addView(installButton);
        status = new TextView(this);
        status.setText("Needs about 4.5 GB free. The files stay on this device; uninstalling removes them.");
        box.addView(status);
        setContentView(box);
    }

    Button button(LinearLayout box, String label, int code) {
        Button b = new Button(this);
        b.setText(label);
        b.setOnClickListener(v -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), code));
        box.addView(b);
        return b;
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        if (result != RESULT_OK || data == null) return;
        if (code == PICK_ISO) { iso = data.getData(); isoButton.setText("1. Disc image selected"); }
        if (code == PICK_ZIP) { zip = data.getData(); zipButton.setText("2. VT3.exe selected"); }
        installButton.setEnabled(iso != null && zip != null);
    }

    void install() {
        isoButton.setEnabled(false);
        zipButton.setEnabled(false);
        installButton.setEnabled(false);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            try {
                say("Checking VT3.exe...");
                buildImage();
                Runnable[] tick = new Runnable[1];
                tick[0] = () -> { status.setText("Unpacking the disc... " + progress() / 10 + "%"); ui.postDelayed(tick[0], 500); };
                ui.post(tick[0]);
                int rc;
                try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(iso, "r")) {
                    rc = extractIso(pfd.getFd(), new File(root, "data2").getPath());
                } finally {
                    ui.removeCallbacks(tick[0]);
                }
                if (rc == -1) throw new IOException("That file is not a disc image (.iso).");
                if (rc == -2) throw new IOException("No Virtua Tennis 3 PC install files on that disc image.");
                if (rc < 0) throw new IOException("Unpacking failed. Check there is about 4.5 GB free.");
                new FileOutputStream(new File(root, "install.ok")).close();
                ui.post(this::launch);
            } catch (Exception e) {
                ui.post(() -> {
                    status.setText("Setup failed: " + e.getMessage());
                    isoButton.setEnabled(true);
                    zipButton.setEnabled(true);
                    installButton.setEnabled(true);
                });
            }
        }).start();
    }

    /* VT3.exe may be picked bare or inside a zip. */
    void buildImage() throws Exception {
        byte[] exe;
        try (InputStream in = getContentResolver().openInputStream(zip)) {
            exe = readAll(in);
        }
        if (exe.length > 1 && exe[0] == 'P' && exe[1] == 'K') {
            byte[] found = null;
            try (ZipInputStream z = new ZipInputStream(new java.io.ByteArrayInputStream(exe))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; )
                    if (e.getName().toLowerCase().endsWith("vt3.exe")) { found = readAll(z); break; }
            }
            if (found == null) throw new IOException("No VT3.exe in that zip.");
            exe = found;
        }
        byte[] img = flattenPe(exe);
        if (img == null || !IMAGE_SHA256.equals(hex(MessageDigest.getInstance("SHA-256").digest(img))))
            throw new IOException("That VT3.exe is not the v1.0.1 build this port needs.");
        File tmp = new File(root, "image.bin.tmp");
        try (OutputStream o = new FileOutputStream(tmp)) { o.write(img); }
        if (!tmp.renameTo(new File(root, "image.bin"))) throw new IOException("Could not write image.bin.");
    }

    void copyAsset(String name, File dst) throws IOException {
        try (InputStream in = getAssets().open(name); OutputStream o = new FileOutputStream(dst)) {
            byte[] b = new byte[1 << 16];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
        }
    }

    void launch() {
        /* An update can ship a new shader pack (it is keyed to the shader translator), so refresh it. */
        try {
            long stamp = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            File mark = new File(root, "shaders.stamp");
            String want = Long.toString(stamp);
            if (!mark.exists() || !want.equals(new String(readAll(new java.io.FileInputStream(mark))))) {
                copyAsset("shaders.pack", new File(root, "shaders.pack"));
                try (OutputStream o = new FileOutputStream(mark)) { o.write(want.getBytes()); }
            }
        } catch (Exception ignored) {
        }
        startActivity(new Intent(this, NativeActivity.class));
        finish();
    }

    void say(String s) { ui.post(() -> status.setText(s)); }

    static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[1 << 16];
        for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
        in.close();
        return o.toByteArray();
    }

    static String hex(byte[] d) {
        StringBuilder s = new StringBuilder();
        for (byte x : d) s.append(String.format("%02x", x));
        return s.toString();
    }
}
