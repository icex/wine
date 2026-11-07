#!/usr/bin/env python3
"""Compile the actual correction helper and test its narrowly scoped behavior."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


class RefreshTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='wine-desktop-refresh-')
        root = Path(cls.directory.name)
        header = Path(__file__).resolve().parents[2] / 'dlls/winex11.drv/desktop_refresh.h'
        include = str(header).replace('\\', '\\\\').replace('"', '\\"')
        source = root / 'probe.c'
        source.write_text('#include "' + include + '"\n'
                          'unsigned int probe(unsigned int w,unsigned int h,unsigned long c)\n'
                          '{return desktop_refresh(w,h,c);}\n')
        library = root / 'probe.so'
        subprocess.run(['cc', '-std=c89', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(source), '-o', str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.library.probe.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong]
        cls.library.probe.restype = ctypes.c_uint

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def rate(self, setting, width=7680, height=2160, clock=0xffffffff):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop('FERESTRE_DESKTOP_MODE', None)
            if setting is not None:
                os.environ['FERESTRE_DESKTOP_MODE'] = setting
            return self.library.probe(width, height, clock)

    def test_clamped_8k_240_mode_uses_the_compositor_rate(self):
        self.assertEqual(self.rate('7680x2160@240.000000'), 240)

    def test_fractional_rates_round_like_normal_randr_modes(self):
        self.assertEqual(self.rate('7680x2160@239.975000'), 240)

    def test_normal_dot_clocks_are_untouched(self):
        for clock in (0, 148500000, 1232250000, 0xfffffffe):
            self.assertEqual(self.rate('7680x2160@240', clock=clock), 0)

    def test_other_resolutions_are_untouched(self):
        self.assertEqual(self.rate('7680x2160@240', width=3840), 0)
        self.assertEqual(self.rate('7680x2160@240', height=1080), 0)

    def test_missing_metadata_is_untouched(self):
        self.assertEqual(self.rate(None), 0)

    def test_bad_metadata_cannot_change_the_mode(self):
        for value in ('', '7680', '7680x2160', '7680x2160@', '7680x2160@0',
                      '7680x2160@1001', '7680x2160@nan', '7680x2160@inf',
                      '7680x2160@1e999', '7680x2160@240garbage',
                      '-1x2160@240', '999999999999999999999x2160@240'):
            with self.subTest(value=value):
                self.assertEqual(self.rate(value), 0)


if __name__ == '__main__':
    unittest.main()
