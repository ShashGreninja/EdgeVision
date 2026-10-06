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

All of it fits in the AWS free tier at demo volumes. `cdk destroy` removes it,
including the table's data.

## Deploy

Needs Node.js, the AWS CLI, and credentials for your account (`aws configure`).

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

Dashboard: `http://127.0.0.1:8000/?api=<EventsApiUrl>`. The local cloud stub
still serves the page; the event list then comes from AWS.

## Check the stack without deploying

```bash
npx tsc -p .     # type-check
npx cdk synth    # writes cdk.out/EdgeVision.template.json
```
