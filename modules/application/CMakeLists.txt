orbelle_module(orbelle_application Application src/session.cpp src/scenario.cpp src/project.cpp
    src/data_lock.cpp src/report.cpp src/project_document.cpp src/hpop_covariance.cpp)
target_link_libraries(orbelle_application PUBLIC Orbelle::Core Orbelle::Results)
target_include_directories(orbelle_application SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
# Keep headless builds independent of the provider dependency toolchain.
install(FILES third_party/PicoSHA2-LICENSE.txt DESTINATION "${CMAKE_INSTALL_DATADIR}/Orbelle/notices"
    RENAME PicoSHA2.txt COMPONENT SDK)
install(FILES third_party/PicoSHA2-LICENSE.txt DESTINATION "orbelle/notices"
    RENAME PicoSHA2.txt COMPONENT Python)

