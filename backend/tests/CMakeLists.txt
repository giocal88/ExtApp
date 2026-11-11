# Unit tests use internal headers, so they link the static object code.
foreach(t desktop_entry scanner entries icon_search icons)
  add_executable(test_${t} test_${t}.c)
  target_include_directories(test_${t} PRIVATE ../src ../third_party/cjson)
  target_link_libraries(test_${t} PRIVATE extapp CURL::libcurl Threads::Threads)
  target_compile_definitions(test_${t} PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
