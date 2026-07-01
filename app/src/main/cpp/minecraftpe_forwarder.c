#include <android/log.h>
#include <android/native_activity.h>
#include <elf.h>
#include <fcntl.h>
#include <jni.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "minecraft_world_hook.h"

#define LOG_TAG "KafkaMcForwarder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef void (*native_activity_on_create_fn)(
        ANativeActivity *activity,
        void *saved_state,
        size_t saved_state_size);

void ANativeActivity_onCreate(
        ANativeActivity *activity,
        void *saved_state,
        size_t saved_state_size);

static int read_exact(int fd, void *buffer, size_t size, off_t offset) {
    size_t consumed = 0;
    while (consumed < size) {
        ssize_t count = pread(
                fd,
                (char *) buffer + consumed,
                size - consumed,
                offset + (off_t) consumed);
        if (count <= 0) {
            return 0;
        }
        consumed += (size_t) count;
    }
    return 1;
}

static int find_real_minecraft_mapping(
        char *path_result,
        size_t path_result_size,
        uintptr_t *load_bias_result) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        LOGE("Could not open /proc/self/maps");
        return 0;
    }

    int found = 0;
    uintptr_t lowest_bias = UINTPTR_MAX;
    char selected_path[PATH_MAX] = {0};
    char line[PATH_MAX + 256];
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long long start;
        unsigned long long offset;
        char permissions[5];
        int path_offset = 0;
        if (sscanf(
                line,
                "%llx-%*llx %4s %llx %*s %*s %n",
                &start,
                permissions,
                &offset,
                &path_offset) < 3) {
            continue;
        }

        char *path = line + path_offset;
        if (strstr(path, "/libminecraftpe.so") == NULL
                || strstr(path, "com.aruked.kafkalauncher") != NULL) {
            continue;
        }

        path[strcspn(path, "\r\n")] = '\0';
        uintptr_t bias = (uintptr_t) start - (uintptr_t) offset;
        if (!found || bias < lowest_bias) {
            found = 1;
            lowest_bias = bias;
            snprintf(selected_path, sizeof(selected_path), "%s", path);
        }
    }
    fclose(maps);

    if (!found) {
        LOGE("Real libminecraftpe.so is not loaded in this process");
        return 0;
    }

    snprintf(path_result, path_result_size, "%s", selected_path);
    *load_bias_result = lowest_bias;
    return 1;
}

static uintptr_t resolve_dynamic_symbol(
        const char *library_path,
        uintptr_t load_bias,
        const char *symbol_name) {
    int fd = open(library_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("Could not open %s", library_path);
        return 0;
    }

    Elf64_Ehdr header;
    if (!read_exact(fd, &header, sizeof(header), 0)
            || memcmp(header.e_ident, ELFMAG, SELFMAG) != 0
            || header.e_ident[EI_CLASS] != ELFCLASS64
            || header.e_shentsize != sizeof(Elf64_Shdr)) {
        LOGE("Unsupported ELF header in %s", library_path);
        close(fd);
        return 0;
    }

    size_t section_bytes = (size_t) header.e_shnum * sizeof(Elf64_Shdr);
    Elf64_Shdr *sections = malloc(section_bytes);
    if (sections == NULL
            || !read_exact(fd, sections, section_bytes, (off_t) header.e_shoff)) {
        LOGE("Could not read ELF sections from %s", library_path);
        free(sections);
        close(fd);
        return 0;
    }

    uintptr_t result = 0;
    for (size_t index = 0; index < header.e_shnum && result == 0; index++) {
        Elf64_Shdr *symbols_section = &sections[index];
        if (symbols_section->sh_type != SHT_DYNSYM
                || symbols_section->sh_entsize != sizeof(Elf64_Sym)
                || symbols_section->sh_link >= header.e_shnum) {
            continue;
        }

        Elf64_Shdr *strings_section = &sections[symbols_section->sh_link];
        char *strings = malloc((size_t) strings_section->sh_size);
        Elf64_Sym *symbols = malloc((size_t) symbols_section->sh_size);
        if (strings == NULL
                || symbols == NULL
                || !read_exact(
                        fd,
                        strings,
                        (size_t) strings_section->sh_size,
                        (off_t) strings_section->sh_offset)
                || !read_exact(
                        fd,
                        symbols,
                        (size_t) symbols_section->sh_size,
                        (off_t) symbols_section->sh_offset)) {
            free(strings);
            free(symbols);
            continue;
        }

        size_t symbol_count =
                (size_t) symbols_section->sh_size / sizeof(Elf64_Sym);
        for (size_t symbol_index = 0;
             symbol_index < symbol_count;
             symbol_index++) {
            Elf64_Sym *symbol = &symbols[symbol_index];
            if (symbol->st_name >= strings_section->sh_size
                    || symbol->st_shndx == SHN_UNDEF) {
                continue;
            }
            if (strcmp(strings + symbol->st_name, symbol_name) == 0) {
                result = load_bias + (uintptr_t) symbol->st_value;
                break;
            }
        }
        free(strings);
        free(symbols);
    }

    free(sections);
    close(fd);
    return result;
}

JNIEXPORT jboolean JNICALL
Java_com_aruked_kafkalauncher_KafkaApplication_nativeInstallHooksForHostedActivity(
        JNIEnv *env,
        jclass clazz,
        jobject activity_object) {
    (void) clazz;

    char real_path[PATH_MAX];
    uintptr_t load_bias;
    if (!find_real_minecraft_mapping(
            real_path,
            sizeof(real_path),
            &load_bias)) {
        LOGI("Hosted Activity hook entry called before real Minecraft library load");
        return JNI_FALSE;
    }

    ANativeActivity activity = {0};
    if ((*env)->GetJavaVM(env, &activity.vm) != JNI_OK) {
        LOGE("Could not obtain JavaVM for hosted Activity hook entry");
        return JNI_FALSE;
    }
    activity.env = env;
    activity.clazz = activity_object;
    int installed = kafka_install_world_entered_hook(real_path, &activity);
    LOGI(
            "Hosted Activity hook entry result=%d real=%s loadBias=%p",
            installed,
            real_path,
            (void *) load_bias);
    return installed ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_aruked_kafkalauncher_KafkaApplication_nativeSetActivityBaseContext(
        JNIEnv *env,
        jclass clazz,
        jobject activity_object,
        jobject replacement_context) {
    (void) clazz;

    jclass context_wrapper_class =
            (*env)->FindClass(env, "android/content/ContextWrapper");
    if (context_wrapper_class == NULL) {
        (*env)->ExceptionClear(env);
        LOGE("Could not resolve ContextWrapper for Activity base-context replacement");
        return JNI_FALSE;
    }

    jfieldID base_field = (*env)->GetFieldID(
            env,
            context_wrapper_class,
            "mBase",
            "Landroid/content/Context;");
    if (base_field == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, context_wrapper_class);
        LOGE("Could not resolve ContextWrapper.mBase");
        return JNI_FALSE;
    }

    (*env)->SetObjectField(env, activity_object, base_field, replacement_context);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, context_wrapper_class);
        LOGE("Could not replace Activity base Context");
        return JNI_FALSE;
    }

    (*env)->DeleteLocalRef(env, context_wrapper_class);
    LOGI("Activity base Context replaced with Minecraft ClassLoader wrapper");
    return JNI_TRUE;
}

__attribute__((visibility("default")))
void ANativeActivity_onCreate(
        ANativeActivity *activity,
        void *saved_state,
        size_t saved_state_size) {
    char real_path[PATH_MAX];
    uintptr_t load_bias;
    if (!find_real_minecraft_mapping(
            real_path,
            sizeof(real_path),
            &load_bias)) {
        return;
    }

    uintptr_t symbol_address = resolve_dynamic_symbol(
            real_path,
            load_bias,
            "ANativeActivity_onCreate");
    if (symbol_address == 0
            || symbol_address == (uintptr_t) ANativeActivity_onCreate) {
        LOGE("Could not resolve real ANativeActivity_onCreate from %s", real_path);
        return;
    }

    native_activity_on_create_fn real_on_create =
            (native_activity_on_create_fn) symbol_address;
    if (kafka_install_world_entered_hook(real_path, activity)) {
        LOGI("Native hook probes installed");
    } else {
        LOGE("Native hook probes were not installed; continuing original startup");
    }
    LOGI(
            "Forwarding to already-loaded original %s; loadBias=%p entry=%p",
            real_path,
            (void *) load_bias,
            (void *) symbol_address);
    real_on_create(activity, saved_state, saved_state_size);
}
