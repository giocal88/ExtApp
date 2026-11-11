# GTK4 + libadwaita frontend. Depends on the backend only via <extapp/extapp.h>.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.12)
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1>=1.5)

add_executable(extapp-gtk
  src/main.c
  src/ea-async.c
  src/ea-icon-search-dialog.c
  src/ea-objects.c
  src/ea-scan-page.c
  src/ea-shortcut-dialog.c
  src/ea-shortcuts-page.c
  src/ea-util.c
  src/ea-window.c
)
set_target_properties(extapp-gtk PROPERTIES OUTPUT_NAME extapp)
target_link_libraries(extapp-gtk PRIVATE ExtApp::backend PkgConfig::GTK4 PkgConfig::ADW)
target_compile_definitions(extapp-gtk PRIVATE
  GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_76
  GDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_12)

install(TARGETS extapp-gtk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/org.codeberg.giocal88.ExtApp.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
