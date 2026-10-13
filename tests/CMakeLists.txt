add_executable(eagls_content_tests content_test.cpp)
target_link_libraries(eagls_content_tests PRIVATE eagls::core)
eagls_configure_target(eagls_content_tests)
add_test(NAME core.content_boundaries COMMAND eagls_content_tests)

add_executable(eagls_exe_patch_tests exe_patch_test.cpp exe_typography_test.cpp)
target_link_libraries(eagls_exe_patch_tests PRIVATE eagls::core)
eagls_configure_target(eagls_exe_patch_tests)
add_test(NAME core.exe_patch COMMAND eagls_exe_patch_tests)

set(EAGLS_LOCAL_CONTENT_DIR "" CACHE PATH "显式选择已提取的本地内容样本目录，默认不读取游戏资源")
if(EAGLS_LOCAL_CONTENT_DIR)
    add_test(NAME core.content_local
        COMMAND eagls_content_tests "${EAGLS_LOCAL_CONTENT_DIR}")
endif()

if(EAGLS_BUILD_CLI)
    # CLI 契约测试：每个组是一张表驱动用例表（见 cli_contract_test.cmake），
    # 失败信息里带用例名；按域分组便于定位，不按用例数膨胀测试数量。
    foreach(group IN ITEMS top content pak exe)
        add_test(NAME cli.contract.${group}
            COMMAND "${CMAKE_COMMAND}"
                "-DCLI=$<TARGET_FILE:eagls_cli>"
                "-DGROUP=${group}"
                "-DEXPECTED_VERSION=${PROJECT_VERSION}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_contract_test.cmake"
        )
        set_tests_properties(cli.contract.${group} PROPERTIES TIMEOUT 30)
    endforeach()
    # 真实 EAGLS.exe 补丁回归：仅在显式启用本地样本且可推导出样本 EXE 时注册。
    set(EAGLS_LOCAL_GAME_EXE "${EAGLS_LOCAL_CONTENT_DIR}/../fixtures/local/yanyun2/EAGLS.exe")
    if(EAGLS_LOCAL_CONTENT_DIR AND EXISTS "${EAGLS_LOCAL_GAME_EXE}")
        add_test(NAME cli.contract.exe_local
            COMMAND "${CMAKE_COMMAND}"
                "-DCLI=$<TARGET_FILE:eagls_cli>"
                "-DGROUP=exe_local"
                "-DSAMPLE=${EAGLS_LOCAL_GAME_EXE}"
                "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/eagls-cn-local.exe"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_contract_test.cmake"
        )
        set_tests_properties(cli.contract.exe_local PROPERTIES TIMEOUT 60)
    endif()
endif()
