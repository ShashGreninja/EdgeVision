import * as cdk from 'aws-cdk-lib';
import * as apigw from 'aws-cdk-lib/aws-apigatewayv2';
import * as integrations from 'aws-cdk-lib/aws-apigatewayv2-integrations';
import * as dynamodb from 'aws-cdk-lib/aws-dynamodb';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as iot from 'aws-cdk-lib/aws-iot';
import * as lambda from 'aws-cdk-lib/aws-lambda';
import * as s3 from 'aws-cdk-lib/aws-s3';
import * as deployment from 'aws-cdk-lib/aws-s3-deployment';
import { Construct } from 'constructs';
import * as path from 'path';

export interface EdgeVisionStackProps extends cdk.StackProps {
  /** AWS IoT thing name of the camera; also its MQTT client id. */
  thingName: string;
}

/**
 * Cloud side of EdgeVision.
 *
 *   camera --MQTT/TLS--> IoT Core --rule--> DynamoDB <--Lambda <--HTTP API-- dashboard
 *
 * The camera publishes small event JSON to edgevision/<thing>/events. The IoT
 * rule stores every event with its arrival time; the HTTP API returns the
 * latest events per device for the dashboard. Events expire after 30 days.
 */
export class EdgeVisionStack extends cdk.Stack {
  constructor(scope: Construct, id: string, props: EdgeVisionStackProps) {
    super(scope, id, props);

    const table = new dynamodb.Table(this, 'Events', {
      partitionKey: { name: 'device', type: dynamodb.AttributeType.STRING },
      sortKey: { name: 'received_at', type: dynamodb.AttributeType.NUMBER },
      billingMode: dynamodb.BillingMode.PAY_PER_REQUEST,
      timeToLiveAttribute: 'expires_at',
      removalPolicy: cdk.RemovalPolicy.DESTROY, // Demo data: delete with the stack.
    });

    // ---- Device identity and permissions -----------------------------------
    const thing = new iot.CfnThing(this, 'Camera', { thingName: props.thingName });

    // A device may connect only as itself and publish only to its own topic.
    const policy = new iot.CfnPolicy(this, 'CameraPolicy', {
      policyName: `${props.thingName}-policy`,
      policyDocument: {
        Version: '2012-10-17',
        Statement: [
          {
            Effect: 'Allow',
            Action: 'iot:Connect',
            Resource: `arn:${cdk.Aws.PARTITION}:iot:${cdk.Aws.REGION}:${cdk.Aws.ACCOUNT_ID}:client/\${iot:Connection.Thing.ThingName}`,
          },
          {
            Effect: 'Allow',
            Action: 'iot:Publish',
            Resource: `arn:${cdk.Aws.PARTITION}:iot:${cdk.Aws.REGION}:${cdk.Aws.ACCOUNT_ID}:topic/edgevision/\${iot:Connection.Thing.ThingName}/events`,
          },
        ],
      },
    });

    // ---- IoT rule: every event into DynamoDB --------------------------------
    const ruleRole = new iam.Role(this, 'RuleRole', {
      assumedBy: new iam.ServicePrincipal('iot.amazonaws.com'),
    });
    table.grantWriteData(ruleRole);

    new iot.CfnTopicRule(this, 'EventsRule', {
      topicRulePayload: {
        sql:
          "SELECT *, topic(2) AS device, timestamp() AS received_at, floor(timestamp() / 1000) + 2592000 AS expires_at " +
          "FROM 'edgevision/+/events'",
        awsIotSqlVersion: '2016-03-23',
        actions: [{ dynamoDBv2: { putItem: { tableName: table.tableName }, roleArn: ruleRole.roleArn } }],
      },
    });

    // ---- Read API for the dashboard ------------------------------------------
    const eventsFn = new lambda.Function(this, 'EventsApi', {
      runtime: lambda.Runtime.PYTHON_3_12,
      handler: 'events_api.handler',
      code: lambda.Code.fromAsset(path.join(__dirname, '..', 'lambda'), { exclude: ['__pycache__/**', '*.pyc'] }),
      environment: { TABLE_NAME: table.tableName, DEFAULT_DEVICE: props.thingName },
      timeout: cdk.Duration.seconds(5),
      memorySize: 128,
    });
    table.grantReadData(eventsFn);

    const http = new apigw.HttpApi(this, 'Api', {
      corsPreflight: { allowOrigins: ['*'], allowMethods: [apigw.CorsHttpMethod.GET] },
    });
    http.addRoutes({
      path: '/events',
      methods: [apigw.HttpMethod.GET],
      integration: new integrations.HttpLambdaIntegration('EventsIntegration', eventsFn),
    });

    // Regional HTTPS frontend. CloudFront is blocked pending project verification.
    // Only the frontend Lambda can read this private bucket.
    const website = new s3.Bucket(this, 'Website', {
      encryption: s3.BucketEncryption.S3_MANAGED,
      blockPublicAccess: s3.BlockPublicAccess.BLOCK_ALL,
      objectOwnership: s3.ObjectOwnership.BUCKET_OWNER_ENFORCED,
      enforceSSL: true,
      removalPolicy: cdk.RemovalPolicy.DESTROY,
      autoDeleteObjects: true,
    });
    const frontend = new lambda.Function(this, 'FrontendFiles', {
      runtime: lambda.Runtime.PYTHON_3_12,
      handler: 'frontend.handler',
      code: lambda.Code.fromAsset(path.join(__dirname, '..', 'lambda'), { exclude: ['__pycache__/**', '*.pyc'] }),
      environment: { WEBSITE_BUCKET: website.bucketName },
      memorySize: 256,
      timeout: cdk.Duration.seconds(15),
    });
    website.grantRead(frontend);
    http.addRoutes({ path: '/api/events', methods: [apigw.HttpMethod.GET],
      integration: new integrations.HttpLambdaIntegration('HostedEvents', eventsFn) });
    const pages = new integrations.HttpLambdaIntegration('FrontendIntegration', frontend);
    http.addRoutes({ path: '/', methods: [apigw.HttpMethod.GET, apigw.HttpMethod.HEAD], integration: pages });
    http.addRoutes({ path: '/{proxy+}', methods: [apigw.HttpMethod.GET, apigw.HttpMethod.HEAD], integration: pages });
    new deployment.BucketDeployment(this, 'PublishFrontend', {
      sources: [deployment.Source.asset(path.join(__dirname, '..', '..', '..', 'dashboard'))],
      destinationBucket: website,
      cacheControl: [deployment.CacheControl.maxAge(cdk.Duration.minutes(5))],
    });

    new cdk.CfnOutput(this, 'FrontendUrl', { value: http.apiEndpoint });
    new cdk.CfnOutput(this, 'EventsApiUrl', { value: `${http.apiEndpoint}/events` });
    new cdk.CfnOutput(this, 'TableName', { value: table.tableName });
    new cdk.CfnOutput(this, 'ThingName', { value: thing.thingName! });
    new cdk.CfnOutput(this, 'PolicyName', { value: policy.policyName! });
  }
}
