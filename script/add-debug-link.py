#!/usr/bin/env python
from __future__ import print_function
import argparse
import os
import sys

from lib.config import LINUX_BINARIES, PLATFORM
from lib.util import execute, get_objcopy_path, get_out_dir

def add_debug_link_into_binaries(directory, target_cpu, debug_dir):
  for binary in LINUX_BINARIES:
    binary_path = os.path.join(directory, binary)
    if os.path.isfile(binary_path):
      add_debug_link_into_binary(binary_path, target_cpu, debug_dir)

def add_debug_link_into_binary(binary_path, target_cpu, debug_dir):
  try:
    objcopy = get_objcopy_path(target_cpu)
  except:
    if PLATFORM == 'linux' and (target_cpu == 'x86' or target_cpu == 'arm' or
       target_cpu == 'arm64'):
      # Skip because no objcopy binary on the given target.
      return
    raise
  debug_name = get_debug_name(binary_path)
  debug_path = os.path.join(debug_dir, debug_name)
  cmd = [objcopy, '--add-gnu-debuglink=' + debug_path, binary_path]
  execute(cmd)

def get_debug_name(binary_path):
  return os.path.basename(binary_path) + '.debug'

def main():
  args = parse_args()
  if args.file:
    add_debug_link_into_binary(args.file, args.target_cpu, args.debug_dir)
  else:
    add_debug_link_into_binaries(args.directory, args.target_cpu,
                                 args.debug_dir)

def parse_args():
  parser = argparse.ArgumentParser(description='Add debug link to binaries')
  parser.add_argument('-d', '--directory',
                      help='Path to the dir that contains files to add links',
                      default=get_out_dir(),
                      required=False)
  parser.add_argument('-f', '--file',
                      help='Path to a specific file to add debug link',
                      required=False)
  parser.add_argument('-s', '--debug-dir',
                      help='Path to the dir that contain the debugs',
                      default=None,
                      required=True)
  parser.add_argument('-v', '--verbose',
                      action='store_true',
                      help='Prints the output of the subprocesses')
  parser.add_argument('--target-cpu',
                      default='',
                      required=False,
                      help='Target cpu of binaries to add debug link')

  return parser.parse_args()

if __name__ == '__main__':
  sys.exit(main())
