package io.github.wylew.albw;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;

/** Start screen: Play, or Choose ROM (copied into the app's folder as rom.3ds, where the native
 *  side looks for it: native/runtime/entry.c). Works with a gamepad (D-pad + A) or touch. */
public class LauncherActivity extends Activity {
    private static final int PICK_ROM = 1;
    /** SHA-1 of the decrypted European .3ds the port is built for (README.md) */
    private static final String EU_SHA1 = "4d2da91fa0edef9eef07e471527461396cc3faf2";

    private TextView status;
    private Button play, choose;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile boolean copying;

    private File dataDir() {
        File d = getExternalFilesDir(null);
        return d != null ? d : getFilesDir();
    }

    private File romFile() { return new File(dataDir(), "rom.3ds"); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(16, 24, 20));
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextColor(Color.rgb(232, 220, 170));
        title.setTextSize(28);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.rgb(200, 200, 190));
        status.setTextSize(14);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, pad / 2, 0, pad);
        root.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        play = new Button(this);
        play.setText("Play");
        play.setOnClickListener(v -> startGame());
        choose = new Button(this);
        choose.setText("Choose ROM");
        choose.setOnClickListener(v -> pickRom());
        row.addView(play);
        row.addView(choose);
        root.addView(row);

        setContentView(root);
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!copying) refresh();
    }

    private void refresh() {
        boolean have = romFile().isFile();
        play.setEnabled(have);
        String where = dataDir().getAbsolutePath();
        if (have) {
            status.setText("ROM ready (" + (romFile().length() >> 20) + " MiB).\n"
                    + "Saves, settings and albw.log: " + where + "\n"
                    + "In game: Start = map / items screen, Back or L3+R3 = menu");
            play.requestFocus();
        } else {
            status.setText("Choose your decrypted European .3ds dump (A Link Between Worlds).\n"
                    + "It is copied to " + where + "/rom.3ds");
            choose.requestFocus();
        }
    }

    private void startGame() {
        if (!romFile().isFile()) return;
        startActivity(new Intent(this, GameActivity.class));
    }

    private void pickRom() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_ROM);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != PICK_ROM || res != RESULT_OK || data == null || data.getData() == null) return;
        copyRom(data.getData());
    }

    private void copyRom(Uri uri) {
        copying = true;
        play.setEnabled(false);
        choose.setEnabled(false);
        status.setText("Copying the ROM...");
        new Thread(() -> {
            File tmp = new File(dataDir(), "rom.3ds.part");
            String msg;
            try (InputStream in = getContentResolver().openInputStream(uri);
                 FileOutputStream out = new FileOutputStream(tmp)) {
                if (in == null) throw new java.io.IOException("cannot open the file");
                MessageDigest sha = MessageDigest.getInstance("SHA-1");
                byte[] buf = new byte[1 << 20];
                long done = 0, lastShown = 0;
                for (int n; (n = in.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                    sha.update(buf, 0, n);
                    done += n;
                    if (done - lastShown >= (64L << 20)) {
                        lastShown = done;
                        final long mb = done >> 20;
                        ui.post(() -> status.setText("Copying the ROM... " + mb + " MiB"));
                    }
                }
                out.getFD().sync();
                StringBuilder hex = new StringBuilder();
                for (byte b : sha.digest()) hex.append(String.format("%02x", b));
                File rom = romFile();
                if (rom.exists() && !rom.delete()) throw new java.io.IOException("cannot replace " + rom);
                if (!tmp.renameTo(rom)) throw new java.io.IOException("cannot rename " + tmp);
                msg = hex.toString().equals(EU_SHA1) ? null
                        : "Copied, but this is not the European ROM the port is built for (SHA-1 " + hex
                          + "). It may not start.";
            } catch (Exception e) {
                tmp.delete();
                msg = "Copy failed: " + e.getMessage();
            }
            final String m = msg;
            ui.post(() -> {
                copying = false;
                choose.setEnabled(true);
                refresh();
                if (m != null) status.setText(m + "\n\n" + status.getText());
            });
        }).start();
    }
}
