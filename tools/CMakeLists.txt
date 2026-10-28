foreach(tool sim replay regress)
  add_executable(ipe-${tool} ${tool}_main.cpp)
  target_link_libraries(ipe-${tool} PRIVATE ipe_eval nlohmann_json::nlohmann_json)
endforeach()
