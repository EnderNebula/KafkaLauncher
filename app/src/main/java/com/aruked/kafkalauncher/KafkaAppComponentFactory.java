package com.aruked.kafkalauncher;

import android.app.Activity;
import android.app.AppComponentFactory;
import android.content.Intent;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;

public final class KafkaAppComponentFactory extends AppComponentFactory {
    private static final String ORIGINAL_MAIN_ACTIVITY =
            "com.mojang.minecraftpe.MainActivity";

    @Override
    public Activity instantiateActivity(
            ClassLoader classLoader,
            String className,
            Intent intent)
            throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        if (!ORIGINAL_MAIN_ACTIVITY.equals(className)) {
            return super.instantiateActivity(classLoader, className, intent);
        }

        try {
            ClassLoader minecraftClassLoader =
                    KafkaApplication.getInstance().getMinecraftClassLoader();
            Thread.currentThread().setContextClassLoader(minecraftClassLoader);
            Class<?> originalClass = Class.forName(
                    ORIGINAL_MAIN_ACTIVITY,
                    true,
                    minecraftClassLoader);
            Constructor<?> constructor = originalClass.getDeclaredConstructor();
            constructor.setAccessible(true);
            Activity activity = (Activity) constructor.newInstance();
            MinecraftHookBridge.setMinecraftActivity(activity);
            return activity;
        } catch (ClassNotFoundException | IllegalAccessException | InstantiationException error) {
            throw error;
        } catch (InvocationTargetException error) {
            throw asInstantiationException(error.getCause());
        } catch (Throwable error) {
            throw asInstantiationException(error);
        }
    }

    private static InstantiationException asInstantiationException(Throwable cause) {
        InstantiationException exception =
                new InstantiationException("Could not instantiate Minecraft MainActivity: " + cause);
        exception.initCause(cause);
        return exception;
    }
}
