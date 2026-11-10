# Define source files
set(SOURCES "audio/audio_codec.cc"
            "audio/audio_service.cc"
            "audio/demuxer/ogg_demuxer.cc"
            "audio/codecs/no_audio_codec.cc"
            "audio/codecs/box_audio_codec.cc"
            "audio/codecs/es8311_audio_codec.cc"
            "audio/codecs/es8374_audio_codec.cc"
            "audio/codecs/es8388_audio_codec.cc"
            "audio/codecs/es8389_audio_codec.cc"
            "audio/codecs/dummy_audio_codec.cc"
            "audio/processors/audio_debugger.cc"
            "led/single_led.cc"
            "led/circular_strip.cc"
            "led/gpio_led.cc"
            "display/display.cc"
            "display/lcd_display.cc"
            "display/oled_display.cc"
            "display/lvgl_display/lvgl_display.cc"
            "display/emote_display.cc"
            "display/lvgl_display/emoji_collection.cc"
            "display/lvgl_display/lvgl_theme.cc"
            "display/lvgl_display/lvgl_font.cc"
            "display/lvgl_display/lvgl_image.cc"
            "display/lvgl_display/gif/lvgl_gif.cc"
            "display/lvgl_display/gif/gifdec.c"
            "display/lvgl_display/jpg/image_to_jpeg.cpp"
            "display/lvgl_display/jpg/jpeg_to_image.c"
            "protocols/protocol.cc"
            "protocols/mqtt_protocol.cc"
            "protocols/websocket_protocol.cc"
            "mcp_server.cc"
            "system_info.cc"
            "application.cc"
            "ota.cc"
            "settings.cc"
            "device_state_machine.cc"
            "assets.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "display" "display/lvgl_display" "display/lvgl_display/jpg" "audio" "audio/demuxer" "protocols")

# Add board common files (only the helpers used by the dooirobot board)
list(APPEND SOURCES
    "boards/common/board.cc"
    "boards/common/wifi_board.cc"
    "boards/common/afsk_demod.cc"
    "boards/common/backlight.cc"
    "boards/common/button.cc"
    "boards/common/system_reset.cc"
)
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)


idf_build_get_property(build_components BUILD_COMPONENTS)
# Function to find component dynamically by pattern
function(find_component_by_pattern PATTERN COMPONENT_VAR PATH_VAR)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${PATTERN}")
            set(${COMPONENT_VAR} ${COMPONENT} PARENT_SCOPE)
            idf_component_get_property(COMPONENT_PATH ${COMPONENT} COMPONENT_DIR)
            set(${PATH_VAR} "${COMPONENT_PATH}" PARENT_SCOPE)
            break()
        endif()
    endforeach()
endfunction()

# Set default BUILTIN_TEXT_FONT and BUILTIN_ICON_FONT
set(BUILTIN_TEXT_FONT font_puhui_14_1)
set(BUILTIN_ICON_FONT font_awesome_14_1)

set(EMOTE_RESOLUTION "320_240")

# Add board files according to BOARD_TYPE
# This project keeps only the dooirobot (ESP32-P4) board; all other vendor boards were removed.
# NOTE: during the component requirement pre-pass CONFIG_* values are not available yet,
# so only complain when the sdkconfig value is actually known.
if(DEFINED CONFIG_BOARD_TYPE_DOOIROBOT AND NOT CONFIG_BOARD_TYPE_DOOIROBOT)
    message(FATAL_ERROR "Unsupported board type: select DOOIROBOT in menuconfig -> Xiaozhi Assistant -> Board Type")
endif()
set(BOARD_TYPE "dooirobot")

# The dooirobot board CMakeLists.txt collects its own sources, include dirs and assets.
# It must be skipped while CMake runs the requirement pre-pass in script mode.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE})
    list(APPEND SOURCES ${BOARD_SOURCES})
endif()

# Select audio processor according to Kconfig
if(CONFIG_USE_AUDIO_PROCESSOR)
    list(APPEND SOURCES "audio/processors/afe_audio_processor.cc")
else()
    list(APPEND SOURCES "audio/processors/no_audio_processor.cc")
endif()
if(CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32P4)
    list(APPEND SOURCES "audio/wake_words/afe_wake_word.cc")
    list(APPEND SOURCES "audio/wake_words/custom_wake_word.cc")
else()
    list(APPEND SOURCES "audio/wake_words/esp_wake_word.cc")
endif()

# Auto Select Additional Sources
if (CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING)
    list(APPEND SOURCES "boards/common/blufi.cpp")
endif ()
# Select language directory according to Kconfig
if(CONFIG_LANGUAGE_ZH_CN)
    set(LANG_DIR "zh-CN")
elseif(CONFIG_LANGUAGE_ZH_TW)
    set(LANG_DIR "zh-TW")
elseif(CONFIG_LANGUAGE_EN_US)
    set(LANG_DIR "en-US")
elseif(CONFIG_LANGUAGE_JA_JP)
    set(LANG_DIR "ja-JP")
elseif(CONFIG_LANGUAGE_KO_KR)
    set(LANG_DIR "ko-KR")
elseif(CONFIG_LANGUAGE_VI_VN)
    set(LANG_DIR "vi-VN")
elseif(CONFIG_LANGUAGE_TH_TH)
    set(LANG_DIR "th-TH")
elseif(CONFIG_LANGUAGE_DE_DE)
    set(LANG_DIR "de-DE")
elseif(CONFIG_LANGUAGE_FR_FR)
    set(LANG_DIR "fr-FR")
elseif(CONFIG_LANGUAGE_ES_ES)
    set(LANG_DIR "es-ES")
elseif(CONFIG_LANGUAGE_IT_IT)
    set(LANG_DIR "it-IT")
elseif(CONFIG_LANGUAGE_RU_RU)
    set(LANG_DIR "ru-RU")
elseif(CONFIG_LANGUAGE_AR_SA)
    set(LANG_DIR "ar-SA")
elseif(CONFIG_LANGUAGE_HI_IN)
    set(LANG_DIR "hi-IN")
elseif(CONFIG_LANGUAGE_PT_PT)
    set(LANG_DIR "pt-PT")
elseif(CONFIG_LANGUAGE_PL_PL)
    set(LANG_DIR "pl-PL")
elseif(CONFIG_LANGUAGE_CS_CZ)
    set(LANG_DIR "cs-CZ")
elseif(CONFIG_LANGUAGE_FI_FI)
    set(LANG_DIR "fi-FI")
elseif(CONFIG_LANGUAGE_TR_TR)
    set(LANG_DIR "tr-TR")
elseif(CONFIG_LANGUAGE_ID_ID)
    set(LANG_DIR "id-ID")
elseif(CONFIG_LANGUAGE_UK_UA)
    set(LANG_DIR "uk-UA")
elseif(CONFIG_LANGUAGE_RO_RO)
    set(LANG_DIR "ro-RO")
elseif(CONFIG_LANGUAGE_BG_BG)
    set(LANG_DIR "bg-BG")
elseif(CONFIG_LANGUAGE_CA_ES)
    set(LANG_DIR "ca-ES")
elseif(CONFIG_LANGUAGE_DA_DK)
    set(LANG_DIR "da-DK")
elseif(CONFIG_LANGUAGE_EL_GR)
    set(LANG_DIR "el-GR")
elseif(CONFIG_LANGUAGE_FA_IR)
    set(LANG_DIR "fa-IR")
elseif(CONFIG_LANGUAGE_FIL_PH)
    set(LANG_DIR "fil-PH")
elseif(CONFIG_LANGUAGE_HE_IL)
    set(LANG_DIR "he-IL")
elseif(CONFIG_LANGUAGE_HR_HR)
    set(LANG_DIR "hr-HR")
elseif(CONFIG_LANGUAGE_HU_HU)
    set(LANG_DIR "hu-HU")
elseif(CONFIG_LANGUAGE_MS_MY)
    set(LANG_DIR "ms-MY")
elseif(CONFIG_LANGUAGE_NB_NO)
    set(LANG_DIR "nb-NO")
elseif(CONFIG_LANGUAGE_NL_NL)
    set(LANG_DIR "nl-NL")
elseif(CONFIG_LANGUAGE_SK_SK)
    set(LANG_DIR "sk-SK")
elseif(CONFIG_LANGUAGE_SL_SI)
    set(LANG_DIR "sl-SI")
elseif(CONFIG_LANGUAGE_SV_SE)
    set(LANG_DIR "sv-SE")
elseif(CONFIG_LANGUAGE_SR_RS)
    set(LANG_DIR "sr-RS")
endif()

# Define generation path
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")

# Collect current language audio files
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/*.ogg)

# If not en-US, collect en-US audio files as fallback for missing files
if(NOT LANG_DIR STREQUAL "en-US")
    file(GLOB EN_US_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/en-US/*.ogg)
    
    # Extract filenames (without path) from current language
    set(EXISTING_NAMES "")
    foreach(SOUND_FILE ${LANG_SOUNDS})
        get_filename_component(FILENAME ${SOUND_FILE} NAME)
        list(APPEND EXISTING_NAMES ${FILENAME})
    endforeach()
    
    # Only add en-US audio files that are missing in current language
    foreach(EN_SOUND ${EN_US_SOUNDS})
        get_filename_component(FILENAME ${EN_SOUND} NAME)
        if(NOT ${FILENAME} IN_LIST EXISTING_NAMES)
            list(APPEND LANG_SOUNDS ${EN_SOUND})
            message(STATUS "Using en-US fallback for missing audio: ${FILENAME}")
        endif()
    endforeach()
endif()

file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg)

# This project targets ESP32-P4 only. The dooirobot board implements its own
# camera path (app_vision/dooi_camera.cc) on top of the esp_video component,
# so the generic common/esp_video.cc, common/rndis_board.cc and
# common/esp32_camera.cc helpers (used by other chips / boards) are not needed.

# 网页资源(html/css/js/wasm)与界面图片(gif/png)必须内嵌并 mmap 到 flash 直接使用：
# 走文件系统读太慢（gif 解码、页面加载都会明显卡顿）。
idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS} ${JPG_FILES} ${INDEX_HTML} ${GIF_FILES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    PRIV_REQUIRES
                        esp_lcd
                        esp_driver_touch_sens
                        esp_pm
                        esp_psram
                        esp_netif
                        esp_driver_gpio
                        esp_driver_uart
                        esp_driver_spi
                        esp_driver_i2c
                        esp_driver_i2s
                        esp_driver_jpeg
                        esp_driver_ppa
                        esp_app_format
                        app_update
                        spi_flash
                        console
                        efuse
                        bt
                        little_eyes
                    )

# Use target_compile_definitions to define BOARD_TYPE, BOARD_NAME
# If BOARD_NAME is empty, use BOARD_TYPE
if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()
target_compile_definitions(${COMPONENT_LIB}
                    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                    PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT} BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT}
                    )

# Add generation rules
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --language "${LANG_DIR}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)

# Force build generation dependencies
add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)

# Apply WiFi patches for ESP32-P4 + C6 ESP-Hosted architecture
# Patches adjust timing/cleanup in 78__esp-wifi-connect managed component
# to accommodate SDIO RPC latency on the co-processor.
find_component_by_pattern("78__esp-wifi-connect" WIFI_CONNECT_COMPONENT WIFI_CONNECT_COMPONENT_PATH)
add_custom_target(apply_wifi_patches ALL
    COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/wifi_connect_patches/apply_patches.py ${PROJECT_DIR}
    COMMENT "Checking and applying WiFi patches for ESP32-P4 + C6 ESP-Hosted"
)
# Ensure patches are applied before main component is compiled
add_dependencies(${COMPONENT_LIB} apply_wifi_patches)

# Apply ESP-DL patches (ESP32-P4 SIMD image transform workaround, esp-dl#305)
# Runs at configure time, i.e. after the component manager has (re-)downloaded
# esp-dl into managed_components and before any compilation starts.
# The script also refreshes CHECKSUMS.json/.component_hash so the component
# manager does not consider the component "tampered" and re-download it.
if(CONFIG_IDF_TARGET_ESP32P4)
    find_component_by_pattern("espressif__esp-dl" ESP_DL_COMPONENT ESP_DL_COMPONENT_PATH)
    if(ESP_DL_COMPONENT_PATH)
        execute_process(
            COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/esp_dl_patches/apply_patches.py ${PROJECT_DIR}
            RESULT_VARIABLE ESP_DL_PATCH_RESULT
        )
        if(NOT ESP_DL_PATCH_RESULT EQUAL 0)
            message(WARNING "esp-dl patch script failed (${ESP_DL_PATCH_RESULT}); "
                            "P4 vision SIMD crash workaround was NOT applied")
        endif()
    else()
        message(STATUS "esp-dl not found in build, skip ESP-DL patches")
    endif()
endif()


# Find ESP-SR component dynamically
find_component_by_pattern("espressif__esp-sr" ESP_SR_COMPONENT ESP_SR_COMPONENT_PATH)
if(ESP_SR_COMPONENT_PATH)
    set(ESP_SR_MODEL_PATH "${ESP_SR_COMPONENT_PATH}/model")
endif()

# Find xiaozhi-fonts component dynamically
find_component_by_pattern("xiaozhi-fonts" XIAOZHI_FONTS_COMPONENT XIAOZHI_FONTS_COMPONENT_PATH)
if(XIAOZHI_FONTS_COMPONENT_PATH)
    set(XIAOZHI_FONTS_PATH "${XIAOZHI_FONTS_COMPONENT_PATH}")
endif()


# Function to build default assets based on configuration
function(build_default_assets_bin)
    # Set output path for generated assets.bin
    set(GENERATED_ASSETS_BIN "${CMAKE_BINARY_DIR}/generated_assets.bin")
    
    # Prepare arguments for build script
    set(BUILD_ARGS
        "--sdkconfig" "${SDKCONFIG}"
        "--output" "${GENERATED_ASSETS_BIN}"
    )
    
    # 分区表里有独立的 model 分区时：模型由 esp-sr 组件打包并烧到该分区，
    # assets.bin 里不再重复存放一份（index.json 里的 multinet_model 配置仍保留）
    partition_table_get_partition_info(model_size "--partition-name model" "size")
    if("${model_size}")
        list(APPEND BUILD_ARGS "--skip_sr_models")
    endif()
    
    # Add builtin text font if defined
    if(BUILTIN_TEXT_FONT)
        list(APPEND BUILD_ARGS "--builtin_text_font" "${BUILTIN_TEXT_FONT}")
    endif()
    
    # Add default emoji collection if defined
    if(DEFAULT_EMOJI_COLLECTION)
        list(APPEND BUILD_ARGS "--emoji_collection" "${DEFAULT_EMOJI_COLLECTION}")
    endif()
    
    # Add default assets extra files if defined
    if(DEFAULT_ASSETS_EXTRA_FILES)
        list(APPEND BUILD_ARGS "--extra_files" "${DEFAULT_ASSETS_EXTRA_FILES}")
    endif()
    
    list(APPEND BUILD_ARGS "--esp_sr_model_path" "${ESP_SR_MODEL_PATH}")
    list(APPEND BUILD_ARGS "--xiaozhi_fonts_path" "${XIAOZHI_FONTS_PATH}")
    
    # Create custom command to build assets
    add_custom_command(
        OUTPUT ${GENERATED_ASSETS_BIN}
        COMMAND python ${PROJECT_DIR}/scripts/build_default_assets.py ${BUILD_ARGS}
        DEPENDS
            ${SDKCONFIG}
            ${PROJECT_DIR}/scripts/build_default_assets.py
        COMMENT "Building default assets.bin based on configuration"
        VERBATIM
    )
    
    # Create target for generated assets
    add_custom_target(generated_default_assets ALL
        DEPENDS ${GENERATED_ASSETS_BIN}
    )
    
    # Set the generated file path in parent scope
    set(GENERATED_ASSETS_LOCAL_FILE ${GENERATED_ASSETS_BIN} PARENT_SCOPE)
    
    message(STATUS "Default assets build configured: ${GENERATED_ASSETS_BIN}")
endfunction()


# Function to get local assets file path (handles both URL and local file)
function(get_assets_local_file assets_source assets_local_file_var)
    # Check if it's a URL (starts with http:// or https://)
    if(assets_source MATCHES "^https?://")
        # It's a URL, download it
        get_filename_component(ASSETS_FILENAME "${assets_source}" NAME)
        set(ASSETS_LOCAL_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}")
        set(ASSETS_TEMP_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}.tmp")
        
        # Check if local file exists
        if(EXISTS ${ASSETS_LOCAL_FILE})
            message(STATUS "Assets file ${ASSETS_FILENAME} already exists, skipping download")
        else()
            message(STATUS "Downloading ${ASSETS_FILENAME}")
            
            # Clean up any existing temp file
            if(EXISTS ${ASSETS_TEMP_FILE})
                file(REMOVE ${ASSETS_TEMP_FILE})
            endif()
            
            # Download to temporary file first
            file(DOWNLOAD ${assets_source} ${ASSETS_TEMP_FILE}
                 STATUS DOWNLOAD_STATUS)
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            if(NOT STATUS_CODE EQUAL 0)
                # Clean up temp file on failure
                if(EXISTS ${ASSETS_TEMP_FILE})
                    file(REMOVE ${ASSETS_TEMP_FILE})
                endif()
                message(FATAL_ERROR "Failed to download ${ASSETS_FILENAME} from ${assets_source}")
            endif()
            
            # Move temp file to final location (atomic operation)
            file(RENAME ${ASSETS_TEMP_FILE} ${ASSETS_LOCAL_FILE})
            message(STATUS "Successfully downloaded ${ASSETS_FILENAME}")
        endif()
    else()
        # It's a local file path
        if(IS_ABSOLUTE "${assets_source}")
            set(ASSETS_LOCAL_FILE "${assets_source}")
        else()
            set(ASSETS_LOCAL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${assets_source}")
        endif()
        
        # Check if local file exists
        if(NOT EXISTS ${ASSETS_LOCAL_FILE})
            message(FATAL_ERROR "Assets file not found: ${ASSETS_LOCAL_FILE}")
        endif()
        
        message(STATUS "Using assets file: ${ASSETS_LOCAL_FILE}")
    endif()
    
    set(${assets_local_file_var} ${ASSETS_LOCAL_FILE} PARENT_SCOPE)
endfunction()


partition_table_get_partition_info(size "--partition-name assets" "size")
partition_table_get_partition_info(offset "--partition-name assets" "offset")
if ("${size}" AND "${offset}")
    # Flash assets based on configuration
    if(CONFIG_FLASH_DEFAULT_ASSETS)
        # Build default assets based on configuration
        build_default_assets_bin()
        esptool_py_flash_to_partition(flash "assets" "${GENERATED_ASSETS_LOCAL_FILE}")
        message(STATUS "Generated default assets flash configured: ${GENERATED_ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_CUSTOM_ASSETS)
        # Flash custom assets
        get_assets_local_file("${CONFIG_CUSTOM_ASSETS_FILE}" ASSETS_LOCAL_FILE)
        esptool_py_flash_to_partition(flash "assets" "${ASSETS_LOCAL_FILE}")
        message(STATUS "Custom assets flash configured: ${ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_EXPRESSION_ASSETS)
        set(ASSETS_NAME "expression_assets")
        set(ASSETS_PARTITION "assets")
        set(ASSETS_FILE "${CMAKE_BINARY_DIR}/${ASSETS_NAME}.bin")

        build_speaker_assets_bin("${ASSETS_PARTITION}" ${EMOTE_RESOLUTION} ${ASSETS_FILE} ${CONFIG_MMAP_FILE_NAME_LENGTH})
        message(STATUS "Generated emote assets: ${ASSETS_FILE} -> ${ASSETS_PARTITION} partition")
        esptool_py_flash_to_partition(flash "${ASSETS_PARTITION}" "${ASSETS_FILE}")
    elseif(CONFIG_FLASH_NONE_ASSETS)
        message(STATUS "Assets flashing disabled (FLASH_NONE_ASSETS)")
    endif()
else()
    message(STATUS "Assets partition not found, using v1 partition table")
endif()


# ----------------------------------------------------------------------------
# 语音模型 -> 独立 model 分区
# 只要分区表里存在名为 "model" 的分区，esp-sr 组件（managed_components/
# espressif__esp-sr/CMakeLists.txt）就会自动把 srmodels.bin 打包并烧到该分区，
# 这里无需重复处理。固件侧由 esp_srmodel_init("model") 直接 mmap 使用。
# 注意：assets.bin 构建时已传 --skip_sr_models，避免模型在 assets 分区里再存一份。
# ----------------------------------------------------------------------------
partition_table_get_partition_info(model_size "--partition-name model" "size")
if("${model_size}")
    message(STATUS "Speech models -> 'model' partition (packed & flashed by esp-sr component)")
else()
    message(WARNING "No 'model' partition in the partition table, speech models will be "
                    "loaded from assets.bin (legacy layout)")
endif()
