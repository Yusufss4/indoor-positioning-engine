foreach(tool sim replay regress)
  add_executable(ipe-${tool} ${tool}_main.cpp)
  target_link_libraries(ipe-${tool} PRIVATE ipe_eval ipe_options nlohmann_json::nlohmann_json)
endforeach()

add_executable(ipe-import import_main.cpp)
target_link_libraries(ipe-import PRIVATE ipe_geo ipe_options nlohmann_json::nlohmann_json)
