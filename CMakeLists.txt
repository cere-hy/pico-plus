# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# 每代必 bump（审计裁决）：TF 升级靠版本串区分代际，同版本会被判拒绝。
# gen19 漏 bump（仍带 pico18 出厂，gen20 审计③点名）；gen20 起恢复每代一跳。
# Per-generation bump (audit ruling): the TF upgrader tells generations apart by
# this string and rejects a same-version package. gen19 missed its bump (shipped
# as pico18 - called out in the gen20 audit); gen20 restores the per-gen step.
if(DEFINED ENV{PICO_PROJECT_VER} AND NOT "$ENV{PICO_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{PICO_PROJECT_VER}")
else()
    set(PROJECT_VER "0.3.9-pico26.9.25")
endif()

# ── 可复现构建：SOURCE_DATE_EPOCH → 固定构建时刻 ─────────────────────────────
# IDF v6.1 没有原生的 SOURCE_DATE_EPOCH 支持：CONFIG_APP_REPRODUCIBLE_BUILD 只是把
# esp_app_desc 的 date/time 置空（APP_COMPILE_TIME_DATE 因 depends 失效而关闭）。界面仍
# 需要一个确定的构建时刻（首页固件卡片、「时间设置」未同步时的默认起点），所以在这里
# 把 epoch 折成固定的 UTC 字符串，以编译定义 PICO_BUILD_DATE/TIME 注入 main。
# 缺省值写死在源码里：源码不动，戳就不动。需要换戳时导出 SOURCE_DATE_EPOCH 重编即可
# （这属于有意的内容变更，SHA 随之改变）。
# IDF v6.1 has no native SOURCE_DATE_EPOCH support: CONFIG_APP_REPRODUCIBLE_BUILD merely
# blanks esp_app_desc's date/time. The UI still needs a definite build instant, so the epoch
# is rendered to fixed UTC strings here and injected into main as compile definitions. The
# default lives in the source tree, so the stamp cannot move unless the source does.
if(DEFINED ENV{SOURCE_DATE_EPOCH} AND NOT "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
    set(PICO_BUILD_EPOCH "$ENV{SOURCE_DATE_EPOCH}")
else()
    set(PICO_BUILD_EPOCH "1793232000")  # 2026-10-29T00:00:00Z
endif()
# %e 与 __DATE__ 的日期位同为空格填充（"Oct 29 2026" / "Oct  9 2026"），代码侧的解析口径不变。
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "LC_ALL=C" date -u -d "@${PICO_BUILD_EPOCH}" "+%b %e %Y"
    OUTPUT_VARIABLE PICO_BUILD_DATE OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _pico_date_rc)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "LC_ALL=C" date -u -d "@${PICO_BUILD_EPOCH}" "+%H:%M:%S"
    OUTPUT_VARIABLE PICO_BUILD_TIME OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _pico_time_rc)
if(NOT _pico_date_rc EQUAL 0 OR NOT _pico_time_rc EQUAL 0)
    message(FATAL_ERROR "SOURCE_DATE_EPOCH=${PICO_BUILD_EPOCH} 不是可解析的 Unix 时间戳")
endif()
message(STATUS "可复现构建戳 Build stamp: ${PICO_BUILD_DATE} ${PICO_BUILD_TIME} UTC (epoch=${PICO_BUILD_EPOCH})")

project(Read_Pico)

# 固件描述时间统一为 UTC，与概览页标识一致；保留已有编译缓存启动器。
# Compile app-description timestamps in UTC to match Overview; preserve any compiler-cache launcher.
idf_component_get_property(app_desc_lib esp_app_format COMPONENT_LIB)
get_target_property(app_desc_launcher ${app_desc_lib} C_COMPILER_LAUNCHER)
if(NOT app_desc_launcher)
    set(app_desc_launcher "")
endif()
set(app_desc_utc_launcher "${CMAKE_COMMAND}" -E env TZ=UTC)
if(app_desc_launcher)
    list(APPEND app_desc_utc_launcher ${app_desc_launcher})
endif()
set_property(TARGET ${app_desc_lib} PROPERTY C_COMPILER_LAUNCHER "${app_desc_utc_launcher}")

# panic 包装器的链接选项在 main/CMakeLists.txt 里挂到 main 组件上：写在 project() 之后
# 用 idf_build_set_property 设全局链接选项已经太晚，链接器看不到 --wrap。
# The panic wrapper's link option hangs off the main component in main/CMakeLists.txt: setting a
# global link option after project() is too late for the linker to see --wrap.
