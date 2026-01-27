add_llvm_tool(clingo-test
  ClingoTest.cpp
)

target_include_directories(clingo-test
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../projects/clingo/clingo/libclingo
)

target_link_libraries(clingo-test
  PRIVATE
  ${CMAKE_BINARY_DIR}/lib/libclingo.a
  ${CMAKE_BINARY_DIR}/lib/libgringo.a
  ${CMAKE_BINARY_DIR}/lib/libreify.a
  ${CMAKE_BINARY_DIR}/lib/libclasp.dylib
  ${CMAKE_BINARY_DIR}/lib/libpotassco.dylib
  LLVMSupport
)
