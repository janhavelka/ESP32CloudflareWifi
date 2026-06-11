param(
  [ValidateSet("Sample", "Batch")]
  [string]$Mode = "Sample",

  [string]$BaseUrl = "",
  [string]$DeviceId = "",
  [string]$Secret = "",
  [string]$Token = $env:TM_DEVICE_TOKEN,
  [long]$Seq = -1,
  [int]$New = 1,
  [int]$Dupes = 1,

  [switch]$InvalidMissingField,
  [switch]$Conflict,
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$DefaultSampleBaseUrl = "https://tm-dev-ingest.jnhavelka.workers.dev"
$DefaultBatchBaseUrl = "https://tunnel-monitor.jnhavelka.workers.dev"
$DefaultSampleDeviceId = "tm-dev-001"
$DefaultSampleSecret = "36dca50bf39fe7d1e474f0f33ddb6fc743e705beacdf02dbb034f0939adfd7b5"

function ConvertTo-Hex {
  param([byte[]]$Bytes)

  -join ($Bytes | ForEach-Object { $_.ToString("x2") })
}

function Get-Sha256Hex {
  param([string]$Text)

  $sha = [System.Security.Cryptography.SHA256]::Create()
  try {
    ConvertTo-Hex $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text))
  } finally {
    $sha.Dispose()
  }
}

function Get-HmacSha256Hex {
  param(
    [string]$SecretText,
    [string]$Text
  )

  $hmac = [System.Security.Cryptography.HMACSHA256]::new(
    [System.Text.Encoding]::UTF8.GetBytes($SecretText)
  )
  try {
    ConvertTo-Hex $hmac.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text))
  } finally {
    $hmac.Dispose()
  }
}

function Get-UtcIso {
  [DateTime]::UtcNow.ToString(
    "yyyy-MM-ddTHH:mm:ssZ",
    [System.Globalization.CultureInfo]::InvariantCulture
  )
}

function Get-UnixSeconds {
  [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
}

function Format-Seq {
  param([long]$Value)

  $Value.ToString("000000000")
}

function Get-CanonicalValue {
  param($Value)

  if ($Value -is [bool]) {
    if ($Value) { return "true" }
    return "false"
  }
  if ($Value -is [int] -or $Value -is [long] -or $Value -is [double] -or $Value -is [decimal]) {
    return [Convert]::ToString($Value, [System.Globalization.CultureInfo]::InvariantCulture)
  }
  if ($Value -is [string]) {
    return $Value
  }
  throw "invalid canonical value"
}

function New-SignedSample {
  param(
    [string]$DeviceId,
    [string]$Secret,
    [long]$Seq,
    [string]$SampleId
  )

  $ts = Get-UtcIso
  $sample = [ordered]@{
    schema = "tm.sample.v1"
    device_id = $DeviceId
    sample_id = $SampleId
    seq = $Seq
    ts = $ts
    channel = "cloud.test"
    value = 1
    unit = "bool"
    quality = "ok"
    auth = [ordered]@{
      t = $ts
      n = $SampleId
      h = ""
      s = ""
    }
  }

  $canonical = @(
    $sample.schema,
    $sample.device_id,
    $sample.sample_id,
    [string]$sample.seq,
    $sample.ts,
    $sample.channel,
    (Get-CanonicalValue $sample.value),
    $sample.unit,
    $sample.quality,
    $sample.auth.t,
    $sample.auth.n
  ) -join "`n"

  $sample.auth.h = Get-Sha256Hex $canonical
  $sample.auth.s = Get-HmacSha256Hex $Secret $canonical
  $sample
}

function New-NullArray {
  param([int]$Count)

  [object[]]::new($Count)
}

function New-FullBatchSample {
  param(
    [long]$Seq,
    [long]$Timestamp
  )

  [ordered]@{
    seq = $Seq
    t = $Timestamp
    vw_f = @(1240.52, 1241.52, 1242.52, 1243.52, 1244.52, 1245.52, 1246.52, 1247.52)
    vw_t = @(18.60, 18.70, 18.80, 18.90, 19.00, 19.10, 19.20, 19.30)
    shzk_t = @(21.20, 21.40, 21.60, 21.80, 22.00, 22.20, 22.40, 22.60, 22.80, 23.00, 23.20, 23.40, 23.60, 23.80, 24.00, 24.20)
    env = [ordered]@{ t_c = 20.80; rh_pct = 74.20; p_pa = 98420 }
    pwr = [ordered]@{ vin_v = 12.410; iin_a = 0.0830 }
  }
}

function New-EmptyBatchSample {
  param([long]$Seq)

  [ordered]@{
    seq = $Seq
    t = $null
    vw_f = (New-NullArray 8)
    vw_t = (New-NullArray 8)
    shzk_t = (New-NullArray 16)
    env = [ordered]@{ t_c = $null; rh_pct = $null; p_pa = $null }
    pwr = [ordered]@{ vin_v = $null; iin_a = $null }
  }
}

function New-Batch {
  param(
    [string]$DeviceId,
    [long]$SeqFirst
  )

  $seqLast = $SeqFirst + 1
  $now = Get-UnixSeconds
  [ordered]@{
    schema = "tm.batch.v1"
    profile = "tm.v1.vw8_shzk16_env_power"
    device_id = $DeviceId
    batch_id = "$DeviceId-$(Format-Seq $SeqFirst)-$(Format-Seq $seqLast)"
    seq_first = $SeqFirst
    seq_last = $seqLast
    sample_period_s = 900
    created_at = $now
    boot_id = "b00017"
    fw = "1.0.0"
    hw = "2.0.0"
    samples = @(
      (New-FullBatchSample -Seq $SeqFirst -Timestamp $now),
      (New-EmptyBatchSample -Seq $seqLast)
    )
    state = [ordered]@{
      uptime_s = 456789
      system = [ordered]@{ ok = $true; status = "OK" }
      rtc = [ordered]@{ ok = $true; status = "OK" }
      fram = [ordered]@{ ok = $true; status = "OK" }
      env = [ordered]@{ ok = $true; status = "OK" }
      ina228 = [ordered]@{ ok = $true; status = "OK" }
      vibwire = [ordered]@{ ok = $true; status = "OK" }
      shzk = [ordered]@{ ok = $true; status = "OK" }
      sd = [ordered]@{ ok = $true; status = "OK" }
      cloud = [ordered]@{ ok = $true; status = "OK" }
    }
    net = [ordered]@{ connected = $true }
    queue = [ordered]@{
      records = 2
      unsent_samples = 2
      oldest_unsent_seq = $SeqFirst
      cursor_start = 0
      cursor_end = 562
      bytes = 562
    }
    events = @()
  }
}

function Send-Json {
  param(
    [string]$Label,
    [string]$Url,
    [hashtable]$Headers,
    [string]$Body
  )

  Write-Host ""
  Write-Host $Label
  Write-Host "POST $Url"

  if ($DryRun) {
    Write-Output $Body
    return
  }

  try {
    $response = Invoke-WebRequest `
      -Method Post `
      -Uri $Url `
      -Headers $Headers `
      -ContentType "application/json" `
      -Body $Body `
      -UseBasicParsing

    Write-Host "HTTP $([int]$response.StatusCode)"
    Write-Output $response.Content
  } catch {
    $webResponse = $_.Exception.Response
    if ($null -eq $webResponse) {
      throw
    }

    $statusCode = [int]$webResponse.StatusCode
    $stream = $webResponse.GetResponseStream()
    $reader = [System.IO.StreamReader]::new($stream)
    try {
      $content = $reader.ReadToEnd()
    } finally {
      $reader.Dispose()
    }

    Write-Host "HTTP $statusCode"
    Write-Output $content
  }
}

if ($Mode -eq "Sample") {
  if ($BaseUrl.Length -eq 0) { $BaseUrl = $DefaultSampleBaseUrl }
  if ($DeviceId.Length -eq 0) { $DeviceId = $DefaultSampleDeviceId }
  if ($Secret.Length -eq 0) { $Secret = $DefaultSampleSecret }
  if ($Seq -lt 0) { $Seq = 1 }

  $url = $BaseUrl.TrimEnd("/") + "/v1/ingest"
  $runId = [DateTime]::UtcNow.ToString("yyyyMMddHHmmss", [System.Globalization.CultureInfo]::InvariantCulture)
  $firstBody = ""

  for ($i = 0; $i -lt $New; $i++) {
    $sampleSeq = $Seq + $i
    $sampleId = "$DeviceId-pc-test-$runId-$(Format-Seq $sampleSeq)"
    $sample = New-SignedSample -DeviceId $DeviceId -Secret $Secret -Seq $sampleSeq -SampleId $sampleId
    $body = $sample | ConvertTo-Json -Compress -Depth 16
    if ($i -eq 0) { $firstBody = $body }

    Send-Json `
      -Label "sample new $($i + 1)/$New seq=$sampleSeq" `
      -Url $url `
      -Headers @{} `
      -Body $body
  }

  for ($i = 0; $i -lt $Dupes; $i++) {
    Send-Json `
      -Label "sample duplicate $($i + 1)/$Dupes" `
      -Url $url `
      -Headers @{} `
      -Body $firstBody
  }

  if ($InvalidMissingField) {
    $invalidSeq = $Seq + $New + 1000
    $sampleId = "$DeviceId-pc-test-$runId-missing-$(Format-Seq $invalidSeq)"
    $invalid = New-SignedSample -DeviceId $DeviceId -Secret $Secret -Seq $invalidSeq -SampleId $sampleId
    $invalid.Remove("unit")

    Send-Json `
      -Label "sample invalid missing unit" `
      -Url $url `
      -Headers @{} `
      -Body ($invalid | ConvertTo-Json -Compress -Depth 16)
  }

  exit
}

if ($BaseUrl.Length -eq 0) { $BaseUrl = $DefaultBatchBaseUrl }
if ($DeviceId.Length -eq 0) { $DeviceId = "tm-001" }
if ($Seq -lt 0) { $Seq = 1234 }
if ($Token.Length -eq 0) {
  throw "Batch mode needs -Token or `$env:TM_DEVICE_TOKEN."
}

$batchUrl = $BaseUrl.TrimEnd("/") + "/api/v1/ingest"
$batchHeaders = @{
  "X-TM-Device" = $DeviceId
  "X-TM-Token" = $Token
}
$firstBatch = $null
$firstBody = ""

for ($i = 0; $i -lt $New; $i++) {
  $seqFirst = $Seq + ($i * 2)
  $batch = New-Batch -DeviceId $DeviceId -SeqFirst $seqFirst
  $body = $batch | ConvertTo-Json -Compress -Depth 32
  if ($i -eq 0) {
    $firstBatch = $batch
    $firstBody = $body
  }

  Send-Json `
    -Label "batch new $($i + 1)/$New seq=$seqFirst-$($seqFirst + 1)" `
    -Url $batchUrl `
    -Headers $batchHeaders `
    -Body $body
}

for ($i = 0; $i -lt $Dupes; $i++) {
  Send-Json `
    -Label "batch duplicate $($i + 1)/$Dupes" `
    -Url $batchUrl `
    -Headers $batchHeaders `
    -Body $firstBody
}

if ($Conflict) {
  $conflictBatch = New-Batch -DeviceId $DeviceId -SeqFirst $Seq
  $conflictBatch.batch_id = $firstBatch.batch_id
  $conflictBatch.created_at = $firstBatch.created_at
  $conflictBatch.samples[0].t = $firstBatch.samples[0].t
  $conflictBatch.samples[0].env.t_c = 20.81

  Send-Json `
    -Label "batch duplicate conflict" `
    -Url $batchUrl `
    -Headers $batchHeaders `
    -Body ($conflictBatch | ConvertTo-Json -Compress -Depth 32)
}

if ($InvalidMissingField) {
  $invalidSeq = $Seq + ($New * 2) + 1000
  $invalid = New-Batch -DeviceId $DeviceId -SeqFirst $invalidSeq
  $invalid.samples[0].env.Remove("t_c")

  Send-Json `
    -Label "batch invalid missing samples[0].env.t_c" `
    -Url $batchUrl `
    -Headers $batchHeaders `
    -Body ($invalid | ConvertTo-Json -Compress -Depth 32)
}
