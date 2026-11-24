param(
    [Parameter(Mandatory = $false)]
    [string]$OutputDir = "",
    [Parameter(Mandatory = $false)]
    [string]$Model = "MiniMax-M2.7",
    [Parameter(Mandatory = $false)]
    [string]$Prompt = "Reply with the single word: pong",
    [Parameter(Mandatory = $false)]
    [int]$MaxTokens = 8
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "lib/capture_common.ps1")

function Get-ModelPricing {
    param([string]$ModelName)

    $pricingByModel = @{
        "MiniMax-M2.7" = @{ input = 0.3; output = 1.2 }
        "MiniMax-M2.7-highspeed" = @{ input = 0.6; output = 2.4 }
        "MiniMax-M2.5" = @{ input = 0.3; output = 1.2 }
        "MiniMax-M2.5-highspeed" = @{ input = 0.6; output = 2.4 }
        "MiniMax-M2.1" = @{ input = 0.3; output = 1.2 }
        "MiniMax-M2.1-highspeed" = @{ input = 0.6; output = 2.4 }
        "MiniMax-M2" = @{ input = 0.3; output = 1.2 }
    }

    if ($pricingByModel.ContainsKey($ModelName)) {
        return $pricingByModel[$ModelName]
    }

    return $null
}

function New-HeaderText {
    param($Response)

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("HTTP $($Response.StatusCode)")
    foreach ($header in $Response.Headers.GetEnumerator() | Sort-Object Key) {
        $values = $header.Value
        if ($values -is [System.Array]) {
            $lines.Add("$($header.Key): $($values -join ', ')")
        } else {
            $lines.Add("$($header.Key): $values")
        }
    }
    return ($lines -join [Environment]::NewLine)
}

function Invoke-ProbeRequest {
    param(
        [string]$Uri,
        [hashtable]$Headers
    )

    $headersPath = [System.IO.Path]::GetTempFileName()
    $bodyPath = [System.IO.Path]::GetTempFileName()
    try {
        & curl.exe -sS -D $headersPath -o $bodyPath `
            -H "Authorization: $($Headers.Authorization)" `
            -H "Content-Type: $($Headers['Content-Type'])" `
            $Uri | Out-Null

        $headerBlock = [System.IO.File]::ReadAllText($headersPath)
        $body = [System.IO.File]::ReadAllText($bodyPath)
    } finally {
        Remove-Item -LiteralPath $headersPath -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $bodyPath -ErrorAction SilentlyContinue
    }

    $headerLines = @($headerBlock -split "\r?\n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $statusCode = 0
    if ($headerLines.Length -gt 0 -and $headerLines[0] -match "^HTTP/\S+\s+(\d{3})") {
        $statusCode = [int]$matches[1]
    }

    return [ordered]@{
        uri = $Uri
        status_code = $statusCode
        raw_headers = $headerBlock
        body = [string]$body
    }
}

function New-NotesTemplate {
    param(
        [string]$Model,
        [int]$PromptTokens,
        [int]$CompletionTokens,
        [double]$EstimatedCost,
        [string]$ProbeSummary
    )

    $lines = @(
        "# MiniMax PAYG Capture Notes",
        "",
        "- Model: $Model",
        "- Prompt tokens: $PromptTokens",
        "- Completion tokens: $CompletionTokens",
        "- Estimated request cost (USD): $EstimatedCost",
        "",
        "## Granularity / Freshness",
        "",
        "- The documented OpenAI-compatible text API returns a usage payload on each successful request, so the validated PAYG signal is immediate per-request usage rather than a separate delayed aggregate usage-history endpoint.",
        "",
        "## Endpoint Discovery",
        "",
        $ProbeSummary,
        "",
        "## Cost Rule",
        "",
        "- JiStats can derive PAYG request cost from the returned token counts plus the published MiniMax PAYG pricing table for the selected model.",
        "- Pricing source: https://platform.minimax.io/docs/guides/pricing-paygo"
    )

    return ($lines -join [Environment]::NewLine)
}

$apiKey = Get-EnvValue "MINIMAX_API_KEY"
if ([string]::IsNullOrEmpty($apiKey)) {
    [Console]::Error.WriteLine("Missing MINIMAX_API_KEY. A MiniMax pay-as-you-go key is required.")
    exit 2
}

$resolvedOutputDir = Initialize-CaptureDirectory -OutputDir $OutputDir `
    -DefaultSubdirectory "minimax/payg" -RepoRoot (Split-Path -Parent $PSScriptRoot)

$timestamp = [DateTimeOffset]::UtcNow.ToString("yyyy-MM-ddTHHmmssZ")
$headers = @{
    Authorization = "Bearer $apiKey"
    "Content-Type" = "application/json"
}

$requestBody = @{
    model = $Model
    messages = @(
        @{
            role = "user"
            content = $Prompt
        }
    )
    max_tokens = $MaxTokens
    temperature = 0
} | ConvertTo-Json -Depth 6

$completionResponse = Invoke-WebRequest `
    -Uri "https://api.minimax.io/v1/chat/completions" `
    -Method Post `
    -Headers $headers `
    -Body $requestBody `
    -UseBasicParsing

$completionBody = $completionResponse.Content | ConvertFrom-Json
$pricing = Get-ModelPricing -ModelName $Model
$promptTokens = [int]$completionBody.usage.prompt_tokens
$completionTokens = [int]$completionBody.usage.completion_tokens
$estimatedCost = $null
if ($null -ne $pricing) {
    $estimatedCost = (($promptTokens * [double]$pricing.input) + ($completionTokens * [double]$pricing.output)) / 1000000.0
}

$probeUris = @(
    "https://api.minimax.io/v1/usage",
    "https://api.minimax.io/api/v1/usage",
    "https://api.minimax.io/v1/models"
)
$probeResults = @($probeUris | ForEach-Object { Invoke-ProbeRequest -Uri $_ -Headers $headers })
$probeSummary = ($probeResults | ForEach-Object {
    "- $($_.uri) -> HTTP $($_.status_code)"
}) -join [Environment]::NewLine

$metadata = [ordered]@{
    captured_at_utc = [DateTimeOffset]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    model = $Model
    prompt = $Prompt
    max_tokens = $MaxTokens
    completion = @{
        status_code = [int]$completionResponse.StatusCode
        output_body = "completion-$timestamp.json"
        output_headers = "completion-$timestamp.headers.txt"
    }
    probes = @{
        output_file = "endpoint-probes-$timestamp.json"
    }
    notes = @{
        output_file = "capture-$timestamp.notes.md"
    }
}

$completionBody | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "completion-$timestamp.json")
(New-HeaderText -Response $completionResponse) | Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "completion-$timestamp.headers.txt")
$probeResults | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "endpoint-probes-$timestamp.json")
$metadata | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "capture-$timestamp.metadata.json")
(New-NotesTemplate -Model $Model -PromptTokens $promptTokens -CompletionTokens $completionTokens -EstimatedCost $estimatedCost -ProbeSummary $probeSummary) |
    Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "capture-$timestamp.notes.md")

Write-Host "Captured MiniMax PAYG evidence to $resolvedOutputDir"
Write-Host "Completion body: completion-$timestamp.json"
Write-Host "Completion headers: completion-$timestamp.headers.txt"
Write-Host "Endpoint probes: endpoint-probes-$timestamp.json"
Write-Host "Metadata: capture-$timestamp.metadata.json"
Write-Host "Notes: capture-$timestamp.notes.md"
