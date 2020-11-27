#!/usr/bin/env python
"""Program to compute exact birth anniversary"""

from __future__ import print_function

import os
import sys
import time

TIME_FMT = '%x %X %Z'

TROPICAL_YEAR = 365.242199 * 86400


def str2tstruc(string):
  """Parse a string into a struct_time object, and "time given" flag."""
  for fmt, havetime, year2d in dtformats():
    try:
      return time.strptime(string, fmt), havetime, year2d
    except ValueError:
      pass
  raise ValueError("'%s' is not a recognized date/time" % string)


def dateformats():
  """Yield all valid date formats."""
  years = ['%Y', '%y']
  tmonths = ['%b', '%B']
  for year in years:
    yield '%s-%%m-%%d' % year  # ISO 8601 numeric
    yield '%%m/%%d/%s' % year  # American numeric
    yield '%%d.%%m.%s' % year  # European numeric
  # Various orderings with textual month
  for year in years:
    for month in tmonths:
      yield '%%d %s %s' % (month, year)
      yield '%s %%d %s' % (month, year)
      yield '%s %s %%d' % (year, month)
      yield '%s %%d %s' % (year, month)


def timeformats():
  """Return list of valid time formats."""
  return ['%I%p', '%I %p', '%I:%M%p', '%I:%M %p', '%H:%M']


def dtformats():
  """Yield all valid date[/time] formats, "time given" and "2-dig yr" flags."""
  for dfmt in dateformats():
    year2d = '%y' in dfmt
    yield dfmt, False, year2d  # Date only
    for tfmt in timeformats():
      yield '%s %s' % (dfmt, tfmt), True, year2d  # Date then time
      yield '%s %s' % (tfmt, dfmt), True, year2d  # Time then date
  yield '%Y-%m-%dT%H:%M', True, False  # ISO 8601 with time


def str2time(string, tzone=None):
  """Parse a string into a timestamp, and "time given" flag."""
  tstruc, havetime, year2d = str2tstruc(string)
  if year2d:
    nstruc = time.localtime(time.time())
    baseyear = nstruc.tm_year - 100
    tlist = list(tstruc)
    yearidx = 0
    tlist[yearidx] = baseyear + (tlist[yearidx] - baseyear) % 100
    tstruc = time.struct_time(tlist)
  if not tzone:
    return time.mktime(tstruc), havetime
  savetz = os.environ.get('TZ')
  os.environ['TZ'] = tzone
  time.tzset()
  tvalue = time.mktime(tstruc)
  if savetz is None:
    del os.environ['TZ']
  else:
    os.environ['TZ'] = savetz
  time.tzset()
  return tvalue, havetime


def main(argv):
  """main function"""
  argv0 = os.path.basename(argv[0])
  argc = len(argv)
  if argc >= 4 and argv[1] == '-z':
    tzone = argv[2]
    args = argv[3:]
  elif argc >= 2:
    tzone = None
    args = argv[1:]
  else:
    print('Usage is %s [-z timezone] date/time' % argv0, file=sys.stderr)
    return 2
  try:
    birth, havetime = str2time(' '.join(args), tzone)
  except ValueError as exc:
    print(repr(exc), file=sys.stderr)
    return 1
  now = time.time()
  age = (now - birth) / TROPICAL_YEAR
  years = int(round(age))
  anniv = birth + years * TROPICAL_YEAR
  tstruc = time.localtime(anniv)
  tstr = time.strftime(TIME_FMT, tstruc)
  if havetime:
    print('%d-year anniversary is at %s' % (years, tstr))
  else:
    tstr2 = time.strftime(TIME_FMT, time.localtime(anniv + 86399))
    print('%d-year anniversary is between %s and %s' % (years, tstr, tstr2))
  return 0


if __name__ == '__main__':  # pragma no cover
  sys.exit(main(sys.argv))