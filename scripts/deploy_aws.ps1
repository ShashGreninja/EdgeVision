# Deploy the EdgeVision backend to AWS and give the camera its identity.
#
#   aws configure                      (once: your credentials and region)
#   .\scripts\deploy_aws.ps1           (defaults: ap-southeast-2, profile edgevision)
#   .\scripts\run_demo.ps1 -Aws -Clip eval\clips\testclip.mp4
#
# Steps: check credentials -> CDK bootstrap (once per account/region) -> CDK
# deploy -> device certificate (private key stays in certs\, never in git or
# CloudFormation) attached to the thing and its policy -> Amazon root CA ->
# IoT data endpoint. Writes certs\aws-deployment.json for run_demo.ps1.
# Safe to re-run: existing certificates are reused.

param(
    [string]$Region = "ap-southeast-2",
    [string]$Profile = "edgevision",
    [string]$Thing = "edge-cam-01"
)

$ErrorActionPreference = "Continue"
$env:AWS_PROFILE = $Profile
$root = Split-Path $PSScriptRoot -Parent
$certs = Join-Path $root "certs"
$aws = "C:\Program Files\Amazon\AWSCLIV2\aws.exe"
if (-not (Test-Path $aws)) { $aws = "aws" }

function Fail([string]$msg) { Write-Host $msg -ForegroundColor Red; exit 1 }

# ---- 1. Credentials ------------------------------------------------------------
$account = & $aws sts get-caller-identity --query Account --output text 2>$null
if ($LASTEXITCODE -ne 0 -or -not $account) { Fail "No working AWS credentials. Run 'aws configure' first." }
Write-Host "AWS account $account, region $Region" -ForegroundColor Cyan

# ---- 2-3. CDK bootstrap and deploy -------------------------------------------------
$env:CDK_DEFAULT_ACCOUNT = $account
$env:CDK_DEFAULT_REGION = $Region
$env:AWS_REGION = $Region
$env:CDK_DISABLE_VERSION_CHECK = "1"
Push-Location (Join-Path $root "cloud\aws")
if (-not (Test-Path node_modules)) { npm ci --no-audit --no-fund }
npx cdk bootstrap "aws://$account/$Region"
if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "cdk bootstrap failed" }
npx cdk deploy --require-approval never --context "thingName=$Thing" --outputs-file cdk-outputs.json
if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "cdk deploy failed" }
$outputs = (Get-Content cdk-outputs.json -Raw | ConvertFrom-Json).EdgeVision
Pop-Location

# ---- 4. Device certificate -------------------------------------------------------------
New-Item -ItemType Directory -Force $certs | Out-Null
$certFile = Join-Path $certs "device.pem.crt"
$keyFile = Join-Path $certs "private.pem.key"
$arnFile = Join-Path $certs "certificate-arn.txt"

if (-not (Test-Path $arnFile)) {
    Write-Host "Creating the device certificate" -ForegroundColor Cyan
    $arn = & $aws iot create-keys-and-certificate --region $Region --set-as-active `
        --certificate-pem-outfile $certFile --private-key-outfile $keyFile --query certificateArn --output text
    if ($LASTEXITCODE -ne 0 -or -not $arn) { Fail "could not create the device certificate" }
    Set-Content -Encoding ascii $arnFile $arn
} else {
    $arn = (Get-Content $arnFile -Raw).Trim()
    Write-Host "Reusing certificate $arn"
}

& $aws iot attach-policy --region $Region --policy-name $outputs.PolicyName --target $arn
& $aws iot attach-thing-principal --region $Region --thing-name $outputs.ThingName --principal $arn
if ($LASTEXITCODE -ne 0) { Fail "could not attach the certificate to the thing" }

# ---- 5. Root CA and endpoint ---------------------------------------------------------------
$ca = Join-Path $certs "AmazonRootCA1.pem"
if (-not (Test-Path $ca)) { Invoke-WebRequest -UseBasicParsing https://www.amazontrust.com/repository/AmazonRootCA1.pem -OutFile $ca }
$endpoint = & $aws iot describe-endpoint --region $Region --endpoint-type iot:Data-ATS --query endpointAddress --output text

$deployment = [ordered]@{
    account = $account; region = $Region; thing = $outputs.ThingName; endpoint = $endpoint
    events_api = $outputs.EventsApiUrl; frontend_url = $outputs.FrontendUrl; table = $outputs.TableName; policy = $outputs.PolicyName; certificate_arn = $arn
}
[IO.File]::WriteAllText((Join-Path $certs "aws-deployment.json"), ($deployment | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))

Write-Host "`nDeployed." -ForegroundColor Green
Write-Host "  IoT endpoint : $endpoint"
Write-Host "  Events API   : $($outputs.EventsApiUrl)"
Write-Host "  Frontend     : $($outputs.FrontendUrl)"
Write-Host "  Run it       : .\scripts\run_demo.ps1 -Aws -Clip eval\clips\testclip.mp4"
Write-Host "  Remove it    : .\scripts\destroy_aws.ps1"
