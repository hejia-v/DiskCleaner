cmake_minimum_required(VERSION 3.21)
project(DiskCleaner VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Svg)
qt_standard_project_setup()

set(DiskCleaner_SOURCES
    src/core.cpp
    src/deletionwatcher.cpp
    src/liveupdate.cpp
    src/liveupdateio.cpp
    src/models.cpp
    src/treemap.cpp
    src/mainwindow.cpp
    src/i18n.cpp
    src/shellintegration.cpp
    src/theme.cpp
    src/uiicons.cpp
    resources/ui.qrc
    src/updatecheck.cpp
)

qt_add_executable(DiskCleaner src/main.cpp ${DiskCleaner_SOURCES})

target_link_libraries(DiskCleaner PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)
target_compile_definitions(DiskCleaner PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(DiskCleaner PRIVATE shell32 advapi32)

option(DISK_CLEANER_BUILD_TESTS "Build treemap and table integration tests" OFF)
if(DISK_CLEANER_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(UiPreview tests/ui_preview.cpp ${DiskCleaner_SOURCES})
    target_include_directories(UiPreview PRIVATE src)
    target_compile_definitions(UiPreview PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(UiPreview PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Test shell32 advapi32)
    qt_add_executable(TreemapTests tests/treemap_tests.cpp ${DiskCleaner_SOURCES})
    target_include_directories(TreemapTests PRIVATE src)
    target_compile_definitions(TreemapTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(TreemapTests PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Test shell32 advapi32)
    add_test(NAME treemap COMMAND TreemapTests)
    qt_add_executable(ResourceTests tests/resource_tests.cpp ${DiskCleaner_SOURCES})
    target_include_directories(ResourceTests PRIVATE src)
    target_compile_definitions(ResourceTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(ResourceTests PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Test shell32 advapi32)
    add_test(NAME resources COMMAND ResourceTests)
    get_target_property(qt_qmake Qt6::qmake IMPORTED_LOCATION)
    execute_process(COMMAND "${qt_qmake}" -query QT_INSTALL_PLUGINS
                    OUTPUT_VARIABLE qt_plugins OUTPUT_STRIP_TRAILING_WHITESPACE)
    set_tests_properties(treemap resources PROPERTIES TIMEOUT 90
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${qt_plugins}/platforms;QT_FORCE_STDERR_LOGGING=1")
endif()


