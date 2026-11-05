cmake_minimum_required(VERSION 3.16)
project(safir LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SAFIR_TARGET "HOST" CACHE STRING "Target platform: HOST or SWITCH")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/libs/vocr ${CMAKE_CURRENT_SOURCE_DIR}/libs/picojpeg ${CMAKE_CURRENT_SOURCE_DIR}/core/ocr)

if (SAFIR_TARGET STREQUAL "HOST")
    message(STATUS "Configuring safir HOST target (Parity verification & tests)")

    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wall -Wextra -Werror -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    elseif (MSVC)
        # Statically linked CRT avoids dynamic ASan runtime loader issues on Windows
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        add_compile_options(/W4 /WX /Zi /fsanitize=address /utf-8 /wd4819 /wd5105)
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
        add_link_options(/DEBUG)
    else()
        add_compile_options(-Wall -Wextra -Werror)
    endif()

    file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/core/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp")
    # Real VOCR6.2 recognizer, vendored from bee-san/vita-vn-ocr (GPLv3, libs/vocr/LICENSE)
    set(VOCR_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/libs/vocr/vocr.c ${CMAKE_CURRENT_SOURCE_DIR}/libs/vocr/vocr_lines.c)
    # Region-only JPEG decoding for caps:sc JPEG screenshots (public domain, libs/picojpeg)
    set(PICOJPEG_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/libs/picojpeg/picojpeg.c)
    if(MSVC)
        # MSVC rejects GCC-style flags; /fp:precise (default) already forbids FP contraction.
        # Vendored picojpeg: C4189 unused local, C4505 unreferenced static fn, C4244 int->uint8 narrowing.
        set_source_files_properties(${PICOJPEG_SOURCES} PROPERTIES COMPILE_OPTIONS "/wd4189;/wd4505;/wd4244")
    else()
        set_source_files_properties(${VOCR_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
        set_source_files_properties(${PICOJPEG_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-but-set-variable;-Wno-unused-function")
    endif()
    list(APPEND CORE_SOURCES ${VOCR_SOURCES})
    list(APPEND CORE_SOURCES ${PICOJPEG_SOURCES})
    add_library(safir_core STATIC ${CORE_SOURCES})
    target_include_directories(safir_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # The test suite and its screenshots are not part of the public repository; it is built
    # only where it exists (the developer's checkout).
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp)
    enable_testing()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/test_dict.bin
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/dict_packer.py
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test_yomitan_fixture.json
                -o ${CMAKE_BINARY_DIR}/test_dict.bin
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/dict_packer.py ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test_yomitan_fixture.json
        COMMENT "Compiling test Yomitan dictionary binary"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/test_long_run.bin
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/dict_packer.py
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test_long_run.json
                -o ${CMAKE_BINARY_DIR}/test_long_run.bin
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/dict_packer.py ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/test_long_run.json
        COMMENT "Compiling the long same-key run test dictionary"
    )
    add_custom_target(generate_test_dict DEPENDS ${CMAKE_BINARY_DIR}/test_dict.bin ${CMAKE_BINARY_DIR}/test_long_run.bin)

    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/test_model.vocr
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_or_synth_models.py
                -o ${CMAKE_BINARY_DIR}/test_model.vocr
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch_or_synth_models.py
        COMMENT "Fetching or synthesizing test VOCR model"
    )
    add_custom_target(generate_test_model DEPENDS ${CMAKE_BINARY_DIR}/test_model.vocr)

    add_executable(safir_tests
        tests/test_main.cpp
        tests/test_dict.cpp
        tests/test_lookup.cpp
        tests/test_glyph_cache.cpp
        tests/test_layout.cpp
        tests/test_switch_state.cpp
        tests/test_e2e_vn.cpp
        tests/test_soak_fuzz.cpp
        tests/test_ocr_real.cpp
        tests/bench_hotpaths.cpp
        switch/mock_libnx.c
        switch/games_ini.c
        switch/bg_worker.c
        switch/overlay_ui.cpp
    )
    target_include_directories(safir_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/switch)
    target_compile_definitions(safir_tests PRIVATE SAFIR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(safir_tests PRIVATE safir_core)
    add_dependencies(safir_tests generate_test_dict generate_test_model)
    # Real-network tests need the released model and the rendered dialogue fixture.
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/switch_screenshot_h1v2.jpg ${CMAKE_BINARY_DIR}/switch_screenshot_h1v2.jpg COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vn_busy_small_720p.jpg ${CMAKE_BINARY_DIR}/vn_busy_small_720p.jpg COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vn_screen_720p.jpg ${CMAKE_BINARY_DIR}/vn_screen_720p.jpg COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/switch_two_lines_panel.jpg ${CMAKE_BINARY_DIR}/switch_two_lines_panel.jpg COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vn_dialogue_1100x210.pgm ${CMAKE_BINARY_DIR}/vn_dialogue_1100x210.pgm COPYONLY)
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.cache/models/VOCR6.2_S_w8.vocr)
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/.cache/models/VOCR6.2_S_w8.vocr ${CMAKE_BINARY_DIR}/real_model.vocr COPYONLY)
    endif()
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.cache/models/VOCR6.2_N_w8g2.vocr)
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/.cache/models/VOCR6.2_N_w8g2.vocr ${CMAKE_BINARY_DIR}/real_model_n.vocr COPYONLY)
    endif()
    add_test(NAME safir_ledger_tests COMMAND safir_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})


    add_executable(render_preview_ppm tools/render_preview_ppm.cpp)
    target_link_libraries(render_preview_ppm PRIVATE safir_core)
    add_test(NAME render_preview_test COMMAND render_preview_ppm preview_popup.bmp preview_popup.ppm WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    endif()

elseif (SAFIR_TARGET STREQUAL "SWITCH")
    message(STATUS "Configuring safir SWITCH target (.ovl overlay)")

    if (NOT DEFINED ENV{DEVKITPRO})
        message(STATUS "DEVKITPRO environment variable is not defined. Skipping Switch build.")
        return()
    endif()

    set(DEVKITPRO $ENV{DEVKITPRO})
    if (NOT EXISTS "${DEVKITPRO}/libnx")
        message(STATUS "libnx not found at ${DEVKITPRO}/libnx. Skipping Switch build.")
        return()
    endif()

    message(STATUS "Found devkitPro libnx at: ${DEVKITPRO}/libnx")

    set(LIBTESLA_INCLUDE_DIRS "")
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/libtesla/include")
        list(APPEND LIBTESLA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/libs/libtesla/include")
    endif()
    if (EXISTS "/opt/libs/libtesla/include")
        list(APPEND LIBTESLA_INCLUDE_DIRS "/opt/libs/libtesla/include")
    endif()

    # In Switch mode, link with libnx and libtesla when building the .ovl
    file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/core/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp")
    # Real VOCR6.2 recognizer, vendored from bee-san/vita-vn-ocr (GPLv3, libs/vocr/LICENSE)
    set(VOCR_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/libs/vocr/vocr.c ${CMAKE_CURRENT_SOURCE_DIR}/libs/vocr/vocr_lines.c)
    set_source_files_properties(${VOCR_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    list(APPEND CORE_SOURCES ${VOCR_SOURCES})
    # Region-only JPEG decoding for caps:sc JPEG screenshots (public domain, libs/picojpeg)
    set(PICOJPEG_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/libs/picojpeg/picojpeg.c)
    set_source_files_properties(${PICOJPEG_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-but-set-variable;-Wno-unused-function")
    list(APPEND CORE_SOURCES ${PICOJPEG_SOURCES})
    add_library(safir_core STATIC ${CORE_SOURCES})
    target_include_directories(safir_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${DEVKITPRO}/libnx/include
    )
    # The overlay hands POOL_OCR_WEIGHTS one heap block sized to the chosen model
    # (safir_pool_attach), so .bss holds no model memory.
    target_compile_definitions(safir_core PRIVATE SAFIR_OCR_WEIGHTS_EXTERNAL)

    add_library(safir_switch STATIC
        switch/games_ini.c
        switch/bg_worker.c
        switch/mock_libnx.c
        switch/overlay_ui.cpp
    )
    target_include_directories(safir_switch PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/switch
        ${LIBTESLA_INCLUDE_DIRS}
        ${DEVKITPRO}/libnx/include
        ${DEVKITPRO}/portlibs/switch/include
    )

    add_executable(safir.elf
        switch/main_ovl.cpp
    )
    set_target_properties(safir.elf PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    target_include_directories(safir.elf PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/switch
        ${LIBTESLA_INCLUDE_DIRS}
        ${DEVKITPRO}/libnx/include
        ${DEVKITPRO}/portlibs/switch/include
    )
    target_link_libraries(safir.elf PRIVATE
        safir_switch
        safir_core
        nx
        m
    )

    find_program(NX_ELF2NRO_EXE NAMES elf2nro HINTS "${DEVKITPRO}/tools/bin")
    find_program(NX_NACPTOOL_EXE NAMES nacptool HINTS "${DEVKITPRO}/tools/bin")

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/safir.nacp
        COMMAND ${NX_NACPTOOL_EXE} --create "Safir" "jenduis" "0.1.0" ${CMAKE_CURRENT_BINARY_DIR}/safir.nacp
        COMMENT "Creating safir.nacp"
    )

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/safir.ovl
        COMMAND ${NX_ELF2NRO_EXE} $<TARGET_FILE:safir.elf> ${CMAKE_CURRENT_BINARY_DIR}/safir.ovl --nacp=${CMAKE_CURRENT_BINARY_DIR}/safir.nacp
        DEPENDS safir.elf ${CMAKE_CURRENT_BINARY_DIR}/safir.nacp
        COMMENT "Building safir.ovl overlay from safir.elf"
    )

    add_custom_target(safir_ovl ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/safir.ovl)
else()
    message(FATAL_ERROR "Unknown SAFIR_TARGET: ${SAFIR_TARGET}. Use HOST or SWITCH.")
endif()
