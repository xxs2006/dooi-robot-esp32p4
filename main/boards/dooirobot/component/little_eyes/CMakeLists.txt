# =============================================================================
#  little_eyes（Uncanny Eyes）—— 跨平台仿生眼睛渲染库（ESP-IDF 组件）
#
#  本目录是"发行版"：只包含 头文件 + 预编译静态库，不含库源码。
#  工程开箱即用，不需要额外的库目录。
#
#    ├── include/                       公共头文件
#    └── lib/<IDF_TARGET>/liblittle_eyes.a   预编译静态库
#
#  库源码仓库：<见同目录 README.md>
#
#  切换用法：
#    1) 默认                      -> 链接 lib/<IDF_TARGET>/liblittle_eyes.a
#    2) -DLITTLE_EYES_PREBUILT=OFF-> 源码编译（LITTLE_EYES_DIR 必须指向带 src/ 的库源码目录）
#
#  注意：组件内一律用 CMAKE_CURRENT_LIST_DIR，不要用 CMAKE_CURRENT_SOURCE_DIR ——
#        IDF 在"早期展开"阶段用 include() 加载组件 CMakeLists，后者会指向 build 目录。
# =============================================================================

cmake_minimum_required(VERSION 3.13)

if(NOT ESP_PLATFORM)
    message(FATAL_ERROR
        "little_eyes 发行版只包含预编译库，不能作为普通 CMake 子工程使用。\n"
        "需要源码请到库仓库：见 ${CMAKE_CURRENT_LIST_DIR}/README.md")
endif()

set(_prebuilt_a "${CMAKE_CURRENT_LIST_DIR}/lib/${IDF_TARGET}/liblittle_eyes.a")
set(_src_core   "${CMAKE_CURRENT_LIST_DIR}/src/eyes_core.c")

option(LITTLE_EYES_PREBUILT "链接预编译静态库（OFF = 源码编译）" ON)

if(LITTLE_EYES_PREBUILT)
    if(NOT EXISTS "${_prebuilt_a}")
        message(FATAL_ERROR
            "little_eyes: 没有 ${IDF_TARGET} 的预编译库\n"
            "  期望路径: ${_prebuilt_a}\n"
            "解决办法（二选一）：\n"
            "  1) 用库源码目录生成预编译库后拷到上面的路径；\n"
            "  2) 改用源码编译：把 LITTLE_EYES_DIR 指向库源码目录，并\n"
            "     idf.py build -DLITTLE_EYES_PREBUILT=OFF")
    endif()

    idf_component_register(
        SRCS         ""                                  # 不编译源码，只提供头文件
        INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
    )

    add_prebuilt_library(little_eyes_prebuilt "${_prebuilt_a}"
                         REQUIRES esp_timer esp_hw_support)
    target_link_libraries(${COMPONENT_LIB} INTERFACE little_eyes_prebuilt)

    message(STATUS "little_eyes: prebuilt library (${IDF_TARGET}) -> ${_prebuilt_a}")
else()
    if(NOT EXISTS "${_src_core}")
        message(FATAL_ERROR
            "little_eyes: 当前目录没有库源码（只有预编译库），无法源码编译。\n"
            "  请把 LITTLE_EYES_DIR 指向库源码目录（该目录下应有 src/eyes_core.c），\n"
            "  或直接去掉 -DLITTLE_EYES_PREBUILT=OFF 使用预编译库。")
    endif()

    set(EYES_SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/src/eyes_core.c
        ${CMAKE_CURRENT_LIST_DIR}/src/eyes_preset.c
        ${CMAKE_CURRENT_LIST_DIR}/src/eyes_port_default.c
        ${CMAKE_CURRENT_LIST_DIR}/src/eyes_port_fb.c
        ${CMAKE_CURRENT_LIST_DIR}/src/assets/eye_source.c
        ${CMAKE_CURRENT_LIST_DIR}/src/assets/disp_240_125.c
        ${CMAKE_CURRENT_LIST_DIR}/src/assets/polarAngle_240.c
        ${CMAKE_CURRENT_LIST_DIR}/src/assets/polarDist_240_130_95_0.c
    )

    idf_component_register(
        SRCS         ${EYES_SOURCES}
        INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
        REQUIRES     esp_timer esp_hw_support
    )

    # 眼睛渲染是全局最热的循环（每帧几万个像素，全是查表 + 定点运算），单独开 -O3
    set_source_files_properties(
        ${CMAKE_CURRENT_LIST_DIR}/src/eyes_core.c
        PROPERTIES COMPILE_OPTIONS "-O3")
endif()
