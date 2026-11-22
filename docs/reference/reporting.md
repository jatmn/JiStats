# Analysis and HTML reporting

Use the dashboard's **Analysis** tab to launch the dedicated Analysis workspace. [AnalysisPresenter](../../src/gui/AnalysisPresenter.cpp) owns this view. [AnalysisQueryEngine](../../src/reporting/AnalysisQueryEngine.cpp) resolves the persisted provider, account, and date-range selection and queries history, breakdowns, trends, coverage, and provenance. The scope controls and summaries should reflect the effective date window when a saved selection falls back. Keep new Analysis behavior on this presenter and query path.

## Exports

Use **File → Export HTML Report…** for a manual report. Configure scheduled HTML export in Settings. [AppController](../../src/app/AppController.cpp) owns the internal schedule while JiStats runs. Manual and scheduled reports share the Analysis provider/account and date scope. Settings retain the schedule and last-run status. Export attempts are stored in `export_runs`.

### Report metadata

Preserve generation time, provider/account and date scope, freshness, backfilled data, unknown gaps, and the mirror-confidence policy version. Keep source labels on rows where available. A historical export describes the captured time range and evidence.

[ReportingService](../../src/reporting/ReportingService.cpp) prepares the report and [HtmlExporter](../../src/reporting/HtmlExporter.cpp) renders it. See the [query tests](../../tests/test_analysis_query_engine.cpp) and [reporting tests](../../tests/test_reporting_service.cpp) for effective scope and persistence coverage.
