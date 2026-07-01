# KafkaLauncher

KafkaLauncher is a proof-of-concept Android launcher for Minecraft Bedrock
Edition. It starts the installed Minecraft app inside KafkaLauncher's own
hosted process, loads Minecraft's native libraries, forwards startup to the
real `libminecraftpe.so`, and installs a small set of in-process hooks.

- Android: designed for modern Android versions, tested around Android 12+
  style package, storage, and native loading behavior.
- Architecture: `arm64-v8a` only.
- Minecraft: only `com.mojang.minecraftpe` version `1.21.51.02`.
- Root: not required.
- Minecraft APK/native libraries: not modified on disk.

## Features

- Launches the installed Minecraft package from a separate KafkaLauncher task.
- Loads Minecraft's native libraries from the installed Minecraft package.
- Installs inline hooks after `libminecraftpe.so` is mapped.
- Shows a `pwned!` toast when the local client enters a world.
- Captures outgoing chat messages and writes them to the launcher report.
- Rewrites messages beginning with `!` before Minecraft sends them:
  `!ih` becomes `hi`.
- Hooks Minecraft's own screen-model send functions:
  - `#...` lets the original message send first, then executes `/say pwn!`.
  - `@...` lets the original message send first, then sends `[pwn!]`.
- Modifies storage location to `/storage/emulated/0/games/com.mojang`

## How It Works

KafkaLauncher does not start Minecraft as a normal external app process.
Instead, it declares an Activity using Minecraft's original class name:

```xml
<activity android:name="com.mojang.minecraftpe.MainActivity" />
```

That Activity belongs to KafkaLauncher's APK and runs in KafkaLauncher's
`:minecraft` process, but `KafkaAppComponentFactory` intercepts Activity
creation. When Android asks for `com.mojang.minecraftpe.MainActivity`,
KafkaLauncher obtains a package context for the installed Minecraft package:

```java
createPackageContext(
    "com.mojang.minecraftpe",
    CONTEXT_IGNORE_SECURITY | CONTEXT_INCLUDE_CODE)
```

It then loads and constructs Minecraft's real Java Activity class with
Minecraft's `PathClassLoader`. This gives the process Minecraft's Java-side
Activity implementation without launching Minecraft's own package process.

`KafkaApplication` also replaces key context surfaces for the hosted Activity:

- `getClassLoader()` returns Minecraft's class loader;
- `getAssets()` and `getResources()` return Minecraft's assets/resources;
- the Activity base `Context` is wrapped so Java code inside Minecraft resolves
  classes and resources from the Minecraft package;
- the original Minecraft Activity theme is applied.

This keeps Java-side code paths close to a normal Minecraft launch while still
running inside KafkaLauncher's process.

## Native Library Loading

KafkaLauncher's APK contains its own library named `libminecraftpe.so`. This is
not the real Minecraft library. It is a small forwarder loaded by Android
because `NativeActivity` resolves the library named by:

```xml
<meta-data
    android:name="android.app.lib_name"
    android:value="minecraftpe" />
```

Before the Activity reaches native startup, Minecraft's own class loader and
native library search path make Android load the real Minecraft libraries from
the installed Minecraft package. The forwarder then scans `/proc/self/maps` to
find the already mapped real `libminecraftpe.so`, ignoring KafkaLauncher's
forwarder library.

After finding the real library mapping, the forwarder reads the ELF dynamic
symbol table, resolves the real `ANativeActivity_onCreate`, installs hooks, and
then calls Minecraft's original native entry point:

```c
real_on_create(activity, saved_state, saved_state_size);
```

So the final call chain is:

1. User taps `Launch Minecraft` in KafkaLauncher.
2. Android starts KafkaLauncher's hosted
   `com.mojang.minecraftpe.MainActivity` in process `:minecraft`.
3. KafkaLauncher constructs Minecraft's real Java Activity through Minecraft's
   class loader.
4. Android loads KafkaLauncher's forwarder `libminecraftpe.so`.
5. The forwarder locates the real Minecraft `libminecraftpe.so`.
6. KafkaLauncher installs hooks in the real library.
7. The forwarder calls the real `ANativeActivity_onCreate`.
8. Minecraft continues booting normally, now inside the hooked process.

## Hooking Model

Hooks are installed only after the real Minecraft library is mapped in the
current process. KafkaLauncher uses [ShadowHook](https://github.com/bytedance/android-inline-hook) when available and falls back
to a small local ARM64 inline hooker when ShadowHook cannot install a specific
address hook.

The Java/native bridge:

- native hook code reports world and chat events to `KafkaApplication`;
- `KafkaApplication` forwards them to `MinecraftHookBridge`;
- `MinecraftHookBridge` updates the report UI and shows the world-enter toast;
- for `!` chat rewrites, Java returns the replacement text to native, and native
  edits Minecraft's in-flight `std::string` before the original send function
  continues.

The `#` and `@` message actions are handled in native code because the current
screen-model pointer is already available in Minecraft's original send call.

## Project Layout

- `app/src/main/java/com/aruked/kafkalauncher/MainActivity.java`
  - launcher UI, permission prompt, report view, launch button.
- `KafkaApplication.java`
  - Minecraft package context setup, Activity context wrapping, resource bridge,
    hook lifecycle.
- `KafkaAppComponentFactory.java`
  - constructs Minecraft's real Java `MainActivity` from Minecraft's class
    loader.
- `MinecraftPathSymlink.java`
  - storage marker and symlink preparation.
- `MinecraftHookBridge.java`
  - Java-side POC behavior.
- `app/src/main/cpp/minecraftpe_forwarder.c`
  - fake `libminecraftpe.so`, real native entry resolution, startup forwarding.
- `app/src/main/cpp/minecraft_world_hook.c`
  - Minecraft hook targets, JNI callbacks, chat string handling.
- `app/src/main/cpp/kafka_inline_hook.c`
  - ShadowHook wrapper and local ARM64 fallback inline hooker.

## Build

Open the `KafkaLauncher` directory in Android Studio and build the `app`
module.

Current build assumptions:

- Android Gradle Plugin `8.9.2`;
- compile SDK `35`;
- min SDK `28`;
- target SDK `34`;
- NDK `27.0.12077973`;
- CMake `3.22.1`;
- ShadowHook `2.0.0`;
- ABI filter: `arm64-v8a`.

## Usage

1. Install Minecraft Bedrock Edition `1.21.51.02` with package name
   `com.mojang.minecraftpe`.
2. Install KafkaLauncher on the same device.
3. Grant KafkaLauncher all-files access when prompted. The storage bridge needs
   this to create `/storage/emulated/0/games/com.mojang`.
4. Open KafkaLauncher.
5. Tap `Launch Minecraft`.
6. Enter a world:
   - a `pwned!` toast should appear;
   - the launcher report should show the world hook event.
7. Send chat messages:
   - `hello` is captured and reported;
   - `!ih` is rewritten to `hi`;
   - `#test` sends `#test`, then runs `/say pwn!`;
   - `@test` sends `@test`, then sends `[pwn!]`.

## Limitations

- This is a POC, not a general MCBE mod loader.
- The hook set is version-specific and currently only targets
  Minecraft `1.21.51.02` `arm64-v8a`.
- Other Minecraft versions may boot but should be treated as unsupported.
- x86/x86_64 and Houdini/ARM translation environments are not supported by the
  ShadowHook path. Use a real ARM64 device for meaningful validation.
- Active message sending without a player-initiated chat send is not implemented
  in this POC.

## License

KafkaLauncher is licensed under the Apache License 2.0.
