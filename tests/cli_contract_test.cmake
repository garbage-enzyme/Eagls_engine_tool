cmake_minimum_required(VERSION 3.20)
# CLI 契约表驱动测试：一个测试组内部逐条执行参数组合，断言退出码与输出通道的约定。
#
# 用例格式：名称|期望退出码|必须出现的输出文本（+ 分隔，可空）|参数...
# 约定：
#   0 = 成功：标准错误为空，标准输出含约定文本；
#   1 = 运行期错误：标准输出为空，标准错误非空；
#   2 = 参数错误：标准输出为空，标准错误含 --help 提示。
#
# 由 tests/CMakeLists.txt 以 -DGROUP=top|content|pak|exe|exe_local 调用。

# 用例按“解析器错误类别”每类保留一条代表（帮助文本、缺动作、缺必填、缺值、
# 未知选项、多余参数、业务规则、退出码 1 通道），同模式的排列组合不逐条展开。
set(top_cases
    "no_args|0|用法：eagls+尚未实现|"
    "help|0|用法：eagls+尚未实现|--help"
    "version|0|eagls ${EXPECTED_VERSION}|--version"
    "unknown|2||--unknown"
)

set(content_cases
    "dat_help|0|用法：eagls|dat --help"
    "dat_missing|2||dat"
    "missing_out|2||dat text sample.dat"
    "missing_value|2||gr decode sample.gr --out --overwrite"
    "unknown_option|2||gr decode sample.gr --out a --unknown"
    "extra_input|2||dat decrypt a.dat b.dat --out a"
    "gr_encode_store_on_decode|2||gr decode sample.gr --out dir --store"
)

set(pak_cases
    "pak_help|0|用法：eagls|pak --help"
    "pak_repack_missing_from|2||pak repack a.idx a.pak"
)

set(exe_cases
    "exe_help|0|用法：eagls|exe --help"
    "exe_check_out_rejected|2||exe check sample.exe --out a"
    "exe_check_missing_file|1||exe check __no_such_file__.exe"
)

if(GROUP STREQUAL "exe_local")
    # 真实样本补丁回归：仅在显式启用本地样本时注册；产物 SHA-256 必须与已验证版本一致。
    execute_process(COMMAND "${CLI}" exe patch "${SAMPLE}" --out "${OUTPUT}" --overwrite
        RESULT_VARIABLE actual_exit OUTPUT_VARIABLE output ERROR_VARIABLE error ENCODING UTF-8 TIMEOUT 60)
    if(NOT "${actual_exit}" STREQUAL "0")
        message(FATAL_ERROR "exe patch 失败：${actual_exit}；${output}；${error}")
    endif()
    if(NOT error STREQUAL "" OR NOT output MATCHES "已写出")
        message(FATAL_ERROR "补丁命令输出不符：${output}；${error}")
    endif()
    file(SHA256 "${OUTPUT}" actual_hash)
    if(NOT actual_hash STREQUAL "c86cceb5fdd195cad415fa22c98d5018c8935dcc6c0257d16e0f9c538e1ef4b6")
        message(FATAL_ERROR "补丁产物 SHA-256 不符：${actual_hash}")
    endif()
    return()
endif()

if(GROUP STREQUAL "top")
    set(cases ${top_cases})
elseif(GROUP STREQUAL "content")
    set(cases ${content_cases})
elseif(GROUP STREQUAL "pak")
    set(cases ${pak_cases})
elseif(GROUP STREQUAL "exe")
    set(cases ${exe_cases})
else()
    message(FATAL_ERROR "未知测试组：${GROUP}")
endif()

foreach(case IN LISTS cases)
    string(REPLACE "|" ";" fields "${case}")
    list(GET fields 0 name)
    list(GET fields 1 expected_exit)
    list(GET fields 2 required_text)
    list(LENGTH fields field_count)
    if(field_count GREATER 3)
        list(GET fields 3 argument_text)
        # 参数写在同一个字段里、以空格分隔，这里按空格拆成真正的参数列表。
        separate_arguments(arguments UNIX_COMMAND "${argument_text}")
    else()
        set(arguments)
    endif()

    execute_process(COMMAND "${CLI}" ${arguments}
        RESULT_VARIABLE actual_exit OUTPUT_VARIABLE output ERROR_VARIABLE error ENCODING UTF-8 TIMEOUT 10)

    if(NOT "${actual_exit}" STREQUAL "${expected_exit}")
        message(FATAL_ERROR
            "[${name}] 退出码不符：预期 ${expected_exit}，实际 ${actual_exit}；输出：${output}；错误：${error}")
    endif()

    if(expected_exit EQUAL 0)
        if(NOT error STREQUAL "")
            message(FATAL_ERROR "[${name}] 成功执行时不应写入标准错误：${error}")
        endif()
        if(name STREQUAL "version")
            string(STRIP "${output}" actual_version)
            if(NOT actual_version STREQUAL "eagls ${EXPECTED_VERSION}")
                message(FATAL_ERROR "[version] 版本输出不符：${actual_version}")
            endif()
        elseif(NOT required_text STREQUAL "")
            string(REPLACE "+" ";" required_parts "${required_text}")
            foreach(part IN LISTS required_parts)
                string(FIND "${output}" "${part}" position)
                if(position EQUAL -1)
                    message(FATAL_ERROR "[${name}] 输出缺少“${part}”：${output}")
                endif()
            endforeach()
        endif()
    elseif(expected_exit EQUAL 1)
        if(NOT output STREQUAL "" OR error STREQUAL "")
            message(FATAL_ERROR "[${name}] 运行期错误必须只向标准错误输出信息：${output}；${error}")
        endif()
    else()
        if(NOT output STREQUAL "" OR NOT error MATCHES "--help")
            message(FATAL_ERROR "[${name}] 参数错误必须只向标准错误输出帮助提示：${output}；${error}")
        endif()
    endif()
endforeach()
