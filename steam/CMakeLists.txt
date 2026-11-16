cmake_minimum_required(VERSION 3.20)
project(wheelio_steam C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES arm64)
add_compile_options(-Wall -Wextra)

# The resolver and the report, shared by the patch, the pre-check and the tests.
add_library(wheelio_steam_core STATIC
    src/anchor_resolver.c
    src/macho_image.c
    src/reference_scanner.c
    src/steam_anchors.c
    src/steam_build_version.c
    src/steam_patch_plan.c
    src/steam_report.c
    src/wheelio_steam_check.c
)
target_include_directories(wheelio_steam_core PUBLIC include src)
set_target_properties(wheelio_steam_core PROPERTIES C_VISIBILITY_PRESET hidden)

# Injected into steam_osx.
add_library(wheelio_steam SHARED
    src/code_patcher.c
    src/status_file.c
    src/steam_patch_apply.c
    src/wheelio_steam_patch.c
)
target_link_libraries(wheelio_steam PRIVATE wheelio_steam_core)
set_target_properties(wheelio_steam PROPERTIES C_VISIBILITY_PRESET hidden)

add_executable(wheelio-steam-check tools/wheelio_steam_check_tool.c)
target_link_libraries(wheelio-steam-check PRIVATE wheelio_steam_core)

include(CTest)
if(BUILD_TESTING)
    set(fixture_variants good string_missing not_referenced string_twice constructor_twice
        flag_mismatch no_flag_test disable_wrong entry_pc_relative)
    set(fixtures)
    foreach(variant IN LISTS fixture_variants)
        string(TOUPPER "FIXTURE_${variant}" define)
        add_library(fixture_${variant} SHARED tests/fake_steamclient.S)
        target_compile_definitions(fixture_${variant} PRIVATE ${define})
        set_target_properties(fixture_${variant} PROPERTIES PREFIX "" LINKER_LANGUAGE C)
        list(APPEND fixtures fixture_${variant})
    endforeach()

    add_executable(test_fixtures tests/test_fixtures.c)
    target_link_libraries(test_fixtures PRIVATE wheelio_steam_core)
    add_dependencies(test_fixtures ${fixtures})
    add_test(NAME fixtures COMMAND test_fixtures $<TARGET_FILE_DIR:fixture_good>)

    add_executable(test_patch_in_process tests/test_patch_in_process.c
        src/code_patcher.c src/steam_patch_apply.c)
    target_link_libraries(test_patch_in_process PRIVATE wheelio_steam_core)
    add_test(NAME patch_in_process COMMAND test_patch_in_process $<TARGET_FILE:fixture_good>)

    add_executable(test_real_steamclient tests/test_real_steamclient.c)
    target_link_libraries(test_real_steamclient PRIVATE wheelio_steam_core)
    add_test(NAME real_steamclient COMMAND test_real_steamclient)
    set_tests_properties(real_steamclient PROPERTIES SKIP_RETURN_CODE 77)
endif()
