if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "IPE_BUILD_WASM requires the Emscripten toolchain (emcmake cmake ...)")
endif()

add_executable(ipe_wasm bindings.cpp)
target_link_libraries(ipe_wasm PRIVATE ipe_eval)
target_link_options(ipe_wasm PRIVATE
  -lembind
  -sMODULARIZE=1
  -sEXPORT_NAME=createIpeModule
  -sALLOW_MEMORY_GROWTH=1
  -sENVIRONMENT=web
  -sDISABLE_EXCEPTION_CATCHING=0)
target_compile_options(ipe_wasm PRIVATE -sDISABLE_EXCEPTION_CATCHING=0)

# Assemble a self-contained static site: wasm module, page and venue data.
set(IPE_SITE_DIR ${CMAKE_BINARY_DIR}/site)
add_custom_command(TARGET ipe_wasm POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${IPE_SITE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_DIR:ipe_wasm>/ipe_wasm.js $<TARGET_FILE_DIR:ipe_wasm>/ipe_wasm.wasm
          ${IPE_SITE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/index.html ${CMAKE_CURRENT_SOURCE_DIR}/app.js
          ${CMAKE_CURRENT_SOURCE_DIR}/map3d.js ${CMAKE_CURRENT_SOURCE_DIR}/style.css ${IPE_SITE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/data ${IPE_SITE_DIR}/data
  COMMENT "Assembling web demo in ${IPE_SITE_DIR}")
