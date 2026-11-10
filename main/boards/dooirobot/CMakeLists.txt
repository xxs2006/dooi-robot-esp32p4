# boards/dooirobot/CMakeLists.txt
# 这个文件会被主 CMake 的 add_subdirectory() 调用

# 收集 dooirobot 板卡专属的源文件
set(BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dooirobot.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/dooi_audio_codec.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/dooi_nvs.c
)

# 收集各个 app 模块下的源文件（只找 .c 和 .cc）
file(GLOB_RECURSE APP_PERIPHERAL_SOURCES      ${CMAKE_CURRENT_SOURCE_DIR}/app_peripheral/*.c      ${CMAKE_CURRENT_SOURCE_DIR}/app_peripheral/*.cc)
file(GLOB_RECURSE APP_UI_SOURCES              ${CMAKE_CURRENT_SOURCE_DIR}/app_ui/*.c              ${CMAKE_CURRENT_SOURCE_DIR}/app_ui/*.cc)
file(GLOB_RECURSE APP_VISON_SOURCES           ${CMAKE_CURRENT_SOURCE_DIR}/app_vision/*.c           ${CMAKE_CURRENT_SOURCE_DIR}/app_vision/*.cc)
file(GLOB_RECURSE APP_REMOTE_CONTROL_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/app_remote_control/*.c    ${CMAKE_CURRENT_SOURCE_DIR}/app_remote_control/*.cc)
file(GLOB_RECURSE APP_BEHAVIOR_SOURCES        ${CMAKE_CURRENT_SOURCE_DIR}/app_behavior/*.c)

# 把所有 app 模块的源文件合并到 BOARD_SOURCES
list(APPEND BOARD_SOURCES
    ${APP_PERIPHERAL_SOURCES}
    ${APP_UI_SOURCES}
    ${APP_VISON_SOURCES}
    ${APP_REMOTE_CONTROL_SOURCES}
    ${APP_BEHAVIOR_SOURCES}
)
set(BOARD_SOURCES ${BOARD_SOURCES} PARENT_SCOPE)


list(APPEND INCLUDE_DIRS 
    ${CMAKE_CURRENT_SOURCE_DIR}     
    ${CMAKE_CURRENT_SOURCE_DIR}/app_peripheral
    ${CMAKE_CURRENT_SOURCE_DIR}/app_ui
    ${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages
    ${CMAKE_CURRENT_SOURCE_DIR}/app_ui/display_hal
    ${CMAKE_CURRENT_SOURCE_DIR}/app_ui/ui_logic
    ${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/uncanny_eyes
    ${LITTLE_EYES_DIR}/include                 # little_eyes 跨平台眼睛库（预编译库组件）
    ${CMAKE_CURRENT_SOURCE_DIR}/app_vision
    ${CMAKE_CURRENT_SOURCE_DIR}/app_remote_control
    ${CMAKE_CURRENT_SOURCE_DIR}/app_behavior
)
set(INCLUDE_DIRS ${INCLUDE_DIRS} PARENT_SCOPE)


# 行为资源 JSON（spiffs）→ behavior 分区，挂载点 /resources
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app_behavior/resources")
    spiffs_create_partition_image(behavior "${CMAKE_CURRENT_SOURCE_DIR}/app_behavior/resources" FLASH_IN_PROJECT)
endif()


# test source; temp use
# file(GLOB JPG_TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/app_vision/test/*.jpg")
# list(APPEND JPG_FILES ${JPG_TEST_FILES})
# set(JPG_FILES ${JPG_FILES} PARENT_SCOPE)

# 收集 index.html 文件
file(GLOB INDEX_HTML_FILES       "${CMAKE_CURRENT_SOURCE_DIR}/app_remote_control/html/index.html")
file(GLOB SETTINGS_HTML_FILES    "${CMAKE_CURRENT_SOURCE_DIR}/app_remote_control/html/settings.html")
file(GLOB GAMEPAD_HTML_FILES     "${CMAKE_CURRENT_SOURCE_DIR}/app_remote_control/html/gamepad.html")
file(GLOB BEHAVIOR_HTML_FILES    "${CMAKE_CURRENT_SOURCE_DIR}/app_remote_control/html/behavior.html")

# Little Eyes Studio 网页资源：镜像位于 component/little_eyes/eyes-studio/web
# （由 little_eyes/tools/sync_to_board.py 生成；studio.html 为 index.html 同内容别名，
#  嵌入符号按 basename 生成，需全局唯一，故不能直接嵌 index.html）
set(STUDIO_WEB ${CMAKE_CURRENT_SOURCE_DIR}/component/little_eyes/eyes-studio/web)
file(GLOB STUDIO_STUDIO_HTML   "${STUDIO_WEB}/studio.html")
file(GLOB STUDIO_CSS_FILES     "${STUDIO_WEB}/css/*.css")
file(GLOB STUDIO_JS_FILES      "${STUDIO_WEB}/js/*.js")
file(GLOB STUDIO_LIB_FILES     "${STUDIO_WEB}/lib/*.js"
                               "${STUDIO_WEB}/lib/*.wasm")

list(APPEND INDEX_HTML ${INDEX_HTML_FILES})
list(APPEND INDEX_HTML ${SETTINGS_HTML_FILES})
list(APPEND INDEX_HTML ${GAMEPAD_HTML_FILES})
list(APPEND INDEX_HTML ${BEHAVIOR_HTML_FILES})
list(APPEND INDEX_HTML ${STUDIO_STUDIO_HTML})
list(APPEND INDEX_HTML ${STUDIO_CSS_FILES})
list(APPEND INDEX_HTML ${STUDIO_JS_FILES})
list(APPEND INDEX_HTML ${STUDIO_LIB_FILES})
set(INDEX_HTML ${INDEX_HTML} PARENT_SCOPE)

# 收集 图像资源 文件
file(GLOB LUCKY_CAT_RED_GIF     "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/lucky_cat/source/lucky_cat_red.gif")
file(GLOB LUCKY_CAT_YELLOW_GIF  "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/lucky_cat/source/lucky_cat_yellow.gif")
file(GLOB LUCKY_CAT_PURPLE_GIF  "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/lucky_cat/source/lucky_cat_purple.gif")
file(GLOB LUCKY_CAT_BLUE_GIF    "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/lucky_cat/source/lucky_cat_blue.gif")
file(GLOB MUSIC_PET_GIF         "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/music_page/source/music_pet.gif")
file(GLOB CUSTOM_PAGE_PNG       "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/custom_page/source/manual.png")

file(GLOB WEATHER_SUNNY_PNG           "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/weather/source/sunny.png")
file(GLOB WEATHER_PARTLY_CLOUDY_PNG    "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/weather/source/partly_cloudy.png")
file(GLOB WEATHER_CLOUDY_PNG           "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/weather/source/cloudy.png")
file(GLOB WEATHER_FOGGY_PNG            "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/weather/source/foggy.png")
file(GLOB WEATHER_DRIZZLE_PNG          "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/weather/source/drizzle.png")
file(GLOB WEATHER_RAINY_PNG            "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/weather/source/rainy.png")
file(GLOB WEATHER_HEAVY_RAIN_PNG       "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/weather/source/heavy_rain.png")
file(GLOB WEATHER_SNOWY_PNG            "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/weather/source/snowy.png")
file(GLOB WEATHER_HEAVY_SNOW_PNG       "${CMAKE_CURRENT_SOURCE_DIR}/app_ui/pages/weather/source/heavy_snow.png")

list(APPEND GIF_FILES ${WEATHER_SUNNY_PNG})
list(APPEND GIF_FILES ${WEATHER_PARTLY_CLOUDY_PNG})
list(APPEND GIF_FILES ${WEATHER_CLOUDY_PNG})
list(APPEND GIF_FILES ${WEATHER_FOGGY_PNG})
list(APPEND GIF_FILES ${WEATHER_DRIZZLE_PNG})
list(APPEND GIF_FILES ${WEATHER_RAINY_PNG})
list(APPEND GIF_FILES ${WEATHER_HEAVY_RAIN_PNG})
list(APPEND GIF_FILES ${WEATHER_SNOWY_PNG})
list(APPEND GIF_FILES ${WEATHER_HEAVY_SNOW_PNG})

list(APPEND GIF_FILES ${CUSTOM_PAGE_PNG})
list(APPEND GIF_FILES ${LUCKY_CAT_RED_GIF})
list(APPEND GIF_FILES ${LUCKY_CAT_YELLOW_GIF})
list(APPEND GIF_FILES ${LUCKY_CAT_PURPLE_GIF})
list(APPEND GIF_FILES ${LUCKY_CAT_BLUE_GIF})
list(APPEND GIF_FILES ${MUSIC_PET_GIF})
set(GIF_FILES ${GIF_FILES} PARENT_SCOPE)
