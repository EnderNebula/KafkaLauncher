package com.aruked.kafkalauncher;

import android.content.Context;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;

final class ProbeReport {
    private static final String FILE_NAME = "minecraft_report.txt";

    private ProbeReport() {
    }

    static void reset(Context context) {
        write(context, "KafkaLauncher report\n", false);
    }

    static void append(Context context, String message) {
        write(context, message + "\n", true);
    }

    static String read(Context context) {
        File file = file(context);
        if (!file.isFile()) {
            return "No report yet.";
        }
        try (RandomAccessFile access = new RandomAccessFile(file, "r");
             FileChannel channel = access.getChannel();
             FileLock ignored = channel.lock(0, Long.MAX_VALUE, true)) {
            byte[] bytes = new byte[(int) access.length()];
            access.readFully(bytes);
            return new String(bytes, StandardCharsets.UTF_8);
        } catch (Throwable error) {
            return "Could not read report: " + error;
        }
    }

    private static void write(Context context, String text, boolean append) {
        try (RandomAccessFile access = new RandomAccessFile(file(context), "rw");
             FileChannel channel = access.getChannel();
             FileLock ignored = channel.lock()) {
            if (append) {
                access.seek(access.length());
            } else {
                access.setLength(0);
            }
            access.write(text.getBytes(StandardCharsets.UTF_8));
            channel.force(true);
        } catch (Throwable ignored) {
        }
    }

    private static File file(Context context) {
        return new File(context.getFilesDir(), FILE_NAME);
    }
}
