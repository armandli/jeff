add_executable(jeff-server main.cpp)

target_include_directories(jeff-server PRIVATE
  ${CMAKE_SOURCE_DIR}/src/inference
)

target_link_libraries(jeff-server PRIVATE
  jeff_inference
  httplib::httplib
  jeff_json
  jeff_compiler_options
)

install(TARGETS jeff-server DESTINATION bin)
