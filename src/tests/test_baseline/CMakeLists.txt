project(test_baseline)
find_package(ICU REQUIRED COMPONENTS uc i18n)
add_executable(test_baseline_checkpoint test_baseline_checkpoint.cpp)
target_link_libraries(test_baseline_checkpoint PRIVATE flowsql_baseline flowsql_common)
target_compile_options(test_baseline_checkpoint PRIVATE -UNDEBUG -Wall -Wextra -Werror)
set_target_properties(test_baseline_checkpoint PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_baseline_checkpoint COMMAND $<TARGET_FILE:test_baseline_checkpoint>)
set_tests_properties(test_baseline_checkpoint PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output LABELS "baseline;checkpoint" TIMEOUT 90)

add_executable(${PROJECT_NAME} test_baseline_plugin.cpp)
add_executable(test_baseline_task_headers test_baseline_task_headers.cpp)
# Reuse the plugin's exact objects so resource regressions can call hidden task implementations.
target_sources(test_baseline_task_headers PRIVATE $<TARGET_OBJECTS:flowsql_baseline>)
target_compile_definitions(test_baseline_task_headers PRIVATE FLOWSQL_BASELINE_CLOSE_RESOURCE_TEST)
add_executable(test_baseline_link_bootstrap_eval test_baseline_link_bootstrap_eval.cpp)
add_executable(test_baseline_link_rolling_eval test_baseline_link_rolling_eval.cpp)
add_executable(test_baseline_rolling_config test_baseline_rolling_config.cpp)
add_executable(test_baseline_rolling_feature_batch
    test_baseline_rolling_feature_batch.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/rolling_feature_batch.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/rolling_estimator.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/monthpos_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/rolling_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/calendar_feature_helper.cpp
)
add_executable(test_baseline_relation_summary
    test_baseline_relation_summary.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_summary.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/routed_summary.cpp
)
add_executable(test_baseline_relation_basis_state
    test_baseline_relation_basis_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_basis.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_basis_state.cpp
)
add_executable(test_baseline_relation_fusion
    test_baseline_relation_fusion.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/relation/relation_fusion.cpp
)
add_executable(test_baseline_observation_adapter
    test_baseline_observation_adapter.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/observation_adapter.cpp
)
add_executable(test_baseline_rolling_state
    test_baseline_rolling_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/rolling_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/monthpos_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/calendar_feature_helper.cpp
)
add_executable(test_baseline_rolling_estimator
    test_baseline_rolling_estimator.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/rolling_estimator.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/rolling_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/monthpos_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/calendar_feature_helper.cpp
)
add_executable(test_baseline_rolling_gate_scale
    test_baseline_rolling_gate_scale.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/update_gate.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/drift_adapt.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/residual_scale.cpp
)
add_executable(test_baseline_rolling_b3
    test_baseline_rolling_b3.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/detection_calibration.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/maturity_gate.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/monthpos_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/score_trust.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/rolling_estimator.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/rolling/rolling_state.cpp
    ${CMAKE_SOURCE_DIR}/plugins/baseline/model/calendar_feature_helper.cpp
)
add_executable(test_baseline_batch_prediction_perf
    test_baseline_batch_prediction_perf.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_task_headers PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_link_bootstrap_eval PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_link_rolling_eval PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_rolling_config PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_rolling_feature_batch PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_relation_summary PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_relation_basis_state PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_relation_fusion PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_observation_adapter PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_rolling_state PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${ICU_INCLUDE_DIRS}
)
target_include_directories(test_baseline_rolling_estimator PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${ICU_INCLUDE_DIRS}
)
target_include_directories(test_baseline_rolling_gate_scale PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_baseline_rolling_b3 PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${ICU_INCLUDE_DIRS}
)
target_include_directories(test_baseline_batch_prediction_perf PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${ICU_INCLUDE_DIRS}
)

target_compile_definitions(test_baseline_rolling_config
    PRIVATE FLOWSQL_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_baseline)
add_dependencies(test_baseline_task_headers flowsql_common flowsql_baseline)
add_dependencies(test_baseline_link_bootstrap_eval flowsql_common flowsql_baseline)
add_dependencies(test_baseline_link_rolling_eval flowsql_common flowsql_baseline)
add_dependencies(test_baseline_rolling_config flowsql_common flowsql_baseline)
add_dependencies(test_baseline_rolling_feature_batch flowsql_common)
add_dependencies(test_baseline_relation_summary flowsql_common)
add_dependencies(test_baseline_relation_basis_state flowsql_common)
add_dependencies(test_baseline_relation_fusion flowsql_common)
add_dependencies(test_baseline_observation_adapter flowsql_common)
add_dependencies(test_baseline_rolling_state flowsql_common)
add_dependencies(test_baseline_rolling_estimator flowsql_common)
add_dependencies(test_baseline_rolling_gate_scale flowsql_common)
add_dependencies(test_baseline_rolling_b3 flowsql_common)
add_dependencies(test_baseline_batch_prediction_perf flowsql_common flowsql_baseline)

target_link_libraries(${PROJECT_NAME} flowsql_common flowsql_baseline)
target_link_libraries(test_baseline_task_headers flowsql_common flowsql_baseline)
target_link_libraries(test_baseline_link_bootstrap_eval flowsql_common flowsql_baseline)
target_link_libraries(test_baseline_link_rolling_eval flowsql_common flowsql_baseline)
target_link_libraries(test_baseline_rolling_config flowsql_common flowsql_baseline)
target_link_libraries(test_baseline_rolling_feature_batch flowsql_common ${ICU_LIBRARIES})
target_link_libraries(test_baseline_relation_summary flowsql_common)
target_link_libraries(test_baseline_relation_basis_state flowsql_common)
target_link_libraries(test_baseline_relation_fusion flowsql_common)
target_link_libraries(test_baseline_observation_adapter flowsql_common)
target_link_libraries(test_baseline_rolling_state flowsql_common ${ICU_LIBRARIES})
target_link_libraries(test_baseline_rolling_estimator flowsql_common ${ICU_LIBRARIES})
target_link_libraries(test_baseline_rolling_gate_scale flowsql_common)
target_link_libraries(test_baseline_rolling_b3 flowsql_common ${ICU_LIBRARIES})
target_link_libraries(test_baseline_batch_prediction_perf flowsql_common flowsql_baseline ${ICU_LIBRARIES})

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_compile_options(test_baseline_task_headers PRIVATE -UNDEBUG)
target_compile_options(test_baseline_link_bootstrap_eval PRIVATE -UNDEBUG)
target_compile_options(test_baseline_link_rolling_eval PRIVATE -UNDEBUG)
target_compile_options(test_baseline_rolling_config PRIVATE -UNDEBUG)
target_compile_options(test_baseline_rolling_feature_batch PRIVATE -UNDEBUG)
target_compile_options(test_baseline_relation_summary PRIVATE -UNDEBUG)
target_compile_options(test_baseline_relation_basis_state PRIVATE -UNDEBUG)
target_compile_options(test_baseline_relation_fusion PRIVATE -UNDEBUG)
target_compile_options(test_baseline_observation_adapter PRIVATE -UNDEBUG)
target_compile_options(test_baseline_rolling_state PRIVATE -UNDEBUG)
target_compile_options(test_baseline_rolling_estimator PRIVATE -UNDEBUG)
target_compile_options(test_baseline_rolling_gate_scale PRIVATE -UNDEBUG)
target_compile_options(test_baseline_rolling_b3 PRIVATE -UNDEBUG)
target_compile_options(test_baseline_batch_prediction_perf PRIVATE -UNDEBUG)

add_thirddepen(${PROJECT_NAME} rapidjson)
add_thirddepen(test_baseline_task_headers rapidjson)
add_thirddepen(test_baseline_batch_prediction_perf rapidjson)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_task_headers PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_link_bootstrap_eval PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_link_rolling_eval PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_rolling_config PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_rolling_feature_batch PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_relation_summary PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_relation_basis_state PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_relation_fusion PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_observation_adapter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_rolling_state PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_rolling_estimator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_rolling_gate_scale PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_rolling_b3 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_baseline_batch_prediction_perf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# Evaluation and performance programs remain independent of the regular regression suite.
if(BUILD_TESTING)
    foreach(baseline_test IN ITEMS
        test_baseline
        test_baseline_task_headers
        test_baseline_rolling_config
        test_baseline_rolling_feature_batch
        test_baseline_relation_summary
        test_baseline_relation_basis_state
        test_baseline_relation_fusion
        test_baseline_observation_adapter
        test_baseline_rolling_state
        test_baseline_rolling_estimator
        test_baseline_rolling_gate_scale
        test_baseline_rolling_b3
    )
        add_test(NAME ${baseline_test} COMMAND ${baseline_test})
        set_tests_properties(${baseline_test} PROPERTIES
            LABELS baseline
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output
        )
    endforeach()
endif()
