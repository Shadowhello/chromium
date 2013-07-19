# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Defines TestPackageExecutable to help run stand-alone executables."""

import logging
import os
import shutil
import sys
import tempfile

from pylib import cmd_helper
from pylib import constants
from pylib import pexpect

from test_package import TestPackage


class TestPackageExecutable(TestPackage):
  """A helper class for running stand-alone executables."""

  _TEST_RUNNER_RET_VAL_FILE = 'gtest_retval'

  def __init__(self, adb, device, suite_path_full, tool, symbols_dir=None):
    """
    Args:
      adb: ADB interface the tests are using.
      device: Device to run the tests.
      suite_path_full: Absolute path to a specific test suite to run,
          empty to run all.
      tool: Name of the Valgrind tool.
      symbols_dir: Directory to put the stripped binaries.
    """
    TestPackage.__init__(self, adb, device, suite_path_full, tool)
    self.symbols_dir = symbols_dir

  def _GetTestSuiteBaseName(self):
    """Returns the base name of the test suite."""
    return os.path.basename(self.suite_path)

  #override
  def GetGTestReturnCode(self):
    ret = None
    ret_code = 1  # Assume failure if we can't find it
    ret_code_file = tempfile.NamedTemporaryFile()
    try:
      if not self.adb.Adb().Pull(
          constants.TEST_EXECUTABLE_DIR + '/' +
          TestPackageExecutable._TEST_RUNNER_RET_VAL_FILE,
          ret_code_file.name):
        logging.critical('Unable to pull gtest ret val file %s',
                         ret_code_file.name)
        raise ValueError
      ret_code = file(ret_code_file.name).read()
      ret = int(ret_code)
    except ValueError:
      logging.critical('Error reading gtest ret val file %s [%s]',
                       ret_code_file.name, ret_code)
      ret = 1
    return ret

  def _AddNativeCoverageExports(self):
    # export GCOV_PREFIX set the path for native coverage results
    # export GCOV_PREFIX_STRIP indicates how many initial directory
    #                          names to strip off the hardwired absolute paths.
    #                          This value is calculated in buildbot.sh and
    #                          depends on where the tree is built.
    # Ex: /usr/local/google/code/chrome will become
    #     /code/chrome if GCOV_PREFIX_STRIP=3
    try:
      depth = os.environ['NATIVE_COVERAGE_DEPTH_STRIP']
    except KeyError:
      logging.info('NATIVE_COVERAGE_DEPTH_STRIP is not defined: '
                   'No native coverage.')
      return ''
    export_string = ('export GCOV_PREFIX="%s/gcov"\n' %
                     self.adb.GetExternalStorage())
    export_string += 'export GCOV_PREFIX_STRIP=%s\n' % depth
    return export_string

  #override
  def ClearApplicationState(self):
    self.adb.KillAllBlocking(self.suite_basename, 30)

  #override
  def CreateCommandLineFileOnDevice(self, test_filter, test_arguments):
    tool_wrapper = self.tool.GetTestWrapper()
    sh_script_file = tempfile.NamedTemporaryFile()
    # We need to capture the exit status from the script since adb shell won't
    # propagate to us.
    sh_script_file.write('cd %s\n'
                         '%s'
                         '%s %s/%s --gtest_filter=%s %s\n'
                         'echo $? > %s' %
                         (constants.TEST_EXECUTABLE_DIR,
                          self._AddNativeCoverageExports(),
                          tool_wrapper, constants.TEST_EXECUTABLE_DIR,
                          self.suite_basename,
                          test_filter, test_arguments,
                          TestPackageExecutable._TEST_RUNNER_RET_VAL_FILE))
    sh_script_file.flush()
    cmd_helper.RunCmd(['chmod', '+x', sh_script_file.name])
    self.adb.PushIfNeeded(
        sh_script_file.name,
        constants.TEST_EXECUTABLE_DIR + '/chrome_test_runner.sh')
    logging.info('Conents of the test runner script: ')
    for line in open(sh_script_file.name).readlines():
      logging.info('  ' + line.rstrip())

  #override
  def GetAllTests(self):
    all_tests = self.adb.RunShellCommand(
        '%s %s/%s --gtest_list_tests' %
        (self.tool.GetTestWrapper(),
         constants.TEST_EXECUTABLE_DIR,
         self.suite_basename))
    return self._ParseGTestListTests(all_tests)

  #override
  def SpawnTestProcess(self):
    args = ['adb', '-s', self.device, 'shell', 'sh',
            constants.TEST_EXECUTABLE_DIR + '/chrome_test_runner.sh']
    logging.info(args)
    return pexpect.spawn(args[0], args[1:], logfile=sys.stdout)

  #override
  def Install(self):
    if self.tool.NeedsDebugInfo():
      target_name = self.suite_path
    else:
      target_name = self.suite_path + '_' + self.device + '_stripped'
      should_strip = True
      if os.path.isfile(target_name):
        logging.info('Found target file %s' % target_name)
        target_mtime = os.stat(target_name).st_mtime
        source_mtime = os.stat(self.suite_path).st_mtime
        if target_mtime > source_mtime:
          logging.info('Target mtime (%d) is newer than source (%d), assuming '
                       'no change.' % (target_mtime, source_mtime))
          should_strip = False

      if should_strip:
        logging.info('Did not find up-to-date stripped binary. Generating a '
                     'new one (%s).' % target_name)
        # Whenever we generate a stripped binary, copy to the symbols dir. If we
        # aren't stripping a new binary, assume it's there.
        if self.symbols_dir:
          if not os.path.exists(self.symbols_dir):
            os.makedirs(self.symbols_dir)
          shutil.copy(self.suite_path, self.symbols_dir)
        strip = os.environ['STRIP']
        cmd_helper.RunCmd([strip, self.suite_path, '-o', target_name])
    test_binary = constants.TEST_EXECUTABLE_DIR + '/' + self.suite_basename
    self.adb.PushIfNeeded(target_name, test_binary)
