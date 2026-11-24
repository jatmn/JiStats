param(
    [Parameter(Mandatory = $false)]
    [string]$OutputDir = "",
    [Parameter(Mandatory = $false)]
    [long]$StartTime = 0,
    [Parameter(Mandatory = $false)]
    [long]$EndTime = 0,
    [Parameter(Mandatory = $false)]
    [ValidateSet("1m", "1h", "1d")]
    [string]$BucketWidth = "1h"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "lib/capture_common.ps1")

function Invoke-OpenAIApiPlatformRequest {
    param(
        [string]$Uri,
        [hashtable]$Headers
    )

    try {
        $response = Invoke-WebRequest -Uri $Uri -Headers $Headers -Method Get -UseBasicParsing
        return @{
            ok = $true
            status_code = [int]$response.StatusCode
            body = ($response.Content | ConvertFrom-Json)
        }
    } catch {
        $statusCode = 0
        $bodyText = ""
        if ($_.Exception.Response -ne $null) {
            $statusCode = [int]$_.Exception.Response.StatusCode
            $stream = $_.Exception.Response.GetResponseStream()
            if ($stream -ne $null) {
                $reader = New-Object System.IO.StreamReader($stream)
                $bodyText = $reader.ReadToEnd()
                $reader.Dispose()
                $stream.Dispose()
            }
        }
        if ([string]::IsNullOrWhiteSpace($bodyText) -and $_.ErrorDetails -ne $null) {
            $bodyText = $_.ErrorDetails.Message
        }

        $body = $null
        if (-not [string]::IsNullOrWhiteSpace($bodyText)) {
            try {
                $body = $bodyText | ConvertFrom-Json
            } catch {
                $body = @{ raw = $bodyText }
            }
        }

        return @{
            ok = $false
            status_code = $statusCode
            body = $body
            error = $_.Exception.Message
        }
    }
}

function New-ReadinessReport {
    param(
        [string]$CredentialSource,
        [bool]$TokenPresent,
        [bool]$OrganizationHeaderPresent,
        [bool]$ProjectIdPresent,
        [string[]]$MissingRequirements
    )

    return [ordered]@{
        captured_at_utc = [DateTimeOffset]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        openai_api_platform = [ordered]@{
            credential_source = if ([string]::IsNullOrEmpty($CredentialSource)) { $null } else { $CredentialSource }
            token_present = $TokenPresent
            organization_header_present = $OrganizationHeaderPresent
            project_id_present = $ProjectIdPresent
            billing_expected = $true
            missing_requirements = $MissingRequirements
            can_attempt_live_probe = ($MissingRequirements.Count -eq 0)
        }
    }
}

function ConvertTo-ArchivePayload {
    param($Response)

    if ($Response.body -ne $null) {
        return $Response.body
    }

    return [ordered]@{
        ok = $Response.ok
        status_code = $Response.status_code
        error = $Response.error
    }
}

function New-CaptureNotesTemplate {
    param(
        [string]$CapturedAt,
        [string]$BucketWidth,
        [long]$StartTime,
        [long]$EndTime
    )

    $effectiveEndTime = if ($EndTime -gt 0) { [string]$EndTime } else { "open-ended" }

    return @"
# OpenAI API-Platform Capture Notes

- Captured at: $CapturedAt
- Bucket width: $BucketWidth
- Start time: $StartTime
- End time: $effectiveEndTime

## Freshness / Lag Notes

- Compare these buckets against the most recent known API activity window.
- Record the practical delay between activity time and visible usage/cost appearance here.

## Redaction Notes

- Record any organization, project, user, or API-key identifiers that were redacted before sharing.
- Record whether project/group identifiers were preserved, hashed, or removed.
"@
}

$resolvedOutputDir = Initialize-CaptureDirectory -OutputDir $OutputDir `
    -DefaultSubdirectory "openai/api-platform" -RepoRoot (Split-Path -Parent $PSScriptRoot)

$credentialSource = "OPENAI_ADMIN_KEY"
$token = Get-EnvValue $credentialSource
if ([string]::IsNullOrEmpty($token)) {
    $credentialSource = "OPENAI_API_KEY"
    $token = Get-EnvValue $credentialSource
}
if ([string]::IsNullOrEmpty($token)) {
    $credentialSource = ""
}

$organization = Get-EnvValue "OPENAI_ORG_ID"
if ([string]::IsNullOrEmpty($organization)) {
    $organization = Get-EnvValue "OPENAI_ORGANIZATION"
}
$projectId = Get-EnvValue "OPENAI_PROJECT_ID"

$missingRequirements = New-Object System.Collections.Generic.List[string]
if ([string]::IsNullOrEmpty($token)) {
    $missingRequirements.Add("missing OPENAI_ADMIN_KEY or OPENAI_API_KEY")
}
if ([string]::IsNullOrEmpty($organization)) {
    $missingRequirements.Add("missing OPENAI_ORG_ID or OPENAI_ORGANIZATION")
}

$capturedAt = [DateTimeOffset]::UtcNow.ToString("yyyy-MM-ddTHHmmssZ")
$readiness = New-ReadinessReport `
    -CredentialSource $credentialSource `
    -TokenPresent (-not [string]::IsNullOrEmpty($token)) `
    -OrganizationHeaderPresent (-not [string]::IsNullOrEmpty($organization)) `
    -ProjectIdPresent (-not [string]::IsNullOrEmpty($projectId)) `
    -MissingRequirements $missingRequirements
$readiness | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "readiness-$capturedAt.json")

if ([string]::IsNullOrEmpty($token)) {
    [Console]::Error.WriteLine("Missing OPENAI_ADMIN_KEY or OPENAI_API_KEY. A separate OpenAI API-platform credential is required.")
    exit 2
}

if ($StartTime -le 0) {
    $StartTime = [DateTimeOffset]::UtcNow.AddHours(-6).ToUnixTimeSeconds()
}

$queryParams = @{
    start_time = $StartTime
    bucket_width = $BucketWidth
}
if ($EndTime -gt 0) {
    $queryParams["end_time"] = $EndTime
}

$queryString = ($queryParams.GetEnumerator() | Sort-Object Key | ForEach-Object {
    "{0}={1}" -f [uri]::EscapeDataString($_.Key), [uri]::EscapeDataString([string]$_.Value)
}) -join "&"

$headers = @{
    Authorization = "Bearer $token"
}
if (-not [string]::IsNullOrEmpty($organization)) {
    $headers["OpenAI-Organization"] = $organization
}

$usageUri = "https://api.openai.com/v1/organization/usage/completions?$queryString"
$costsUri = "https://api.openai.com/v1/organization/costs?$queryString"

$usageResponse = Invoke-OpenAIApiPlatformRequest -Uri $usageUri -Headers $headers
$costsResponse = Invoke-OpenAIApiPlatformRequest -Uri $costsUri -Headers $headers

$metadata = [ordered]@{
    captured_at_utc = [DateTimeOffset]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    readiness_file = "readiness-$capturedAt.json"
    credential_source = $credentialSource
    organization_header_present = -not [string]::IsNullOrEmpty($organization)
    project_id_present = -not [string]::IsNullOrEmpty($projectId)
    request = @{
        start_time = $StartTime
        end_time = if ($EndTime -gt 0) { $EndTime } else { $null }
        bucket_width = $BucketWidth
    }
    usage = @{
        uri = $usageUri
        status_code = $usageResponse.status_code
        output_file = "usage-$capturedAt.json"
        ok = $usageResponse.ok
    }
    costs = @{
        uri = $costsUri
        status_code = $costsResponse.status_code
        output_file = "costs-$capturedAt.json"
        ok = $costsResponse.ok
    }
    notes = @{
        output_file = "capture-$capturedAt.notes.md"
    }
}

(ConvertTo-ArchivePayload -Response $usageResponse) | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "usage-$capturedAt.json")
(ConvertTo-ArchivePayload -Response $costsResponse) | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "costs-$capturedAt.json")
$metadata | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "capture-$capturedAt.metadata.json")
$notes = New-CaptureNotesTemplate -CapturedAt $capturedAt -BucketWidth $BucketWidth -StartTime $StartTime -EndTime $EndTime
$notes | Set-Content -Encoding utf8 -LiteralPath (Join-Path $resolvedOutputDir "capture-$capturedAt.notes.md")

Write-Host "Captured OpenAI API-platform evidence to $resolvedOutputDir"
Write-Host "Readiness: readiness-$capturedAt.json"
Write-Host "Usage: usage-$capturedAt.json"
Write-Host "Costs: costs-$capturedAt.json"
Write-Host "Metadata: capture-$capturedAt.metadata.json"
Write-Host "Notes: capture-$capturedAt.notes.md"

if (-not $usageResponse.ok -or -not $costsResponse.ok) {
    exit 3
}
