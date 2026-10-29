# Remove everything deploy_aws.ps1 created: the device certificate (detached,
# deactivated, deleted) and the CDK stack (table and its data included).
# The CDK bootstrap stack (CDKToolkit) is left in place, as other CDK apps may
# use it; delete it from the CloudFormation console if you no longer need it.

param([switch]$Yes)

$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
$deploymentFile = Join-Path $root "certs\aws-deployment.json"
$aws = "C:\Program Files\Amazon\AWSCLIV2\aws.exe"
if (-not (Test-Path $aws)) { $aws = "aws" }

if (-not (Test-Path $deploymentFile)) { Write-Host "No certs\aws-deployment.json: nothing deployed from here." -ForegroundColor Yellow; exit 0 }
$d = Get-Content $deploymentFile -Raw | ConvertFrom-Json

if (-not $Yes) {
    $answer = Read-Host "Delete the EdgeVision stack and its data in $($d.account)/$($d.region)? (yes/no)"
    if ($answer -ne "yes") { exit 1 }
}

$certId = ($d.certificate_arn -split "/")[-1]
& $aws iot detach-thing-principal --region $d.region --thing-name $d.thing --principal $d.certificate_arn
& $aws iot detach-policy --region $d.region --policy-name $d.policy --target $d.certificate_arn
& $aws iot update-certificate --region $d.region --certificate-id $certId --new-status INACTIVE
& $aws iot delete-certificate --region $d.region --certificate-id $certId

Push-Location (Join-Path $root "cloud\aws")
$env:CDK_DEFAULT_ACCOUNT = $d.account
$env:CDK_DEFAULT_REGION = $d.region
$env:CDK_DISABLE_VERSION_CHECK = "1"
npx cdk destroy --force --context "thingName=$($d.thing)"
Pop-Location

Remove-Item (Join-Path $root "certs\certificate-arn.txt"), (Join-Path $root "certs\device.pem.crt"), (Join-Path $root "certs\private.pem.key"), $deploymentFile -ErrorAction SilentlyContinue
Write-Host "Removed." -ForegroundColor Green
