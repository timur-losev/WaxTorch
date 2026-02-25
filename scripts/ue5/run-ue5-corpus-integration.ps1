[CmdletBinding()]
param(
  [string]$RepoRoot = "j:\UE5.2SRC\",
  [string]$ServerUrl = "http://127.0.0.1:8080",
  [string]$QueriesFile = "cpp/tests/integration/ue5-query-pack.json",
  [string]$ReportOut = "cpp/tests/integration/ue5-corpus-integration-report.json",
  [bool]$Resume = $true,
  [int]$FlushEveryChunks = 128,
  [int]$IngestBatchSize = 1,
  [int]$MaxFiles = 0,
  [int]$MaxChunks = 0,
  [int]$MaxRamMB = 0,
  [int]$PollIntervalSeconds = 2,
  [int]$TimeoutMinutes = 180,
  [switch]$StartServer,
  [string]$ServerExe = "cpp/build/bin/waxcpp_rag_server.exe",
  [string]$ServerWorkDir = ".",
  [int]$AnswerMaxContextItems = 12,
  [int]$AnswerMaxContextTokens = 6000,
  [int]$AnswerMaxOutputTokens = 768,
  [double]$MinInfluenceRate = 0.0,
  [double]$MinRequiredPathMatchRate = 0.0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Step([string]$message) {
  Write-Host "[ue5-integration] $message"
}

function Resolve-OptionalPath([string]$path, [string]$baseDir) {
  if ([string]::IsNullOrWhiteSpace($path)) {
    return $path
  }
  if ([System.IO.Path]::IsPathRooted($path)) {
    return $path
  }
  return [System.IO.Path]::GetFullPath((Join-Path $baseDir $path))
}

function Invoke-JsonRpc([string]$method, [hashtable]$params) {
  $payload = @{
    jsonrpc = "2.0"
    id = [int](Get-Random -Minimum 1000 -Maximum 999999)
    method = $method
    params = $params
  } | ConvertTo-Json -Depth 20 -Compress

  try {
    $response = Invoke-WebRequest -Uri $ServerUrl -Method POST -ContentType "application/json" -Body $payload
  } catch {
    throw ("RPC transport failed for method '{0}' to '{1}': {2}" -f $method, $ServerUrl, $_.Exception.Message)
  }
  $raw = $response.Content
  if ([string]::IsNullOrWhiteSpace($raw)) {
    throw "Empty response for method '$method'"
  }
  if ($raw.StartsWith("Error:")) {
    throw "$method failed: $raw"
  }
  try {
    return ($raw | ConvertFrom-Json -Depth 40)
  } catch {
    return [pscustomobject]@{ raw = $raw }
  }
}

function Wait-ServerReachable([int]$maxAttempts, [int]$delaySeconds) {
  for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
    try {
      $null = Invoke-JsonRpc -method "index.status" -params @{}
      return
    } catch {
      if ($attempt -eq $maxAttempts) {
        throw ("Server at '{0}' is not reachable after {1} attempts: {2}" -f $ServerUrl, $maxAttempts, $_.Exception.Message)
      }
      Start-Sleep -Seconds $delaySeconds
    }
  }
}

function Wait-IndexTerminalState() {
  $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
  while ((Get-Date) -lt $deadline) {
    $status = Invoke-JsonRpc -method "index.status" -params @{}
    $state = [string]$status.state
    $phase = [string]$status.phase
    $indexed = [int64]$status.indexed_chunks
    $committed = [int64]$status.committed_chunks
    $rss = [double]$status.process_rss_mb
    Write-Step ("index.status state={0} phase={1} indexed={2} committed={3} rss_mb={4:N1}" -f $state, $phase, $indexed, $committed, $rss)
    if ($state -ne "running") {
      return $status
    }
    Start-Sleep -Seconds $PollIntervalSeconds
  }
  throw "Indexing timeout after ${TimeoutMinutes} minutes"
}

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
  throw "RepoRoot is empty"
}

$repoRootResolved = Resolve-OptionalPath -path $RepoRoot -baseDir (Get-Location).Path
$queriesFileResolved = Resolve-OptionalPath -path $QueriesFile -baseDir (Get-Location).Path
$reportOutResolved = Resolve-OptionalPath -path $ReportOut -baseDir (Get-Location).Path
$serverExeResolved = Resolve-OptionalPath -path $ServerExe -baseDir (Get-Location).Path
$serverWorkDirResolved = Resolve-OptionalPath -path $ServerWorkDir -baseDir (Get-Location).Path

if (-not (Test-Path -LiteralPath $repoRootResolved)) {
  throw "RepoRoot does not exist: $RepoRoot"
}
if (-not (Test-Path -LiteralPath $queriesFileResolved)) {
  throw "Queries file does not exist: $QueriesFile"
}

$serverProcess = $null
try {
  if ($StartServer.IsPresent) {
    if (-not (Test-Path -LiteralPath $serverExeResolved)) {
      throw "Server executable not found: $ServerExe"
    }
    Write-Step "Starting server process: $serverExeResolved"
    $serverProcess = Start-Process -FilePath $serverExeResolved -WorkingDirectory $serverWorkDirResolved -PassThru
    Wait-ServerReachable -maxAttempts 30 -delaySeconds 1
  } else {
    Wait-ServerReachable -maxAttempts 3 -delaySeconds 1
  }

  Write-Step "Starting index job"
  $startParams = @{
    repo_root = (Resolve-Path -LiteralPath $repoRootResolved).Path
    resume = [bool]$Resume
    flush_every_chunks = $FlushEveryChunks
    ingest_batch_size = $IngestBatchSize
    max_files = $MaxFiles
    max_chunks = $MaxChunks
    max_ram_mb = $MaxRamMB
  }
  $startStatus = Invoke-JsonRpc -method "index.start" -params $startParams
  Write-Step ("index.start accepted state={0} job_id={1}" -f [string]$startStatus.state, [string]$startStatus.job_id)

  $finalStatus = Wait-IndexTerminalState
  $finalState = [string]$finalStatus.state
  if ($finalState -ne "stopped") {
    throw ("Index job finished in non-success state: {0}; last_error={1}" -f $finalState, [string]$finalStatus.last_error)
  }
  Write-Step "Indexing completed successfully"

  $queries = Get-Content -LiteralPath $queriesFileResolved -Raw | ConvertFrom-Json -Depth 20
  if ($null -eq $queries -or $queries.Count -eq 0) {
    throw "Queries file is empty: $QueriesFile"
  }

  $results = @()
  foreach ($q in $queries) {
    $queryText = [string]$q.query
    if ([string]::IsNullOrWhiteSpace($queryText)) {
      continue
    }
    $queryId = if ($q.PSObject.Properties.Name -contains "id") { [string]$q.id } else { $queryText }
    Write-Step "answer.generate query_id=$queryId"
    $answerParams = @{
      query = $queryText
      max_context_items = $AnswerMaxContextItems
      max_context_tokens = $AnswerMaxContextTokens
      max_output_tokens = $AnswerMaxOutputTokens
    }
    $answer = Invoke-JsonRpc -method "answer.generate" -params $answerParams
    $citations = @()
    if ($answer.PSObject.Properties.Name -contains "citations" -and $null -ne $answer.citations) {
      $citations = @($answer.citations)
    }
    $citationPaths = @()
    foreach ($c in $citations) {
      $citationPaths += [string]$c.relative_path
    }

    $requiredPaths = @()
    if ($q.PSObject.Properties.Name -contains "must_cite_path_substr" -and $null -ne $q.must_cite_path_substr) {
      $requiredPaths = @($q.must_cite_path_substr)
    }
    $allRequiredMatched = $true
    foreach ($needle in $requiredPaths) {
      $needleText = [string]$needle
      $matched = $false
      foreach ($path in $citationPaths) {
        if ($path.IndexOf($needleText, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
          $matched = $true
          break
        }
      }
      if (-not $matched) {
        $allRequiredMatched = $false
        break
      }
    }

    $results += [pscustomobject]@{
      id = $queryId
      query = $queryText
      answer_length = ([string]$answer.answer).Length
      citations_count = $citations.Count
      context_items_used = [int]$answer.context_items_used
      context_tokens_used = [int]$answer.context_tokens_used
      required_path_match = $allRequiredMatched
      citation_paths = $citationPaths
    }
  }

  $total = $results.Count
  $withCitations = @($results | Where-Object { $_.citations_count -gt 0 }).Count
  $withContext = @($results | Where-Object { $_.context_items_used -gt 0 }).Count
  $requiredMatched = @($results | Where-Object { $_.required_path_match -eq $true }).Count
  $influenceRate = if ($total -gt 0) { [Math]::Round(($withCitations / $total), 4) } else { 0.0 }
  $requiredPathMatchRate = if ($total -gt 0) { [Math]::Round(($requiredMatched / $total), 4) } else { 0.0 }

  $report = [pscustomobject]@{
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    server_url = $ServerUrl
    repo_root = (Resolve-Path -LiteralPath $repoRootResolved).Path
    index_start_params = $startParams
    index_final_status = $finalStatus
    query_count = $total
    queries_with_citations = $withCitations
    queries_with_context = $withContext
    queries_required_paths_matched = $requiredMatched
    corpus_influence_rate = $influenceRate
    required_path_match_rate = $requiredPathMatchRate
    results = $results
  }

  $reportDir = Split-Path -Path $reportOutResolved -Parent
  if (-not [string]::IsNullOrWhiteSpace($reportDir)) {
    New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
  }
  $report | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $reportOutResolved -Encoding UTF8
  Write-Step "Report saved: $reportOutResolved"
  Write-Step ("Summary: queries={0} with_citations={1} influence_rate={2} required_match_rate={3}" -f $total, $withCitations, $influenceRate, $requiredPathMatchRate)

  if ($MinInfluenceRate -gt 0.0 -and $influenceRate -lt $MinInfluenceRate) {
    throw ("Influence threshold failed: actual={0} < min={1}" -f $influenceRate, $MinInfluenceRate)
  }
  if ($MinRequiredPathMatchRate -gt 0.0 -and $requiredPathMatchRate -lt $MinRequiredPathMatchRate) {
    throw ("Required-path threshold failed: actual={0} < min={1}" -f $requiredPathMatchRate, $MinRequiredPathMatchRate)
  }
}
finally {
  if ($null -ne $serverProcess -and -not $serverProcess.HasExited) {
    Write-Step "Stopping server process id=$($serverProcess.Id)"
    Stop-Process -Id $serverProcess.Id -Force
  }
}
