cmake_minimum_required(VERSION 3.20)

set(arguments)
set(expected_exit 0)
set(expected_text "用法：eagls")

if(CASE STREQUAL "help")
    set(arguments --help)
elseif(CASE STREQUAL "short_help")
    set(arguments -h)
elseif(CASE STREQUAL "version")
    set(arguments --version)
    set(expected_text "eagls ${EXPECTED_VERSION}")
elseif(CASE STREQUAL "unknown")
    set(arguments --unknown)
    set(expected_exit 2)
elseif(CASE STREQUAL "extra_args")
    set(arguments --version extra)
    set(expected_exit 2)
elseif(CASE STREQUAL "unimplemented")
    set(arguments extract)
    set(expected_exit 2)
elseif(NOT CASE STREQUAL "no_args")
    message(FATAL_ERROR "未知测试用例：${CASE}")
endif()

execute_process(
    COMMAND "${CLI}" ${arguments}
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    ENCODING UTF-8
    TIMEOUT 5
)

if(NOT "${actual_exit}" STREQUAL "${expected_exit}")
    message(FATAL_ERROR "退出码不符：预期 ${expected_exit}，实际 ${actual_exit}；${error}")
endif()

if(expected_exit EQUAL 0)
    if(NOT error STREQUAL "")
        message(FATAL_ERROR "成功执行时不应写入标准错误：${error}")
    endif()
    if(CASE STREQUAL "version")
        string(STRIP "${output}" actual_version)
        if(NOT actual_version STREQUAL expected_text)
            message(FATAL_ERROR "版本输出不符：${output}")
        endif()
    else()
        string(FIND "${output}" "${expected_text}" text_position)
        string(FIND "${output}" "尚未实现" status_position)
        if(text_position EQUAL -1 OR status_position EQUAL -1)
            message(FATAL_ERROR "帮助缺少用法或未实现状态说明：${output}")
        endif()
    endif()
else()
    string(FIND "${error}" "--help" help_position)
    if(NOT output STREQUAL "" OR help_position EQUAL -1)
        message(FATAL_ERROR "非法参数应只在标准错误中给出帮助提示。")
    endif()
endif()
