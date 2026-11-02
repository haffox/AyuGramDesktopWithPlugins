# The plugin engine is compiled into the REAL Telegram/AyuGram target, not a baseline.
# Keep Python.h outside the host PCH (Qt's macros and Windows headers are not safe there).
if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This embedded-runtime packaging targets Windows x64. Configure the matching CPython layout before porting to another platform.")
endif()
if (NOT QT_VERSION_MAJOR EQUAL 6)
    message(FATAL_ERROR "Python plugins require Qt 6: use prepare/win.bat qt6 and configure.bat qt6.")
endif()
set(Python3_FIND_STRATEGY LOCATION)
find_package(Python3 3.12 EXACT REQUIRED COMPONENTS Interpreter Development.Embed)
find_package(Threads REQUIRED)
set(AYU_PYTHON_RUNTIME_DIR "" CACHE PATH "Extracted CPython 3.12 x64 embeddable distribution")
if (NOT EXISTS "${AYU_PYTHON_RUNTIME_DIR}/python312.dll" OR
    NOT EXISTS "${AYU_PYTHON_RUNTIME_DIR}/python312.zip")
    message(FATAL_ERROR "Set AYU_PYTHON_RUNTIME_DIR to the extracted CPython 3.12 x64 embed package.")
endif()
add_library(ayu_python_runtime STATIC
    "${src_loc}/plugins/python_runtime.cpp"
    "${src_loc}/plugins/python_runtime.h"
)
init_non_host_target(ayu_python_runtime)
target_link_libraries(ayu_python_runtime PRIVATE Python3::Python Threads::Threads)
# CPython owns and releases its own allocations; no FILE*/CRT allocations cross the ABI.
# /MT here matches the static-Qt host, while python312.dll retains its own /MD CRT.
target_sources(Telegram PRIVATE
    "${src_loc}/plugins/plugin_manager.cpp"
    "${src_loc}/plugins/plugin_manager.h"
    "${src_loc}/plugins/customization_manager.cpp"
    "${src_loc}/plugins/customization_manager.h"
    "${src_loc}/plugins/plugin_settings.cpp"
    "${src_loc}/plugins/plugin_settings.h"
    "${src_loc}/plugins/settings_store.cpp"
    "${src_loc}/plugins/settings_store.h"
    "${src_loc}/plugins/desktop_integration.cpp"
    "${src_loc}/plugins/desktop_integration.h"
)
target_link_libraries(Telegram PRIVATE ayu_python_runtime)
# Windows resolves python312.dll while loading the executable, before main().
# Keep that DLL beside AyuGram.exe, and the stdlib/.pyd modules in python/.
add_custom_command(TARGET Telegram POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${AYU_PYTHON_RUNTIME_DIR}" "$<TARGET_FILE_DIR:Telegram>/python"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${AYU_PYTHON_RUNTIME_DIR}/python312.dll" "$<TARGET_FILE_DIR:Telegram>/python312.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${AYU_PYTHON_RUNTIME_DIR}/vcruntime140.dll" "$<TARGET_FILE_DIR:Telegram>/vcruntime140.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${AYU_PYTHON_RUNTIME_DIR}/vcruntime140_1.dll" "$<TARGET_FILE_DIR:Telegram>/vcruntime140_1.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${res_loc}/plugin-sdk" "$<TARGET_FILE_DIR:Telegram>/plugin-sdk"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${res_loc}/plugin-examples" "$<TARGET_FILE_DIR:Telegram>/plugin-examples"
    VERBATIM
)
