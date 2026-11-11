add_executable(extapp-cli main.c)
target_link_libraries(extapp-cli PRIVATE ExtApp::backend)
install(TARGETS extapp-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
