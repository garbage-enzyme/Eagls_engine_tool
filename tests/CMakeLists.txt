add_executable(eagls_core_tests version_test.cpp)
target_link_libraries(eagls_core_tests PRIVATE eagls::core)
eagls_configure_target(eagls_core_tests)
add_test(NAME core.version COMMAND eagls_core_tests "${PROJECT_VERSION}")

if(EAGLS_BUILD_CLI)
    foreach(case IN ITEMS no_args help short_help version unknown extra_args unimplemented)
        add_test(NAME cli.${case}
            COMMAND "${CMAKE_COMMAND}"
                "-DCLI=$<TARGET_FILE:eagls_cli>"
                "-DCASE=${case}"
                "-DEXPECTED_VERSION=${PROJECT_VERSION}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.cmake"
        )
        set_tests_properties(cli.${case} PROPERTIES TIMEOUT 10)
    endforeach()
endif()
