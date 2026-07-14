cmake_minimum_required(VERSION 3.19)
project(NewPipeline_Wicked)

if(APPLE)
    message(FATAL_ERROR
        "NewPipeline_Wicked macOS builds use NewPipeline_Wicked_MacOS.xcodeproj. "
        "The native Cocoa targets retain their existing WebRTC archive sanitizing and runtime packaging.")
elseif(UNIX)
    message(FATAL_ERROR
        "NewPipeline_Wicked Linux WebRTC packaging is not implemented. Use macOS Xcode or Windows VS2022 x64.")
elseif(NOT WIN32)
    message(FATAL_ERROR "NewPipeline_Wicked supports macOS Xcode and Windows VS2022 x64 only.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "NewPipeline_Wicked Windows requires Visual Studio 2022 / MSVC.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NewPipeline_Wicked Windows requires an x64 generator (-A x64).")
endif()

set(NEWPIPELINE_WEBRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/WebRTC/7827")
set(NEWPIPELINE_WEBRTC_INCLUDE "${NEWPIPELINE_WEBRTC_ROOT}/include")
set(NEWPIPELINE_WEBRTC_DEBUG_LIB "${NEWPIPELINE_WEBRTC_ROOT}/debug/webrtc.lib")
set(NEWPIPELINE_WEBRTC_RELEASE_LIB "${NEWPIPELINE_WEBRTC_ROOT}/release/webrtc.lib")
set(NEWPIPELINE_WEBRTC_REQUIRED_FILES
    "${NEWPIPELINE_WEBRTC_INCLUDE}/api/peer_connection_interface.h"
    "${NEWPIPELINE_WEBRTC_INCLUDE}/absl/strings/string_view.h"
    "${NEWPIPELINE_WEBRTC_INCLUDE}/third_party/libyuv/include/libyuv/convert.h"
    "${NEWPIPELINE_WEBRTC_DEBUG_LIB}"
    "${NEWPIPELINE_WEBRTC_RELEASE_LIB}"
)
foreach(REQUIRED_FILE IN LISTS NEWPIPELINE_WEBRTC_REQUIRED_FILES)
    if(NOT EXISTS "${REQUIRED_FILE}")
        message(FATAL_ERROR
            "NewPipeline_Wicked WebRTC 7827 package is incomplete: ${REQUIRED_FILE}\n"
            "Place VS2022 x64 /MTd Debug and /MT Release libraries at "
            "WebRTC/7827/debug/webrtc.lib and WebRTC/7827/release/webrtc.lib.")
    endif()
endforeach()

add_library(NewPipeline_Wicked_WebRTC STATIC IMPORTED GLOBAL)
set_target_properties(NewPipeline_Wicked_WebRTC PROPERTIES
    IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
    IMPORTED_LOCATION_DEBUG "${NEWPIPELINE_WEBRTC_DEBUG_LIB}"
    IMPORTED_LOCATION_RELEASE "${NEWPIPELINE_WEBRTC_RELEASE_LIB}"
    IMPORTED_LOCATION_RELWITHDEBINFO "${NEWPIPELINE_WEBRTC_RELEASE_LIB}"
    IMPORTED_LOCATION_MINSIZEREL "${NEWPIPELINE_WEBRTC_RELEASE_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${NEWPIPELINE_WEBRTC_INCLUDE}"
)

set(NEWPIPELINE_SHARED_SOURCE
    NewPipelineRuntime.cpp
    NewPipelineProtocol.cpp
    NewPipelineTransport.cpp
    NewPipelineWebRTCBridge.cpp
    NewPipelineScene.cpp
    NewPipelineWindowsHost.cpp
)
set(NEWPIPELINE_SHARED_HEADERS
    NewPipelineRuntime.h
    NewPipelineProtocol.h
    NewPipelineTransport.h
    NewPipelineWebRTCBridge.h
    NewPipelineScene.h
    NewPipelineWindowsHost.h
)

function(newpipeline_wicked_add_windows_target TARGET_NAME ENTRY APP_SOURCE APP_HEADER RENDER_SOURCE RENDER_HEADER)
    add_executable(${TARGET_NAME} WIN32
        ${ENTRY}
        ${APP_SOURCE}
        ${APP_HEADER}
        ${RENDER_SOURCE}
        ${RENDER_HEADER}
        ${NEWPIPELINE_SHARED_SOURCE}
        ${NEWPIPELINE_SHARED_HEADERS}
    )
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
    target_compile_definitions(${TARGET_NAME} PRIVATE
        WEBRTC_WIN=1
        NOMINMAX=1
        WIN32_LEAN_AND_MEAN=1
        _SILENCE_CXX20_IS_POD_DEPRECATION_WARNING=1
        $<$<CONFIG:Debug>:NDEBUG>
    )
    target_compile_options(${TARGET_NAME} PRIVATE
        /URTC_DISABLE_CHECK_MSG
        /utf-8
        /wd4005 /wd4068 /wd4127 /wd4146 /wd4245 /wd4819 /wd4996
    )
    set_property(TARGET ${TARGET_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_target_properties(${TARGET_NAME} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/WickedEngine"
        FOLDER "Samples/NewPipeline_Wicked"
    )
    target_link_libraries(${TARGET_NAME} PRIVATE
        WickedEngine
        NewPipeline_Wicked_WebRTC
        ws2_32 winhttp iphlpapi winmm secur32 crypt32 advapi32
        msdmo dmoguids wmcodecdspuuid
    )
    add_dependencies(${TARGET_NAME} Content)
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WICKED_ROOT_DIR}/WickedEngine/dxcompiler.dll"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>/dxcompiler.dll"
        VERBATIM
    )
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES
        ${ENTRY} ${APP_SOURCE} ${APP_HEADER} ${RENDER_SOURCE} ${RENDER_HEADER}
        ${NEWPIPELINE_SHARED_SOURCE} ${NEWPIPELINE_SHARED_HEADERS})
endfunction()

newpipeline_wicked_add_windows_target(
    NewPipeline_Wicked_Client
    main_windows_client.cpp
    NewPipelineClientApp.cpp NewPipelineClientApp.h
    NewPipelineClientRenderPath.cpp NewPipelineClientRenderPath.h)
target_sources(NewPipeline_Wicked_Client PRIVATE
    NewPipelineClientLightmap.cpp
    NewPipelineClientLightmap.h
    "${WICKED_ROOT_DIR}/Editor/xatlas.cpp"
    "${WICKED_ROOT_DIR}/Editor/xatlas.h")

newpipeline_wicked_add_windows_target(
    NewPipeline_Wicked_Server
    main_windows_server.cpp
    NewPipelineServerApp.cpp NewPipelineServerApp.h
    NewPipelineServerRenderPath.cpp NewPipelineServerRenderPath.h)
