add_executable(eagls_core_tests version_test.cpp)
target_link_libraries(eagls_core_tests PRIVATE eagls::core)
eagls_configure_target(eagls_core_tests)
add_test(NAME core.version COMMAND eagls_core_tests "${PROJECT_VERSION}")

add_executable(eagls_content_tests content_test.cpp)
target_link_libraries(eagls_content_tests PRIVATE eagls::core)
eagls_configure_target(eagls_content_tests)
add_test(NAME core.content_boundaries COMMAND eagls_content_tests)

set(EAGLS_LOCAL_CONTENT_DIR "" CACHE PATH "显式选择已提取的本地内容样本目录，默认不读取游戏资源")
if(EAGLS_LOCAL_CONTENT_DIR)
    add_test(NAME core.content_local
        COMMAND eagls_content_tests "${EAGLS_LOCAL_CONTENT_DIR}")
endif()


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
    foreach(case IN ITEMS dat_help gr_help dat_missing gr_missing missing_out duplicate_out
                          unknown_option missing_value extra_input unsupported_action)
        add_test(NAME cli.content_${case}
            COMMAND "${CMAKE_COMMAND}"
                "-DCLI=$<TARGET_FILE:eagls_cli>"
                "-DCASE=${case}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/content_cli_test.cmake"
        )
        set_tests_properties(cli.content_${case} PROPERTIES TIMEOUT 10)
    endforeach()
endif()
