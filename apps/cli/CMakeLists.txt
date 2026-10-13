add_executable(eagls_cli main.cpp)
set_target_properties(eagls_cli PROPERTIES OUTPUT_NAME eagls)
target_link_libraries(eagls_cli PRIVATE eagls::core)
eagls_configure_target(eagls_cli)
