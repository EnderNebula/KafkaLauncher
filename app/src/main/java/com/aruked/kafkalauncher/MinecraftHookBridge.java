package com.aruked.kafkalauncher;

import android.app.Activity;
import android.widget.Toast;

public final class MinecraftHookBridge {
    private static Activity minecraftActivity;
    private static NativeMessageSender nativeMessageSender;

    private MinecraftHookBridge() {
    }

    public static void setMinecraftActivity(Activity activity) {
        minecraftActivity = activity;
    }

    public static void setNativeMessageSender(NativeMessageSender sender) {
        nativeMessageSender = sender;
    }

    public static void onClientEnteredWorld() {
        Activity activity = minecraftActivity;
        if (activity == null) {
            return;
        }
        ProbeReport.append(activity, "[HOOK] Entered world");
        activity.runOnUiThread(() ->
                Toast.makeText(activity, "pwned!", Toast.LENGTH_LONG).show());
    }

    public static void onPlayerChatMessage(String message) {
        Activity activity = minecraftActivity;
        if (activity == null || message == null) {
            return;
        }
        if (message.startsWith("!") && message.length() > 1) {
            sendMinecraftMessage(activity, reverse(message.substring(1)), true);
        }
    }

    private static void sendMinecraftMessage(Activity activity, String message, boolean reportFailure) {
        NativeMessageSender sender = nativeMessageSender;
        if ((sender == null || !sender.sendMinecraftMessage(message)) && reportFailure) {
            ProbeReport.append(activity, "[WARN] Minecraft message sender is not ready");
        }
    }

    private static String reverse(String value) {
        return new StringBuilder(value).reverse().toString();
    }

    public interface NativeMessageSender {
        boolean sendMinecraftMessage(String message);
    }
}
