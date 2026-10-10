cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

project(across VERSION 0.2.0 LANGUAGES CXX)

option(ACROSS_BUILD_CLIENT "Build the local across command-line client" ON)
option(ACROSS_BUILD_SERVER "Build the remote across-server command-line client" ON)
option(ACROSS_BUILD_DESKTOP "Build the raylib desktop application" ON)
option(ACROSS_BUILD_TESTS "Build automated tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(across_enable_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
  endif()
endfunction()

include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(across_core
  src/core/config.cpp
  src/core/management.cpp
  src/core/packages.cpp
  src/core/deployer.cpp
  src/core/process.cpp
  src/core/transport.cpp
  src/core/watcher.cpp
)
target_include_directories(across_core PUBLIC include)
target_link_libraries(across_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_definitions(across_core PUBLIC
  ACROSS_VERSION="${PROJECT_VERSION}"
  ACROSS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
across_enable_warnings(across_core)

if(ACROSS_BUILD_CLIENT)
  add_executable(across src/cli/main.cpp)
  target_link_libraries(across PRIVATE across_core)
  across_enable_warnings(across)
endif()

if(ACROSS_BUILD_SERVER)
  add_executable(across-server src/server/main.cpp)
  target_link_libraries(across-server PRIVATE across_core)
  across_enable_warnings(across-server)
endif()

if(ACROSS_BUILD_DESKTOP)
  FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
    URL_HASH SHA256=aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e
    )
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(raylib)
  if(APPLE)
    set_source_files_properties(assets/Across.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    add_executable(across-desktop MACOSX_BUNDLE src/desktop/main.cpp assets/Across.icns)
  else()
    add_executable(across-desktop src/desktop/main.cpp)
  endif()
  target_link_libraries(across-desktop PRIVATE across_core raylib)
  target_compile_definitions(across-desktop PRIVATE
    ACROSS_ASSET_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
  )
  across_enable_warnings(across-desktop)
  if(APPLE)
    file(GLOB_RECURSE across_server_bundle_sources CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/include/across/*.hpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/server/*.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/*.cpp"
    )
    set_property(TARGET across-desktop APPEND PROPERTY LINK_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt;${across_server_bundle_sources};${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf;${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Inter-SemiBold.ttf;${CMAKE_CURRENT_SOURCE_DIR}/assets/across-icon.png;${CMAKE_CURRENT_SOURCE_DIR}/assets/Across.icns"
    )
    set_target_properties(across-desktop PROPERTIES
      OUTPUT_NAME "Across"
      MACOSX_BUNDLE_BUNDLE_NAME "Across"
      MACOSX_BUNDLE_ICON_FILE "Across.icns"
      MACOSX_BUNDLE_GUI_IDENTIFIER "dev.across.deployment-manager"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
    set(across_bundle_server "$<TARGET_FILE_DIR:across-desktop>/../Resources/server")
    set(across_bundle_assets "$<TARGET_FILE_DIR:across-desktop>/../Resources/assets")
    add_custom_command(TARGET across-desktop POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${across_bundle_server}/src"
      COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" "${across_bundle_server}/CMakeLists.txt"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/include" "${across_bundle_server}/include"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/src/core" "${across_bundle_server}/src/core"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/src/cli" "${across_bundle_server}/src/cli"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/src/server" "${across_bundle_server}/src/server"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${across_bundle_assets}"
      COMMAND /usr/bin/codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:across-desktop>"
      COMMENT "Embedding the remote CLI and desktop assets"
    )
  else()
    add_custom_command(TARGET across-desktop POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:across-desktop>/assets"
      COMMENT "Copying desktop assets"
    )
  endif()
endif()

include(GNUInstallDirs)
if(TARGET across)
  install(TARGETS across RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET across-server)
  install(TARGETS across-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET across-desktop)
  install(TARGETS across-desktop
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/across/assets)
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_DATADIR}/across/server/include)
install(DIRECTORY src/core src/server src/cli DESTINATION ${CMAKE_INSTALL_DATADIR}/across/server/src)
install(FILES CMakeLists.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/across/server)

if(ACROSS_BUILD_TESTS)
  enable_testing()
  add_executable(across-tests tests/core_tests.cpp)
  target_link_libraries(across-tests PRIVATE across_core)
  across_enable_warnings(across-tests)
  add_test(NAME across-tests COMMAND across-tests)
  add_executable(across-server-tests tests/server_tests.cpp)
  target_link_libraries(across-server-tests PRIVATE across_core)
  across_enable_warnings(across-server-tests)
  add_test(NAME across-server-tests COMMAND across-server-tests)
endif()
