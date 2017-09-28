# This application has its own Kconfig options.
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)

# CONF_FILE is prj.conf, plus (in addition to, not or) the following, if they
# exist:
#   - local.conf
#   - boards/${BOARD}.conf
#   - boards/${BOARD}-local.conf
#
# The possibly nonexistent files need to be absolute paths; otherwise, the
# behavior of EXISTS is not well-defined.
set(CONF_FILE prj.conf)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/local.conf)
  set(CONF_FILE ${CONF_FILE} " local.conf")
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}.conf)
  set(CONF_FILE ${CONF_FILE} " boards/${BOARD}.conf")
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}-local.conf)
  set(CONF_FILE ${CONF_FILE} " boards/${BOARD}-local.conf")
endif()

# DTC overlays are per-board; each board must define a boards/$BOARD.overlay.
set(DTC_OVERLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boards)

# Mandatory Zephyr boilerplate.
include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(NONE)

# Application "library" build configuration. TODO: move these out of this tree.
target_sources(app PRIVATE src/lib/flash_block.c)
target_sources(app PRIVATE src/lib/mcuboot.c)
target_sources(app PRIVATE src/lib/product_id.c)
target_sources(app PRIVATE src/lib/lwm2m_credentials.c)

# Application build configuration.
target_include_directories(app PRIVATE $ENV{ZEPHYR_BASE}/net/ip)
target_include_directories(app PRIVATE $ENV{ZEPHYR_BASE}/tests/include)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lib)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE src/app_work_queue.c)
target_sources(app PRIVATE src/lwm2m.c)
target_sources_ifdef(CONFIG_NET_L2_BT        app PRIVATE src/bluetooth.c)
target_sources_ifdef(CONFIG_SYS_LOG_EXT_HOOK app PRIVATE src/tstamp_log.c)

target_link_libraries_ifdef(CONFIG_MBEDTLS app mbedTLS)
