#!/usr/bin/env python
"""Program to run multiple instances of a command in parallel."""

# Compatible with Python >=2.6

from __future__ import print_function

import argparse
import errno
import fcntl
import math
import os
import select
import shlex
import signal
import sys
import time

try:
  import subprocess32 as subprocess
except ImportError:
  import subprocess

SUBST_HELP = [
    '  Default (-a or positional) substitution options:',
    '    %P full path to <item> (i.e., verbatim <item>)',
    '    %B base name of <item> (full path w/o extension)',
    '    %D directory of <item>',
    '    %F file name of <item> (w/o directory)',
    '    %N name of <item> (base name w/o directory)',
    '    %E extension of <item>',
    '',
    '  Argument file (-f) substitution options:',
    '    %0 first element on line',
    '    %1 second element on line',
    '    %2 third element on line',
    '    %3 fourth element on line',
    '    %4 fifth element on line',
    '    %5 sixth element on line',
    '    %6 seventh element on line',
    '    %7 eighth element on line',
    '',
    '  Machine list (-m) substitution options:',
    '    %M machine name',
]


SIG_LIST = [
    'SIGINT',
    'SIGTERM',
    'SIGHUP',
    'SIGQUIT',
    'SIGUSR1',
    'SIGUSR2',
    ]
SIG_MAP = dict(zip([getattr(signal, _x) for _x in SIG_LIST], SIG_LIST))
SIG_WAIT = set([getattr(signal, _x) for _x in ['SIGUSR1', 'SIGUSR2']])


def Eprint(message):
  """Print message to stderr."""
  print(message, file=sys.stderr)
  sys.stderr.flush()


class Error(Exception):
  """Base class for all locally defined exceptions."""


class UnknownInterpolation(Error):
  """Unknown interpolation character."""


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


class PollCompat(object):
  """Class for fallback select.poll object."""
  # Required imports (other than select): errno, os
  POLLIN = 1 << 0
  POLLPRI = 1 << 1
  POLLOUT = 1 << 2
  # Order of _POLLBITS must match poll.select order
  _POLLBITS = [POLLIN, POLLOUT, POLLPRI]
  _POLLALL = sum(_POLLBITS)

  def __init__(self):
    self.registry = {}
    self.rset = set()
    self.wset = set()
    self.xset = set()
    # Order of self.setlist must match _POLLBITS and poll.select
    self.setlist = [self.rset, self.wset, self.xset]
    self.setmap = zip(self._POLLBITS, self.setlist)
    self.lists = [list(x) for x in self.setlist]

  def register(self, xfd, eventmask=_POLLALL):
    """Register an fd with the poller."""
    self.registry[xfd] = eventmask
    fset = set([xfd])
    for bit, curset in self.setmap:
      if eventmask & bit:
        curset |= fset
    self.lists = [list(x) for x in self.setlist]

  def modify(self, xfd, eventmask):
    """Modify an already registered fd."""
    try:
      self.unregister(xfd)
    except KeyError:
      raise IOError(errno.ENOENT, os.strerror(errno.ENOENT))
    self.register(xfd, eventmask)

  def unregister(self, xfd):
    """Unregister a previously registered fd."""
    mask = self.registry[xfd]
    fset = set([xfd])
    for bit, curset in self.setmap:
      if mask & bit:
        curset -= fset
    del self.registry[xfd]
    self.lists = [list(x) for x in self.setlist]

  def poll(self, timeout=None):
    """Poll the registered set."""
    args = self.lists + [timeout / 1000.0] if timeout else []
    found = select.select(*args)
    resdict = {}
    for ready, bit in zip(found, self._POLLBITS):
      for xfd in ready:
        resdict.setdefault(xfd, 0)
        resdict[xfd] |= bit
    return list(resdict.items())


class Poller(object):
  """Safely interruptible select.poll()."""
  # This ensures that the signal exception is limited to the actual poll.
  sigs_rcvd = set()
  interruptible = False

  try:
    POLL_CLS = select.poll
    POLLIN = select.POLLIN
    POLLPRI = select.POLLPRI
    POLLOUT = select.POLLOUT
    POLL_FIX = False
  except AttributeError:
    POLL_CLS = PollCompat
    POLLIN = PollCompat.POLLIN
    POLLPRI = PollCompat.POLLPRI
    POLLOUT = PollCompat.POLLOUT
    POLL_FIX = True

  def __init__(self):
    self.poller = poller = self.POLL_CLS()
    self.register = poller.register
    self.modify = poller.modify
    self.unregister = poller.unregister

  def poll(self, timeout=None):
    """Poll with signal interrupt allowed."""
    cls = type(self)
    cur_sigs = cls.sigs_rcvd
    try:
      # Try to avoid race with exception setup
      result = self.poller.poll(1)  # Very short timeout before interruptible
      if result:
        return result
      cls.interruptible = True
      # Avoid a race with a signal immediately preceding "interruptible"
      if cur_sigs == cls.sigs_rcvd:
        result = self.poller.poll(timeout)
      else:
        result = []
      cls.interruptible = False
    except SignalInterrupt as intexc:
      _ = intexc  # Only for debugging
      cls.interruptible = False
      result = []
    return result

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


def Interpolate(text, value, mapdict):
  """Interpolate a string using versions of a value."""
  result = []
  pos = 0
  while True:
    loc = text.find('%', pos)
    if loc < 0 or loc >= len(text) - 1:
      break
    result += [text[pos:loc]]
    char = text[loc + 1]
    pos = loc + 2
    if char == '%':
      result += ['%']
      break
    func = mapdict.get(char)
    if not func:
      raise UnknownInterpolation('%%%s' % char)
    try:
      result += [func(value)]
    except IndexError:
      pass
  result += [text[pos:]]
  return ''.join(result)


NULL_MAP = {}

MACH_MAP = {
    'M': str,
    }

PATH_MAP = {
    'P': str,
    'B': lambda x: os.path.splitext(x)[0],
    'D': os.path.dirname,
    'F': os.path.basename,
    'N': lambda x: os.path.splitext(os.path.basename(x))[0],
    'E': lambda x: os.path.splitext(os.path.basename(x))[1],
    }

ARG_MAP = {
    '0': lambda x: x.split()[0],
    '1': lambda x: x.split()[1],
    '2': lambda x: x.split()[2],
    '3': lambda x: x.split()[3],
    '4': lambda x: x.split()[4],
    '5': lambda x: x.split()[5],
    '6': lambda x: x.split()[6],
    '7': lambda x: x.split()[7],
    }


def TimeStr(tstamp):
  """Get string version of timestamp, with milliseconds."""
  tsplit = math.modf(tstamp)
  tsint = time.strftime('%H:%M:%S', time.localtime(int(tsplit[1])))
  tsfrac = ('%.3f' % tsplit[0]).split('.')[-1]
  return '.'.join([tsint, tsfrac])


def ElapsedStr(delta):
  """Get string version of elapsed time."""
  tsplit = math.modf(delta)
  mins, secs = divmod(int(tsplit[1]), 60)
  secf = secs + tsplit[0]
  if not mins:
    return '%.3fs' % secf
  hours, mins = divmod(mins, 60)
  if not hours:
    return '%02d:%06.3f' % (mins, secf)
  return '%02d:%02d:%06.3f' % (hours, mins, secf)


class Line(object):  # pylint: disable=too-few-public-methods
  """Class for line of output."""
  __slots__ = ('iserr', 'time', 'text')

  def __init__(self, iserr, text, tstamp=None):
    self.iserr = iserr
    self.time = tstamp or time.time()
    self.text = text

  def Print(self, name=None, tstamp=False, where=(sys.stdout, sys.stderr)):
    """Print line with optional name and/or timestamp."""
    print(self.Format(self.text, self.iserr, name, tstamp and self.time),
          file=where[self.iserr])

  @staticmethod
  def Format(text, iserr=False, name=None, tstamp=None):
    """Format output line with optional name and/or timestamp."""
    if not isinstance(text, str):
      text = text.decode(encoding='latin-1')
    sep = '::' if iserr else ':'
    if tstamp:
      tstext = TimeStr(tstamp)
      if name:
        return '%s @%s%s %s' % (name, tstext, sep, text)
      return '%s%s %s' % (tstext, sep, text)
    if name:
      return '%s%s %s' % (name, sep, text)
    return text


class Process(object):  # pylint: disable=too-many-instance-attributes
  """Class for subprocesses."""
  KILL_DELAY = 7
  KILL_TIMEOUT = 3

  # Assumes the command won't expect input via stdin
  def __init__(self, name, args, shell=False):
    # pylint: disable=too-many-arguments
    self.name = name
    self.realname = name
    if not name:
      self.name = '(command)'
    # Dummy input pipe
    self.input = subprocess.PIPE
    # Need piped output for ^C to be delivered here
    self.output = subprocess.PIPE
    self.errout = subprocess.PIPE
    self.shell = shell
    if shell:
      self.args = ShellStr(args)
      self.executable = os.environ.get('SHELL')
    else:
      self.args = args
      self.executable = None
    self.ret = None
    self.finished = None
    self.outdata = []
    self.partial = [b'', b'']
    self.sigfail = False
    self.kill_time = None
    self.killed = False
    # Python >=3.8 doesn't like line-buffered binary, so use unbuffered
    self.proc = subprocess.Popen(
        self.args, bufsize=0, shell=self.shell, executable=self.executable,
        stdin=self.input, stdout=self.output, stderr=self.errout
        )
    self.started = time.time()
    # Since we don't currently handle input, close the input pipe immediately.
    self.proc.stdin.close()
    self._SetBothNonblocking(True)

  def _SetBothNonblocking(self, nonblock):
    self._SetNonblocking(self.proc.stdout, nonblock)
    self._SetNonblocking(self.proc.stderr, nonblock)

  @staticmethod
  def _SetNonblocking(fileobj, nonblock):
    ofl = fcntl.fcntl(fileobj, fcntl.F_GETFL)
    newflag = ofl | os.O_NONBLOCK if nonblock else ofl & ~os.O_NONBLOCK
    fcntl.fcntl(fileobj, fcntl.F_SETFL, newflag)

  def Register(self, poller):
    """Register pipe(s) with poll object."""
    poller.register(self.proc.stdout, poller.POLLIN)
    poller.register(self.proc.stderr, poller.POLLIN)

  def Unregister(self, poller):
    """Unregister pipe(s) with poll object."""
    poller.unregister(self.proc.stdout)
    poller.unregister(self.proc.stderr)

  def _GetOutput(self, iserr=0):
    stream = self.proc.stderr if iserr else self.proc.stdout
    # Note that file iterators don't work properly with nonblocking I/O
    # in Python 2, so we need to do read() and split().
    try:
      data = stream.read()
    except IOError:
      return False
    return self._AddOutput(iserr, data)

  def _AddOutput(self, iserr, data):
    if not data:
      return False
    lines = data.split(b'\n')
    if self.partial[iserr]:
      self.partial[iserr] += lines[0]
      del lines[0]
      if lines:
        self.outdata.append(Line(iserr, self.partial[iserr]))
        self.partial[iserr] = b''
      else:
        return True
    if lines[-1]:
      self.partial[iserr] = lines[-1]
    del lines[-1]
    for line in lines:
      self.outdata.append(Line(iserr, line))
    return True

  def _GetBothOutputs(self):
    return self._GetOutput(0) or self._GetOutput(1)

  def _AddBothOutputs(self, out, err):
    self._AddOutput(0, out)
    self._AddOutput(1, err)

  def Poll(self):
    """Poll subprocess for activity; return False, True, or exit code."""
    if self.proc.poll() is None:
      return self._GetBothOutputs()
    self.finished = time.time()
    self._SetBothNonblocking(False)
    self._GetBothOutputs()
    return self.proc.returncode

  def Print(self, name=False, tstamp=False, where=(sys.stdout, sys.stderr)):
    """Print results with optional name and/or timestamp."""
    for line in self.outdata:
      line.Print(name=name and self.name, tstamp=tstamp, where=where)
    self.outdata = []

  def PrintLast(self, name=None, tstamp=False, where=(sys.stdout, sys.stderr)):
    """Print partial line with optional name and/or timestamp."""
    for iserr in range(2):
      if self.partial[iserr]:
        print(Line.Format(self.partial[iserr],
                          iserr, name and self.name, tstamp and time.time()),
              file=where[iserr])

  def Signal(self, sig, set_kill=False):
    """Send signal to subprocess."""
    try:
      self.proc.send_signal(sig)
    except OSError as exc:
      self.SignalFailed(exc, 'sending signal %d to' % sig)
    if set_kill:
      self.SetKill()

  def SignalFailed(self, exc, msg):
    """Handle failure to signal subprocess."""
    if exc.errno != errno.EPERM:
      raise
    self.sigfail = True
    Eprint('Error %s subprocess %s: %s' % (msg, self.name, exc.strerror))

  def SetKill(self, final=False):
    """Set up kill timeout."""
    if self.killed:
      return
    now = time.time()
    if not final:
      self.kill_time = now + self.KILL_DELAY
    else:
      self.killed = True
      self.kill_time = now + self.KILL_TIMEOUT

  def Kill(self):
    """Kill subprocess."""
    try:
      self.proc.kill()
    except OSError as exc:
      self.SignalFailed(exc, 'killing')


def SplitArgs(arglist):
  """Split list of argument strings into single list of args."""
  result = []
  for entry in arglist:
    splitter = shlex.shlex(entry)
    splitter.whitespace += ','
    splitter.whitespace_split = True
    result.extend(list(splitter))
  return result


def ShellStr(command):
  """Convert command/arg list to single string for shell."""
  # Doesn't quote args so that they can have their shell functions
  return ' '.join(command)


def ParseArgs(prog, args):
  """Parse arguments from command line.

  Args:
    prog: program filename for usage text
    args: list of arguments

  Returns:
    parse result
  """
  # MAYBE: Turn this into a class, but figure out how to handle the 'prog' arg.
  parser = argparse.ArgumentParser(
      prog=prog,
      usage='%(prog)s [options] [items or command]',
      description='Apply a command to multiple items in parallel',
      epilog='\n'.join(SUBST_HELP),
      formatter_class=argparse.RawDescriptionHelpFormatter,
      )
  parser.add_argument('-s', '--sequential', action='store_true',
                      help='report output sequentially per process')
  parser.add_argument('-n', '--names', action='store_true',
                      help='tag output lines with item names')
  parser.add_argument('-t', '--times', action='store_true',
                      help='tag output lines with timestamps')
  parser.add_argument('-v', '--verbose', action='store_true',
                      help='show more info on termination')
  parser.add_argument('-c', '--command',
                      help='command to apply')
  argopts = parser.add_mutually_exclusive_group()
  argopts.add_argument('-a', '--args', action='append',
                       help='arguments (paths)')
  argopts.add_argument('-f', '--arg-file', type=argparse.FileType(mode='r'),
                       help='file containing argument lines')
  argopts.add_argument('-m', '--machines', action='append',
                       help='target machines (via ssh)')
  parser.add_argument('-4', '--ipv4', action='store_true',
                      help="force IPv4 with -m's ssh")
  parser.add_argument('-6', '--ipv6', action='store_true',
                      help="force IPv6 with -m's ssh")
  parser.add_argument('-S', '--shell', action='store_true',
                      help='run with shell')
  parser.add_argument('-K', '--kill-hung', action='store_true',
                      help='kill hung subprocesses')
  parser.add_argument('--signal-test', action='store_true',
                      help='enable signal-testing features')
  parser.add_argument('remaining', nargs=argparse.REMAINDER)
  parsed = parser.parse_args(args)
  args = parsed.remaining
  if args and args[0] == '--':
    del args[0]
  return parser, parsed, args


def main(argv):
  # pylint: disable=too-many-locals,too-many-branches,too-many-statements
  """Main function."""
  prog = os.path.basename(argv[0])
  _, parsed, args = ParseArgs(prog, argv[1:])
  pdb_module = sys.modules.get('pdb')
  if pdb_module:
    pdb_module.set_trace()
    pass  # Here to set breakpoints
  if parsed.command:
    command = shlex.split(parsed.command)
  else:
    command = args
    args = None
  if not command:
    Eprint('%s: must specify command' % prog)
    return 2
  poller = Poller()
  if poller.POLL_FIX:
    Eprint('%Substituting for missing select.poll')
  if parsed.signal_test:
    print('[This pid = %d]' % os.getpid())
  procs = []
  done = []
  retval = 0
  mapdict = PATH_MAP
  if parsed.arg_file:
    args = [l.rstrip() for l in parsed.arg_file.readlines()]
    mapdict = ARG_MAP
  if parsed.args:
    args = SplitArgs(parsed.args)
  if parsed.machines:
    args = SplitArgs(parsed.machines)
    mapdict = MACH_MAP
    sshopts = '-4T' if parsed.ipv4 else '-6T' if parsed.ipv6 else '-T'
    command = ['ssh', sshopts, '%M'] + command
  if not args:
    if parsed.names:
      Eprint('%s: -n illegal with empty target list' % prog)
      return 2
    args = ['']
    mapdict = NULL_MAP
  for sig in SIG_MAP:
    poller.Signal(sig)
  started = time.time()
  if parsed.verbose and parsed.times:
    print('[Started (%d) at %s]' % (len(args), TimeStr(started)))
  for arg in args:
    if arg:
      name = shlex.split(arg)[0]
    else:
      name = None
    cmd = [Interpolate(x, arg, mapdict) for x in command]
    try:
      proc = Process(name, cmd, shell=parsed.shell)
    except OSError as exc:
      Eprint(repr(exc))
      return 127
    if parsed.times:
      if proc.realname:
        msg = '[%s started at %%s]' % proc.realname
      else:
        msg = '[Started at %s]'
      Eprint(msg % TimeStr(proc.started))
    proc.Register(poller)
    procs.append(proc)
  if parsed.verbose and not parsed.times:
    print('[Started (%d): %s]'
          % (len(procs), ','.join([x.name for x in procs])))
  sigs_sent = set()
  while procs:
    if poller.sigs_rcvd:
      sigs_to_send = poller.sigs_rcvd - sigs_sent
      set_kill = parsed.signal_test or sigs_sent - SIG_WAIT
      for sig in sigs_to_send:
        if parsed.verbose or parsed.signal_test:
          now = time.time()
          Eprint('[Forwarding signal %d (%s) to subprocesses at %s]'
                 % (sig, SIG_MAP.get(sig, '?'), TimeStr(now)))
          sys.stderr.flush()
        for proc in procs:
          proc.Signal(sig, set_kill=set_kill)
      sigs_sent |= sigs_to_send
    activity = False
    hung_check = False
    now = time.time()
    deadprocs = 0
    for proc in procs[:]:
      ret = proc.Poll()
      if ret is False:
        if not proc.kill_time:
          continue
        hung_check = True
        if proc.kill_time > now:
          continue
        killed = (proc.name, TimeStr(now))
        if proc.sigfail:
          Eprint('%%Unsignaled subprocess %s still running at %s' % killed)
          deadprocs += 1
        elif not parsed.kill_hung:
          Eprint('%%Subprocess %s hung at %s' % killed)
          proc.SetKill(final=False)
        elif not proc.killed:
          Eprint('%%Killing hung subprocess %s at %s' % killed)
          proc.Kill()
          proc.SetKill(final=True)
          activity = True
        elif proc.killed is True:
          Eprint('%%Timed out killing subprocess %s at %s' % killed)
          proc.killed = now
          deadprocs += 1
        continue
      if ret is True:
        activity = True
        # When down to last process, output in real time
        if not parsed.sequential or len(procs) < 2:
          proc.Print(parsed.names, parsed.times)
        # Reset any signal timeout after output
        if proc.kill_time:
          proc.SetKill(final=False)
        continue
      proc.ret = ret
      procs.remove(proc)
      done.append(proc)
      proc.Unregister(poller)
      proc.Print(parsed.names, parsed.times)
      proc.PrintLast(parsed.names, parsed.times)
      if ret or parsed.verbose or parsed.times:
        if proc.realname:
          nstr = ' for ' + proc.realname
        else:
          nstr = ''
        if parsed.times:
          tstr = (' at %s, took %s'
                  % (TimeStr(proc.finished),
                     ElapsedStr(proc.finished - proc.started)))
        else:
          tstr = ''
        Eprint('[Returned %d%s%s]' % (ret, nstr, tstr))
        if ret > retval:
          retval = ret
      if parsed.verbose and procs:
        if len(done) > 1:
          results = ['%s=%d' % (p.name, p.ret) for p in done]
          Eprint('[Returns (%d/%d): %s; retval = %d]'
                 % (len(done), len(args), ', '.join(results), retval))
        names = [x.name for x in procs]
        Eprint('[Still running (%d/%d): %s]'
               % (len(procs), len(args), ','.join(names)))
      # If transitioning to last process while sequential, catch up
      if parsed.sequential and len(procs) == 1:
        procs[0].Print(parsed.names, parsed.times)
      activity = True
    if deadprocs and deadprocs >= len(procs):
      Eprint('%%Abandoning %d unsignalable subprocesses' % deadprocs)
      retval = 999
      break
    if not activity:
      poller.poll(100 if hung_check else 5000)
  finished = time.time()
  numdone = len(done)
  if numdone > 1:
    if parsed.verbose:
      if not parsed.times:
        results = ['%s=%d' % (p.name, p.ret) for p in done]
        Eprint('[Returns: %s]' % ', '.join(results))
      else:
        for proc in done:
          Eprint('[%s returned %d, took %s]'
                 % (proc.name, proc.ret,
                    ElapsedStr(proc.finished - proc.started)))
      Eprint('[All %d processes complete, final return = %d]'
             % (numdone, retval))
    else:
      results = ['%s=%d' % (p.name, p.ret) for p in done if p.ret]
      if results:
        Eprint('[Failures: %s]' % ', '.join(results))
  if parsed.times:
    Eprint('[Finished at %s, took %s]'
           % (TimeStr(finished), ElapsedStr(finished - started)))
  return retval


if __name__ == '__main__':
  sys.exit(main(sys.argv))  # pragma: no cover
