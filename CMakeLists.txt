cmake_minimum_required(VERSION 3.7)
project(esp-eMQTT5)

add_library(esp-eMQTT5 
	src/Network/Clients/MQTTClient.cpp
        test/main/main.cpp)

set_target_properties(eMQTT5 PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

include_directories(
        test/build/include
        test/xtensa-esp32-elf/xtensa-esp32-elf/include
        test/esp-idf/components/esp32/include
        test/esp-idf/components/freertos/include
        test/esp-idf/components/log/include
        test/esp-idf/components/tcpip_adapter/include
        test/esp-idf/components/lwip/include/lwip/port
        test/esp-idf/components/lwip/include/lwip
        test/esp-idf/components/nvs_flash/include
        test/esp-idf/components/mbedtls/mbedtls/include
        test/esp-idf/components/mbedtls/port/include
        .)


target_compile_definitions(esp-eMQTT5 PUBLIC _DEBUG=$<CONFIG:Debug> MinimalFootPrint=$<STREQUAL:${REDUCED_FOOTPRINT},ON>)

IF (WIN32)
ELSE()
  target_compile_options(esp-eMQTT5 PUBLIC $<$<CONFIG:MinSizeRel>:-fno-rtti -ffunction-sections -fdata-sections>)
ENDIF()

target_include_directories(esp-eMQTT5 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

