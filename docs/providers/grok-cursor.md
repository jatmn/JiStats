# Grok and Cursor subscriptions

Open **File → Settings → Grok / Cursor** and enable either integration. The dashboard shows the allowance and reset time when the account reports them. JiStats keeps quota snapshots in its local database for Analysis.

## Grok

Sign in with `grok login` in the Grok CLI. JiStats reads the standard local sign-in from `GROK_HOME` or `%USERPROFILE%\.grok`. API-key and custom CLI authentication configurations do not supply quota through this integration. [Grok CLI reference](https://docs.x.ai/build/cli/reference)

If Provider Details asks you to run `grok login` again, renew the sign-in in the Grok CLI. JiStats reads the updated credential on its next scheduled quota check. For a fixed `GROK_AUTH` override, update the variable and restart JiStats. The CLI owns credential renewal.

## Cursor

Set the Windows user environment variable `CURSOR_API_KEY` to a Cursor user API key before starting JiStats. JiStats also accepts an existing access token supplied through `CURSOR_AUTH_TOKEN`. The quota reader connects to the default Cursor endpoint. It uses the returned included-use percentage, or the individual Auto and API percentages when that is all the account returns. [Cursor CLI authentication](https://cursor.com/docs/cli/reference/authentication) · [Cursor usage and limits](https://cursor.com/help/models-and-usage/usage-limits)

These integrations fetch subscription allowance only. Some account types do not provide a percentage. If both connections represent the same shared subscription, compare the individual readings without adding them together. [Shared subscription limits](https://docs.x.ai/x-subscription/usage-limits)
