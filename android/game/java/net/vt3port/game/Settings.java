package net.vt3port.game;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;

/* Reached from the launcher icon's long-press shortcut. Each setting is a marker file beside the
 * game data that android_main reads at start-up (a vt3.env entry for the same variable wins). */
public class Settings extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        File fps60 = new File(getExternalFilesDir(null), "fps60");
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setPadding(48, 48, 48, 48);
        TextView title = new TextView(this);
        title.setText("Virtua Tennis 3 settings");
        title.setTextSize(24);
        box.addView(title);
        Switch saver = new Switch(this);
        saver.setText("60 Hz battery saver (less heat and power; the game is smoothest at 120 Hz)");
        saver.setChecked(fps60.exists());
        saver.setOnCheckedChangeListener((v, on) -> {
            try {
                if (on) fps60.createNewFile(); else fps60.delete();
            } catch (IOException e) {
                v.setChecked(fps60.exists());
            }
        });
        box.addView(saver);
        TextView note = new TextView(this);
        note.setText("Applies the next time the game starts. Press Back to close.");
        box.addView(note);
        setContentView(box);
        saver.requestFocus();
    }
}
