# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Apply ESP-IDF patches (idempotent — safe to re-run)
# Patches are checked into patches/ and applied to $IDF_PATH at configure time.
# git apply --check verifies the patch hasn't been applied yet.
file(GLOB IDF_PATCHES "${CMAKE_SOURCE_DIR}/patches/*.patch")
foreach(PATCH_FILE ${IDF_PATCHES})
    execute_process(
        COMMAND git apply --check "${PATCH_FILE}"
        WORKING_DIRECTORY $ENV{IDF_PATH}
        RESULT_VARIABLE PATCH_CHECK_RESULT
        OUTPUT_QUIET ERROR_QUIET
    )
    if(PATCH_CHECK_RESULT EQUAL 0)
        execute_process(
            COMMAND git apply "${PATCH_FILE}"
            WORKING_DIRECTORY $ENV{IDF_PATH}
            RESULT_VARIABLE PATCH_APPLY_RESULT
        )
        if(PATCH_APPLY_RESULT EQUAL 0)
            message(STATUS "Applied ESP-IDF patch: ${PATCH_FILE}")
        else()
            message(WARNING "Failed to apply ESP-IDF patch: ${PATCH_FILE}")
        endif()
    else()
        message(STATUS "ESP-IDF patch already applied (skipping): ${PATCH_FILE}")
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=sdmmc_read_sectors" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--undefined=__wrap_sdmmc_read_sectors" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=sdmmc_write_sectors" APPEND)
idf_build_set_property(LINK_OPTIONS "-Wl,--undefined=__wrap_sdmmc_write_sectors" APPEND)

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(tusb_msc)
