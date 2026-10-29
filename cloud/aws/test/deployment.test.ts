import { test } from 'node:test';
import * as cdk from 'aws-cdk-lib';
import { Template, Match } from 'aws-cdk-lib/assertions';
import { EdgeVisionStack } from '../lib/edgevision-stack';

const app = new cdk.App();
const stack = new EdgeVisionStack(app, 'TestEdgeVision', {
  thingName: 'edge-cam-01', env: { account: '123456789012', region: 'ap-southeast-2' },
});
const template = Template.fromStack(stack);

test('website origin is encrypted and blocks direct public access', () => {
  template.hasResourceProperties('AWS::S3::Bucket', {
    BucketEncryption: { ServerSideEncryptionConfiguration: [{ ServerSideEncryptionByDefault: { SSEAlgorithm: 'AES256' } }] },
    PublicAccessBlockConfiguration: { BlockPublicAcls: true, BlockPublicPolicy: true, IgnorePublicAcls: true, RestrictPublicBuckets: true },
  });
});
test('regional HTTPS routes serve pages and live events separately', () => {
  template.resourceCountIs('AWS::CloudFront::Distribution', 0);
  for (const route of ['GET /', 'GET /{proxy+}', 'GET /api/events', 'GET /events']) {
    template.hasResourceProperties('AWS::ApiGatewayV2::Route', { RouteKey: route });
  }
  template.hasResourceProperties('AWS::Lambda::Function', { Handler: 'frontend.handler', Environment: { Variables: { WEBSITE_BUCKET: Match.anyValue() } } });
  template.hasOutput('FrontendUrl', { Value: Match.anyValue() });
});
