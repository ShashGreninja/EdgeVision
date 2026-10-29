import base64
import importlib.util
import io
import os
from pathlib import Path
import unittest
from unittest.mock import patch, MagicMock
from botocore.exceptions import ClientError

os.environ['WEBSITE_BUCKET'] = 'private-site'
with patch('boto3.client') as create:
    spec = importlib.util.spec_from_file_location('frontend', Path(__file__).parents[1] / 'lambda' / 'frontend.py')
    frontend = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(frontend)

class FrontendTests(unittest.TestCase):
    def setUp(self):
        frontend.s3 = MagicMock()
        frontend.s3.get_object.return_value = {'Body': io.BytesIO(b'video bytes'), 'ContentLength': 11, 'ETag': '"one"'}

    def test_home_and_binary_assets(self):
        result = frontend.handler({'rawPath': '/'}, None)
        frontend.s3.get_object.assert_called_once_with(Bucket='private-site', Key='index.html')
        self.assertEqual(base64.b64decode(result['body']), b'video bytes')
        self.assertEqual(result['headers']['Content-Type'], 'text/html; charset=utf-8')
        self.assertEqual(result['headers']['X-Content-Type-Options'], 'nosniff')

    def test_range_requests_support_video_seeking(self):
        frontend.s3.get_object.return_value['ContentRange'] = 'bytes 0-10/20'
        result = frontend.handler({'rawPath': '/assets/visit.mp4', 'headers': {'range': 'bytes=0-10'}}, None)
        self.assertEqual(result['statusCode'], 206)
        self.assertEqual(result['headers']['Content-Type'], 'video/mp4')
        self.assertEqual(frontend.s3.get_object.call_args.kwargs['Range'], 'bytes=0-10')

    def test_traversal_and_non_site_files_are_rejected(self):
        for path in ('/../private.key', '/%2e%2e/app.html', '/certs/private.pem', '/assets/unknown.exe'):
            self.assertEqual(frontend.handler({'rawPath': path}, None)['statusCode'], 404)
        frontend.s3.get_object.assert_not_called()

    def test_missing_asset_is_not_success(self):
        frontend.s3.get_object.side_effect = ClientError({'Error': {'Code': 'NoSuchKey'}}, 'GetObject')
        self.assertEqual(frontend.handler({'rawPath': '/missing.html'}, None)['statusCode'], 404)

if __name__ == '__main__':
    unittest.main()
