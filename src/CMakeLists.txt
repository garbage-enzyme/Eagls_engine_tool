add_library(eagls_core STATIC version.cpp)
add_library(eagls::core ALIAS eagls_core)

# 前端只接触公开头文件，生成配置及实现细节不向外暴露。
target_include_directories(eagls_core
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_BINARY_DIR}/generated"
)
target_compile_features(eagls_core PUBLIC cxx_std_17)
eagls_configure_target(eagls_core)
