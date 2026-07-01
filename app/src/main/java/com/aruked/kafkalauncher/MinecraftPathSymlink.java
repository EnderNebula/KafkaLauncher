package com.aruked.kafkalauncher;

import android.content.Context;
import android.os.Environment;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;

final class MinecraftPathSymlink {
    private static final String OLD_MOJANG_PATH = "/storage/emulated/0/games/com.mojang";
    private static final String MIGRATION_MARKER_NAME = "AndroidApi30ScopedStorage.completed";
    private static final byte[] MIGRATION_MARKER_BYTES = new byte[] {'d', 'o', 'n', 'e', 0};

    private MinecraftPathSymlink() {
    }

    static boolean install(Context context) {
        File oldPath = new File(OLD_MOJANG_PATH);
        File scopedExternalRoot = context.getExternalFilesDir(null);
        if (scopedExternalRoot == null) {
            ProbeReport.append(context, "[ERROR] Scoped external files directory is unavailable");
            return false;
        }
        File scopedExternalPath = new File(scopedExternalRoot, "games/com.mojang");
        File internalDataPath = new File(new File(context.getDataDir(), "games"), "com.mojang");
        if (isReady(oldPath, scopedExternalPath, internalDataPath)) {
            ProbeReport.append(context, "[OK] Storage bridge ready");
            return true;
        }

        ProbeReport.append(context, "[START] Preparing storage bridge");

        if (!ensureOldPath(context, oldPath)) {
            return false;
        }
        if (!ensureScopedExternalMarker(context, scopedExternalPath)) {
            return false;
        }
        return installOne(context, oldPath, internalDataPath, "internal-data");
    }

    private static boolean ensureOldPath(Context context, File oldPath) {
        if (oldPath.exists() && !oldPath.isDirectory()) {
            ProbeReport.append(context, "[ERROR] Old path exists but is not a directory");
            return false;
        }
        if (!oldPath.exists() && !oldPath.mkdirs()) {
            ProbeReport.append(context, "[ERROR] Could not create old path");
            return false;
        }
        ProbeReport.append(context, "[OK] Old path ready");
        return true;
    }

    private static boolean ensureScopedExternalMarker(Context context, File scopedExternalPath) {
        if (scopedExternalPath.exists() && !scopedExternalPath.isDirectory()) {
            ProbeReport.append(context, "[ERROR] Scoped external path exists but is not a directory");
            return false;
        }
        if (!scopedExternalPath.exists() && !scopedExternalPath.mkdirs()) {
            ProbeReport.append(context, "[ERROR] Could not create scoped external directory");
            return false;
        }

        File marker = new File(scopedExternalPath, MIGRATION_MARKER_NAME);
        try (FileOutputStream output = new FileOutputStream(marker, false)) {
            output.write(MIGRATION_MARKER_BYTES);
            output.flush();
            ProbeReport.append(context, "[OK] Migration marker written to scoped external");
            return true;
        } catch (IOException error) {
            ProbeReport.append(context, "[ERROR] Could not write migration marker: " + error);
            return false;
        }
    }

    private static boolean isReady(File oldPath, File scopedExternalPath, File internalDataPath) {
        if (!oldPath.isDirectory() || !scopedExternalPath.isDirectory()) {
            return false;
        }
        File marker = new File(scopedExternalPath, MIGRATION_MARKER_NAME);
        if (!marker.isFile() || marker.length() != MIGRATION_MARKER_BYTES.length) {
            return false;
        }
        byte[] bytes = new byte[MIGRATION_MARKER_BYTES.length];
        try (FileInputStream input = new FileInputStream(marker)) {
            if (input.read(bytes) != bytes.length) {
                return false;
            }
        } catch (IOException error) {
            return false;
        }
        for (int i = 0; i < MIGRATION_MARKER_BYTES.length; i++) {
            if (bytes[i] != MIGRATION_MARKER_BYTES[i]) {
                return false;
            }
        }
        LinkState state = readLinkState(internalDataPath);
        return state.isSymlink && oldPath.getAbsolutePath().equals(state.target);
    }

    private static boolean installOne(Context context, File external, File link, String label) {
        File linkParent = link.getParentFile();
        if (linkParent == null || (!linkParent.exists() && !linkParent.mkdirs())) {
            ProbeReport.append(context, "[ERROR] Could not create " + label + " parent directory");
            return false;
        }

        LinkState state = readLinkState(link);
        if (state.isSymlink) {
            if (external.getAbsolutePath().equals(state.target)) {
                ProbeReport.append(context, "[OK] " + label + " symlink already points to external path");
                return true;
            }
            if (!link.delete()) {
                ProbeReport.append(context, "[ERROR] Could not replace " + label
                        + " symlink: " + state.target);
                return false;
            }
        } else if (link.exists()) {
            if (!isEmptyDirectory(link)) {
                ProbeReport.append(context, "[ERROR] " + label
                        + " path already exists and is not empty");
                ProbeReport.append(context, "[INFO] Move or delete it before symlink test");
                return false;
            }
            if (!link.delete()) {
                ProbeReport.append(context, "[ERROR] Could not remove empty " + label + " directory");
                return false;
            }
        }

        try {
            Os.symlink(external.getAbsolutePath(), link.getAbsolutePath());
            LinkState created = readLinkState(link);
            if (created.isSymlink) {
                ProbeReport.append(context, "[OK] " + label + " symlink created: " + created.target);
                return true;
            }
            ProbeReport.append(context, "[ERROR] " + label
                    + " symlink() returned but path is not a symlink");
            return false;
        } catch (ErrnoException error) {
            ProbeReport.append(context, "[ERROR] " + label + " symlink failed: errno=" + error.errno
                    + " " + error.getMessage());
            return false;
        }
    }

    static boolean hasAllFilesAccess() {
        return android.os.Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    private static LinkState readLinkState(File path) {
        try {
            int mode = Os.lstat(path.getAbsolutePath()).st_mode;
            boolean isSymlink = (mode & OsConstants.S_IFMT) == OsConstants.S_IFLNK;
            return new LinkState(isSymlink, isSymlink ? Os.readlink(path.getAbsolutePath()) : null);
        } catch (ErrnoException error) {
            return new LinkState(false, null);
        }
    }

    private static boolean isEmptyDirectory(File path) {
        if (!path.isDirectory()) {
            return false;
        }
        String[] entries = path.list();
        return entries != null && entries.length == 0;
    }

    private static final class LinkState {
        final boolean isSymlink;
        final String target;

        LinkState(boolean isSymlink, String target) {
            this.isSymlink = isSymlink;
            this.target = target;
        }
    }
}
