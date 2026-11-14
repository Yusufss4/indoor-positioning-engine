# Builds tests/install as a project of its own against the engine and runs it on a simulated walk.
#   cmake -DMODE=install|subdirectory -DSOURCE_DIR=... -DBUILD_DIR=... -DCONFIG=... -DCXX=... -DSIM=...
#         -P run.cmake
function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    string(REPLACE ";" " " command "${ARGN}")
    message(FATAL_ERROR "failed (${result}): ${command}")
  endif()
endfunction()

set(work ${BUILD_DIR}/package_test/${MODE})
file(REMOVE_RECURSE ${work})
set(args -DCMAKE_CXX_COMPILER=${CXX} -DCMAKE_BUILD_TYPE=${CONFIG}
         -DIPE_EXAMPLE_SOURCE=${SOURCE_DIR}/examples/minimal.cpp)
if(MODE STREQUAL "install")
  run(${CMAKE_COMMAND} --install ${BUILD_DIR} --prefix ${work}/prefix --config ${CONFIG})
  list(APPEND args -DCMAKE_PREFIX_PATH=${work}/prefix)
else()
  # The JSON library this build already downloaded, so that the test needs no network.
  list(APPEND args -DIPE_SOURCE_DIR=${SOURCE_DIR} -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${JSON_DIR})
endif()
run(${CMAKE_COMMAND} -S ${SOURCE_DIR}/tests/install -B ${work}/build ${args})
run(${CMAKE_COMMAND} --build ${work}/build --config ${CONFIG} --parallel 4)

run(${SIM} --scenario ${SOURCE_DIR}/data/scenarios/office_nominal.json --out ${work}/trace.csv)
find_program(consumer consumer PATHS ${work}/build ${work}/build/${CONFIG} NO_DEFAULT_PATH REQUIRED)
run(${consumer} ${SOURCE_DIR}/data/venues/office_floor.json ${work}/trace.csv)
