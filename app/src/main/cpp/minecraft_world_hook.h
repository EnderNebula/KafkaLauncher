#ifndef KAFKA_MINECRAFT_WORLD_HOOK_H
#define KAFKA_MINECRAFT_WORLD_HOOK_H

#include <android/native_activity.h>

int kafka_install_world_entered_hook(
        const char *minecraft_library_path,
        ANativeActivity *activity);

#endif
