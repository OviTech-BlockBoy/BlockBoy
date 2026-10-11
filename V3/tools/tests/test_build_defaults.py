"""Check signing-default generation without building or touching partitions.csv."""
import ast
import os
from pathlib import Path
import re
import unittest
from unittest.mock import Mock, mock_open

ROOT = Path(__file__).resolve().parents[2]


class SigningDefaultsTests(unittest.TestCase):
    def prepare(self, key):
        tree = ast.parse((ROOT / 'rg_tool.py').read_text(encoding='utf-8'))
        fn = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'build_app')
        host_os = Mock()
        host_os.path = os.path
        host_os.getcwd.return_value = str(ROOT)
        output = mock_open()
        namespace = dict(os=host_os, re=re, SIGNING_KEY=key, IDF_PY='idf.py',
                         PROJECT_VER='3.1.1', open=output, run=Mock(), print=Mock())
        exec(compile(ast.Module(body=[fn], type_ignores=[]), 'rg_tool.py', 'exec'), namespace)
        return namespace, host_os, output

    def test_key_default_is_last(self):
        namespace, host_os, output = self.prepare('private/example.pem')
        namespace['build_app']('gbsp', '0v1Tech-BlockBoy-N16R8')
        defaults = host_os.putenv.call_args.args[1].split(';')
        self.assertTrue(defaults[-1].endswith('signing.defaults'))
        expected = 'CONFIG_SECURE_BOOT_SIGNING_KEY="' + os.path.abspath('private/example.pem').replace('\\', '/') + '"\n'
        output().write.assert_any_call(expected)
        self.assertEqual(namespace['run'].call_count, 1)

    def test_no_key_does_not_generate_signing_override(self):
        namespace, host_os, output = self.prepare('')
        namespace['build_app']('gbsp', '0v1Tech-BlockBoy-N16R8')
        self.assertNotIn('signing.defaults', host_os.putenv.call_args.args[1])
        self.assertEqual(output.call_count, 1)  # temporary partition table only

    def test_reject_config_injection(self):
        namespace, _, output = self.prepare('bad"key.pem')
        with self.assertRaises(ValueError):
            namespace['build_app']('gbsp', '0v1Tech-BlockBoy-N16R8')
        output.assert_not_called()
        namespace['run'].assert_not_called()


if __name__ == '__main__':
    unittest.main()
