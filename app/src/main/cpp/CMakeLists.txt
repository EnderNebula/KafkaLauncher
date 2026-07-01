cmake_minimum_required(VERSION 3.22.1)
project(kafka_minecraft_launcher LANGUAGES C CXX ASM)

find_package(shadowhook REQUIRED CONFIG)

add_library(minecraftpe SHARED
        minecraftpe_forwarder.c
        kafka_inline_hook.c
        minecraft_world_hook.c)

target_link_libraries(minecraftpe PRIVATE android log shadowhook::shadowhook)
