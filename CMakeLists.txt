cmake_minimum_required(VERSION 3.21)
project(OpenGhostNative VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_FIND_PACKAGE_RESOLVE_SYMLINKS ON)
# OpenGhost's native QML uses recent Qt APIs.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2
    QuickDialogs2 ShaderTools)
qt_policy(SET QTP0001 NEW)

function(native_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

set(diagram_sources src/diagram.cpp src/diagram_core.cpp src/diagram_paint.cpp
    src/diagram_flow.cpp src/diagram_charts.cpp src/diagram_page.cpp src/diagram_figures.cpp
    src/diagram_data.cpp src/diagram_subjects.cpp src/diagram_scene.cpp src/filekinds.cpp src/cssfont.cpp)
add_library(native_rich STATIC src/markdown.cpp src/media.cpp src/highlight.cpp src/tex.cpp
    ${diagram_sources})
target_include_directories(native_rich PUBLIC src)
target_link_libraries(native_rich PUBLIC Qt6::Gui)
native_warnings(native_rich)

# The semantic boundary and frontend state link ONLY QtCore. A future wire
# adapter will implement Backend over ByteTransport, never inside the window.
add_library(native_contract STATIC src/backend/types.h src/backend/backend.h
    src/backend/transport.h src/backend/fake_backend.cpp
    src/frontend/preferences.cpp src/frontend/chat_service.cpp
    src/frontend/attachments.cpp src/frontend/usage.cpp src/frontend/store.cpp
    src/frontend/library.cpp src/frontend/host.h)
target_include_directories(native_contract PUBLIC src)
target_link_libraries(native_contract PUBLIC Qt6::Core)
native_warnings(native_contract)

add_library(native_window STATIC src/model.cpp src/settings.cpp src/window.cpp src/motion.cpp
    src/appearance.cpp src/presentation.cpp src/offline_services.h src/platform/desktop.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(native_window PRIVATE src/platform/linux.cpp)
else()
    target_sources(native_window PRIVATE src/platform/portable.cpp)
endif()
qt_add_qml_module(native_window URI OpenGhost.Native VERSION 1.0 NO_PLUGIN RESOURCE_PREFIX /
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/OpenGhost/Native
    SOURCES src/rich.h src/rich.cpp src/ghost.h src/ghost.cpp src/icon.h src/icon.cpp
        src/shadow.h src/shadow.cpp src/reveal.h src/reveal.cpp
        src/exposure.h src/exposure.cpp src/wave.h src/wave.cpp
        src/selection.h src/selection.cpp
        src/diagramview.h src/diagramview.cpp src/medialoader.h src/medialoader.cpp)
target_link_libraries(native_window PUBLIC native_contract native_rich Qt6::Gui Qt6::Network
    Qt6::Quick Qt6::QuickControls2)
native_warnings(native_window)
qt_add_shaders(native_window "ui_shaders" PREFIX / FILES
    shaders/blur.frag shaders/oil.frag shaders/glass.frag shaders/veil.frag shaders/tone.frag
    shaders/frame.frag shaders/mist.frag)

qt_add_executable(openghost-native src/main.cpp)
set_target_properties(openghost-native PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.openghost.native-ui")
target_link_libraries(openghost-native PRIVATE native_window Qt6::Qml Qt6::QuickDialogs2)
native_warnings(openghost-native)
foreach(name Main ChatEntry LiveText Sidebar SettingsDialog Markdown Block InlineText CodeBlock
        TableBlock DiagramBlock WorkingGhost WelcomeGhost Splash EffortControl Metrics
        IconButton Spring ModelGlyph ModelButton ModelStage EffortStage Feather UsagePage
        AppearancePage GeneralPage OverlayScrollBar JumpButton CopyButton Entrance SelectArea
        SelectDriver SelectableLabel SelectionMenu SelectionVeil VeilFilter TitleTip ButtonTip
        Backdrop RowLift TextGlow ApprovalCard ModePicker ModeDock MediaBlock MediaStack)
    set_source_files_properties(qml/${name}.qml PROPERTIES QT_RESOURCE_ALIAS ${name}.qml)
    list(APPEND qml_files qml/${name}.qml)
endforeach()
qt_add_qml_module(openghost-native URI OpenGhost.Ui VERSION 1.0 RESOURCE_PREFIX /
    QML_FILES ${qml_files} IMPORTS OpenGhost.Native)
qt_add_resources(openghost-native icon PREFIX / BASE resources FILES resources/openghost.png)

# Small opt-in launch/rendering check. No Python, Node, Rust or live backend needed.
option(OPENGHOST_BUILD_SMOKE_TEST "Build the standalone UI smoke check" OFF)
if(OPENGHOST_BUILD_SMOKE_TEST)
    enable_testing()
    find_package(Qt6 6.11 REQUIRED COMPONENTS Test)
    target_sources(openghost-native PRIVATE tests/smoke.cpp tests/parity/native.cpp)
    target_link_libraries(openghost-native PRIVATE Qt6::Test)
    target_compile_definitions(openghost-native PRIVATE OPENGHOST_SMOKE_TEST)
    add_executable(native_contract_test tests/contract.cpp src/settings.cpp)
    target_link_libraries(native_contract_test PRIVATE native_contract Qt6::Test)
    native_warnings(native_contract_test)
    add_test(NAME native_contract_test COMMAND native_contract_test)
    set_tests_properties(native_contract_test PROPERTIES TIMEOUT 15)
    # Reply pictures and videos: parsing, address rules and the loader's
    # policy, with an injected fetch. No network.
    add_executable(native_media_test tests/media.cpp)
    target_link_libraries(native_media_test PRIVATE native_window Qt6::Test)
    native_warnings(native_media_test)
    add_test(NAME native_media_test COMMAND native_media_test)
    set_tests_properties(native_media_test PROPERTIES TIMEOUT 15
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME native_ui_smoke COMMAND openghost-native --smoke-test)
    add_test(NAME native_fake_ui_smoke COMMAND openghost-native --smoke-test --fake-backend)
    set_tests_properties(native_ui_smoke native_fake_ui_smoke PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QPA_PLATFORMTHEME=generic;QT_FORCE_STDERR_LOGGING=1;OPENGHOST_REDUCED_MOTION=0")
endif()

include(GNUInstallDirs)
install(FILES LICENSE NOTICE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/openghost-native)
install(DIRECTORY licenses/ DESTINATION ${CMAKE_INSTALL_DATADIR}/openghost-native/licenses)
install(TARGETS openghost-native BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/openghost.png RENAME openghost-native.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES resources/openghost-native.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()
# reference/openghost is deliberately never built, imported or installed.
