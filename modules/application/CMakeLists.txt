orbitforge_module(orbitforge_application Application src/session.cpp src/scenario.cpp src/project.cpp
    src/data_lock.cpp src/report.cpp src/project_document.cpp)
target_link_libraries(orbitforge_application PUBLIC OrbitForge::Core OrbitForge::Results)
target_include_directories(orbitforge_application SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
# Keep headless builds independent of the provider dependency toolchain.
install(FILES third_party/PicoSHA2-LICENSE.txt DESTINATION "${CMAKE_INSTALL_DATADIR}/OrbitForge/notices"
    RENAME PicoSHA2.txt COMPONENT SDK)
install(FILES third_party/PicoSHA2-LICENSE.txt DESTINATION "orbitforge/notices"
    RENAME PicoSHA2.txt COMPONENT Python)

