# Native code calls these methods through JNI method lookup.
-keep class com.aruked.kafkalauncher.KafkaApplication {
    public void onMinecraftClientEnteredWorld();
    public void onMinecraftHookError(java.lang.String);
    private static native boolean nativeInstallHooksForHostedActivity(android.app.Activity);
    private static native boolean nativeSetActivityBaseContext(android.app.Activity, android.content.Context);
}

# Android instantiates these components by class name.
-keep class com.aruked.kafkalauncher.KafkaAppComponentFactory { *; }
