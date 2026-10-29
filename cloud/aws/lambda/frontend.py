"""Serve the public demo files from a private, same-Region S3 bucket."""
import base64
import os
import re
from urllib.parse import unquote

import boto3
from botocore.exceptions import ClientError

s3 = boto3.client('s3')
TYPES = {'.html': 'text/html; charset=utf-8', '.css': 'text/css', '.js': 'text/javascript',
         '.json': 'application/json', '.jpg': 'image/jpeg', '.mp4': 'video/mp4',
         '.woff2': 'font/woff2', '.pdf': 'application/pdf', '.txt': 'text/plain',
         '.docx': 'application/vnd.openxmlformats-officedocument.wordprocessingml.document'}
SECURITY = {'X-Content-Type-Options': 'nosniff', 'X-Frame-Options': 'DENY',
            'Referrer-Policy': 'strict-origin-when-cross-origin',
            'Strict-Transport-Security': 'max-age=31536000'}


def handler(event, context):
    key = unquote(event.get('rawPath', '/')).lstrip('/') or 'index.html'
    content_type = TYPES.get(os.path.splitext(key)[1])
    if not content_type or any(part in ('.', '..') for part in key.split('/')) or '\\' in key:
        return {'statusCode': 404, 'headers': SECURITY, 'body': 'Not found'}
    incoming = {k.lower(): v for k, v in (event.get('headers') or {}).items()}
    request = {'Bucket': os.environ['WEBSITE_BUCKET'], 'Key': key}
    if 'range' in incoming:
        if not re.fullmatch(r'bytes=(?:\d+-\d*|-\d+)', incoming['range']):
            return {'statusCode': 416, 'headers': SECURITY, 'body': 'Invalid range'}
        request['Range'] = incoming['range']
    try:
        result = s3.get_object(**request)
    except ClientError as error:
        code = error.response['Error']['Code']
        if code in ('NoSuchKey', 'AccessDenied', 'InvalidRange'):
            return {'statusCode': 416 if code == 'InvalidRange' else 404, 'headers': SECURITY, 'body': 'Not found'}
        raise
    with result['Body'] as stream:
        if result['ContentLength'] > 4_000_000:
            return {'statusCode': 413, 'headers': SECURITY, 'body': 'Asset exceeds demo hosting limit'}
        headers = {**SECURITY, 'Content-Type': content_type, 'Cache-Control': 'public, max-age=300',
                   'ETag': result['ETag'], 'Accept-Ranges': 'bytes', 'Content-Length': str(result['ContentLength'])}
        if incoming.get('if-none-match') == result['ETag'] and 'Range' not in request:
            return {'statusCode': 304, 'headers': {k: v for k, v in headers.items() if k != 'Content-Length'}, 'body': ''}
        if 'ContentRange' in result:
            headers['Content-Range'] = result['ContentRange']
        method = event.get('requestContext', {}).get('http', {}).get('method', 'GET')
        body = b'' if method == 'HEAD' else stream.read()
        return {'statusCode': 206 if 'ContentRange' in result else 200, 'headers': headers,
                'isBase64Encoded': True, 'body': base64.b64encode(body).decode('ascii')}
