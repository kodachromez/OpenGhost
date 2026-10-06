cmake_minimum_required(VERSION 3.21)
project(OpenGhostCpp VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_FIND_PACKAGE_RESOLVE_SYMLINKS ON)
# OpenGhost's native QML uses recent Qt APIs.
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2
    QuickDialogs2 ShaderTools)
qt_policy(SET QTP0001 NEW)
# The built-in browser panel (browser-panel.js): its pages are Qt WebEngine
# guests, as the reference's are Chromium webviews. OFF builds have no panel and
# report no browser (host.browser = null), as the reference's web build does.
option(OPENGHOST_BROWSER "Build the built-in browser panel on Qt WebEngine" ON)
if(OPENGHOST_BROWSER)
    find_package(Qt6 6.11 REQUIRED COMPONENTS WebEngineQuick)
endif()

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
    src/backend/backend.cpp src/backend/transport.h src/backend/fake_backend.cpp
    src/backend/pi_backend.cpp src/backend/pi_process.h src/backend/pi_process.cpp
    src/frontend/preferences.cpp src/frontend/chat_service.cpp src/frontend/plugins.cpp
    src/frontend/frontend_plugin.h src/frontend/frontend_plugins.h src/frontend/frontend_plugins.cpp
    src/frontend/example_plugin.h src/frontend/example_plugin.cpp
    src/frontend/usage.cpp src/frontend/store.cpp
    src/frontend/library.cpp src/frontend/host.h)
target_include_directories(native_contract PUBLIC src)
target_link_libraries(native_contract PUBLIC Qt6::Core)
native_warnings(native_contract)
# The browser panel's state and host.browser snapshot; no engine, QtCore only.
target_sources(native_contract PRIVATE src/frontend/browser.h src/frontend/browser.cpp
    src/frontend/browser_automation.h src/frontend/browser_tools.h src/frontend/browser_tools.cpp)
qt_add_resources(native_contract browser_contract PREFIX /browser BASE src/browser
    FILES src/browser/schemas.json)
# The Pi backend's bridge extension, handed to `pi -e` (runs inside Pi, not here).
qt_add_resources(native_contract pi_bridge PREFIX /pi BASE src/backend/pi
    FILES src/backend/pi/openghost-bridge.js)
if(OPENGHOST_BROWSER)
    add_library(native_browser_automation STATIC src/browser/qt_browser_automation.cpp
        src/browser/qt_browser_automation.h)
    target_link_libraries(native_browser_automation PUBLIC native_contract Qt6::Quick Qt6::WebEngineQuick)
    qt_add_resources(native_browser_automation browser_observe PREFIX /browser BASE src/browser
        FILES src/browser/observe.js)
    native_warnings(native_browser_automation)
endif()

# Attachment and pinned-file reading decodes pictures, so it needs QtGui.
add_library(native_window STATIC src/model.cpp src/settings.cpp src/window.cpp src/motion.cpp
    src/appearance.cpp src/presentation.cpp src/offline_services.h src/platform/desktop.cpp
    src/frontend/attachments.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(native_window PRIVATE src/platform/linux.cpp)
else()
    target_sources(native_window PRIVATE src/platform/portable.cpp)
endif()
qt_add_qml_module(native_window URI OpenGhost.Cpp VERSION 1.0 NO_PLUGIN RESOURCE_PREFIX /
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/OpenGhost/Cpp
    SOURCES src/rich.h src/rich.cpp src/ghost.h src/ghost.cpp src/icon.h src/icon.cpp
        src/shadow.h src/shadow.cpp src/reveal.h src/reveal.cpp
        src/exposure.h src/exposure.cpp src/wave.h src/wave.cpp
        src/selection.h src/selection.cpp
        src/diagramview.h src/diagramview.cpp src/medialoader.h src/medialoader.cpp
        src/mediafetch.h src/mediafetch.cpp src/videoinfo.h src/videoinfo.cpp
        src/browserfocus.h src/browserfocus.cpp
        src/splashaura.h src/splashaura.cpp)
target_link_libraries(native_window PUBLIC native_contract native_rich Qt6::Gui Qt6::Network
    Qt6::Quick Qt6::QuickControls2)
native_warnings(native_window)
qt_add_shaders(native_window "ui_shaders" PREFIX / FILES
    shaders/blur.frag shaders/oil.frag shaders/glass.frag shaders/veil.frag shaders/tone.frag
    shaders/frame.frag shaders/mist.frag)

# Tool Calls, a frontend plugin (plugins/tool_calls): Ghosty's tool cards as the
# renderer of the transcript's tool rows. Without it (OFF) tool rows are kept
# but not drawn, as with the plugin turned off.
option(OPENGHOST_TOOL_CALLS "Build the Tool Calls frontend plugin" ON)
if(OPENGHOST_TOOL_CALLS)
    # The card's texts (formatting and selection) and the plugin: QtCore only.
    add_library(openghost_tool_calls_core STATIC plugins/tool_calls/toolcard.h
        plugins/tool_calls/toolcard.cpp plugins/tool_calls/tool_calls_plugin.h
        plugins/tool_calls/tool_calls_plugin.cpp)
    target_include_directories(openghost_tool_calls_core PUBLIC plugins/tool_calls)
    target_link_libraries(openghost_tool_calls_core PUBLIC native_contract)
    native_warnings(openghost_tool_calls_core)
    # The same texts for the card's QML (ToolText).
    add_library(openghost_tool_calls STATIC)
    qt_add_qml_module(openghost_tool_calls URI OpenGhost.ToolCalls VERSION 1.0 NO_PLUGIN
        RESOURCE_PREFIX / OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/OpenGhost/ToolCalls
        SOURCES plugins/tool_calls/tooltext.h plugins/tool_calls/tooltext.cpp)
    target_link_libraries(openghost_tool_calls PUBLIC openghost_tool_calls_core Qt6::Qml)
    native_warnings(openghost_tool_calls)
endif()

# Thinking, a frontend plugin (plugins/thinking): Ghosty's thinking row as the
# renderer of the transcript's thinking rows. Always built; Settings → Appearance
# turns it on or off (off: thinking is kept but not drawn).
add_library(openghost_thinking STATIC plugins/thinking/thinking_plugin.h
    plugins/thinking/thinking_plugin.cpp)
target_include_directories(openghost_thinking PUBLIC plugins/thinking)
target_link_libraries(openghost_thinking PUBLIC native_contract)
native_warnings(openghost_thinking)

qt_add_executable(openghost-cpp src/main.cpp)
set_target_properties(openghost-cpp PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.openghost.cpp")
target_link_libraries(openghost-cpp PRIVATE native_window openghost_thinking Qt6::Qml
    Qt6::QuickDialogs2)
native_warnings(openghost-cpp)
foreach(name Main ChatEntry LiveText Sidebar SettingsDialog Markdown Block InlineText CodeBlock
        TableBlock DiagramBlock WorkingGhost WelcomeGhost Splash EffortControl Metrics
        IconButton Spring ModelGlyph ModelButton ModelStage EffortStage EffortMenu Feather UsagePage
        AppearancePage GeneralPage OverlayScrollBar JumpButton CopyButton Entrance SelectArea
        SelectDriver SelectableLabel SelectionMenu SelectionVeil VeilFilter TitleTip ButtonTip
        Backdrop RowLift TextGlow ApprovalCard ModePicker ModeDock MediaBlock MediaStack MediaGlass)
    set_source_files_properties(qml/${name}.qml PROPERTIES QT_RESOURCE_ALIAS ${name}.qml)
    list(APPEND qml_files qml/${name}.qml)
endforeach()
# The Thinking plugin's row, in the window's module (it uses LiveText and SelectArea).
set_source_files_properties(plugins/thinking/qml/ThinkingEntry.qml
    PROPERTIES QT_RESOURCE_ALIAS ThinkingEntry.qml)
list(APPEND qml_files plugins/thinking/qml/ThinkingEntry.qml)
if(OPENGHOST_TOOL_CALLS)
    # The plugin's card, in the window's module (it uses LiveText and SelectArea).
    foreach(name ToolEntry ToolCard)
        set_source_files_properties(plugins/tool_calls/qml/${name}.qml
            PROPERTIES QT_RESOURCE_ALIAS ${name}.qml)
        list(APPEND qml_files plugins/tool_calls/qml/${name}.qml)
    endforeach()
    target_link_libraries(openghost-cpp PRIVATE openghost_tool_calls)
    target_compile_definitions(openghost-cpp PRIVATE OPENGHOST_TOOL_CALLS)
    if(OPENGHOST_BUILD_SMOKE_TEST)
        target_sources(openghost-cpp PRIVATE tests/tool_calls_smoke.cpp)
    endif()
endif()
if(OPENGHOST_BROWSER)
    foreach(name BrowserPanel BrowserGuest BrowserPopup)
        set_source_files_properties(qml/${name}.qml PROPERTIES QT_RESOURCE_ALIAS ${name}.qml)
        list(APPEND qml_files qml/${name}.qml)
    endforeach()
    target_link_libraries(openghost-cpp PRIVATE native_browser_automation)
    target_compile_definitions(openghost-cpp PRIVATE OPENGHOST_BROWSER)
endif()
qt_add_qml_module(openghost-cpp URI OpenGhost.Ui VERSION 1.0 RESOURCE_PREFIX /
    QML_FILES ${qml_files} IMPORTS OpenGhost.Cpp)
qt_add_resources(openghost-cpp icon PREFIX / BASE resources FILES resources/openghost.png)

# Small opt-in launch/rendering check. No Python, Node, Rust or live backend needed.
option(OPENGHOST_BUILD_SMOKE_TEST "Build the standalone UI smoke check" OFF)
if(OPENGHOST_BUILD_SMOKE_TEST)
    enable_testing()
    find_package(Qt6 6.11 REQUIRED COMPONENTS Test GuiPrivate)
    target_sources(openghost-cpp PRIVATE tests/smoke.cpp tests/plugin_smoke.cpp
        tests/frontend_plugin_smoke.cpp tests/chat_settings_smoke.cpp tests/parity/native.cpp
        tests/splash_frames.cpp)
    target_link_libraries(openghost-cpp PRIVATE Qt6::Test Qt6::GuiPrivate)
    target_compile_definitions(openghost-cpp PRIVATE OPENGHOST_SMOKE_TEST)
    add_executable(native_contract_test tests/contract.cpp src/settings.cpp)
    target_link_libraries(native_contract_test PRIVATE native_contract Qt6::Test)
    native_warnings(native_contract_test)
    add_test(NAME native_contract_test COMMAND native_contract_test)
    set_tests_properties(native_contract_test PROPERTIES TIMEOUT 15)
    # PiBackend's turn lifecycle against a scripted stand-in for `pi --mode rpc`.
    add_executable(native_pi_test tests/pi.cpp)
    target_link_libraries(native_pi_test PRIVATE native_contract Qt6::Test)
    target_compile_definitions(native_pi_test PRIVATE
        OPENGHOST_FAKE_PI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/pi")
    native_warnings(native_pi_test)
    add_test(NAME native_pi_test COMMAND native_pi_test)
    set_tests_properties(native_pi_test PROPERTIES TIMEOUT 60)

    add_executable(native_pi_audit_test tests/pi_audit.cpp)
    target_link_libraries(native_pi_audit_test PRIVATE native_contract Qt6::Test)
    target_compile_definitions(native_pi_audit_test PRIVATE
        OPENGHOST_AUDIT_PI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/pi-audit")
    native_warnings(native_pi_audit_test)
    add_test(NAME native_pi_audit_test COMMAND native_pi_audit_test)
    set_tests_properties(native_pi_audit_test PROPERTIES TIMEOUT 60)
    # The real `pi` on PATH (skipped without one), with a scripted faux model:
    # access modes relayed, permission requests shown and answered (a test-only
    # stand-in permission plugin asks), extension dialogs/errors.
    add_executable(native_pi_real_test tests/pi_real.cpp)
    target_link_libraries(native_pi_real_test PRIVATE native_contract Qt6::Test)
    target_compile_definitions(native_pi_real_test PRIVATE
        OPENGHOST_REAL_PI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/pi/real")
    native_warnings(native_pi_real_test)
    add_test(NAME native_pi_real_test COMMAND native_pi_real_test)
    set_tests_properties(native_pi_real_test PROPERTIES TIMEOUT 300)
    add_executable(native_plugins_test tests/plugins.cpp)
    target_link_libraries(native_plugins_test PRIVATE native_contract Qt6::Test)
    native_warnings(native_plugins_test)
    add_test(NAME native_plugins_test COMMAND native_plugins_test)
    set_tests_properties(native_plugins_test PROPERTIES TIMEOUT 15)
    # The frontend plugin SDK: lifecycle, hooks, cleanup and saved choices.
    add_executable(native_frontend_plugins_test tests/frontend_plugins.cpp)
    target_link_libraries(native_frontend_plugins_test PRIVATE native_contract Qt6::Test)
    native_warnings(native_frontend_plugins_test)
    add_test(NAME native_frontend_plugins_test COMMAND native_frontend_plugins_test)
    set_tests_properties(native_frontend_plugins_test PROPERTIES TIMEOUT 15)

    # Reply pictures and videos: parsing, address rules and the loader's
    # policy, metadata/cache and bounded transport (loopback only).
    add_executable(native_attachments_test tests/attachments.cpp)
    target_link_libraries(native_attachments_test PRIVATE native_window Qt6::Test)
    target_compile_definitions(native_attachments_test PRIVATE
        OPENGHOST_FAKE_PI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/pi")
    native_warnings(native_attachments_test)
    add_test(NAME native_attachments_test COMMAND native_attachments_test)
    set_tests_properties(native_attachments_test PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # Settings → Providers' sign-in form across cancel/sign-in-again races.
    add_executable(native_sign_in_test tests/sign_in.cpp)
    target_link_libraries(native_sign_in_test PRIVATE native_window Qt6::Test)
    target_compile_definitions(native_sign_in_test PRIVATE
        OPENGHOST_FAKE_PI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/pi")
    native_warnings(native_sign_in_test)
    add_test(NAME native_sign_in_test COMMAND native_sign_in_test)
    set_tests_properties(native_sign_in_test PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(native_media_test tests/media.cpp)
    target_link_libraries(native_media_test PRIVATE native_window Qt6::Test)
    native_warnings(native_media_test)
    add_test(NAME native_media_test COMMAND native_media_test)
    set_tests_properties(native_media_test PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(OPENGHOST_TOOL_CALLS)
        # The Tool Calls plugin's hook through On/Off and its card texts.
        add_executable(native_tool_calls_test tests/tool_calls.cpp)
        target_link_libraries(native_tool_calls_test PRIVATE openghost_tool_calls_core Qt6::Test)
        native_warnings(native_tool_calls_test)
        add_test(NAME native_tool_calls_test COMMAND native_tool_calls_test)
        set_tests_properties(native_tool_calls_test PROPERTIES TIMEOUT 15)
    endif()
    add_test(NAME native_ui_smoke COMMAND openghost-cpp --smoke-test)
    add_test(NAME native_fake_ui_smoke COMMAND openghost-cpp --smoke-test --fake-backend)
    set_tests_properties(native_ui_smoke native_fake_ui_smoke PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QPA_PLATFORMTHEME=generic;QT_FORCE_STDERR_LOGGING=1;OPENGHOST_REDUCED_MOTION=0")
    # The splash against splash.js's timeline, frame by frame on a stepped
    # 240 Hz clock (its rendered frames too, where a GPU scene graph runs).
    add_test(NAME native_splash_test COMMAND openghost-cpp --splash-check)
    add_test(NAME native_splash_test_60hz COMMAND openghost-cpp --splash-check)
    set_tests_properties(native_splash_test native_splash_test_60hz PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QPA_PLATFORMTHEME=generic;QT_FORCE_STDERR_LOGGING=1;OPENGHOST_REDUCED_MOTION=0")
    set_property(TEST native_splash_test_60hz APPEND PROPERTY ENVIRONMENT "OPENGHOST_SPLASH_HZ=60")
    # The browser panel's state machine, and its real guests in the UI smoke.
    add_executable(native_browser_test tests/browser.cpp)
    target_link_libraries(native_browser_test PRIVATE native_contract Qt6::Test)
    native_warnings(native_browser_test)
    add_test(NAME native_browser_test COMMAND native_browser_test)
    add_executable(native_browser_operations_test tests/browser_operations.cpp)
    target_link_libraries(native_browser_operations_test PRIVATE native_contract Qt6::Test)
    native_warnings(native_browser_operations_test)
    add_test(NAME native_browser_operations_test COMMAND native_browser_operations_test)
    set_tests_properties(native_browser_operations_test PROPERTIES TIMEOUT 20)
    if(OPENGHOST_BROWSER)
        add_executable(native_browser_automation_test tests/browser_automation.cpp)
        target_link_libraries(native_browser_automation_test PRIVATE native_browser_automation Qt6::Test)
        native_warnings(native_browser_automation_test)
        add_test(NAME native_browser_automation_test COMMAND native_browser_automation_test)
        set_tests_properties(native_browser_automation_test PROPERTIES TIMEOUT 150
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QPA_PLATFORMTHEME=generic;QT_FORCE_STDERR_LOGGING=1")
    endif()
    set_tests_properties(native_browser_test PROPERTIES TIMEOUT 15)
    target_sources(openghost-cpp PRIVATE tests/browser_smoke.cpp)
    set_tests_properties(native_ui_smoke native_fake_ui_smoke PROPERTIES TIMEOUT 60)
endif()

include(GNUInstallDirs)
install(FILES LICENSE NOTICE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/openghost-cpp)
install(DIRECTORY licenses/ DESTINATION ${CMAKE_INSTALL_DATADIR}/openghost-cpp/licenses)
install(TARGETS openghost-cpp BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/openghost.png RENAME openghost-cpp.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES resources/openghost-cpp.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()
# reference/openghost is deliberately never built, imported or installed.
