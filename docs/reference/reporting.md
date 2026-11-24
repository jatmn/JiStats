# Analysis and HTML reporting

Open the dedicated Analysis workspace from the dashboard. The provider, account, and date controls stay visible while the history pages scroll. The scope strip shows the effective UTC range, including when a saved preset falls back. Choose **Custom** to enter inclusive dates in `YYYY-MM-DD` format. The previous results stay visible while a new range loads.

- **Activity** shows recorded history with a trend chart, daily rows, and provider and account breakdowns. Counts represent stored usage entries or local activity records, depending on the source available. Empty periods can lack evidence. They appear as blank chart intervals and as a dash in the daily count column. Use the chart's Left and Right keys or click a bucket to inspect the interval, count, and source.
- **Quota history** shows subscription quota samples for a selected provider and account. Select a quota window to inspect remaining percentages, recorded units, and resets. The chart spaces samples by capture time. The lowest recorded value can include a daily minimum retained by a rollup.
- **Coverage** explains the records and gaps behind the selected range. When recent local Codex throughput is available, its fixed 5-minute and 30-minute rates are labeled separately from the selected historical dates.

[AnalysisPresenter](../../src/gui/AnalysisPresenter.cpp) owns the display. [AnalysisQueryEngine](../../src/reporting/AnalysisQueryEngine.cpp) resolves the persisted scope used for Analysis and HTML reporting. Providers and accounts remain selectable while they have retained daily usage rollups. Activity shows the retained daily total and identifies records without hourly detail.

Quota rollups preserve observed window changes across UTC days within the selected range. Analysis reads retained snapshots for partial days and days whose reset evidence is unavailable in a rollup. A full legacy day keeps its saved within-day count when too few snapshots remain to replay it.

## Exports

HTML is the primary v1.0 export format. Reports are snapshots for sharing, archiving, or offline review. Use the dashboard and Analysis for current data and interactive review.

Use **File → Export HTML Report…** for a manual report. Configure scheduled HTML export in Settings. [AppController](../../src/app/AppController.cpp) owns the internal schedule while JiStats runs. Manual and scheduled reports share the Analysis provider/account and date scope. The report header and historical totals both follow the effective Analysis filters, including historical accounts without live dashboard rows. **All Providers** and **All Accounts** include retained records across the selected dates. Settings retain the schedule and last-run status. Export attempts are stored in `export_runs`.

### Report metadata

Preserve generation time, provider/account and date scope, freshness, backfilled data, unknown gaps, and the mirror-confidence policy version. Report Generated records the time of export. Data Timestamp shows the latest recorded sync in the selected scope or Unknown when none is available. An offline refresh preserves the selected account’s last recorded sync. Reports show the stored mirror-rule versions when available and `unknown` when none are available. Keep source labels and confidence indicators on rows where available. A historical export describes the captured time range and evidence. The recovery summary uses stored markers and recovery intervals that intersect the selected scope. The report flags unknown gaps from scoped markers and flags backfill when the included usage comes from backfilled raw events or retained daily rollups. The badge shows both flags when both apply. Reports use daily rollups for full UTC days and timestamped usage rows for partial days. The current quota cards describe the provider state at export time.

[ReportingService](../../src/reporting/ReportingService.cpp) prepares the report and [HtmlExporter](../../src/reporting/HtmlExporter.cpp) renders it. See the [query tests](../../tests/test_analysis_query_engine.cpp) and [reporting tests](../../tests/test_reporting_service.cpp) for effective scope and persistence coverage.

Data Freshness uses the dashboard's provider contribution rules within the selected scope. A Kimi installation without quota credentials can appear as Sample. When that is the only provider in scope, Data Freshness shows Unknown. A scoped provider with live quota evidence can make the report Current.
