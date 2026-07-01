#include "kafka_inline_hook.h"

#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <shadowhook.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_TAG "KafkaInlineHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define ARM64_INSTRUCTION_SIZE 4u
#define ARM64_PATCH_SIZE 16u
#define ARM64_TRAMPOLINE_SIZE 512u
#define ARM64_BR_X17 0xd61f0220u
#define ARM64_BLR_X17 0xd63f0220u
#define KAFKA_FORCE_ARM64_FALLBACK 0

typedef struct {
    uint8_t *cursor;
    uint8_t *end;
} code_writer;

static int shadowhook_initialized;
static int shadowhook_disabled;
static char fallback_error[160];
static pthread_mutex_t install_mutex = PTHREAD_MUTEX_INITIALIZER;

static int writer_u32(code_writer *writer, uint32_t value) {
    if ((size_t) (writer->end - writer->cursor) < sizeof(value)) {
        return 0;
    }
    memcpy(writer->cursor, &value, sizeof(value));
    writer->cursor += sizeof(value);
    return 1;
}

static int writer_u64(code_writer *writer, uint64_t value) {
    if ((size_t) (writer->end - writer->cursor) < sizeof(value)) {
        return 0;
    }
    memcpy(writer->cursor, &value, sizeof(value));
    writer->cursor += sizeof(value);
    return 1;
}

static int64_t sign_extend(uint64_t value, unsigned int bits) {
    uint64_t sign = 1ull << (bits - 1u);
    return (int64_t) ((value ^ sign) - sign);
}

static int emit_absolute_jump(code_writer *writer, uintptr_t target) {
    return writer_u32(writer, 0x58000051u)
            && writer_u32(writer, ARM64_BR_X17)
            && writer_u64(writer, target);
}

static int emit_absolute_call(code_writer *writer, uintptr_t target) {
    return writer_u32(writer, 0x58000071u)
            && writer_u32(writer, ARM64_BLR_X17)
            && writer_u32(writer, 0x14000003u)
            && writer_u64(writer, target);
}

static int branch_target_is_overwritten(uintptr_t target, uintptr_t hook_target) {
    return target >= hook_target && target < hook_target + ARM64_PATCH_SIZE;
}

static int relocate_branch(
        code_writer *writer,
        uint32_t instruction,
        uintptr_t instruction_address,
        uintptr_t hook_target) {
    int is_bl = (instruction & 0xfc000000u) == 0x94000000u;
    int64_t displacement = sign_extend(
            ((uint64_t) instruction & 0x03ffffffu) << 2u,
            28u);
    uintptr_t target = instruction_address + displacement;
    if (branch_target_is_overwritten(target, hook_target)) {
        return 0;
    }
    return is_bl ? emit_absolute_call(writer, target) : emit_absolute_jump(writer, target);
}

static int relocate_conditional_branch(
        code_writer *writer,
        uint32_t instruction,
        uintptr_t instruction_address,
        uintptr_t hook_target) {
    int64_t displacement = sign_extend(
            ((uint64_t) ((instruction >> 5u) & 0x7ffffu)) << 2u,
            21u);
    uintptr_t target = instruction_address + displacement;
    if (branch_target_is_overwritten(target, hook_target)) {
        return 0;
    }

    uint32_t inverse_condition = (instruction ^ 1u) & 0xff00001fu;
    return writer_u32(writer, inverse_condition | (5u << 5u))
            && emit_absolute_jump(writer, target);
}

static int relocate_compare_or_test_branch(
        code_writer *writer,
        uint32_t instruction,
        uintptr_t instruction_address,
        uintptr_t hook_target,
        int is_test_branch) {
    uint32_t immediate = is_test_branch
            ? (instruction >> 5u) & 0x3fffu
            : (instruction >> 5u) & 0x7ffffu;
    unsigned int displacement_bits = is_test_branch ? 16u : 21u;
    int64_t displacement = sign_extend((uint64_t) immediate << 2u, displacement_bits);
    uintptr_t target = instruction_address + displacement;
    if (branch_target_is_overwritten(target, hook_target)) {
        return 0;
    }

    uint32_t inverse = instruction ^ 0x01000000u;
    uint32_t immediate_mask = is_test_branch ? 0x0007ffe0u : 0x00ffffe0u;
    inverse = (inverse & ~immediate_mask) | (5u << 5u);
    return writer_u32(writer, inverse) && emit_absolute_jump(writer, target);
}

static int relocate_adr(
        code_writer *writer,
        uint32_t instruction,
        uintptr_t instruction_address) {
    uint32_t destination = instruction & 0x1fu;
    uint64_t immlo = (instruction >> 29u) & 3u;
    uint64_t immhi = (instruction >> 5u) & 0x7ffffu;
    int is_adrp = (instruction & 0x80000000u) != 0;
    uintptr_t value;
    if (is_adrp) {
        int64_t displacement = sign_extend((immhi << 14u) | (immlo << 12u), 33u);
        value = (instruction_address & ~(uintptr_t) 0xfffu) + displacement;
    } else {
        int64_t displacement = sign_extend((immhi << 2u) | immlo, 21u);
        value = instruction_address + displacement;
    }

    return writer_u32(writer, 0x58000040u | destination)
            && writer_u32(writer, 0x14000003u)
            && writer_u64(writer, value);
}

static int relocate_literal_load(
        code_writer *writer,
        uint32_t instruction,
        uintptr_t instruction_address) {
    uint32_t destination = instruction & 0x1fu;
    if (destination == 31u) {
        return 0;
    }

    int64_t displacement = sign_extend(
            ((uint64_t) ((instruction >> 5u) & 0x7ffffu)) << 2u,
            21u);
    uintptr_t literal_address = instruction_address + displacement;
    uint32_t load_from_register;
    switch (instruction & 0xff000000u) {
        case 0x18000000u:
            load_from_register = 0xb9400000u;
            break;
        case 0x58000000u:
            load_from_register = 0xf9400000u;
            break;
        case 0x98000000u:
            load_from_register = 0xb9800000u;
            break;
        default:
            return 0;
    }
    load_from_register |= destination | (destination << 5u);

    return writer_u32(writer, 0x58000060u | destination)
            && writer_u32(writer, load_from_register)
            && writer_u32(writer, 0x14000003u)
            && writer_u64(writer, literal_address);
}

static int relocate_instruction(
        code_writer *writer,
        uint32_t instruction,
        uintptr_t instruction_address,
        uintptr_t hook_target) {
    if ((instruction & 0x7c000000u) == 0x14000000u) {
        return relocate_branch(writer, instruction, instruction_address, hook_target);
    }
    if ((instruction & 0xff000010u) == 0x54000000u) {
        return relocate_conditional_branch(writer, instruction, instruction_address, hook_target);
    }
    if ((instruction & 0x7e000000u) == 0x34000000u) {
        return relocate_compare_or_test_branch(
                writer, instruction, instruction_address, hook_target, 0);
    }
    if ((instruction & 0x7e000000u) == 0x36000000u) {
        return relocate_compare_or_test_branch(
                writer, instruction, instruction_address, hook_target, 1);
    }
    if ((instruction & 0x1f000000u) == 0x10000000u) {
        return relocate_adr(writer, instruction, instruction_address);
    }
    if ((instruction & 0x3b000000u) == 0x18000000u) {
        return relocate_literal_load(writer, instruction, instruction_address);
    }
    return writer_u32(writer, instruction);
}

static int set_page_permissions(void *address, size_t size, int permissions) {
    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t) address & ~((uintptr_t) page_size - 1u);
    uintptr_t end = ((uintptr_t) address + size + (uintptr_t) page_size - 1u)
            & ~((uintptr_t) page_size - 1u);
    return mprotect((void *) start, end - start, permissions) == 0;
}

static int install_arm64_fallback(void *target, void *replacement, void **original) {
#if !defined(__aarch64__)
    snprintf(fallback_error, sizeof(fallback_error), "ARM64 fallback is unavailable for this ABI");
    return 0;
#else
    uint8_t *trampoline = mmap(
            NULL,
            ARM64_TRAMPOLINE_SIZE,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0);
    if (trampoline == MAP_FAILED) {
        snprintf(fallback_error, sizeof(fallback_error), "trampoline mmap failed: %d", errno);
        return 0;
    }

    code_writer writer = {trampoline, trampoline + ARM64_TRAMPOLINE_SIZE};
    for (size_t offset = 0; offset < ARM64_PATCH_SIZE; offset += ARM64_INSTRUCTION_SIZE) {
        uint32_t instruction;
        memcpy(&instruction, (uint8_t *) target + offset, sizeof(instruction));
        if (!relocate_instruction(
                &writer,
                instruction,
                (uintptr_t) target + offset,
                (uintptr_t) target)) {
            snprintf(
                    fallback_error,
                    sizeof(fallback_error),
                    "unsupported ARM64 relocation at +0x%zx: 0x%08x",
                    offset,
                    instruction);
            munmap(trampoline, ARM64_TRAMPOLINE_SIZE);
            return 0;
        }
    }
    if (!emit_absolute_jump(&writer, (uintptr_t) target + ARM64_PATCH_SIZE)) {
        snprintf(fallback_error, sizeof(fallback_error), "trampoline capacity exceeded");
        munmap(trampoline, ARM64_TRAMPOLINE_SIZE);
        return 0;
    }

    __builtin___clear_cache((char *) trampoline, (char *) writer.cursor);
    if (mprotect(trampoline, ARM64_TRAMPOLINE_SIZE, PROT_READ | PROT_EXEC) != 0) {
        snprintf(fallback_error, sizeof(fallback_error), "trampoline mprotect failed: %d", errno);
        munmap(trampoline, ARM64_TRAMPOLINE_SIZE);
        return 0;
    }

    uint8_t patch[ARM64_PATCH_SIZE];
    code_writer patch_writer = {patch, patch + sizeof(patch)};
    if (!emit_absolute_jump(&patch_writer, (uintptr_t) replacement)
            || !set_page_permissions(target, ARM64_PATCH_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        snprintf(fallback_error, sizeof(fallback_error), "target mprotect failed: %d", errno);
        munmap(trampoline, ARM64_TRAMPOLINE_SIZE);
        return 0;
    }

    memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache((char *) target, (char *) target + sizeof(patch));
    set_page_permissions(target, ARM64_PATCH_SIZE, PROT_READ | PROT_EXEC);
    *original = trampoline;
    return 1;
#endif
}

const char *kafka_hook_backend_name(kafka_hook_backend backend) {
    switch (backend) {
        case KAFKA_HOOK_BACKEND_SHADOWHOOK:
            return "shadowhook";
        case KAFKA_HOOK_BACKEND_ARM64_FALLBACK:
            return "arm64-fallback";
        default:
            return "none";
    }
}

static int install_locked(
        void *target,
        void *replacement,
        void **original,
        kafka_hook_result *result) {
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    if (target == NULL || replacement == NULL || original == NULL) {
        if (result != NULL) {
            result->error = "invalid hook arguments";
        }
        return 0;
    }

    if (KAFKA_FORCE_ARM64_FALLBACK) {
        shadowhook_disabled = 1;
    }

    if (!shadowhook_disabled && !shadowhook_initialized) {
        int init_result = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
        if (init_result == SHADOWHOOK_ERRNO_OK) {
            shadowhook_initialized = 1;
        } else {
            LOGE("ShadowHook init failed: %d - %s", init_result, shadowhook_to_errmsg(init_result));
            shadowhook_disabled = 1;
        }
    }

    if (!shadowhook_disabled && shadowhook_initialized) {
        void *stub = shadowhook_hook_func_addr(target, replacement, original);
        if (stub != NULL && *original != NULL) {
            if (result != NULL) {
                result->backend = KAFKA_HOOK_BACKEND_SHADOWHOOK;
                result->backend_handle = stub;
            }
            return 1;
        }
        int hook_error = shadowhook_get_errno();
        LOGE("ShadowHook address hook failed: %d - %s", hook_error, shadowhook_to_errmsg(hook_error));
        shadowhook_disabled = 1;
    }

    *original = NULL;
    if (install_arm64_fallback(target, replacement, original)) {
        LOGI("Installed ARM64 fallback hook at %p", target);
        if (result != NULL) {
            result->backend = KAFKA_HOOK_BACKEND_ARM64_FALLBACK;
            result->backend_handle = *original;
        }
        return 1;
    }

    LOGE("ARM64 fallback hook failed: %s", fallback_error);
    if (result != NULL) {
        result->error = fallback_error;
    }
    return 0;
}

int kafka_inline_hook_install(
        void *target,
        void *replacement,
        void **original,
        kafka_hook_result *result) {
    pthread_mutex_lock(&install_mutex);
    int installed = install_locked(target, replacement, original, result);
    pthread_mutex_unlock(&install_mutex);
    return installed;
}
