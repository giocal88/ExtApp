# libextapp — frontend-agnostic backend (C11 + POSIX + libcurl)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Vendored cJSON (MIT), compiled privately into the backend.
add_library(extapp_cjson OBJECT third_party/cjson/cJSON.c)
set_target_properties(extapp_cjson PROPERTIES POSITION_INDEPENDENT_CODE ON
                                             C_VISIBILITY_PRESET hidden)
target_compile_options(extapp_cjson PRIVATE -w)

if(EXTAPP_SHARED_BACKEND)
  set(_extapp_lib_type SHARED)
else()
  set(_extapp_lib_type STATIC)
endif()

add_library(extapp ${_extapp_lib_type}
  src/context.c
  src/desktop_entry.c
  src/entries.c
  src/http.c
  src/icon_provider_appimagehub.c
  src/icon_provider_commons.c
  src/icon_provider_flathub.c
  src/icon_search.c
  src/icons.c
  src/scanner.c
  src/util.c
  $<TARGET_OBJECTS:extapp_cjson>
)
add_library(ExtApp::backend ALIAS extapp)

set_target_properties(extapp PROPERTIES
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION 0)

target_include_directories(extapp
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE src third_party/cjson)
target_link_libraries(extapp PRIVATE CURL::libcurl Threads::Threads)

if(EXTAPP_SHARED_BACKEND)
  install(TARGETS extapp LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY include/extapp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
