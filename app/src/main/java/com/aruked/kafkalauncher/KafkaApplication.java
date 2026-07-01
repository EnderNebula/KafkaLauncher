package com.aruked.kafkalauncher;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicBoolean;

public final class KafkaApplication extends Application {
    private static final String TAG = "KafkaLauncher";
    private static final String MC_PACKAGE = "com.mojang.minecraftpe";
    private static KafkaApplication instance;

    static {
        System.loadLibrary("minecraftpe");
    }

    private Context minecraftContext;
    private ClassLoader minecraftClassLoader;
    private Object minecraftResourceHandles;
    private final AtomicBoolean hookInstalledReported = new AtomicBoolean();

    public KafkaApplication() {
        instance = this;
    }

    public static KafkaApplication getInstance() {
        if (instance == null) {
            throw new IllegalStateException("KafkaApplication is not available");
        }
        return instance;
    }

    @Override
    public ClassLoader getClassLoader() {
        return minecraftClassLoader != null ? minecraftClassLoader : super.getClassLoader();
    }

    @Override
    public AssetManager getAssets() {
        return minecraftContext != null ? minecraftContext.getAssets() : super.getAssets();
    }

    @Override
    public Resources getResources() {
        return minecraftContext != null ? minecraftContext.getResources() : super.getResources();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        MinecraftHookBridge.setNativeMessageSender(KafkaApplication::nativeSendMinecraftMessage);
        if (!isMinecraftProcess()) {
            return;
        }

        ProbeReport.append(this, "[START] Minecraft process initialized");
        try {
            Context context = getMinecraftContext();
            minecraftClassLoader = context.getClassLoader();
            Thread.currentThread().setContextClassLoader(minecraftClassLoader);
            initializeMinecraftApplicationClass(context, minecraftClassLoader);
            installMinecraftResources();
            installMinecraftActivityAdapter(context);
            initializeMinecraftFirebaseIfAvailable(context, minecraftClassLoader);
            ProbeReport.append(this, "[OK] Minecraft host initialized");
        } catch (Throwable error) {
            Log.e(TAG, "Could not initialize hosted Minecraft", error);
            ProbeReport.append(this, "[ERROR] Minecraft host initialization failed: " + error);
        }
    }

    private boolean isMinecraftProcess() {
        return Application.getProcessName().endsWith(":minecraft");
    }

    public Context getMinecraftContext() throws PackageManager.NameNotFoundException {
        if (minecraftContext == null) {
            minecraftContext = createPackageContext(
                    MC_PACKAGE,
                    Context.CONTEXT_IGNORE_SECURITY | Context.CONTEXT_INCLUDE_CODE);
        }
        return minecraftContext;
    }

    public ClassLoader getMinecraftClassLoader() throws PackageManager.NameNotFoundException {
        if (minecraftClassLoader == null) {
            minecraftClassLoader = getMinecraftContext().getClassLoader();
        }
        return minecraftClassLoader;
    }

    public void onMinecraftClientEnteredWorld() {
        MinecraftHookBridge.onClientEnteredWorld();
    }

    public void onMinecraftHookError(String message) {
        ProbeReport.append(this, "[ERROR] " + message);
    }

    public void onMinecraftHookInstalled(String backend) {
        if (hookInstalledReported.compareAndSet(false, true)) {
            ProbeReport.append(this, "[OK] Inline hook installed: " + backend);
        }
    }

    public void onMinecraftChatProbe(String hookName, String message) {
        if (message == null || message.isEmpty()) {
            return;
        }
        ProbeReport.append(this, formatChatHookReport(hookName, message));
        if ("chat-controller".equals(hookName)) {
            MinecraftHookBridge.onPlayerChatMessage(message);
        }
    }

    private String formatChatHookReport(String hookName, String message) {
        if ("chat-controller".equals(hookName)) {
            return "[HOOK chat-controller] " + message;
        }
        if ("model-sendChatMessage".equals(hookName)) {
            return "[HOOK sendChatMessage] " + message;
        }
        if ("model-executeCommand".equals(hookName)) {
            return "[HOOK executeCommand] " + message;
        }
        return "[HOOK " + hookName + "] " + message;
    }

    private void initializeMinecraftApplicationClass(Context context, ClassLoader classLoader)
            throws ClassNotFoundException {
        String className = context.getApplicationInfo().className;
        if (className != null && !className.isEmpty()) {
            Class.forName(className, true, classLoader);
        }
    }

    private void installMinecraftResources() throws Exception {
        if (Build.VERSION.SDK_INT >= 30) {
            minecraftResourceHandles = MinecraftResourceInstaller.install(this, MC_PACKAGE);
        }
    }

    private void installMinecraftActivityAdapter(Context context)
            throws PackageManager.NameNotFoundException {
        ActivityInfo info = context.getPackageManager().getActivityInfo(
                new android.content.ComponentName(
                        MC_PACKAGE,
                        "com.mojang.minecraftpe.MainActivity"),
                PackageManager.GET_META_DATA);
        int minecraftTheme = info.getThemeResource();

        registerActivityLifecycleCallbacks(new ActivityLifecycleCallbacks() {
            private boolean isMinecraft(Activity activity) {
                return "com.mojang.minecraftpe.MainActivity"
                        .equals(activity.getClass().getName());
            }

            private void prepare(Activity activity) {
                if (!isMinecraft(activity)) {
                    return;
                }
                if (minecraftTheme != 0) {
                    activity.setTheme(minecraftTheme);
                }
                activity.setTitle(R.string.minecraft_task_name);
                applyMinecraftTaskDescription(activity);
                installMinecraftClassLoaderContext(activity);
                installHooks(activity);
            }

            @Override
            public void onActivityPreCreated(Activity activity, Bundle state) {
                prepare(activity);
            }

            @Override
            public void onActivityCreated(Activity activity, Bundle state) {
                if (Build.VERSION.SDK_INT < 29) {
                    prepare(activity);
                } else if (isMinecraft(activity)) {
                    installHooks(activity);
                }
            }

            @Override
            public void onActivityStarted(Activity activity) {
                if (isMinecraft(activity)) {
                    installHooks(activity);
                }
            }

            @Override public void onActivityResumed(Activity activity) {}
            @Override public void onActivityPaused(Activity activity) {}
            @Override public void onActivityStopped(Activity activity) {}
            @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
            @Override
            public void onActivityDestroyed(Activity activity) {
                if (isMinecraft(activity) && activity.isFinishing()) {
                    returnToLauncherAndStopMinecraftProcess();
                }
            }
        });
    }

    private void returnToLauncherAndStopMinecraftProcess() {
        ProbeReport.append(this, "[STOP] Minecraft Activity finished");
        try {
            Intent launcher = new Intent(this, MainActivity.class);
            launcher.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                    | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT
                    | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            startActivity(launcher);
        } catch (Throwable error) {
            Log.e(TAG, "Could not return to launcher", error);
            ProbeReport.append(this, "[ERROR] Could not return to launcher: " + error);
        }
        new Handler(Looper.getMainLooper()).postDelayed(
                () -> Process.killProcess(Process.myPid()),
                300);
    }

    private void installHooks(Activity activity) {
        nativeInstallHooksForHostedActivity(activity);
    }

    private void applyMinecraftTaskDescription(Activity activity) {
        try {
            PackageManager packageManager = getPackageManager();
            ApplicationInfo appInfo = packageManager.getApplicationInfo(MC_PACKAGE, 0);
            String label = String.valueOf(packageManager.getApplicationLabel(appInfo));
            Drawable icon = packageManager.getApplicationIcon(MC_PACKAGE);
            activity.setTaskDescription(new ActivityManager.TaskDescription(
                    label,
                    drawableToBitmap(icon, activity),
                    0));
        } catch (Throwable error) {
            Log.w(TAG, "Could not apply Minecraft task description", error);
        }
    }

    private Bitmap drawableToBitmap(Drawable drawable, Context context) {
        int width = drawable.getIntrinsicWidth();
        int height = drawable.getIntrinsicHeight();
        if (width <= 0 || height <= 0) {
            int size = Math.round(48 * context.getResources().getDisplayMetrics().density);
            width = size;
            height = size;
        }
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        drawable.setBounds(0, 0, canvas.getWidth(), canvas.getHeight());
        drawable.draw(canvas);
        return bitmap;
    }

    private void installMinecraftClassLoaderContext(Activity activity) {
        Context replacement =
                new MinecraftClassLoaderContext(
                        activity.getBaseContext(),
                        minecraftContext,
                        minecraftClassLoader);
        try {
            Field field = ContextWrapper.class.getDeclaredField("mBase");
            field.setAccessible(true);
            if (!(field.get(activity) instanceof MinecraftClassLoaderContext)) {
                field.set(activity, replacement);
            }
        } catch (Throwable reflectionError) {
            if (!nativeSetActivityBaseContext(activity, replacement)) {
                Log.e(TAG, "Could not replace Minecraft Activity base context", reflectionError);
            }
        }
    }

    private void initializeMinecraftFirebaseIfAvailable(Context context, ClassLoader classLoader)
            throws Exception {
        Class<?> optionsClass;
        try {
            optionsClass = Class.forName(
                    "com.google.firebase.FirebaseOptions",
                    true,
                    classLoader);
        } catch (ClassNotFoundException ignored) {
            ProbeReport.append(this, "[OK] Minecraft Firebase unavailable; skipped");
            return;
        }

        Method fromResource = optionsClass.getMethod("fromResource", Context.class);
        Object options = fromResource.invoke(null, context);
        if (options == null) {
            ProbeReport.append(this, "[OK] Minecraft Firebase options unavailable; skipped");
            return;
        }

        Class<?> appClass;
        try {
            appClass = Class.forName("com.google.firebase.FirebaseApp", true, classLoader);
        } catch (ClassNotFoundException ignored) {
            ProbeReport.append(this, "[OK] Minecraft FirebaseApp unavailable; skipped");
            return;
        }

        Method initializeApp = appClass.getMethod(
                "initializeApp",
                Context.class,
                optionsClass);
        initializeApp.invoke(null, this, options);
    }

    private static final class MinecraftClassLoaderContext extends ContextWrapper {
        private final Context minecraftContext;
        private final ClassLoader classLoader;

        MinecraftClassLoaderContext(
                Context base,
                Context minecraftContext,
                ClassLoader classLoader) {
            super(base);
            this.minecraftContext = minecraftContext;
            this.classLoader = classLoader;
        }

        @Override
        public ClassLoader getClassLoader() {
            return classLoader;
        }

        @Override
        public AssetManager getAssets() {
            return minecraftContext.getAssets();
        }

        @Override
        public Resources getResources() {
            return minecraftContext.getResources();
        }
    }

    private static native boolean nativeInstallHooksForHostedActivity(Activity activity);

    private static native boolean nativeSetActivityBaseContext(
            Activity activity,
            Context replacement);

    private static native boolean nativeSendMinecraftMessage(String message);
}
