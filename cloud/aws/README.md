# EdgeVision on AWS

```
camera --MQTT over mutual TLS--> AWS IoT Core --IoT rule--> DynamoDB <-- Lambda <-- HTTP API (GET /events) <-- dashboard
```

The stack (`lib/edgevision-stack.ts`) creates:

| Resource | Purpose |
|---|---|
| IoT thing `edge-cam-01` | The camera's identity |
| IoT policy `edge-cam-01-policy` | Connect only as itself; publish only to `edgevision/<thing>/events` |
| IoT topic rule | `SELECT *, topic(2) AS device, timestamp() AS received_at ... FROM 'edgevision/+/events'` into DynamoDB |
| DynamoDB table | Events keyed by `device` + `received_at`, expire after 30 days, on-demand billing |
| Lambda + HTTP API | `GET /events?device=edge-cam-01&limit=50`, newest first, CORS open for the dashboard |
| Private S3 bucket + frontend Lambda | HTTPS landing page, demo, guide and downloads through regional API Gateway; no public bucket access |
| HTTP API routes | `/api/events` invokes the event reader; `/` and `/{proxy+}` serve frontend files |

Storage, requests, transfer and deployment helpers consume AWS usage/credits; do not assume hosting is always free. `cdk destroy` removes the site and backend, including the table's demo data.

## Deploy

The short way, from the repo root (does everything below):

```powershell
.\scripts\deploy_aws.ps1 -Region ap-southeast-2 -Profile edgevision
```

Step by step: needs Node.js, the AWS CLI, and credentials for your account (`aws configure`).

```bash
cd cloud/aws
npm ci
npx cdk bootstrap        # once per account and region
npx cdk deploy           # prints EventsApiUrl
```

## Give the camera a certificate

The device certificate is created outside CDK so that its private key never
passes through CloudFormation. Keep these files out of git (`.gitignore`
already excludes `certs/`).

```bash
mkdir certs
aws iot create-keys-and-certificate --set-as-active \
  --certificate-pem-outfile certs/device.pem.crt \
  --private-key-outfile certs/private.pem.key \
  --query certificateArn --output text
# Use the ARN printed above:
aws iot attach-policy --policy-name edge-cam-01-policy --target <certificate-arn>
aws iot attach-thing-principal --thing-name edge-cam-01 --principal <certificate-arn>
curl -o certs/AmazonRootCA1.pem https://www.amazontrust.com/repository/AmazonRootCA1.pem
aws iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text
```

## Point the system at AWS

Run the Wi-Fi module against IoT Core instead of local Mosquitto, and open the
dashboard with the API URL from `cdk deploy`:

```bash
python netmodule/wifi_module.py --broker <endpoint> --port 8883 \
  --ca certs/AmazonRootCA1.pem --cert certs/device.pem.crt --key certs/private.pem.key
```

Open the `FrontendUrl` printed by deployment for the landing page. `app.html` opens recorded playback; `app.html?mode=live#events` reads current cloud events through `/api/events`. `guide.html` contains the project guide and Word/PDF downloads. Firmware and the Wi-Fi emulator still run on your PC. A browser may request local-network permission before the hosted page can reach the simulator; the local dashboard opened by `run_demo.ps1` is also available.

The S3 bucket blocks public access, uses encryption and requires TLS. The file-serving Lambda has read-only bucket permissions and sends security headers, MIME types and five-minute browser caching. Video byte ranges support playback and seeking. Each asset must stay below 4 MB to fit the buffered Lambda response; the current video is about 1.8 MB. Live event responses are not cached. No Lambda@Edge or cross-Region resources are used. The event API remains public and is appropriate for demo metadata only. Use Lambda/API Gateway CloudWatch metrics and Lambda logs for monitoring; API access logs and CloudTrail S3 data events can be enabled for more detailed auditing with additional usage charges.

CloudFront creation was rejected by AWS because the project needs verification through AWS Support. The regional endpoint provides HTTPS hosting without that dependency. All Regional resources are in `ap-southeast-2`; confirm your assigned Region in AWS Settings > View all projects > Overview > Additional Info > Region. Monitor credit/spend status in AWS Settings > Billing.

## Check the stack without deploying

```bash
npx tsc -p .     # type-check
npm test        # private bucket and regional route assertions
npx cdk synth    # writes cdk.out/EdgeVision.template.json
```

From the repo root, install the optional Python test dependencies with `uv pip install --python .venv/Scripts/python.exe -r cloud/aws/test/requirements.txt`, then run `.venv/Scripts/python.exe -m unittest discover -s cloud/aws/test -p 'test_*.py'`.
