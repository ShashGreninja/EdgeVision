#!/usr/bin/env node
import * as cdk from 'aws-cdk-lib';
import { EdgeVisionStack } from '../lib/edgevision-stack';

const app = new cdk.App();

new EdgeVisionStack(app, 'EdgeVision', {
  thingName: app.node.tryGetContext('thingName') ?? 'edge-cam-01',
  env: { account: process.env.CDK_DEFAULT_ACCOUNT, region: process.env.CDK_DEFAULT_REGION },
});
