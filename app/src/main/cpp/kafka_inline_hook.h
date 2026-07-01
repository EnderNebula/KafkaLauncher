#ifndef KAFKA_INLINE_HOOK_H
#define KAFKA_INLINE_HOOK_H

typedef enum {
    KAFKA_HOOK_BACKEND_NONE = 0,
    KAFKA_HOOK_BACKEND_SHADOWHOOK = 1,
    KAFKA_HOOK_BACKEND_ARM64_FALLBACK = 2
} kafka_hook_backend;

typedef struct {
    kafka_hook_backend backend;
    void *backend_handle;
    const char *error;
} kafka_hook_result;

/*
 * Installs an address-only hook. ShadowHook is preferred; the local ARM64
 * relocator is used only when ShadowHook cannot initialize or install.
 */
int kafka_inline_hook_install(
        void *target,
        void *replacement,
        void **original,
        kafka_hook_result *result);

const char *kafka_hook_backend_name(kafka_hook_backend backend);

#endif
