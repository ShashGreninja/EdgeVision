"""GET /events?device=<thing>&limit=<n>: latest events for one camera, newest first.

Returns the same shape as cloud/local/cloud_stub.py, so the dashboard works
against either backend: {"events": [{...event fields..., "device", "received_at"}]}.
"""

import json
import os
from decimal import Decimal

import boto3
from boto3.dynamodb.conditions import Key

_table = boto3.resource("dynamodb").Table(os.environ["TABLE_NAME"])


def _plain(value):
    """DynamoDB returns numbers as Decimal; JSON wants int/float."""
    if isinstance(value, Decimal):
        return int(value) if value == value.to_integral_value() else float(value)
    if isinstance(value, list):
        return [_plain(v) for v in value]
    if isinstance(value, dict):
        return {k: _plain(v) for k, v in value.items()}
    return value


def handler(event, context):
    params = event.get("queryStringParameters") or {}
    device = params.get("device") or os.environ.get("DEFAULT_DEVICE", "edge-cam-01")
    try:
        limit = max(1, min(200, int(params.get("limit", "50"))))
    except ValueError:
        limit = 50

    result = _table.query(
        KeyConditionExpression=Key("device").eq(device),
        ScanIndexForward=False,  # Newest first.
        Limit=limit,
    )
    events = [_plain(item) for item in result.get("Items", [])]
    for e in events:
        e.pop("expires_at", None)

    return {
        "statusCode": 200,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps({"events": events}),
    }
