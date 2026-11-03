add_library(jeff_inference STATIC
  model.cpp
  tokenizer.cpp
)

target_include_directories(jeff_inference PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(jeff_inference PUBLIC "${TORCH_LIBRARIES}" jeff_compiler_options)
target_compile_definitions(jeff_inference PRIVATE ${TORCH_CXX_FLAGS})
