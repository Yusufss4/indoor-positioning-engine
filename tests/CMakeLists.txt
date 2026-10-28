FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(ipe_tests
  test_baro.cpp
  test_ekf.cpp
  test_eval.cpp
  test_geometry.cpp
  test_georef.cpp
  test_import.cpp
  test_particle_filter.cpp
  test_pdr.cpp
  test_rng.cpp
  test_rssi_model.cpp
  test_simulator.cpp
  test_trace.cpp
  test_venue.cpp)
target_link_libraries(ipe_tests PRIVATE ipe_eval ipe_geo GTest::gtest_main)
target_compile_definitions(ipe_tests PRIVATE IPE_DATA_DIR="${PROJECT_SOURCE_DIR}/data")

include(GoogleTest)
gtest_discover_tests(ipe_tests DISCOVERY_TIMEOUT 60)

if(IPE_BUILD_TOOLS)
  set(IPE_REGRESSION_SEEDS 3 CACHE STRING "Seeds per scenario in the accuracy regression test")
  add_test(NAME accuracy_regression
           COMMAND ipe-regress --scenarios ${PROJECT_SOURCE_DIR}/data/scenarios
                   --baseline ${PROJECT_SOURCE_DIR}/tests/regression/baseline.json
                   --seeds ${IPE_REGRESSION_SEEDS})
  set_tests_properties(accuracy_regression PROPERTIES LABELS regression TIMEOUT 900)

  # Real phone traces (docs/real_data.md). The data is not in the repository:
  # the test is skipped until datasets/msil/fetch.sh and
  # convert.py --wifi --scans --out datasets/msil/out_scans have run.
  set(IPE_MSIL_MANIFEST ${PROJECT_SOURCE_DIR}/datasets/msil/out_scans/manifest_holdout.json)
  add_test(NAME real_data_regression
           COMMAND sh -c "test -f \"$1\" || exit 77; exec \"$0\" --traces \"$1\" --presets pf,wcl --config \"$2\" --baseline \"$3\""
                   $<TARGET_FILE:ipe-regress> ${IPE_MSIL_MANIFEST} ${PROJECT_SOURCE_DIR}/data/configs/msil_wifi.json
                   ${PROJECT_SOURCE_DIR}/tests/regression/msil_baseline.json)
  set_tests_properties(real_data_regression PROPERTIES LABELS "regression;realdata" SKIP_RETURN_CODE 77 TIMEOUT 1800)
endif()
