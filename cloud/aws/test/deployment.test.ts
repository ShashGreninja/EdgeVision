import { test } from 'node:test';
import * as cdk from 'aws-cdk-lib';
import { Template, Match } from 'aws-cdk-lib/assertions';
import { CachePolicy } from 'aws-cdk-lib/aws-cloudfront';
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
  template.hasResourceProperties('AWS::CloudFront::OriginAccessControl', {
    OriginAccessControlConfig: Match.objectLike({ SigningBehavior: 'always', SigningProtocol: 'sigv4' }),
  });
});
test('HTTPS website serves the landing page and never caches API requests', () => {
  template.hasResourceProperties('AWS::CloudFront::Distribution', {
    DistributionConfig: Match.objectLike({
      DefaultRootObject: 'index.html',
      DefaultCacheBehavior: Match.objectLike({ ViewerProtocolPolicy: 'redirect-to-https', ResponseHeadersPolicyId: Match.anyValue() }),
      CacheBehaviors: Match.arrayWith([Match.objectLike({
        PathPattern: 'api/*', ViewerProtocolPolicy: 'https-only',
        CachePolicyId: CachePolicy.CACHING_DISABLED.cachePolicyId,
        FunctionAssociations: Match.arrayWith([Match.objectLike({ EventType: 'viewer-request' })]),
      })]),
    }),
  });
  template.hasOutput('FrontendUrl', { Value: Match.anyValue() });
});
