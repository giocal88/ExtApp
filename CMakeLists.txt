cmake_minimum_required(VERSION 3.20)
project(ExtApp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(EXTAPP_BUILD_GTK   "Build the GTK4/libadwaita frontend" ON)
option(EXTAPP_BUILD_CLI   "Build the extapp-cli command line frontend" ON)
option(EXTAPP_BUILD_TESTS "Build backend unit tests" ON)
option(EXTAPP_SHARED_BACKEND "Build libextapp as a shared library" OFF)

include(GNUInstallDirs)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_subdirectory(backend)

if(EXTAPP_BUILD_CLI)
  add_subdirectory(frontend/cli)
endif()

if(EXTAPP_BUILD_GTK)
  add_subdirectory(frontend/gtk4)
endif()

if(EXTAPP_BUILD_TESTS)
  if(EXTAPP_SHARED_BACKEND)
    # Unit tests call internal (hidden) backend functions: static build only.
    message(STATUS "EXTAPP_SHARED_BACKEND=ON: skipping backend unit tests")
  else()
    enable_testing()
    add_subdirectory(backend/tests)
  endif()
endif()
