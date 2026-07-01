package com.aruked.kafkalauncher;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.res.loader.ResourcesLoader;
import android.content.res.loader.ResourcesProvider;
import android.os.ParcelFileDescriptor;

import java.io.File;

/**
 * Kept separate so Android 9 and 10 never have to verify API 30 resource
 * loader types while loading KafkaApplication.
 */
final class MinecraftResourceInstaller {
    private MinecraftResourceInstaller() {
    }

    static Object install(Context context, String packageName) throws Exception {
        ApplicationInfo info = context.getPackageManager().getApplicationInfo(packageName, 0);
        ParcelFileDescriptor apkFd = ParcelFileDescriptor.open(
                new File(info.sourceDir),
                ParcelFileDescriptor.MODE_READ_ONLY);
        ResourcesProvider provider = ResourcesProvider.loadFromApk(apkFd);
        ResourcesLoader loader = new ResourcesLoader();
        loader.addProvider(provider);
        context.getResources().addLoaders(loader);
        return new Object[] {apkFd, provider, loader};
    }
}
