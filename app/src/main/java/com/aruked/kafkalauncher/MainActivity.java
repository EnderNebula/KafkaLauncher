package com.aruked.kafkalauncher;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public final class MainActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable refreshTask = new Runnable() {
        @Override
        public void run() {
            refreshReport();
            handler.postDelayed(this, 1000);
        }
    };
    private TextView reportView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(createContentView());
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.removeCallbacks(refreshTask);
        refreshTask.run();
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refreshTask);
        super.onPause();
    }

    private View createContentView() {
        int padding = dp(16);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(padding, padding, padding, padding);
        page.setBackgroundColor(Color.rgb(245, 247, 246));

        TextView title = new TextView(this);
        title.setText("KafkaLauncher");
        title.setTextSize(20);
        title.setTextColor(Color.rgb(20, 30, 27));
        title.setPadding(0, 0, 0, dp(12));
        page.addView(title);

        Button launch = new Button(this);
        launch.setText("Launch Minecraft");
        launch.setOnClickListener(view -> launchMinecraft());
        page.addView(launch, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(52)));

        reportView = new TextView(this);
        reportView.setTextSize(12);
        reportView.setTextColor(Color.rgb(28, 35, 33));
        reportView.setTextIsSelectable(true);
        reportView.setTypeface(android.graphics.Typeface.MONOSPACE);
        reportView.setPadding(0, dp(12), 0, dp(24));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(reportView);
        page.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        return page;
    }

    private void launchMinecraft() {
        ProbeReport.reset(this);
        ProbeReport.append(this, "[START] Requesting Minecraft task");
        try {
            if (!MinecraftPathSymlink.hasAllFilesAccess()) {
                ProbeReport.append(this, "[WARN] All files access is required for storage bridge");
                Intent settings = new Intent(
                        Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivity(settings);
                refreshReport();
                return;
            }
            if (!prepareStorageBridge()) {
                ProbeReport.append(this, "[ERROR] Storage bridge initialization failed; Minecraft not started");
                refreshReport();
                return;
            }
            Intent intent = new Intent();
            intent.setClassName(getPackageName(), "com.mojang.minecraftpe.MainActivity");
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
            ProbeReport.append(this, "[OK] Minecraft task started");
        } catch (Throwable error) {
            ProbeReport.append(this, "[ERROR] Could not start Minecraft: " + error);
        }
        refreshReport();
    }

    private boolean prepareStorageBridge() {
        if (!MinecraftPathSymlink.hasAllFilesAccess()) {
            ProbeReport.append(this, "[WARN] All files access is required for storage bridge");
            return false;
        }
        boolean installed = MinecraftPathSymlink.install(this);
        refreshReport();
        return installed;
    }

    private void refreshReport() {
        if (reportView != null) {
            reportView.setText(ProbeReport.read(this));
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
