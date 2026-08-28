set(COMPONENT_SRCDIRS ". drivers/audio fonts libs/netplay libs/lodepng")
set(COMPONENT_ADD_INCLUDEDIRS ". libs/netplay libs/lodepng")
# Embedded theme JSON files — accessible from C via _binary_<basename>_json_start/end.
# Adding a theme here makes it appear in the Theme picker without needing an SD-card copy.
set(COMPONENT_EMBED_TXTFILES "themes/dmg.json" "themes/classic.json")
if((IDF_VERSION_MAJOR EQUAL 5) AND (IDF_VERSION_MINOR GREATER 2)) # esp-idf 5.3+
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client mbedtls esp_driver_ledc driver usb usb_host_uac bt esp_hid")
elseif(IDF_VERSION_MAJOR EQUAL 5) # esp-idf 5.0 - 5.2
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client mbedtls usb usb_host_uac bt esp_hid")
else() # esp-idf 4.1 - 4.4
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update esp_adc_cal json nvs_flash esp_wifi esp_http_client")
endif()

register_component()

# Small size is preferred because of the small cache and most things here aren't performance sensitive!
# rg_display and rg_audio benefit from higher optimization (which of -O2 or -O3 is better depends...)

component_compile_options(
    -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    -DLODEPNG_NO_COMPILE_ERROR_TEXT
    -Os
    -Wno-unused-function
)
set_source_files_properties(
    rg_audio.c rg_display.c rg_utils.c rg_surface.c libs/lodepng/lodepng.c
    PROPERTIES COMPILE_FLAGS
    -O2
)

if(RG_ENABLE_NETPLAY)
    component_compile_options(-DRG_ENABLE_NETWORKING -DRG_ENABLE_NETPLAY)
elseif(RG_ENABLE_NETWORKING)
    component_compile_options(-DRG_ENABLE_NETWORKING)
endif()

if(RG_ENABLE_PROFILING)
    component_compile_options(-DRG_ENABLE_PROFILING)
endif()

if(RG_PROJECT_VER)
    component_compile_options(-DRG_PROJECT_VER="${RG_PROJECT_VER}")
endif()

if(RG_BUILD_RELEASE)
    component_compile_options(-DRG_BUILD_RELEASE=1)
endif()

set(RG_BUILD_INFO "${RG_BUILD_TARGET}-${RG_BUILD_RELEASE} ${PROJECT_VER} SDK:${IDF_VER} ${IDF_TARGET}")
string(TIMESTAMP RG_BUILD_TIME "%s")

component_compile_options(-D${RG_BUILD_TARGET}=1)
component_compile_options(-DRG_PROJECT_APP="${RG_PROJECT_APP}")

# gbsp keeps 128KB of internal SRAM in static BSS (96KB vram + 32KB iwram),
# leaving too little heap for the USB host's isochronous buffers. Measured:
# uac_host_device_start() fails with ESP_ERR_NO_MEM (2938 bytes free, largest
# block 1216), and when it does start it dies within seconds. Hot-pluggable
# audio sinks fall back to the built-in output in this app.
if(RG_PROJECT_APP STREQUAL "gbsp")
    component_compile_options(-DRG_AUDIO_NO_HOTPLUG_SINK=1)
endif()
component_compile_options(-DRG_BUILD_INFO="${RG_BUILD_INFO}")
component_compile_options(-DRG_BUILD_TIME=${RG_BUILD_TIME})

message(STATUS "RG_PROJECT_APP: ${RG_PROJECT_APP}")
message(STATUS "RG_PROJECT_VER: ${RG_PROJECT_VER}")
message(STATUS "RG_BUILD_INFO: ${RG_BUILD_INFO}")
