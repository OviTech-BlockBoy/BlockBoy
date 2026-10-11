include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# rg_tool.py passes -DRG_PROJECT_VER=<version>. Without this line ESP-IDF stamps
# "1" into the app descriptor (esp_app_desc_t.version), and the flasher's
# downgrade check reads the installed version from exactly that field. So the
# real firmware version has to land there, not only in a compile define.
if(RG_PROJECT_VER)
    set(PROJECT_VER "${RG_PROJECT_VER}")
endif()
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

macro(rg_setup_compile_options)
    component_compile_options(
        -D${RG_BUILD_TARGET}=1
        -DRETRO_GO=1
        -fjump-tables -ftree-switch-conversion
        ${ARGV}
    )

    if(RG_ENABLE_NETPLAY)
        component_compile_options(-DRG_ENABLE_NETWORKING -DRG_ENABLE_NETPLAY)
    elseif(RG_ENABLE_NETWORKING)
        component_compile_options(-DRG_ENABLE_NETWORKING)
    endif()

    if(RG_OTA_ALLOW_INSECURE_MANIFEST)
        # Workbench only: lets tools/serve_ota.py serve updates over plain http.
        # Set in no shipped configuration; updater.c logs a warning on every
        # update check so a build that got away is easy to spot.
        component_compile_options(-DRG_OTA_ALLOW_INSECURE_MANIFEST=1)
    endif()

    if(RG_ENABLE_PROFILING)
        # Still debating whether -fno-inline is necessary or not...
        component_compile_options(-DRG_ENABLE_PROFILING -finstrument-functions)
    endif()
endmacro()
