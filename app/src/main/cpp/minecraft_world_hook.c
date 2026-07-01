#include "minecraft_world_hook.h"
#include "kafka_inline_hook.h"

#include <android/log.h>
#include <jni.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "KafkaWorldHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define MAX_MAPPINGS 32

typedef struct {
    uintptr_t start;
    uintptr_t end;
    uintptr_t offset;
} mapping;

typedef void (*initial_spawn_fn)(void *client_instance);
typedef uintptr_t (*chat_dispatch_fn)(void *chat_screen_controller);
typedef uintptr_t (*screen_model_message_fn)(
        void *screen_model,
        const void *arg1,
        const void *arg2,
        const void *arg3,
        const void *arg4,
        const void *arg5,
        const void *arg6,
        const void *arg7);
static JavaVM *java_vm;
static jobject kafka_application;
static jmethodID world_entered_method;
static jmethodID hook_error_method;
static jmethodID hook_installed_method;
static jmethodID chat_message_method;
static initial_spawn_fn original_initial_spawn;
static int hook_installation_state;
static chat_dispatch_fn original_chat_dispatch;
static screen_model_message_fn original_screen_model_send_chat;
static screen_model_message_fn original_screen_model_execute_command;
static int inside_chat_hook;
static char pending_chat_message[256];
static int pending_chat_message_ready;

static int collect_minecraft_mappings(
        const char *library_path,
        mapping *mappings,
        size_t *mapping_count,
        uintptr_t *load_bias) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        return 0;
    }

    size_t count = 0;
    uintptr_t lowest_bias = UINTPTR_MAX;
    char line[PATH_MAX + 256];
    while (count < MAX_MAPPINGS && fgets(line, sizeof(line), maps) != NULL) {
        unsigned long long start;
        unsigned long long end;
        unsigned long long offset;
        char permissions[5];
        int path_offset = 0;
        if (sscanf(
                line,
                "%llx-%llx %4s %llx %*s %*s %n",
                &start,
                &end,
                permissions,
                &offset,
                &path_offset) < 4) {
            continue;
        }

        char *path = line + path_offset;
        path[strcspn(path, "\r\n")] = '\0';
        if (strcmp(path, library_path) != 0 || permissions[0] != 'r') {
            continue;
        }

        mappings[count].start = (uintptr_t) start;
        mappings[count].end = (uintptr_t) end;
        mappings[count].offset = (uintptr_t) offset;
        uintptr_t candidate_bias = mappings[count].start - mappings[count].offset;
        if (candidate_bias < lowest_bias) {
            lowest_bias = candidate_bias;
        }
        count++;
    }
    fclose(maps);
    *mapping_count = count;
    *load_bias = lowest_bias;
    return count != 0;
}

static uintptr_t pointer_value_for_maps(const void *address) {
#if defined(__aarch64__)
    return ((uintptr_t) address) & 0x00ffffffffffffffULL;
#else
    return (uintptr_t) address;
#endif
}

static int address_range_is_readable(const void *address, size_t size) {
    if (address == NULL || size == 0) {
        return 0;
    }

    uintptr_t target_start = pointer_value_for_maps(address);
    uintptr_t target_end = target_start + size;
    if (target_end < target_start) {
        return 0;
    }

    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        return 0;
    }

    int readable = 0;
    char line[PATH_MAX + 256];
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long long start;
        unsigned long long end;
        char permissions[5];
        if (sscanf(line, "%llx-%llx %4s", &start, &end, permissions) != 3) {
            continue;
        }
        if (permissions[0] == 'r'
                && target_start >= (uintptr_t) start
                && target_end <= (uintptr_t) end) {
            readable = 1;
            break;
        }
    }
    fclose(maps);
    return readable;
}

static int is_safe_chat_text(const char *text);

static int read_std_string_default(
        const void *string_object,
        char *buffer,
        size_t buffer_size,
        size_t *string_size) {
    if (buffer_size == 0 || !address_range_is_readable(string_object, 24)) {
        return 0;
    }

    const uint8_t *object = (const uint8_t *) string_object;
    size_t length;
    const char *data;
    if ((object[0] & 1u) == 0) {
        length = object[0] >> 1;
        data = (const char *) object + 1;
    } else {
        memcpy(&length, object + sizeof(size_t), sizeof(length));
        memcpy(&data, object + sizeof(size_t) * 2u, sizeof(data));
    }

    if (length == 0 || length >= 4096 || !address_range_is_readable(data, length)) {
        return 0;
    }

    size_t copy_length = length < buffer_size - 1 ? length : buffer_size - 1;
    memcpy(buffer, data, copy_length);
    buffer[copy_length] = '\0';
    if (string_size != NULL) {
        *string_size = length;
    }
    return 1;
}

static int read_std_string_alternate(
        const void *string_object,
        char *buffer,
        size_t buffer_size,
        size_t *string_size) {
    if (buffer_size == 0 || !address_range_is_readable(string_object, 24)) {
        return 0;
    }

    const uint8_t *object = (const uint8_t *) string_object;
    size_t length;
    const char *data;
    if ((object[23] & 1u) == 0) {
        length = object[23] >> 1;
        data = (const char *) object;
    } else {
        memcpy(&data, object, sizeof(data));
        memcpy(&length, object + sizeof(size_t), sizeof(length));
    }

    if (length == 0 || length >= 4096 || !address_range_is_readable(data, length)) {
        return 0;
    }

    size_t copy_length = length < buffer_size - 1 ? length : buffer_size - 1;
    memcpy(buffer, data, copy_length);
    buffer[copy_length] = '\0';
    if (string_size != NULL) {
        *string_size = length;
    }
    return 1;
}

static int read_std_string(
        const void *string_object,
        char *buffer,
        size_t buffer_size,
        size_t *string_size) {
    return read_std_string_default(string_object, buffer, buffer_size, string_size)
            || read_std_string_alternate(string_object, buffer, buffer_size, string_size);
}

static int append_utf8(char *buffer, size_t buffer_size, size_t *offset, uint32_t codepoint) {
    if (codepoint == 0 || codepoint > 0x10ffffu
            || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
        return 0;
    }
    if (codepoint < 0x20u && codepoint != '\n' && codepoint != '\r' && codepoint != '\t') {
        return 0;
    }

    if (codepoint <= 0x7fu) {
        if (*offset + 1 >= buffer_size) {
            return 0;
        }
        buffer[(*offset)++] = (char) codepoint;
        return 1;
    }
    if (codepoint <= 0x7ffu) {
        if (*offset + 2 >= buffer_size) {
            return 0;
        }
        buffer[(*offset)++] = (char) (0xc0u | (codepoint >> 6u));
        buffer[(*offset)++] = (char) (0x80u | (codepoint & 0x3fu));
        return 1;
    }
    if (codepoint <= 0xffffu) {
        if (*offset + 3 >= buffer_size) {
            return 0;
        }
        buffer[(*offset)++] = (char) (0xe0u | (codepoint >> 12u));
        buffer[(*offset)++] = (char) (0x80u | ((codepoint >> 6u) & 0x3fu));
        buffer[(*offset)++] = (char) (0x80u | (codepoint & 0x3fu));
        return 1;
    }
    if (*offset + 4 >= buffer_size) {
        return 0;
    }
    buffer[(*offset)++] = (char) (0xf0u | (codepoint >> 18u));
    buffer[(*offset)++] = (char) (0x80u | ((codepoint >> 12u) & 0x3fu));
    buffer[(*offset)++] = (char) (0x80u | ((codepoint >> 6u) & 0x3fu));
    buffer[(*offset)++] = (char) (0x80u | (codepoint & 0x3fu));
    return 1;
}

static int read_utf16_units(
        const uint16_t *data,
        size_t length,
        char *buffer,
        size_t buffer_size) {
    if (data == NULL || length == 0 || length >= 2048
            || !address_range_is_readable(data, length * sizeof(uint16_t))) {
        return 0;
    }

    size_t out = 0;
    for (size_t index = 0; index < length; index++) {
        uint32_t codepoint = data[index];
        if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
            if (index + 1 >= length) {
                return 0;
            }
            uint32_t low = data[++index];
            if (low < 0xdc00u || low > 0xdfffu) {
                return 0;
            }
            codepoint = 0x10000u + (((codepoint - 0xd800u) << 10u) | (low - 0xdc00u));
        } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
            return 0;
        }
        if (!append_utf8(buffer, buffer_size, &out, codepoint)) {
            return 0;
        }
    }
    buffer[out] = '\0';
    return is_safe_chat_text(buffer);
}

static int read_std_u16string_default(
        const void *string_object,
        char *buffer,
        size_t buffer_size) {
    if (buffer_size == 0 || !address_range_is_readable(string_object, 24)) {
        return 0;
    }

    const uint8_t *object = (const uint8_t *) string_object;
    size_t length;
    const uint16_t *data;
    if ((object[0] & 1u) == 0) {
        length = object[0] >> 1;
        data = (const uint16_t *) (object + sizeof(uint16_t));
    } else {
        memcpy(&length, object + sizeof(size_t), sizeof(length));
        memcpy(&data, object + sizeof(size_t) * 2u, sizeof(data));
    }
    return read_utf16_units(data, length, buffer, buffer_size);
}

static int read_std_u16string_alternate(
        const void *string_object,
        char *buffer,
        size_t buffer_size) {
    if (buffer_size == 0 || !address_range_is_readable(string_object, 24)) {
        return 0;
    }

    const uint8_t *object = (const uint8_t *) string_object;
    size_t length;
    const uint16_t *data;
    if ((object[23] & 1u) == 0) {
        length = object[23] >> 1;
        data = (const uint16_t *) object;
    } else {
        memcpy(&data, object, sizeof(data));
        memcpy(&length, object + sizeof(size_t), sizeof(length));
    }
    return read_utf16_units(data, length, buffer, buffer_size);
}

static int read_std_u16string(
        const void *string_object,
        char *buffer,
        size_t buffer_size) {
    return read_std_u16string_default(string_object, buffer, buffer_size)
            || read_std_u16string_alternate(string_object, buffer, buffer_size);
}

static int read_message_string(
        const void *candidate,
        char *buffer,
        size_t buffer_size,
        const void **string_object) {
    if (read_std_string(candidate, buffer, buffer_size, NULL)) {
        if (string_object != NULL) {
            *string_object = candidate;
        }
        return 1;
    }
    if (read_std_u16string(candidate, buffer, buffer_size)) {
        if (string_object != NULL) {
            *string_object = candidate;
        }
        return 1;
    }
    if (address_range_is_readable(candidate, sizeof(void *))) {
        const void *nested = NULL;
        memcpy(&nested, candidate, sizeof(nested));
        if (read_std_string(nested, buffer, buffer_size, NULL)) {
            if (string_object != NULL) {
                *string_object = nested;
            }
            return 1;
        }
        if (read_std_u16string(nested, buffer, buffer_size)) {
            if (string_object != NULL) {
                *string_object = nested;
            }
            return 1;
        }
    }
    return 0;
}

static int read_char_span(
        const void *data,
        uintptr_t length_value,
        char *buffer,
        size_t buffer_size) {
    if (buffer_size == 0 || data == NULL || length_value == 0 || length_value >= 4096) {
        return 0;
    }
    size_t length = (size_t) length_value;
    if (length >= buffer_size || !address_range_is_readable(data, length)) {
        return 0;
    }
    memcpy(buffer, data, length);
    buffer[length] = '\0';
    return is_safe_chat_text(buffer);
}

static int read_message_arguments(
        const void *arg1,
        const void *arg2,
        const void *arg3,
        const void *arg4,
        const void *arg5,
        const void *arg6,
        const void *arg7,
        char *buffer,
        size_t buffer_size,
        const void **string_object) {
    static const struct {
        int data_index;
        int length_index;
    } spans[] = {
            {1, 2},
            {1, 3},
            {1, 4},
            {2, 3},
            {2, 4},
            {3, 4}
    };
    const void *args[] = {NULL, arg1, arg2, arg3, arg4, arg5, arg6, arg7};
    for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
        const void *data = args[spans[i].data_index];
        uintptr_t length_value = (uintptr_t) args[spans[i].length_index];
        if (read_char_span(data, length_value, buffer, buffer_size)) {
            if (string_object != NULL) {
                *string_object = data;
            }
            return 1;
        }
    }

    static const int sources[] = {1, 2, 3, 4, 5, 6, 7};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        const void *candidate = args[sources[i]];
        if (read_message_string(candidate, buffer, buffer_size, string_object)) {
            return 1;
        }
    }
    return 0;
}

static int scan_message_field(
        const void *base,
        size_t start_offset,
        size_t end_offset,
        char *buffer,
        size_t buffer_size,
        void **string_object) {
    for (size_t offset = start_offset; offset <= end_offset; offset += sizeof(void *)) {
        void *candidate = (uint8_t *) base + offset;
        if (read_std_string(candidate, buffer, buffer_size, NULL)
                && is_safe_chat_text(buffer)) {
            if (string_object != NULL) {
                *string_object = candidate;
            }
            return 1;
        }
    }
    return 0;
}

static int write_std_string_default_same_or_shorter(void *string_object, const char *value) {
    if (value == NULL || !address_range_is_readable(string_object, 24)) {
        return 0;
    }

    uint8_t *object = (uint8_t *) string_object;
    size_t old_length;
    char *data;
    if ((object[0] & 1u) == 0) {
        old_length = object[0] >> 1;
        data = (char *) object + 1;
    } else {
        memcpy(&old_length, object + sizeof(size_t), sizeof(old_length));
        memcpy(&data, object + sizeof(size_t) * 2u, sizeof(data));
    }

    size_t new_length = strlen(value);
    if (new_length > old_length
            || new_length >= 4096
            || !address_range_is_readable(data, old_length + 1)) {
        return 0;
    }

    memcpy(data, value, new_length);
    data[new_length] = '\0';
    if ((object[0] & 1u) == 0) {
        object[0] = (uint8_t) (new_length << 1u);
    } else {
        memcpy(object + sizeof(size_t), &new_length, sizeof(new_length));
    }
    return 1;
}

static int write_std_string_alternate_same_or_shorter(void *string_object, const char *value) {
    if (value == NULL || !address_range_is_readable(string_object, 24)) {
        return 0;
    }

    uint8_t *object = (uint8_t *) string_object;
    size_t old_length;
    char *data;
    if ((object[23] & 1u) == 0) {
        old_length = object[23] >> 1;
        data = (char *) object;
    } else {
        memcpy(&data, object, sizeof(data));
        memcpy(&old_length, object + sizeof(size_t), sizeof(old_length));
    }

    size_t new_length = strlen(value);
    if (new_length > old_length
            || new_length >= 4096
            || !address_range_is_readable(data, old_length + 1)) {
        return 0;
    }

    memcpy(data, value, new_length);
    data[new_length] = '\0';
    if ((object[23] & 1u) == 0) {
        object[23] = (uint8_t) (new_length << 1u);
    } else {
        memcpy(object + sizeof(size_t), &new_length, sizeof(new_length));
    }
    return 1;
}

static int write_std_string_same_or_shorter(void *string_object, const char *value) {
    return write_std_string_default_same_or_shorter(string_object, value)
            || write_std_string_alternate_same_or_shorter(string_object, value);
}

static int is_utf8_continuation(unsigned char value) {
    return (value & 0xc0u) == 0x80u;
}

static int is_safe_chat_text(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    size_t length = strlen(text);
    if (length > 512) {
        return 0;
    }

    size_t index = 0;
    while (index < length) {
        unsigned char c0 = (unsigned char) text[index];
        if (c0 <= 0x7fu) {
            if (c0 < 0x20u) {
                return 0;
            }
            index++;
            continue;
        }

        if (c0 >= 0xc2u && c0 <= 0xdfu) {
            if (index + 1 >= length
                    || !is_utf8_continuation((unsigned char) text[index + 1])) {
                return 0;
            }
            index += 2;
            continue;
        }

        if (c0 == 0xe0u) {
            if (index + 2 >= length
                    || (unsigned char) text[index + 1] < 0xa0u
                    || (unsigned char) text[index + 1] > 0xbfu
                    || !is_utf8_continuation((unsigned char) text[index + 2])) {
                return 0;
            }
            index += 3;
            continue;
        }
        if ((c0 >= 0xe1u && c0 <= 0xecu) || (c0 >= 0xeeu && c0 <= 0xefu)) {
            if (index + 2 >= length
                    || !is_utf8_continuation((unsigned char) text[index + 1])
                    || !is_utf8_continuation((unsigned char) text[index + 2])) {
                return 0;
            }
            index += 3;
            continue;
        }
        if (c0 == 0xedu) {
            if (index + 2 >= length
                    || (unsigned char) text[index + 1] < 0x80u
                    || (unsigned char) text[index + 1] > 0x9fu
                    || !is_utf8_continuation((unsigned char) text[index + 2])) {
                return 0;
            }
            index += 3;
            continue;
        }

        if (c0 == 0xf0u) {
            if (index + 3 >= length
                    || (unsigned char) text[index + 1] < 0x90u
                    || (unsigned char) text[index + 1] > 0xbfu
                    || !is_utf8_continuation((unsigned char) text[index + 2])
                    || !is_utf8_continuation((unsigned char) text[index + 3])) {
                return 0;
            }
            index += 4;
            continue;
        }
        if (c0 >= 0xf1u && c0 <= 0xf3u) {
            if (index + 3 >= length
                    || !is_utf8_continuation((unsigned char) text[index + 1])
                    || !is_utf8_continuation((unsigned char) text[index + 2])
                    || !is_utf8_continuation((unsigned char) text[index + 3])) {
                return 0;
            }
            index += 4;
            continue;
        }
        if (c0 == 0xf4u) {
            if (index + 3 >= length
                    || (unsigned char) text[index + 1] < 0x80u
                    || (unsigned char) text[index + 1] > 0x8fu
                    || !is_utf8_continuation((unsigned char) text[index + 2])
                    || !is_utf8_continuation((unsigned char) text[index + 3])) {
                return 0;
            }
            index += 4;
            continue;
        }

        return 0;
    }
    return 1;
}

static int set_pending_chat_message(const char *utf) {
    if (!is_safe_chat_text(utf)) {
        return 0;
    }
    size_t length = strlen(utf);
    if (length >= sizeof(pending_chat_message)) {
        return 0;
    }
    memcpy(pending_chat_message, utf, length);
    pending_chat_message[length] = '\0';
    pending_chat_message_ready = 1;
    return 1;
}

static int make_short_std_string(uint8_t *string_object, size_t object_size, const char *value) {
    if (string_object == NULL || object_size < 24 || value == NULL) {
        return 0;
    }
    size_t length = strlen(value);
    if (length > 22) {
        return 0;
    }
    memset(string_object, 0, object_size);
    string_object[0] = (uint8_t) (length << 1u);
    memcpy(string_object + 1, value, length);
    return 1;
}

static int send_with_screen_model(void *screen_model, const char *utf, int force_command) {
    if (screen_model == NULL || utf == NULL) {
        return 0;
    }

    uint8_t message_object[24];
    if (!make_short_std_string(message_object, sizeof(message_object), utf)) {
        LOGE("[MODEL ACTION] failed to build short std::string for \"%s\"", utf);
        return 0;
    }

    if (force_command || utf[0] == '/') {
        if (original_screen_model_execute_command == NULL) {
            LOGE("[MODEL ACTION] executeCommand candidate is not installed");
            return 0;
        }
        LOGI("[MODEL ACTION] executeCommand msg=\"%s\"", utf);
        original_screen_model_execute_command(
                screen_model, message_object, NULL, NULL, NULL, NULL, NULL, NULL);
        return 1;
    }

    if (original_screen_model_send_chat == NULL) {
        LOGE("[MODEL ACTION] sendChatMessage candidate is not installed");
        return 0;
    }
    LOGI("[MODEL ACTION] sendChatMessage msg=\"%s\"", utf);
    original_screen_model_send_chat(
            screen_model, message_object, NULL, NULL, NULL, NULL, NULL, NULL);
    return 1;
}

static uintptr_t find_binary_pattern(
        const mapping *mappings,
        size_t mapping_count,
        const uint8_t *pattern,
        size_t pattern_size) {
    for (size_t index = 0; index < mapping_count; index++) {
        const mapping *region = &mappings[index];
        if (region->end - region->start < pattern_size) {
            continue;
        }
        const uint8_t *cursor = (const uint8_t *) region->start;
        const uint8_t *limit = (const uint8_t *) (region->end - pattern_size);
        for (; cursor <= limit; cursor += sizeof(uint32_t)) {
            if (memcmp(cursor, pattern, pattern_size) == 0) {
                return (uintptr_t) cursor;
            }
        }
    }
    return 0;
}

static int initialize_java_bridge(ANativeActivity *activity) {
    java_vm = activity->vm;
    JNIEnv *env = activity->env;
    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID get_application = activity_class == NULL ? NULL : (*env)->GetMethodID(
            env,
            activity_class,
            "getApplication",
            "()Landroid/app/Application;");
    if (activity_class != NULL) {
        (*env)->DeleteLocalRef(env, activity_class);
    }
    if (get_application == NULL) {
        (*env)->ExceptionClear(env);
        return 0;
    }

    jobject local_application = (*env)->CallObjectMethod(
            env,
            activity->clazz,
            get_application);
    if ((*env)->ExceptionCheck(env) || local_application == NULL) {
        (*env)->ExceptionClear(env);
        return 0;
    }

    kafka_application = (*env)->NewGlobalRef(env, local_application);
    jclass application_class = (*env)->GetObjectClass(env, local_application);
    (*env)->DeleteLocalRef(env, local_application);
    if (kafka_application == NULL || application_class == NULL) {
        return 0;
    }

    world_entered_method = (*env)->GetMethodID(
            env,
            application_class,
            "onMinecraftClientEnteredWorld",
            "()V");
    hook_error_method = (*env)->GetMethodID(
            env,
            application_class,
            "onMinecraftHookError",
            "(Ljava/lang/String;)V");
    hook_installed_method = (*env)->GetMethodID(
            env,
            application_class,
            "onMinecraftHookInstalled",
            "(Ljava/lang/String;)V");
    chat_message_method = (*env)->GetMethodID(
            env,
            application_class,
            "onMinecraftChatProbe",
            "(Ljava/lang/String;Ljava/lang/String;)V");
    (*env)->DeleteLocalRef(env, application_class);
    if (world_entered_method == NULL
            || hook_error_method == NULL
            || hook_installed_method == NULL
            || chat_message_method == NULL) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    return 1;
}

static jstring new_java_string_from_utf8(JNIEnv *env, const char *utf8) {
    if (utf8 == NULL) {
        return NULL;
    }

    jchar utf16[1024];
    size_t out = 0;
    size_t length = strlen(utf8);
    for (size_t index = 0; index < length && out < sizeof(utf16) / sizeof(utf16[0]);) {
        uint32_t codepoint;
        unsigned char c0 = (unsigned char) utf8[index];
        if (c0 <= 0x7fu) {
            codepoint = c0;
            index++;
        } else if (c0 >= 0xc2u && c0 <= 0xdfu && index + 1 < length) {
            codepoint = ((uint32_t) (c0 & 0x1fu) << 6u)
                    | ((uint32_t) utf8[index + 1] & 0x3fu);
            index += 2;
        } else if (c0 >= 0xe0u && c0 <= 0xefu && index + 2 < length) {
            codepoint = ((uint32_t) (c0 & 0x0fu) << 12u)
                    | (((uint32_t) utf8[index + 1] & 0x3fu) << 6u)
                    | ((uint32_t) utf8[index + 2] & 0x3fu);
            index += 3;
        } else if (c0 >= 0xf0u && c0 <= 0xf4u && index + 3 < length) {
            codepoint = ((uint32_t) (c0 & 0x07u) << 18u)
                    | (((uint32_t) utf8[index + 1] & 0x3fu) << 12u)
                    | (((uint32_t) utf8[index + 2] & 0x3fu) << 6u)
                    | ((uint32_t) utf8[index + 3] & 0x3fu);
            index += 4;
        } else {
            return (*env)->NewStringUTF(env, utf8);
        }

        if (codepoint <= 0xffffu) {
            utf16[out++] = (jchar) codepoint;
        } else {
            if (out + 1 >= sizeof(utf16) / sizeof(utf16[0])) {
                break;
            }
            codepoint -= 0x10000u;
            utf16[out++] = (jchar) (0xd800u | (codepoint >> 10u));
            utf16[out++] = (jchar) (0xdc00u | (codepoint & 0x3ffu));
        }
    }
    return (*env)->NewString(env, utf16, (jsize) out);
}

static void notify_java_hook_error(const char *message) {
    if (java_vm == NULL || kafka_application == NULL || hook_error_method == NULL) {
        return;
    }

    JNIEnv *env = NULL;
    int result = (*java_vm)->GetEnv(java_vm, (void **) &env, JNI_VERSION_1_6);
    int attached_here = 0;
    if (result == JNI_EDETACHED) {
        if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) {
            return;
        }
        attached_here = 1;
    } else if (result != JNI_OK) {
        return;
    }

    jstring java_message = new_java_string_from_utf8(env, message);
    if (java_message != NULL) {
        (*env)->CallVoidMethod(env, kafka_application, hook_error_method, java_message);
        (*env)->DeleteLocalRef(env, java_message);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (attached_here) {
        (*java_vm)->DetachCurrentThread(java_vm);
    }
}

static void notify_java_hook_installed(const char *backend) {
    if (java_vm == NULL || kafka_application == NULL || hook_installed_method == NULL) {
        return;
    }

    JNIEnv *env = NULL;
    int result = (*java_vm)->GetEnv(java_vm, (void **) &env, JNI_VERSION_1_6);
    int attached_here = 0;
    if (result == JNI_EDETACHED) {
        if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) {
            return;
        }
        attached_here = 1;
    } else if (result != JNI_OK) {
        return;
    }

    jstring java_backend = new_java_string_from_utf8(env, backend);
    if (java_backend != NULL) {
        (*env)->CallVoidMethod(env, kafka_application, hook_installed_method, java_backend);
        (*env)->DeleteLocalRef(env, java_backend);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (attached_here) {
        (*java_vm)->DetachCurrentThread(java_vm);
    }
}

static void notify_java_world_entered(void) {
    if (java_vm == NULL || kafka_application == NULL || world_entered_method == NULL) {
        return;
    }

    JNIEnv *env = NULL;
    int result = (*java_vm)->GetEnv(java_vm, (void **) &env, JNI_VERSION_1_6);
    int attached_here = 0;
    if (result == JNI_EDETACHED) {
        if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) {
            return;
        }
        attached_here = 1;
    } else if (result != JNI_OK) {
        return;
    }

    (*env)->CallVoidMethod(env, kafka_application, world_entered_method);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (attached_here) {
        (*java_vm)->DetachCurrentThread(java_vm);
    }
}

static void notify_java_chat_message(const char *hook_name, const char *message) {
    if (java_vm == NULL || kafka_application == NULL || chat_message_method == NULL) {
        return;
    }

    JNIEnv *env = NULL;
    int result = (*java_vm)->GetEnv(java_vm, (void **) &env, JNI_VERSION_1_6);
    int attached_here = 0;
    if (result == JNI_EDETACHED) {
        if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) {
            return;
        }
        attached_here = 1;
    } else if (result != JNI_OK) {
        return;
    }

    jstring java_hook_name = new_java_string_from_utf8(env, hook_name);
    jstring java_message = new_java_string_from_utf8(env, message);
    if (java_hook_name != NULL && java_message != NULL) {
        (*env)->CallVoidMethod(
                env,
                kafka_application,
                chat_message_method,
                java_hook_name,
                java_message);
    }
    if (java_hook_name != NULL) {
        (*env)->DeleteLocalRef(env, java_hook_name);
    }
    if (java_message != NULL) {
        (*env)->DeleteLocalRef(env, java_message);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (attached_here) {
        (*java_vm)->DetachCurrentThread(java_vm);
    }
}

static void initial_spawn_hook(void *client_instance) {
    original_initial_spawn(client_instance);
    LOGI("Initial-spawn inline hook fired");
    notify_java_world_entered();
}

static uintptr_t screen_model_send_chat_hook(
        void *screen_model,
        const void *arg1,
        const void *arg2,
        const void *arg3,
        const void *arg4,
        const void *arg5,
        const void *arg6,
        const void *arg7) {
    char captured[256];
    int action = 0;
    if (read_message_arguments(
            arg1,
            arg2,
            arg3,
            arg4,
            arg5,
            arg6,
            arg7,
            captured,
            sizeof(captured),
            NULL)
            && is_safe_chat_text(captured)) {
        LOGI("[MODEL SEND] msg=\"%s\"", captured);

        notify_java_chat_message("model-sendChatMessage", captured);

        if (captured[0] == '#') {
            action = 1;
        } else if (captured[0] == '@') {
            action = 2;
        }
    } else {
        LOGE("[MODEL SEND] failed to read message");
    }
    uintptr_t result = original_screen_model_send_chat(
            screen_model, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
    if (action == 1) {
        send_with_screen_model(screen_model, "/say pwn!", 1);
    } else if (action == 2) {
        send_with_screen_model(screen_model, "[pwn!]", 0);
    }
    return result;
}

static uintptr_t screen_model_execute_command_hook(
        void *screen_model,
        const void *arg1,
        const void *arg2,
        const void *arg3,
        const void *arg4,
        const void *arg5,
        const void *arg6,
        const void *arg7) {
    char captured[256];
    if (read_message_arguments(
            arg1,
            arg2,
            arg3,
            arg4,
            arg5,
            arg6,
            arg7,
            captured,
            sizeof(captured),
            NULL)
            && is_safe_chat_text(captured)) {
        LOGI("[MODEL COMMAND] msg=\"%s\"", captured);
        notify_java_chat_message("model-executeCommand", captured);
    } else {
        LOGE("[MODEL COMMAND] failed to read message");
    }
    return original_screen_model_execute_command(
            screen_model, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

static uintptr_t chat_dispatch_hook(void *chat_screen_controller) {
    void *message_argument = (uint8_t *) chat_screen_controller + 0x918;
    char captured[256];
    if ((!read_std_string(message_argument, captured, sizeof(captured), NULL)
            || !is_safe_chat_text(captured))
            && !scan_message_field(
                    chat_screen_controller,
                    0x800,
                    0xa80,
                    captured,
                    sizeof(captured),
                    &message_argument)) {
        LOGE("[CHAT CONTROLLER] failed to read message");
        return original_chat_dispatch(chat_screen_controller);
    }

    LOGI("[CHAT CONTROLLER] msg=\"%s\"", captured);

    pending_chat_message_ready = 0;
    pending_chat_message[0] = '\0';
    inside_chat_hook = 1;
    notify_java_chat_message("chat-controller", captured);
    inside_chat_hook = 0;

    if (pending_chat_message_ready) {
        if (write_std_string_same_or_shorter(message_argument, pending_chat_message)) {
            LOGI("[CHAT CONTROLLER] rewrite \"%s\" -> \"%s\"",
                    captured,
                    pending_chat_message);
        } else {
            LOGE("[CHAT CONTROLLER] rewrite failed for \"%s\" -> \"%s\"",
                    captured,
                    pending_chat_message);
            notify_java_hook_error("Minecraft chat rewrite failed");
        }
        pending_chat_message_ready = 0;
        pending_chat_message[0] = '\0';
    }

    return original_chat_dispatch(chat_screen_controller);
}

static int looks_like_arm64_function_entry(uintptr_t address) {
    if (!address_range_is_readable((const void *) address, sizeof(uint32_t))) {
        return 0;
    }
    uint32_t instruction = *(const uint32_t *) address;
    if ((instruction & 0xffc003ffu) == 0xa98003fdu) {
        return 1; // stp x29, x30, [sp, #imm]
    }
    if ((instruction & 0xffc003ffu) == 0xa90003fdu) {
        return 1; // stp x29, x30, [sp, #imm]
    }
    if ((instruction & 0xffc003e0u) == 0xd10003e0u) {
        return 1; // sub sp, sp, #imm
    }
    if ((instruction & 0xfffffc1fu) == 0xd503201fu) {
        return 1; // nop/bti style landing pad family
    }
    return 0;
}

static int install_chat_dispatch_hook(
        const mapping *mappings,
        size_t mapping_count,
        uintptr_t load_bias) {
    const uintptr_t chat_dispatch_rva = 0x62c3660;
    uintptr_t target = load_bias + chat_dispatch_rva;

    int in_mapping = 0;
    for (size_t map_index = 0; map_index < mapping_count; map_index++) {
        if (target >= mappings[map_index].start
                && target + sizeof(uint32_t) <= mappings[map_index].end) {
            in_mapping = 1;
            break;
        }
    }
    if (!in_mapping) {
        LOGE("[CHAT CONTROLLER] target is outside executable mappings");
        return 0;
    }
    if (!looks_like_arm64_function_entry(target)) {
        LOGE("[CHAT CONTROLLER] target does not look like an ARM64 function entry");
        return 0;
    }

    kafka_hook_result result;
    if (!kafka_inline_hook_install(
            (void *) target,
            (void *) chat_dispatch_hook,
            (void **) &original_chat_dispatch,
            &result)) {
        LOGE("[CHAT CONTROLLER] install failed backend=%s error=%s",
                kafka_hook_backend_name(result.backend),
                result.error == NULL ? "unknown error" : result.error);
        return 0;
    }
    LOGI("[CHAT CONTROLLER] installed backend=%s", kafka_hook_backend_name(result.backend));
    notify_java_hook_installed("chat-controller");
    return 1;
}

static int install_screen_model_message_hook(
        const mapping *mappings,
        size_t mapping_count,
        uintptr_t load_bias,
        uintptr_t rva,
        const char *label,
        void *replacement,
        screen_model_message_fn *original) {
    uintptr_t target = load_bias + rva;

    int in_mapping = 0;
    for (size_t map_index = 0; map_index < mapping_count; map_index++) {
        if (target >= mappings[map_index].start
                && target + sizeof(uint32_t) <= mappings[map_index].end) {
            in_mapping = 1;
            break;
        }
    }
    if (!in_mapping) {
        LOGE("[%s] target is outside executable mappings", label);
        return 0;
    }
    if (!looks_like_arm64_function_entry(target)) {
        LOGE("[%s] target does not look like an ARM64 function entry", label);
        return 0;
    }

    kafka_hook_result result;
    if (!kafka_inline_hook_install(
            (void *) target,
            replacement,
            (void **) original,
            &result)) {
        LOGE("[%s] install failed backend=%s error=%s",
                label,
                kafka_hook_backend_name(result.backend),
                result.error == NULL ? "unknown error" : result.error);
        return 0;
    }
    LOGI("[%s] installed backend=%s", label, kafka_hook_backend_name(result.backend));
    notify_java_hook_installed(label);
    return 1;
}

static int install_screen_model_message_hooks(
        const mapping *mappings,
        size_t mapping_count,
        uintptr_t load_bias) {
    int send_ok = install_screen_model_message_hook(
            mappings,
            mapping_count,
            load_bias,
            0x6bcfb3c,
            "model-sendChatMessage",
            (void *) screen_model_send_chat_hook,
            &original_screen_model_send_chat);
    int command_ok = install_screen_model_message_hook(
            mappings,
            mapping_count,
            load_bias,
            0x6bce67c,
            "model-executeCommand",
            (void *) screen_model_execute_command_hook,
            &original_screen_model_execute_command);
    return send_ok || command_ok;
}

static int install_initial_spawn_inline_hook(
        const mapping *mappings,
        size_t mapping_count,
        uintptr_t load_bias) {
    static const uint8_t minecraft_1_21_51_signature[] = {
            0xfd, 0x7b, 0xbc, 0xa9, 0xfc, 0x5f, 0x01, 0xa9,
            0xfd, 0x03, 0x00, 0x91, 0xf6, 0x57, 0x02, 0xa9,
            0xf4, 0x4f, 0x03, 0xa9, 0xff, 0x03, 0x08, 0xd1,
            0x55, 0xd0, 0x3b, 0xd5, 0xf3, 0x03, 0x00, 0xaa
    };
    static const struct {
        const char *version;
        const uint8_t *bytes;
        size_t size;
    } signatures[] = {
            {"1.21.51.02", minecraft_1_21_51_signature,
                    sizeof(minecraft_1_21_51_signature)}
    };

    uintptr_t target = 0;
    const char *matched_version = NULL;
    for (size_t index = 0; index < sizeof(signatures) / sizeof(signatures[0]); index++) {
        target = find_binary_pattern(
                mappings,
                mapping_count,
                signatures[index].bytes,
                signatures[index].size);
        if (target != 0) {
            matched_version = signatures[index].version;
            break;
        }
    }
    if (target == 0) {
        LOGE("No supported initial-spawn signature found");
        notify_java_hook_error("Unsupported Minecraft inline-hook signature");
        return 0;
    }

    kafka_hook_result hook_result;
    if (!kafka_inline_hook_install(
            (void *) target,
            (void *) initial_spawn_hook,
            (void **) &original_initial_spawn,
            &hook_result)) {
        char message[160];
        snprintf(
                message,
                sizeof(message),
                "Inline hook failed: %s",
                hook_result.error == NULL ? "unknown error" : hook_result.error);
        LOGE("%s", message);
        notify_java_hook_error(message);
        return 0;
    }
    LOGI("Installed %s hook for Minecraft %s",
            kafka_hook_backend_name(hook_result.backend),
            matched_version);
    notify_java_hook_installed(kafka_hook_backend_name(hook_result.backend));
    return 1;
}

int kafka_install_world_entered_hook(
        const char *minecraft_library_path,
        ANativeActivity *activity) {
    int expected_state = 0;
    if (!__atomic_compare_exchange_n(
            &hook_installation_state,
            &expected_state,
            1,
            0,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {
        return expected_state == 2;
    }

    mapping mappings[MAX_MAPPINGS];
    size_t mapping_count = 0;
    uintptr_t load_bias = 0;
    if (!collect_minecraft_mappings(
            minecraft_library_path,
            mappings,
            &mapping_count,
            &load_bias)
            || !initialize_java_bridge(activity)) {
        __atomic_store_n(&hook_installation_state, 0, __ATOMIC_RELEASE);
        return 0;
    }

    int world_hook_installed = install_initial_spawn_inline_hook(
            mappings,
            mapping_count,
            load_bias);
    int chat_hooks_installed = install_chat_dispatch_hook(mappings, mapping_count, load_bias);
    int model_hooks_installed = install_screen_model_message_hooks(mappings, mapping_count, load_bias);
    __atomic_store_n(
            &hook_installation_state,
            (world_hook_installed || chat_hooks_installed || model_hooks_installed) ? 2 : 3,
            __ATOMIC_RELEASE);
    return world_hook_installed || chat_hooks_installed || model_hooks_installed;
}

JNIEXPORT jboolean JNICALL
Java_com_aruked_kafkalauncher_KafkaApplication_nativeSendMinecraftMessage(
        JNIEnv *env,
        jclass clazz,
        jstring message) {
    (void) clazz;
    if (message == NULL) {
        return JNI_FALSE;
    }

    const char *utf = (*env)->GetStringUTFChars(env, message, NULL);
    if (utf == NULL) {
        return JNI_FALSE;
    }

    int sent = 0;
    if (inside_chat_hook) {
        sent = set_pending_chat_message(utf);
        if (!sent) {
            LOGE("[CHAT CONTROLLER] rejected invalid or too long UTF-8 rewrite");
        }
    }

    (*env)->ReleaseStringUTFChars(env, message, utf);
    return sent ? JNI_TRUE : JNI_FALSE;
}
