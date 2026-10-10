project(flowsql_baseline)
find_package(ICU REQUIRED COMPONENTS uc i18n)
find_package(OpenSSL REQUIRED)

add_library(${PROJECT_NAME} SHARED
    baseline_plugin.cpp
    checkpoint/checkpoint.cpp
    checkpoint/model_parameters.cpp
    bootstrap/bootstrap_engine.cpp
    config/runtime_config.cpp
    config_parser.cpp
    model/calendar_feature_helper.cpp
    model/event_calendar_matcher.cpp
    model/formal_predictor.cpp
    model/readiness_helper.cpp
    bootstrap/formal_model_trainer.cpp
    relation/relation_basis.cpp
    relation/relation_basis_state.cpp
    relation/relation_fusion.cpp
    relation/relation_summary.cpp
    relation/routed_summary.cpp
    rolling/drift_adapt.cpp
    rolling/detection_calibration.cpp
    rolling/maturity_gate.cpp
    rolling/monthpos_state.cpp
    rolling/observation_adapter.cpp
    rolling/residual_scale.cpp
    rolling/rolling_estimator.cpp
    rolling/rolling_feature_batch.cpp
    rolling/rolling_config.cpp
    rolling/rolling_state.cpp
    rolling/rolling_task_runner.cpp
    rolling/score_trust.cpp
    rolling/update_gate.cpp
    solver/solver_backend.cpp
    task/baseline_task_base.cpp
    task/bootstrap_task_store.cpp
    task/relation_task.cpp
    task/ratio_task.cpp
    task/task_registry.cpp
    task/value_task.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${ICU_INCLUDE_DIRS}
)

add_thirddepen(${PROJECT_NAME} eigen rapidjson yaml-cpp)
target_link_libraries(${PROJECT_NAME} ${ICU_LIBRARIES} OpenSSL::Crypto)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
