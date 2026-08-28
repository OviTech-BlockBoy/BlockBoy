cmake_minimum_required(VERSION 3.5)
# Deliberately no wifi/esp_http_server: the flasher is the recovery route and
# keeps as few dependencies as possible. The launcher downloads, this flashes.
set(COMPONENTS "main retro-go")
include(../base.cmake)
project(flasher)
