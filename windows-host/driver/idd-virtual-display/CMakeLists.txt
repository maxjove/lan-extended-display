if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "led_idd_driver is a Windows/MSVC-only UMDF driver target")
endif()

set(LED_WDK_ROOT "" CACHE PATH "Windows Driver Kit root, for example F:/Windows Kits/10")
set(LED_WDK_VERSION "10.0.26100.0" CACHE STRING "Windows Driver Kit version")
set(LED_UMDF_VERSION "2.25" CACHE STRING "UMDF framework version")
set(LED_IDDCX_VERSION "1.2" CACHE STRING "IddCx interface version")

if(NOT LED_WDK_ROOT)
    if(DEFINED ENV{WindowsSdkDir})
        file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" LED_WDK_ROOT)
    elseif(EXISTS "F:/Windows Kits/10")
        set(LED_WDK_ROOT "F:/Windows Kits/10")
    elseif(EXISTS "C:/Program Files (x86)/Windows Kits/10")
        set(LED_WDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
    endif()
endif()

string(REGEX REPLACE "^2\\." "" LED_UMDF_MINOR "${LED_UMDF_VERSION}")

if(NOT EXISTS "${LED_WDK_ROOT}/Include/${LED_WDK_VERSION}/um/iddcx/${LED_IDDCX_VERSION}/IddCx.h")
    message(FATAL_ERROR "IddCx headers not found. Set LED_WDK_ROOT/LED_WDK_VERSION/LED_IDDCX_VERSION.")
endif()

set(LED_WDK_ARCH "x64")
if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
    set(LED_WDK_ARCH "arm64")
elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "Win32")
    set(LED_WDK_ARCH "x86")
endif()

add_library(led_idd_driver SHARED
    src/driver.cpp)

target_compile_features(led_idd_driver PRIVATE cxx_std_20)

target_compile_definitions(led_idd_driver PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    UMDF_VERSION_MAJOR=2
    UMDF_VERSION_MINOR=${LED_UMDF_MINOR})

target_include_directories(led_idd_driver PRIVATE
    "${LED_WDK_ROOT}/Include/wdf/umdf/${LED_UMDF_VERSION}"
    "${LED_WDK_ROOT}/Include/wdf/shared"
    "${LED_WDK_ROOT}/Include/${LED_WDK_VERSION}/shared"
    "${LED_WDK_ROOT}/Include/${LED_WDK_VERSION}/um"
    "${LED_WDK_ROOT}/Include/${LED_WDK_VERSION}/um/iddcx/${LED_IDDCX_VERSION}"
    "${LED_WDK_ROOT}/Include/${LED_WDK_VERSION}/ucrt")

target_link_directories(led_idd_driver PRIVATE
    "${LED_WDK_ROOT}/Lib/wdf/umdf/${LED_WDK_ARCH}/${LED_UMDF_VERSION}"
    "${LED_WDK_ROOT}/Lib/${LED_WDK_VERSION}/um/${LED_WDK_ARCH}"
    "${LED_WDK_ROOT}/Lib/${LED_WDK_VERSION}/um/${LED_WDK_ARCH}/iddcx/${LED_IDDCX_VERSION}"
    "${LED_WDK_ROOT}/Lib/${LED_WDK_VERSION}/ucrt/${LED_WDK_ARCH}")

target_link_libraries(led_idd_driver PRIVATE
    WdfDriverStubUm
    iddcxstub
    d3d11
    dxgi
    advapi32
    ntdll)

target_compile_options(led_idd_driver PRIVATE
    /W4
    /WX-
    /EHsc-
    /GR-)

set_target_properties(led_idd_driver PROPERTIES
    OUTPUT_NAME "led_idd"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/driver/idd-virtual-display"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/driver/idd-virtual-display"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/driver/idd-virtual-display")

target_link_options(led_idd_driver PRIVATE
    /SUBSYSTEM:WINDOWS)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/led_idd.inf.in"
    "${CMAKE_BINARY_DIR}/driver/idd-virtual-display/led_idd.inf"
    @ONLY)

add_custom_target(led_idd_package
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:led_idd_driver>"
        "${CMAKE_BINARY_DIR}/driver/idd-virtual-display/led_idd.dll"
    DEPENDS led_idd_driver
    COMMENT "Preparing LED IddCx driver package")
