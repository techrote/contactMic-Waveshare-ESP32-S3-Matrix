"""Installed CLI, no source-path injection or physical dependencies."""
import contextlib
import importlib.metadata
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import contactmic
from contactmic.cli import main, parser
from contactmic.receiver import ReceiverError

class CliTests(unittest.TestCase):
    def run_cli(self,*args,console=False,columns=80):
        exe=shutil.which('contactmic') if console else sys.executable
        self.assertIsNotNone(exe,'Install ./host first')
        command=[exe] if console else [exe,'-I','-m','contactmic']
        env=os.environ.copy();env.pop('PYTHONPATH',None);env['COLUMNS']=str(columns)
        with tempfile.TemporaryDirectory() as cwd:
            return subprocess.run([*command,*args],cwd=cwd,env=env,text=True,capture_output=True,timeout=15)

    def test_distribution_metadata(self):
        dist=importlib.metadata.distribution('contactmic-host')
        self.assertEqual(dist.version,contactmic.__version__)
        self.assertEqual(dist.metadata['Requires-Python'],'>=3.11')
        self.assertTrue(all('extra ==' in x for x in (dist.requires or [])))
        self.assertTrue(any(ep.name=='contactmic' for ep in dist.entry_points))
        self.assertFalse(Path(contactmic.__file__).resolve().is_relative_to(Path(__file__).resolve().parents[1]/'src'))

    def test_help_all_entry_points_widths_and_commands(self):
        for console in (False,True):
            for columns in (32,80):
                result=self.run_cli('--help',console=console,columns=columns)
                self.assertEqual(result.returncode,0,result.stderr);self.assertIn('diagnostics',result.stdout)
        for command in ('discover','audio-devices','monitor','record','diagnostics'):
            self.assertEqual(self.run_cli(command,'--help').returncode,0)

    def test_version(self):
        for console in (False,True):
            result=self.run_cli('--version',console=console)
            self.assertEqual(result.returncode,0);self.assertEqual(result.stdout.strip(),f'contactmic {contactmic.__version__}')

    def test_no_arguments_and_callable(self):
        self.assertIn('usage:',self.run_cli().stdout)
        out=io.StringIO()
        with contextlib.redirect_stdout(out):self.assertEqual(main([]),0)
        self.assertIn('diagnostics',out.getvalue())

    def test_invalid_commands_and_values(self):
        for args in (('bogus',),('--unknown',),('record',),('monitor','--gain','nan'),('monitor','--axis','q')):
            result=self.run_cli(*args);self.assertEqual(result.returncode,2);self.assertNotIn('Traceback',result.stderr)
        err=io.StringIO()
        with contextlib.redirect_stderr(err):self.assertEqual(main(['monitor','--gain','-1']),2)
        self.assertIn('gain',err.getvalue())

    def test_imports_do_not_load_hardware(self):
        code="""
import importlib.abc,sys
class Deny(importlib.abc.MetaPathFinder):
 def find_spec(self,fullname,path=None,target=None):
  if fullname.split('.')[0] in {'bleak','sounddevice','numpy'}: raise AssertionError(fullname)
sys.meta_path.insert(0,Deny())
import contactmic.cli, contactmic.receiver, contactmic.audio
"""
        with tempfile.TemporaryDirectory() as cwd:
            result=subprocess.run([sys.executable,'-I','-c',code],cwd=cwd,text=True,capture_output=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)

    def test_missing_ble_is_actionable(self):
        err=io.StringIO()
        with patch('contactmic.receiver.bleak_api',side_effect=ReceiverError('install BLE extra')),contextlib.redirect_stderr(err):
            self.assertEqual(main(['discover']),2)
        self.assertIn('install BLE extra',err.getvalue());self.assertNotIn('Traceback',err.getvalue())
