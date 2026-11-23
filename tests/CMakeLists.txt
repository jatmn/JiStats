FetchContent_MakeAvailable(catch2)

# Both builds use the same Catch2 tests and production sources for portable components.
add_executable(JiStatsTests
    ${catch2_SOURCE_DIR}/extras/catch_amalgamated.cpp
    test_mirror_engine.cpp
    test_logger.cpp
    test_local_observation_service.cpp
    test_observation_persistence_wiring.cpp
    test_startup_manager.cpp
    test_schema_manager.cpp
    test_storage_mirror_state.cpp
    test_storage_gap_markers.cpp
    test_storage_activity_events.cpp
    test_storage_usage_rollups.cpp
    test_json_parser.cpp
    test_runtime_status.cpp

    ${CMAKE_SOURCE_DIR}/src/analytics/MirrorEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/analytics/GapClassifier.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
    ${CMAKE_SOURCE_DIR}/src/core/JsonParser.cpp
    ${CMAKE_SOURCE_DIR}/src/observation/LocalObservationService.cpp
    ${CMAKE_SOURCE_DIR}/src/observation/ObservationPersistenceBridge.cpp
    ${CMAKE_SOURCE_DIR}/src/db/SchemaManager.cpp
    ${CMAKE_SOURCE_DIR}/src/db/StorageService.cpp
    ${CMAKE_SOURCE_DIR}/src/db/SettingsService.cpp
    ${CMAKE_SOURCE_DIR}/src/db/QueryResult.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/StartupManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/RuntimeStatus.cpp
)

if(WIN32)
    target_sources(JiStatsTests PRIVATE
        test_analytics_engine.cpp
        test_notification_service.cpp
        test_taskbar_presenter.cpp
        test_dashboard_presenter.cpp
        test_tray_controller.cpp
        test_app_controller.cpp
        test_app_controller_offline_notice.cpp
        test_app_observation_persistence.cpp
        test_analysis_presenter.cpp
        test_analysis_query_engine.cpp
        test_provider_data_path.cpp
        test_minimax_adapter.cpp
        test_minimax_live_capture.cpp
        test_claude_local_parser.cpp
        test_kimi_adapter.cpp
        test_reporting_service.cpp
        test_windows_credential_store.cpp
        openai_fixture_tests.cpp
        test_openai_export_import.cpp
        test_codex_local_usage_import.cpp

        ${CMAKE_SOURCE_DIR}/src/analytics/AnalyticsEngine.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SyncScheduler.cpp
        ${CMAKE_SOURCE_DIR}/src/core/HttpClient.cpp
        ${CMAKE_SOURCE_DIR}/src/app/AppController.cpp
        ${CMAKE_SOURCE_DIR}/src/app/TrayController.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/WindowsAppAssets.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/WindowsCredentialStore.cpp
        ${CMAKE_SOURCE_DIR}/src/gui/AnalysisPresenter.cpp
        ${CMAKE_SOURCE_DIR}/src/gui/DashboardPresenter.cpp
        ${CMAKE_SOURCE_DIR}/src/gui/NotificationService.cpp
        ${CMAKE_SOURCE_DIR}/src/taskbar/TaskbarPresenter.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/ProviderManager.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/SubscriptionQuotaAdapter.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/minimax/MiniMaxAdapter.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/minimax/MiniMaxLiveCapture.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/openai/OpenAIAdapter.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/openai/OpenAIExportImport.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/openai/CodexLocalSqliteReader.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/openai/CodexLocalUsageImporter.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/claude/ClaudeCodeAdapter.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/claude/ClaudeCodeLocalParser.cpp
        ${CMAKE_SOURCE_DIR}/src/providers/kimi/KimiAdapter.cpp
        ${CMAKE_SOURCE_DIR}/src/reporting/AnalysisQueryEngine.cpp
        ${CMAKE_SOURCE_DIR}/src/reporting/ReportingService.cpp
        ${CMAKE_SOURCE_DIR}/src/reporting/HtmlExporter.cpp
    )
endif()

target_include_directories(JiStatsTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${catch2_SOURCE_DIR}/extras
)

if(JISTATS_SQLITE_TARGET)
    target_link_libraries(JiStatsTests PRIVATE ${JISTATS_SQLITE_TARGET})
endif()

if(JISTATS_HAS_SPDLOG)
    target_link_libraries(JiStatsTests PRIVATE spdlog)
    target_compile_definitions(JiStatsTests PRIVATE JISTATS_HAS_SPDLOG=1)
endif()

target_compile_definitions(JiStatsTests PRIVATE JISTATS_ENABLE_TEST_HOOKS=1)

if(WIN32)
    target_link_libraries(JiStatsTests PRIVATE
        comctl32
        comdlg32
        user32
        gdi32
        gdiplus
        shell32
        advapi32
        ws2_32
        winhttp
    )
    target_compile_definitions(JiStatsTests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    if(MSVC)
        target_compile_options(JiStatsTests PRIVATE /utf-8 /W4 /EHsc /FS)
    elseif(MINGW)
        target_link_libraries(JiStatsTests PRIVATE shlwapi)
        target_sources(JiStatsTests PRIVATE ${CMAKE_SOURCE_DIR}/src/platform/MinGwCompat.cpp)
    endif()
endif()

# Output to bin/ alongside main executable
set_target_properties(JiStatsTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Register with CTest (run from repo root so relative fixture paths work)
add_test(NAME JiStatsTests COMMAND $<TARGET_FILE:JiStatsTests>)
set_tests_properties(JiStatsTests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
