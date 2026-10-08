cmake_minimum_required(VERSION 3.16)
project(exif-editor VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EXIV2 REQUIRED IMPORTED_TARGET exiv2)
find_package(ZLIB REQUIRED)

add_executable(exif-editor main.cpp mainwindow.cpp mainwindow.h provenance.cpp provenance.h)
target_link_libraries(exif-editor PRIVATE Qt6::Widgets PkgConfig::EXIV2 ZLIB::ZLIB)

include(GNUInstallDirs)
install(TARGETS exif-editor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES exif-editor.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
