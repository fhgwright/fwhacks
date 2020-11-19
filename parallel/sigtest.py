#!/usr/bin/env python
"""Program to test signal reception."""

# Compatible with Python >=2.6

from __future__ import print_function

import os
import signal
import sys
import time

SLEEP_TIME = 15

SIG_LIST = [
    'SIGINT',
    'SIGTERM',
    'SIGHUP',
    'SIGQUIT',
    'SIGUSR1',
    'SIGUSR2',
    ]
SIG_MAP = dict(zip([getattr(signal, x) for x in SIG_LIST], SIG_LIST))
SIG_WAIT = set([getattr(signal, x) for x in ['SIGUSR1', 'SIGUSR2']])


class Error(Exception):
  """Base class for all locally defined exceptions."""


class SignalInterrupt(Error):
  """Exception for interrupts via signals."""
  # Python >=3.5 retries EINTR, so we need an exception to break out of
  # poll, etc.  Used instead of InterruptedError, which doesn't exist
  # in all Python versions.
  # However, one has to be careful to raise this exception sparingly.
  # Not only does one want to avoid using it outside the intended calls,
  # but some OS/Python combinations seem to have a race with the try/except
  # setup such that the exception may miss being caught by the try/except.
  # In this case, the backtrace actually shows the exception as occuring
  # within the try/except that should catch it.


class Sleeper(object):
  """Safely interruptible sleep."""
  # This ensures that the signal exception is limited to the actual sleep.
  MINI_SLEEP = 0.0001
  sigs_rcvd = set()
  interruptible = False

  @classmethod
  def sleep(cls, secs):
    """Sleep with signal interrupt allowed."""
    cur_sigs = cls.sigs_rcvd
    try:
      # Try to avoid race with exception setup
      time.sleep(cls.MINI_SLEEP)  # Very short sleep before interruptible
      cls.interruptible = True
      # Avoid a race with a signal immediately preceding "interruptible"
      if cur_sigs == cls.sigs_rcvd:
        time.sleep(secs - cls.MINI_SLEEP)
      cls.interruptible = False
    except SignalInterrupt as intexc:
      _ = intexc  # Only for debugging
      cls.interruptible = False

  @classmethod
  def Signal(cls, signum):
    """Arm signal handler for given signal."""
    signal.signal(signum, cls._SignalHandler)

  @classmethod
  def _SignalHandler(cls, signum, stack):
    """Signal handler to record signal.

    Args:
      signum: number of signal
      stack: stack frame from signal (unused)
    """
    _ = stack
    cls.sigs_rcvd |= set([signum])
    if cls.interruptible:
      raise SignalInterrupt(signum)


def main(argv):
  """main function."""
  _ = argv
  for sig in SIG_MAP:
    Sleeper.Signal(sig)
  sigs_reported = set()
  while True:
    print('Process %d sleeping for %d seconds; awaiting possible signal.'
          % (os.getpid(), SLEEP_TIME))
    sys.stdout.flush()
    Sleeper.sleep(SLEEP_TIME)
    new_sigs = Sleeper.sigs_rcvd - sigs_reported
    for sig in new_sigs:
      print('Received signal %d (%s).' % (sig, SIG_MAP.get(sig, '?')))
    sigs_reported |= new_sigs
    if Sleeper.sigs_rcvd - SIG_WAIT or not new_sigs:
      break
  if Sleeper.sigs_rcvd:
    if not new_sigs:
      print('No new signals received')
    return 1
  print('No signals received.')
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv))
