/* ***************************************************************** */
/* ptyrec: C99/POSIX terminal automation and optional SRT subtitles. */
/* ***************************************************************** */

/* Overall data flow                                                          */
/* =================                                                          */
/*                                                                            */
/*                           parent process                                   */
/*                                                                            */
/*  command files                                                             */
/*       |                                                                    */
/*       v                                                                    */
/*  +----------------+     script.bytes      +-----------------------+        */
/*  | parser: step() | --------------------> | schedule_script()     |        */
/*  +----------------+                       |                       |        */
/*       |                                   | wait until script.due |        */
/*       | no-byte directives                | queue one input unit  |        */
/*       | (\s, \S, \W, \R, \k, \m, \D)      +----------+------------+        */
/*       |                                              |                     */
/*       |                         +--------------------+-------------+       */
/*       |                         |                                  |       */
/*       |                         v                                  v       */
/*       |                  +-------------+                     +----------+  */
/*       |                  | input queue |                     | display  |  */
/*       |                  |             |                     | queue    |  */
/*       |                  | child input |                     | \d[] SGR |  */
/*       |                  +------+------+                     +-----+----+  */
/*       |                         |                                  |       */
/*       |                         v                                  |       */
/*       |                  PTY master write                          |       */
/*       |                         |                                  |       */
/*       |                         v                                  |       */
/*       |                 +---------------+                          |       */
/*       |                 | child process |                          |       */
/*       |                 | PTY slave     |                          |       */
/*       |                 +-------+-------+                          |       */
/*       |                         |                                  |       */
/*       |                  PTY master read                           |       */
/*       |                         |                                  |       */
/*       |              +----------+----------+                       |       */
/*       |              |                     |                       |       */
/*       |              v                     v                       |       */
/*       |       +------------+       +--------------+                |       */
/*       |       | binary log |       | output queue |                |       */
/*       |       +------------+       +-------+------+                |       */
/*       |                                    |                       |       */
/*       |                                    +------------+----------+       */
/*       |                                                 |                  */
/*       |                                                 v                  */
/*       |                                               stdout               */
/*       |                                                                    */
/*       |     A scripted unit is sent to subtitle_unit() only after          */
/*       |     its complete byte sequence has drained from the input          */
/*       |     queue into the PTY.                                            */
/*       |                                                                    */
/*       +------------------------------+                                     */
/*                                      |                                     */
/*                                      v                                     */
/*                              +---------------+                             */
/*                              | subtitle_unit |                             */
/*                              +-------+-------+                             */
/*                                      |                                     */
/*                       +--------------+--------------+                      */
/*                       |                             |                      */
/*                       v                             v                      */
/*                consume ANSI SGR              apply subtitle map            */
/*                as style state                and write SRT cue             */
/*                                                                            */
/* Event loop                                                                 */
/* ==========                                                                 */
/*                                                                            */
/*                   +------------------------+                               */
/*                   | update process state   |                               */
/*                   |                        |                               */
/*                   | - reap child           |                               */
/*                   | - handle termination   |                               */
/*                   | - propagate resize     |                               */
/*                   | - enforce deadlines    |                               */
/*                   +-----------+------------+                               */
/*                               |                                            */
/*                               v                                            */
/*                   +------------------------+                               */
/*                   | schedule_script()      |                               */
/*                   |                        |                               */
/*                   | parse due directives   |                               */
/*                   | stage one input unit   |                               */
/*                   +-----------+------------+                               */
/*                               |                                            */
/*                               v                                            */
/*                   +------------------------+                               */
/*                   | build select() sets    |                               */
/*                   |                        |                               */
/*                   | read:                  |                               */
/*                   |   wake pipe            |                               */
/*                   |   child error pipe     |                               */
/*                   |   PTY master           |                               */
/*                   |   stdin                |                               */
/*                   |                        |                               */
/*                   | write:                 |                               */
/*                   |   PTY master           |                               */
/*                   |   stdout               |                               */
/*                   +-----------+------------+                               */
/*                               |                                            */
/*                               v                                            */
/*                   +------------------------+                               */
/*                   | select()               |                               */
/*                   |                        |                               */
/*                   | wait for I/O, signal   |                               */
/*                   | wakeup, or deadline    |                               */
/*                   +-----------+------------+                               */
/*                               |                                            */
/*                               v                                            */
/*                   +------------------------+                               */
/*                   | service ready I/O      |                               */
/*                   | and repeat             |                               */
/*                   +-----------+------------+                               */
/*                               |                                            */
/*                               +--------------------+                       */
/*                                                    |                       */
/*                                                    v                       */
/*                                             next iteration                 */
/*                                                                            */
/*                                                                            */
/* Signal handling                                                            */
/* ===============                                                            */
/*                                                                            */
/*    signal                                                                  */
/*       |                                                                    */
/*       v                                                                    */
/*  handler() sets a sig_atomic_t flag                                        */
/*       |                                                                    */
/*       v                                                                    */
/*  handler() writes one byte to wake[1]                                      */
/*       |                                                                    */
/*       v                                                                    */
/*  wake[0] becomes readable                                                  */
/*       |                                                                    */
/*       v                                                                    */
/*  select() returns and the main loop handles the event safely               */
/*                                                                            */
/*                                                                            */
/* Child startup reporting                                                    */
/* =======================                                                    */
/*                                                                            */
/*    child setup or exec failure       successful exec                       */
/*                 |                          |                               */
/*                 v                          v                               */
/*       write errno to err[1]       close-on-exec closes err[1]              */
/*                 |                          |                               */
/*                 v                          v                               */
/*       parent accumulates bytes       parent reads EOF                      */
/*       in exec_error using            with error_used == 0                  */
/*       error_used                                                           */
/*                 |                          |                               */
/*                 v                          v                               */
/*       complete int reports error      child is ready                       */
/*                                                                            */
/*                                                                            */
/* Scheduling state                                                           */
/* ================                                                           */
/*                                                                            */
/*  script.pending                                                            */
/*      A logical scripted unit has been decoded into script.bytes.           */
/*                                                                            */
/*  script.position / script.length                                           */
/*      Identify the unscheduled portion of the decoded unit.                 */
/*                                                                            */
/*  script.unit                                                               */
/*      Number of bytes that must remain together when queued.                */
/*                                                                            */
/*  script.sent                                                               */
/*      The current portion has been queued and is draining.                  */
/*                                                                            */
/*  script.queued                                                             */
/*      Number of bytes belonging to that queued portion.                     */
/*                                                                            */
/*  script.delivered                                                          */
/*      The queued portion has already been handled for subtitles.            */
/*                                                                            */
/*  script.due                                                                */
/*      Earliest monotonic time at which the next unit may be queued.         */
/*                                                                            */
/*  The next pacing interval starts only after the previous queued unit       */
/*  has drained.  This prevents parser speed and partial writes from          */
/*  changing the visible typing delay.                                        */
/* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#if HAVE_SYS_TERMIO_H
#include <sys/termio.h>
#endif

#if HAVE_STROPTS_H
#include <stropts.h>
#elif HAVE_SYS_STROPTS_H
#include <sys/stropts.h>
#endif

#if HAVE_PTY_H
#include <pty.h>
#elif HAVE_UTIL_H
#include <util.h>
#elif HAVE_LIBUTIL_H
#include <libutil.h>
#endif

#include "ptyrec.h"

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901
#error C99 compiler required
#endif

#if UCHAR_MAX != 255
#error Eight-bit bytes required
#endif

/* C99 has no standard _Noreturn keyword.  Tell supported compilers and the */
/* Clang static analyzer that these functions never return.                 */
/* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
#if defined(__clang__) || defined(__GNUC__)
#define PTYREC_NORETURN __attribute__((__noreturn__))
#else
#define PTYREC_NORETURN
#endif

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof(a)[0])

/* Parent-owned relay state; the child inherits a private copy after fork.  */
/* Descriptor markers and ownership flags guide restoration and cleanup.    */
/*                                                                          */
/* The script object owns command parsing and pacing state.                 */
/* The remaining descriptors and terminal state belong to the parent relay. */
/* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
static struct script script;

static int terminal  = -1;
static int in_flags  = -1;
static int out_flags = -1;

static struct termios saved;

static int wake[2] = { -1, -1 };

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t resized;

static pid_t child = -1;
static int   reaped;
static char *slave_name;

static dev_t log_dev;
static dev_t subtitle_dev;
static dev_t map_dev;

static ino_t log_ino;
static ino_t subtitle_ino;
static ino_t map_ino;

static int log_known;
static int subtitle_known;
static int map_known;

/* A matching node with zero text length suppresses subtitle text. */
/* Later map entries are at the list head and take precedence.     */
/* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
static struct subtitle_map *subtitle_maps;
static FILE                *subtitle_file;
static const char          *subtitle_path     = "ptyrec.srt";
static unsigned long        subtitle_duration = 300;
static unsigned long        subtitle_index;
static uint64_t             subtitle_origin;

/* Signed subtitle timestamp adjustment in milliseconds. */
/* A negative value moves cues towards the beginning.    */
/* ''''''''''''''''''''''''''''''''''''''''''''''''''''' */
static int64_t subtitle_offset;

static int  subtitle_enabled;
static int  subtitle_started;
static int  subtitle_color_set;
static char subtitle_color[8];
static int  subtitle_bold;
static int  subtitle_italic;
static int  subtitle_underline;
static int  subtitle_strike;
static int  subtitle_suppress_unit;

static int display_only_unit;
static int display_style_active;

static int         arg_index = 1;
static const char *opt_arg;

static void
subtitle_unit(const unsigned char *, size_t);

/* Helpers needed by restoration and cleanup before their definitions. */
/* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
static int
set_terminal_mode(int, int, const struct termios *);

static pid_t
wait_child(pid_t, int *, int);

static size_t
write_all_best_effort(int, const unsigned char *, size_t);

/* ======================================================================== */
/* Restore saved terminal mode and stream flags; reset saved-state markers. */
/* ======================================================================== */
static void
restore(void)
{
  if (terminal >= 0)
  {
    (void)set_terminal_mode(terminal, TCSANOW, &saved);
    terminal = -1;
  }

  if (in_flags >= 0)
    (void)fcntl(0, F_SETFL, in_flags);

  if (out_flags >= 0)
    (void)fcntl(1, F_SETFL, out_flags);

  in_flags  = -1;
  out_flags = -1;

  /* A fatal exit must not leave the user's terminal in an SGR style. */
  /* Restoration is best-effort and must not call fail() recursively. */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (display_style_active)
  {
    static const unsigned char reset[] = { 0x1b, '[', '0', 'm' };

    (void)write_all_best_effort(1, reset, sizeof(reset));

    display_style_active = 0;
  }
}

/* ===================================================================== */
/* Release all owned subtitle map nodes, retaining each successor first. */
/* ===================================================================== */
static void
free_maps(void)
{
  while (subtitle_maps)
  {
    struct subtitle_map *next = subtitle_maps->next;
    free(subtitle_maps);
    subtitle_maps = next;
  }
}

/* ======================================== */
/* Parent atexit cleanup: restore terminal, */
/* release files and stop a live child.     */
/* ======================================== */
static void
cleanup(void)
{
  restore();

  while (script.depth)
    (void)fclose(script.files[--script.depth]);

  if (subtitle_file)
    (void)fclose(subtitle_file);

  subtitle_file = NULL;
  free_maps();

  if (child > 0 && !reaped)
  {
    int status;
    (void)kill(-child, SIGHUP);
    (void)kill(child, SIGKILL);
    (void)wait_child(child, &status, 0);
  }

  free(slave_name);
}

/* ========================================================================== */
/* Restore the terminal, print a fatal diagnostic, and invoke atexit cleanup. */
/* ========================================================================== */
static PTYREC_NORETURN void
fail(const char *fmt, ...)
{
  va_list ap;

  restore();
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);

  exit(EXIT_FAILURE);
}

/* ======================================================================= */
/* Capture errno before cleanup and report the corresponding system error. */
/* ======================================================================= */
static PTYREC_NORETURN void
syserr(const char *what)
{
  int e = errno;

  fail("%s: %s", what, strerror(e));
}

/* ==================================================================== */
/* Apply terminal attributes, retrying only an interrupted system call. */
/* ==================================================================== */
static int
set_terminal_mode(int fd, int action, const struct termios *mode)
{
  int rc;

  do
  {
    rc = tcsetattr(fd, action, mode);
  } while (rc < 0 && errno == EINTR);

  return rc;
}

/* ======================================================================== */
/* Wait for a child, retaining the caller's blocking or nonblocking policy. */
/* ======================================================================== */
static pid_t
wait_child(pid_t pid, int *status, int options)
{
  pid_t result;

  do
  {
    result = waitpid(pid, status, options);
  } while (result < 0 && errno == EINTR);

  return result;
}

/* ======================================================================== */
/* Write every byte or report a fatal error using the supplied description. */
/* ======================================================================== */
static void
write_all(int fd, const unsigned char *p, size_t n, const char *what)
{
  while (n)
  {
    ssize_t rc = write(fd, p, n);

    if (rc > 0)
    {
      p += (size_t)rc;
      n -= (size_t)rc;
      continue;
    }

    if (rc < 0 && errno == EINTR)
      continue;

    if (!rc)
      fail("%s made no progress", what);

    syserr(what);
  }
}

/* ================================================================ */
/* Write as much as possible without invoking fatal-error handling. */
/* ================================================================ */
static size_t
write_all_best_effort(int fd, const unsigned char *p, size_t n)
{
  size_t written = 0;

  while (n)
  {
    ssize_t rc = write(fd, p, n);

    if (rc > 0)
    {
      p += (size_t)rc;
      n -= (size_t)rc;

      written += (size_t)rc;
    }
    else if (rc < 0 && errno == EINTR)
      continue;
    else
      break;
  }

  return written;
}

/* =================================================== */
/* Return monotonic milliseconds, or wall time clamped */
/* against backward jumps.                             */
/* =================================================== */
static uint64_t
now_ms(void)
{
#if HAVE_MONOTONIC
  struct timespec t;

  if (clock_gettime(CLOCK_MONOTONIC, &t) < 0)
    syserr("clock_gettime");

  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#else
  struct timeval  t;
  static uint64_t last;
  uint64_t        value;

  if (gettimeofday(&t, NULL) < 0)
    syserr("gettimeofday");

  value = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_usec / 1000;

  if (value > last)
    last = value;

  return last;
#endif
}

/* ==================================================================== */
/* Merge an absolute deadline into the relative wait; clamp to INT_MAX. */
/* ==================================================================== */
static void
deadline(int *timeout, uint64_t due, uint64_t now)
{
  uint64_t n  = due > now ? due - now : 0;
  int      ms = n > (uint64_t)INT_MAX ? INT_MAX : (int)n;

  if (*timeout < 0 || ms < *timeout)
    *timeout = ms;
}

/* ====================================================================== */
/* Record signal flags and wake the relay through the nonblocking pipe.   */
/* Retry interrupted writes; a full pipe already provides a pending wake. */
/* ====================================================================== */
static void
handler(int sig)
{
  int           e = errno;
  unsigned char b = 0;

#ifdef SIGWINCH
  if (sig == SIGWINCH)
    resized = 1;
  else
#endif
  {
    if (sig != SIGCHLD && !stopping)
      stopping = sig;
  }

  /* Only async-signal-safe operations are permitted in this handler.         */
  /* The byte value is irrelevant; a readable pipe records the pending event. */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (wake[1] >= 0)
  {
    ssize_t rc;

    do
    {
      rc = write(wake[1], &b, 1);
    } while (rc < 0 && errno == EINTR);
  }

  errno = e;
}

/* ====================================================================== */
/* Identify interruption and would-block errors that permit retrying I/O. */
/* ====================================================================== */
static int
retry_io(void)
{
  return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

/* ========================================================================= */
/* Signal both the child process group and the child; ignore exited targets. */
/* ========================================================================= */
static void
signal_child(int sig)
{
  (void)kill(-child, sig);
  (void)kill(child, sig);
}

/* ======================================================================= */
/* Enable O_NONBLOCK without losing existing flags; return original flags. */
/* ======================================================================= */
static int
nonblock(int fd)
{
  int flags = fcntl(fd, F_GETFL);

  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    syserr("nonblocking descriptor");

  return flags;
}

/* ======================================================= */
/* Add FD_CLOEXEC while preserving other descriptor flags. */
/* ======================================================= */
static void
cloexec(int fd)
{
  int flags = fcntl(fd, F_GETFD);

  if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    syserr("close-on-exec");
}

/* ======================================================================== */
/* Validate FD_SETSIZE, add the descriptor and update the wait upper bound. */
/* ======================================================================== */
static void
add_fd(int fd, fd_set *set, int *nfds)
{
  if (fd < 0 || fd >= FD_SETSIZE)
    fail("Descriptor exceeds FD_SETSIZE");

  FD_SET(fd, set);

  if (fd >= *nfds)
    *nfds = fd + 1;
}

/* ===================================================================== */
/* Wait with select and self-pipe wakeups under a temporary signal mask. */
/* Restore the caller's mask and preserve errno from the select call.    */
/* ===================================================================== */
static int
wait_io(int nfds, fd_set *r, fd_set *w, int ms, const sigset_t *mask)
{
  struct timeval  t;
  struct timeval *tp = NULL;

  sigset_t before;

  int rc;
  int e;

  if (ms >= 0)
  {
    t.tv_sec  = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
    tp        = &t;
  }

  /* The self-pipe retains wakeups occurring before select starts. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (sigprocmask(SIG_SETMASK, mask, &before) < 0)
    return -1;

  rc = select(nfds, r, w, NULL, tp);
  e  = errno;

  if (sigprocmask(SIG_SETMASK, &before, NULL) < 0)
    return -1;

  errno = e;

  return rc;
}

/* ======================================================================== */
/* Return the live binary queue length in the half-open range [first,last). */
/* ======================================================================== */
static size_t
count(const struct queue *q)
{
  return q->last - q->first;
}

/* ================================================================= */
/* Compact only at a full tail and return contiguous queue capacity. */
/* ================================================================= */
static size_t
room(struct queue *q)
{
  if (q->first && q->last == CAP)
  {
    size_t n = count(q);

    memmove(q->data, q->data + q->first, n);

    q->first = 0;
    q->last  = n;
  }

  return CAP - q->last;
}

/* ================================================================ */
/* Append binary bytes without a terminator. Reject queue overflow. */
/* ================================================================ */
static void
append(struct queue *q, const unsigned char *p, size_t n)
{
  if (room(q) < n)
    fail("Queue overflow");

  memcpy(q->data + q->last, p, n);
  q->last += n;
}

/* =============================================================== */
/* Write every log byte, handling short writes and retrying EINTR. */
/* =============================================================== */
static void
log_bytes(int fd, const unsigned char *p, size_t n)
{
  write_all(fd, p, n, "write log");
}

/* ========================================================================== */
/* Write a queued prefix and emit a scripted subtitle once when input drains. */
/* ========================================================================== */
static int
flush(int fd, struct queue *q, int logfd, int is_pty)
{
  ssize_t rc;

  if (!count(q))
    return 1;

  /* A nonblocking write may consume only part of the queued prefix. */
  /* Advance the queue only by the number of bytes actually written. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  rc = write(fd, q->data + q->first, count(q));

  if (rc < 0)
  {
    if (retry_io())
      return 1;

    if (is_pty && (errno == EIO || errno == EPIPE))
      return 0;

    syserr("write stream");
  }

  if (!rc)
    fail("Stream write made no progress");

  if (logfd >= 0)
    log_bytes(logfd, q->data + q->first, (size_t)rc);

  q->first += (size_t)rc;

  if (q->first == q->last)
  {
    q->first = 0;
    q->last  = 0;

    if (is_pty && script.pending && script.sent && !script.delivered)
    {
      subtitle_unit(script.bytes + script.position - script.queued,
                    script.queued);
      script.delivered = 1;
    }
  }

  return 1;
}

/* ========================================================================== */
/* Parse bounded unsigned decimal input, checking arithmetic before overflow. */
/* ========================================================================== */
static unsigned long
number(const char *p, unsigned long max)
{
  unsigned long n = 0;

  if (!*p)
    fail("Empty number");

  while (*p)
  {
    unsigned d;

    if (*p < '0' || *p > '9')
      fail("Invalid number");

    d = (unsigned)(*p++ - '0');

    if (d > max || n > (max - d) / 10)
      fail("Number exceeds %lu", max);

    n = n * 10 + d;
  }
  return n;
}

/* ==================================================== */
/* Parse grouped short options, retaining argv position */
/* and borrowed operands.                               */
/* ==================================================== */
static int
option(int argc, char **argv)
{
  static const char *scan;

  int c;

  if (!scan || !*scan)
  {
    if (arg_index >= argc || argv[arg_index][0] != '-' || !argv[arg_index][1])
      return -1;

    if (!strcmp(argv[arg_index], "--"))
    {
      arg_index++;
      return -1;
    }

    scan = argv[arg_index++] + 1;
  }

  c = (unsigned char)*scan++;

  if (c == 'V')
    return c;

  if (!strchr("liwhsdo", c))
    return '?';

  if (*scan)
  {
    opt_arg = scan;
    scan    = NULL;
  }
  else if (arg_index < argc)
  {
    opt_arg = argv[arg_index++];
    scan    = NULL;
  }
  else
    return '?';

  return c;
}

/* ======================================================================== */
/* Push a regular command file; reject nesting overflow and output aliases. */
/* ======================================================================== */
static void
push_file(const char *path)
{
  struct stat st;
  int         fd;
  FILE       *f;

  if (script.depth == DEPTH)
    fail("Include nesting limit exceeded");

  fd = open(path, O_RDONLY | O_NONBLOCK);

  if (fd < 0)
    syserr(path);

  if (fstat(fd, &st) < 0)
    syserr("fstat command file");

  if (!S_ISREG(st.st_mode))
    fail("Command files must be regular files");

  if ((log_known && st.st_dev == log_dev && st.st_ino == log_ino)
      || (subtitle_known && st.st_dev == subtitle_dev
          && st.st_ino == subtitle_ino))
    fail("Cannot include an output file");

  cloexec(fd);
  f = fdopen(fd, "rb");

  if (!f)
    syserr("fdopen");

  script.files[script.depth++] = f;
}

/* ======================================================================= */
/* Read one byte or EOF, clearing stdio error state before an EINTR retry. */
/* ======================================================================= */
static int
file_byte(FILE *f)
{
  int c;

  for (;;)
  {
    errno = 0;
    c     = fgetc(f);

    if (c != EOF || !ferror(f))
      return c;

    if (errno == EINTR)
    {
      clearerr(f);
      continue;
    }

    fail("Command file read error");
  }
}

/* ========================================================================= */
/* Pop exhausted includes and resume their parent; EOF means an empty stack. */
/* ========================================================================= */
static int
next_byte(void)
{
  while (script.depth)
  {
    FILE *f = script.files[script.depth - 1];
    int   c = file_byte(f);

    if (c != EOF)
      return c;

    if (fclose(f))
      syserr("close command file");

    script.depth--;
  }

  return EOF;
}

/* ========================================================================= */
/* Read a bracket argument from one file only; reject NUL, EOF and overflow. */
/* ========================================================================= */
static void
argument(char *p)
{
  FILE  *f;
  size_t n = 0;
  int    c;

  if (!script.depth)
    fail("Missing directive argument");

  f = script.files[script.depth - 1];

  if (file_byte(f) != '[')
    fail("Expected '['");

  for (;;)
  {
    c = file_byte(f);

    if (c == EOF)
      fail("Missing closing ']'");

    if (!c)
      fail("NUL in directive argument");

    if (c == ']')
      break;

    if (n == ARG_CAP - 1)
      fail("Directive argument too long");

    p[n++] = (char)c;
  }

  p[n] = '\0';
}

/* ===================================================================== */
/* Decode an ASCII hexadecimal digit, returning -1 for other characters. */
/* ===================================================================== */
static int
hex(int c)
{
  if (c >= '0' && c <= '9')
    return c - '0';

  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;

  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;

  return -1;
}

/* ======================================================================= */
/* Decode compact or whitespace-separated byte pairs into bounded storage. */
/* ======================================================================= */
static size_t
hex_sequence(const char *text, unsigned char *bytes, size_t max)
{
  const unsigned char *p = (const unsigned char *)text;
  size_t               n = 0;

  while (*p)
  {
    int hi;
    int lo;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;

    if (!*p)
      break;

    hi = hex(*p++);

    if (hi < 0 || !*p)
      fail("Expected hexadecimal byte pairs");

    lo = hex(*p++);

    if (lo < 0)
      fail("Expected hexadecimal byte pairs");

    if (n == max)
      fail("Hex sequence too long");

    bytes[n++] = (unsigned char)(hi * 16 + lo);
  }

  if (!n)
    fail("Empty hex sequence");

  return n;
}

/* ========================= */
/* Decode binary map fields. */
/* ========================= */
static size_t
map_decode(const char *p, unsigned char *out)
{
  size_t n = 0;

  while (*p)
  {
    unsigned c = (unsigned char)*p++;

    if (c == '\\')
    {
      c = (unsigned char)*p++;

      if (!c)
        fail("Trailing backslash in subtitle map");

      if (c == 'u' || (c == 'x' && *p == '['))
      {
        int    bracket = *p == '[';
        size_t first   = n;

        if (bracket)
          p++;

        for (;;)
        {
          int hi;
          int lo;

          if (bracket)
            while (*p == ' ' || *p == '\t')
              p++;

          hi = hex((unsigned char)*p);

          if (hi < 0)
            break;

          p++;
          lo = hex((unsigned char)*p);

          if (lo < 0)
            fail("Odd hex sequence in subtitle map");
          p++;

          if (n == ARG_CAP)
            fail("Subtitle map field too long");

          out[n++] = (unsigned char)(hi * 16 + lo);
        }

        if (n == first)
          fail("Empty hex sequence in subtitle map");

        if (bracket)
        {
          if (*p != ']')
            fail("Missing ] in subtitle map");
          p++;
        }

        continue;
      }

      if (c == 'x')
      {
        unsigned value = 0;
        int      d;
        int      digits = 0;

        while ((d = hex((unsigned char)*p)) >= 0)
        {
          if (value > (UCHAR_MAX - (unsigned)d) / 16)
            fail("Map hex escape exceeds one byte");

          value = value * 16 + (unsigned)d;
          p++;
          digits = 1;
        }

        if (!digits)
          fail("Empty map hex escape");

        c = value;
      }
      else if (c >= '0' && c <= '7')
      {
        int      j;
        unsigned value = c - '0';

        for (j = 1; j < 3 && *p >= '0' && *p <= '7'; ++j)
          value = value * 8 + (unsigned)(*p++ - '0');

        if (value > UCHAR_MAX)
          fail("Map octal escape exceeds one byte");

        c = value;
      }
      else
      {
        switch (c)
        {
          case 'n':
            c = '\n';
            break;

          case 'r':
            c = '\r';
            break;

          case 't':
            c = '\t';
            break;

          case 'b':
            c = '\b';
            break;

          case 'a':
            c = '\a';
            break;

          case 'f':
            c = '\f';
            break;

          case 'v':
            c = '\v';
            break;

          case 'e':
            c = 0x1b;
            break;

          case '\\':
          case '\'':
          case '"':
          case '?':
            break;

          default:
            fail("Unsupported escape in subtitle map");
        }
      }
    }

    if (n == ARG_CAP)
      fail("Subtitle map field too long");

    out[n++] = (unsigned char)c;
  }

  return n;
}

/* ====================================================================== */
/* Load binary mappings; a missing replacement maps the key to nothing.   */
/* Bracketed keys retain spaces; later entries override earlier mappings. */
/* ====================================================================== */
static void
subtitle_load(const char *path)
{
  struct stat st;

  FILE *f;
  int   fd = open(path, O_RDONLY | O_NONBLOCK);

  if (fd < 0)
    syserr("open subtitle map");

  if (fstat(fd, &st) < 0)
    syserr("fstat subtitle map");

  if (!S_ISREG(st.st_mode))
    fail("Subtitle map must be a regular file");

  if ((log_known && st.st_dev == log_dev && st.st_ino == log_ino)
      || (subtitle_known && st.st_dev == subtitle_dev
          && st.st_ino == subtitle_ino))
    fail("Subtitle map must differ from output files");

  cloexec(fd);
  f = fdopen(fd, "rb");

  if (!f)
    syserr("fdopen subtitle map");

  free_maps();

  /* Read the map one physical line at a time.                      */
  /* Prepending entries makes later source lines win during lookup. */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  for (;;)
  {
    char   line[ARG_CAP], *p, *text;
    size_t n = 0;
    int    c;

    struct subtitle_map *entry;

    while ((c = file_byte(f)) != EOF && c != '\n')
    {
      if (!c)
        fail("NUL in subtitle map source");

      if (n == sizeof(line) - 1)
        fail("Subtitle map line too long");

      line[n++] = (char)c;
    }

    if (n && line[n - 1] == '\r')
      n--;

    line[n] = '\0';
    p       = line;

    while (*p == ' ' || *p == '\t')
      p++;

    if (!*p)
    {
      if (c == EOF)
        break;
      continue;
    }

    /* Find the key boundary without splitting bracketed hex escapes.     */
    /* A key-only line leaves text at NUL, yielding an empty replacement. */
    /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
    text = p;

    while (*text && *text != ' ' && *text != '\t')
    {
      if (*text == '\\' && text[1])
      {
        if ((text[1] == 'u' || text[1] == 'x') && text[2] == '[')
        {
          text += 3;

          while (*text && *text != ']')
            text++;

          if (!*text)
            fail("Missing ] in subtitle map key");

          text++;
        }
        else
          text += 2;
      }
      else
        text++;
    }

    /* Separate the key only when a field separator is present. */
    /* Otherwise, text already points to the empty replacement. */
    /* """""""""""""""""""""""""""""""""""""""""""""""""""""""" */
    if (*text)
    {
      *text++ = '\0';

      while (*text == ' ' || *text == '\t')
        text++;
    }

    entry = calloc(1, sizeof(*entry));

    if (!entry)
      syserr("allocate subtitle map");

    entry->next   = subtitle_maps;
    subtitle_maps = entry;
    entry->length = map_decode(p, entry->key);

    /* Zero replacement length means suppression, not an absent mapping. */
    /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
    entry->text_length = map_decode(text, entry->text);

    if (!entry->length)
      fail("Empty subtitle map key");

    if (c == EOF)
      break;
  }

  if (fclose(f))
    syserr("close subtitle map");

  map_dev   = st.st_dev;
  map_ino   = st.st_ino;
  map_known = 1;
}

/* ======================================================================== */
/* Open SRT lazily; check actual file aliases before truncating the output. */
/* ======================================================================== */
static void
subtitle_open(void)
{
  struct stat st;

  size_t i;
  int    fd, flags = O_WRONLY | O_CREAT | O_NONBLOCK;

  if (subtitle_file)
    return;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  fd = open(subtitle_path, flags, 0644);

  if (fd < 0)
    syserr("open subtitles");

  if (fstat(fd, &st) < 0)
    syserr("fstat subtitles");

  if (!S_ISREG(st.st_mode))
    fail("Subtitles must be a regular file");

  /* Subtitle files are writable by the owner and readable by all.       */
  /* Apply the final mode explicitly so it does not depend on the umask. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (fchmod(fd, 0644) < 0)
    syserr("set subtitle permissions");

  if ((log_known && st.st_dev == log_dev && st.st_ino == log_ino)
      || (map_known && st.st_dev == map_dev && st.st_ino == map_ino))
    fail("Subtitle output must differ from log and map");

  for (i = 0; i < script.depth; ++i)
  {
    struct stat cs;

    if (fstat(fileno(script.files[i]), &cs) < 0)
      syserr("fstat commands");

    if (st.st_dev == cs.st_dev && st.st_ino == cs.st_ino)
      fail("Subtitle output must differ from command files");
  }

  cloexec(fd);

  if (ftruncate(fd, 0) < 0)
    syserr("truncate subtitles");

  subtitle_file = fdopen(fd, "wb");

  if (!subtitle_file)
    syserr("fdopen subtitles");

  subtitle_dev   = st.st_dev;
  subtitle_ino   = st.st_ino;
  subtitle_known = 1;
}

/* ===================================================================== */
/* Write explicitly sized binary subtitle text without reinterpretation. */
/* ===================================================================== */
static void
subtitle_write(const unsigned char *p, size_t n)
{
  if (n && fwrite(p, 1, n, subtitle_file) != n)
    syserr("write subtitle text");
}

/* ============================================================ */
/* Write elapsed milliseconds as an SRT timestamp using PRIu64. */
/* ============================================================ */
static void
subtitle_time(uint64_t ms)
{
  if (fprintf(subtitle_file,
              "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ",%03" PRIu64,
              ms / 3600000,
              ms / 60000 % 60,
              ms / 1000 % 60,
              ms % 1000)
      < 0)
    syserr("write subtitle time");
}

/* ========================================================= */
/* Find an exact binary key. Return a borrowed node or NULL. */
/* ========================================================= */
static struct subtitle_map *
subtitle_match(const unsigned char *p, size_t n)
{
  struct subtitle_map *entry;

  for (entry = subtitle_maps; entry; entry = entry->next)
    if (entry->length == n && !memcmp(entry->key, p, n))
      return entry;

  return NULL;
}

/* ===================================== */
/* Reset subtitle colors and attributes. */
/* ===================================== */
static void
subtitle_sgr_reset(void)
{
  subtitle_color_set = 0;
  subtitle_bold      = 0;
  subtitle_italic    = 0;
  subtitle_underline = 0;
  subtitle_strike    = 0;
}

/* ================================================================= */
/* Convert an xterm 256-color foreground index to an HTML RGB color. */
/* ================================================================= */
static void
subtitle_indexed_color(unsigned index, char output[8])
{
  static const unsigned char basic[16][3] = {
    { 0, 0, 0 },       { 128, 0, 0 },   { 0, 128, 0 },   { 128, 128, 0 },
    { 0, 0, 128 },     { 128, 0, 128 }, { 0, 128, 128 }, { 192, 192, 192 },
    { 128, 128, 128 }, { 255, 0, 0 },   { 0, 255, 0 },   { 255, 255, 0 },
    { 0, 0, 255 },     { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 }
  };

  static const unsigned char level[6] = { 0, 95, 135, 175, 215, 255 };

  unsigned red;
  unsigned green;
  unsigned blue;

  if (index < 16)
  {
    red   = basic[index][0];
    green = basic[index][1];
    blue  = basic[index][2];
  }
  else if (index < 232)
  {
    unsigned cube = index - 16;

    red   = level[cube / 36];
    green = level[cube / 6 % 6];
    blue  = level[cube % 6];
  }
  else
  {
    red = green = blue = 8 + 10 * (index - 232);
  }

  (void)snprintf(output, 8, "#%02X%02X%02X", red, green, blue);
}

/* ======================================================================== */
/* Interpret one complete ANSI SGR unit as persistent subtitle style state. */
/* Reject malformed input before changing any state.                        */
/* ======================================================================== */
static int
subtitle_sgr(const unsigned char *bytes, size_t length)
{
  static const char * const normal[8] = { "#000000", "#FF0000", "#00FF00",
                                          "#FFFF00", "#0000FF", "#FF00FF",
                                          "#00FFFF", "#FFFFFF" };

  static const char * const bright[8] = { "#808080", "#FF0000", "#00FF00",
                                          "#FFFF00", "#0000FF", "#FF00FF",
                                          "#00FFFF", "#FFFFFF" };

  unsigned values[32];
  size_t   count = 0;
  size_t   i     = 2;

  if (length < 3 || bytes[0] != 0x1b || bytes[1] != '['
      || bytes[length - 1] != 'm')
    return 0;

  /* Treat ESC[m as an SGR reset, equivalent here to ESC[0m. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (length == 3)
  {
    subtitle_sgr_reset();
    return 1;
  }

  /* Reject empty fields, trailing separators, overflow and excess values. */
  /* Complete parsing precedes every modification of subtitle style state. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  while (i < length - 1)
  {
    unsigned value = 0;

    if (count == ARRAY_LENGTH(values) || bytes[i] < '0' || bytes[i] > '9')
      return 0;

    do
    {
      unsigned digit = (unsigned)(bytes[i++] - '0');

      if (value > (UINT_MAX - digit) / 10)
        return 0;

      value = value * 10 + digit;
    } while (i < length - 1 && bytes[i] >= '0' && bytes[i] <= '9');

    values[count++] = value;

    if (i == length - 1)
      break;

    if (bytes[i++] != ';' || i == length - 1)
      return 0;
  }

  /* Validate indexed and RGB foreground/background forms atomically. */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""' */
  for (i = 0; i < count; ++i)
  {
    if (values[i] == 38 || values[i] == 48)
    {
      if (i + 1 >= count)
        return 0;

      if (values[i + 1] == 5)
      {
        if (i + 2 >= count || values[i + 2] > 255)
          return 0;
        i += 2;
      }
      else if (values[i + 1] == 2)
      {
        if (i + 4 >= count || values[i + 2] > 255 || values[i + 3] > 255
            || values[i + 4] > 255)
          return 0;
        i += 4;
      }
      else
        return 0;
    }
  }

  /* Apply the sequence only after every parameter has been validated. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  for (i = 0; i < count; ++i)
  {
    unsigned value = values[i];

    switch (value)
    {
      case 0:
        subtitle_sgr_reset();
        break;

      case 1:
        subtitle_bold = 1;
        break;

      case 3:
        subtitle_italic = 1;
        break;

      case 4:
        subtitle_underline = 1;
        break;

      case 9:
        subtitle_strike = 1;
        break;

      case 22:
        subtitle_bold = 0;
        break;

      case 23:
        subtitle_italic = 0;
        break;

      case 24:
        subtitle_underline = 0;
        break;

      case 29:
        subtitle_strike = 0;
        break;

      case 39:
        subtitle_color_set = 0;
        break;

      default:
        if (value >= 30 && value <= 37)
        {
          strcpy(subtitle_color, normal[value - 30]);
          subtitle_color_set = 1;
        }
        else if (value >= 90 && value <= 97)
        {
          strcpy(subtitle_color, bright[value - 90]);
          subtitle_color_set = 1;
        }
        else if (value == 38 && values[i + 1] == 5)
        {
          subtitle_indexed_color(values[i + 2], subtitle_color);
          subtitle_color_set = 1;
          i += 2;
        }
        else if (value == 38 && values[i + 1] == 2)
        {
          (void)snprintf(subtitle_color,
                         sizeof(subtitle_color),
                         "#%02X%02X%02X",
                         values[i + 2],
                         values[i + 3],
                         values[i + 4]);

          subtitle_color_set = 1;
          i += 4;
        }
        else if (value == 48)
        {
          /* Validate background color for the terminal, but do not   */
          /* retain it because the SRT state has no background field. */
          /* """""""""""""""""""""""""""""""""""""""""""""""""""""""" */
          i += values[i + 1] == 5 ? 2 : 4;
        }
        break;
    }
  }

  return 1;
}

static void
subtitle_unit(const unsigned char *bytes, size_t length)
{
  struct subtitle_map *entry;

  uint64_t start;
  size_t   i;

  if (!subtitle_enabled || !length)
    return;

  /* Inside a \k region, consume SGR as subtitle state, not cue text.   */
  /* Outside a \k region, this function returns before interpreting it. */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (subtitle_sgr(bytes, length))
    return;

  /* Check for output before writing the cue index and timestamps. */
  /* An empty exact mapping overrides all per-byte substitutions.  */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  entry = subtitle_match(bytes, length);

  if (entry)
  {
    if (!entry->text_length)
      return;
  }
  else
  {
    for (i = 0; i < length; ++i)
    {
      entry = subtitle_match(bytes + i, 1);

      if (!entry || entry->text_length)
        break;
    }

    if (i == length)
      return;
  }

  if (!subtitle_started)
    fail("Subtitle clock not started");

  /* Apply the signed offset once to the elapsed subtitle time.       */
  /* Clamp backward offsets at zero because SRT has no negative time. */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  {
    uint64_t elapsed = now_ms() - subtitle_origin;

    if (subtitle_offset < 0)
    {
      uint64_t magnitude = (uint64_t)(-subtitle_offset);

      start = elapsed > magnitude ? elapsed - magnitude : 0;
    }
    else
    {
      uint64_t magnitude = (uint64_t)subtitle_offset;

      if (elapsed > UINT64_MAX - magnitude)
        fail("Subtitle time overflow");

      start = elapsed + magnitude;
    }
  }

  if (start > UINT64_MAX - subtitle_duration)
    fail("Subtitle time overflow");

  if (subtitle_index == ULONG_MAX)
    fail("Too many subtitle cues");

  if (fprintf(subtitle_file, "%lu\n", ++subtitle_index) < 0)
    syserr("write subtitle index");

  subtitle_time(start);

  if (fputs(" --> ", subtitle_file) == EOF)
    syserr("write subtitles");

  subtitle_time(start + subtitle_duration);

  if (fputc('\n', subtitle_file) == EOF)
    syserr("write subtitles");

  if (subtitle_color_set)
  {
    static const unsigned char tag[] = "<font color=\"";

    subtitle_write(tag, sizeof(tag) - 1);
    subtitle_write((const unsigned char *)subtitle_color,
                   strlen(subtitle_color));
    subtitle_write((const unsigned char *)"\">", 2);
  }

  if (subtitle_bold)
    subtitle_write((const unsigned char *)"<b>", 3);

  if (subtitle_italic)
    subtitle_write((const unsigned char *)"<i>", 3);

  if (subtitle_underline)
    subtitle_write((const unsigned char *)"<u>", 3);

  if (subtitle_strike)
    subtitle_write((const unsigned char *)"<s>", 3);

  entry = subtitle_match(bytes, length);

  if (entry)
    subtitle_write(entry->text, entry->text_length);
  else
  {
    for (i = 0; i < length; ++i)
    {
      const char *replacement = NULL;

      entry = subtitle_match(bytes + i, 1);

      if (entry)
      {
        subtitle_write(entry->text, entry->text_length);
        continue;
      }

      switch (bytes[i])
      {
        case ' ':
          replacement = "\xe2\x90\xa3";
          break;

        case '\t':
          replacement = "\xe2\x87\xa5";
          break;

        case '\n':
          replacement = "\xe2\x8f\x8e";
          break;

        case '\r':
          replacement = "\xe2\x90\x8d";
          break;

        case '\b':
          replacement = "\xe2\x8c\xab";
          break;

        case 0x1b:
          replacement = "ESC";
          break;

        default:
          break;
      }

      if (replacement)
        subtitle_write((const unsigned char *)replacement, strlen(replacement));
      else
        subtitle_write(bytes + i, 1);
    }
  }

  if (subtitle_strike)
    subtitle_write((const unsigned char *)"</s>", 4);

  if (subtitle_underline)
    subtitle_write((const unsigned char *)"</u>", 4);

  if (subtitle_italic)
    subtitle_write((const unsigned char *)"</i>", 4);

  if (subtitle_bold)
    subtitle_write((const unsigned char *)"</b>", 4);

  if (subtitle_color_set)
    subtitle_write((const unsigned char *)"</font>", 7);

  if (fputs("\n\n", subtitle_file) == EOF || fflush(subtitle_file) == EOF)
    syserr("write subtitles");
}

/* ====================================================================== */
/* Push missing ptem/ldterm modules only on configured STREAMS terminals. */
/* ====================================================================== */
static void
streams_setup(int fd)
{
#if HAVE_STREAMS
  int stream = isastream(fd);

  if (stream < 0)
    syserr("isastream");

  if (stream)
  {
    const char *modules[] = { "ptem", "ldterm" };
    size_t      i;

    for (i = 0; i < 2; ++i)
    {
      int found = ioctl(fd, I_FIND, modules[i]);

      if (found < 0 || (!found && ioctl(fd, I_PUSH, modules[i]) < 0))
        syserr("STREAMS terminal modules");
    }
  }
#else
  (void)fd;
#endif
}

/* ==================================================================== */
/* Set PTY geometry; retry on the named slave and preserve ioctl errno. */
/* ==================================================================== */
static void
resize_pty(int master, unsigned w, unsigned h)
{
  struct winsize ws;

  int fd;
  int rc;
  int e;

  memset(&ws, 0, sizeof(ws));

  ws.ws_col = (unsigned short)w;
  ws.ws_row = (unsigned short)h;

  if (ioctl(master, TIOCSWINSZ, &ws) == 0)
    return;

  fd = open(slave_name, O_RDWR | O_NOCTTY);

  if (fd < 0)
    syserr("open slave for resize");

  rc = ioctl(fd, TIOCSWINSZ, &ws);
  e  = errno;
  close(fd);

  if (rc < 0)
  {
    errno = e;
    syserr("TIOCSWINSZ");
  }
}

/* ========================================================================= */
/* Validate one literal scalar; continuations stay in the same command file. */
/* ========================================================================= */
static size_t
literal_utf8(FILE *f, int first, unsigned char *b)
{
  size_t n;
  size_t i;

  unsigned lo = 0x80;
  unsigned hi = 0xbf;

  b[0] = (unsigned char)first;

  if (first < 0x80)
    return 1;

  if (first >= 0xc2 && first <= 0xdf)
    n = 2;

  else if (first >= 0xe0 && first <= 0xef)
  {
    n = 3;

    if (first == 0xe0)
      lo = 0xa0;

    if (first == 0xed)
      hi = 0x9f;
  }
  else if (first >= 0xf0 && first <= 0xf4)
  {
    n = 4;

    if (first == 0xf0)
      lo = 0x90;

    if (first == 0xf4)
      hi = 0x8f;
  }
  else
  {
    fail("Invalid UTF-8 leading byte in script");
    return 0;
  }

  for (i = 1; i < n; ++i)
  {
    int c = file_byte(f);

    if (c == EOF)
      fail("Incomplete UTF-8 literal in script");

    if ((unsigned)c < lo || (unsigned)c > hi)
      fail("Invalid UTF-8 continuation byte in script");

    b[i] = (unsigned char)c;
    lo   = 0x80;
    hi   = 0xbf;
  }

  return n;
}

/* ==================================================================== */
/* Compare two ASCII strings without regard to case.                    */
/* This deliberately avoids locale-dependent color-name interpretation. */
/* ==================================================================== */
static int
ascii_equal(const char *left, const char *right)
{
  while (*left && *right)
  {
    unsigned char a = (unsigned char)*left++;
    unsigned char b = (unsigned char)*right++;

    if (a >= 'A' && a <= 'Z')
      a = (unsigned char)(a - 'A' + 'a');

    if (b >= 'A' && b <= 'Z')
      b = (unsigned char)(b - 'A' + 'a');

    if (a != b)
      return 0;
  }

  return *left == '\0' && *right == '\0';
}

/* ===================================================================== */
/* Return nonzero when text contains needle, ignoring ASCII letter case. */
/* ===================================================================== */
static int
ascii_contains(const char *text, const char *needle)
{
  size_t length = strlen(needle);

  if (!length)
    return 1;

  while (*text)
  {
    size_t i;

    for (i = 0; i < length; ++i)
    {
      unsigned char a = (unsigned char)text[i];
      unsigned char b = (unsigned char)needle[i];

      if (!a)
        return 0;

      if (a >= 'A' && a <= 'Z')
        a = (unsigned char)(a - 'A' + 'a');

      if (b >= 'A' && b <= 'Z')
        b = (unsigned char)(b - 'A' + 'a');

      if (a != b)
        break;
    }

    if (i == length)
      return 1;

    text++;
  }

  return 0;
}

/* ======================================================================= */
/* Use common terminal environment declarations to detect 24-bit color.    */
/* If support is not advertised, fall back conservatively to eight colors. */
/* ======================================================================= */
static int
terminal_truecolor(void)
{
  const char *value;

  value = getenv("COLORTERM");

  if (value
      && (ascii_contains(value, "truecolor") || ascii_contains(value, "24bit")))
    return 1;

  value = getenv("TERM");

  if (value
      && (ascii_contains(value, "truecolor") || ascii_contains(value, "24bit")
          || ascii_contains(value, "direct")))
    return 1;

  return 0;
}

/* ====================================================================== */
/* Map a portable color name to its classic ANSI foreground color number. */
/* ====================================================================== */
static int
ansi_color_name(const char *name)
{
  static const char * const names[8] = { "black", "red",     "green", "yellow",
                                         "blue",  "magenta", "cyan",  "white" };
  size_t                    i;

  for (i = 0; i < ARRAY_LENGTH(names); ++i)
    if (ascii_equal(name, names[i]))
      return 30 + (int)i;

  return -1;
}

/* ======================================================== */
/* Map a symbolic attribute name to its ANSI SGR parameter. */
/* ======================================================== */
static int
ansi_attribute(const char *name)
{
  if (ascii_equal(name, "reset") || ascii_equal(name, "normal"))
    return 0;

  if (ascii_equal(name, "bold") || ascii_equal(name, "bright"))
    return 1;

  if (ascii_equal(name, "light") || ascii_equal(name, "faint")
      || ascii_equal(name, "dim"))
    return 2;

  if (ascii_equal(name, "italic"))
    return 3;

  if (ascii_equal(name, "underline") || ascii_equal(name, "underlined"))
    return 4;

  if (ascii_equal(name, "blink"))
    return 5;

  if (ascii_equal(name, "reverse") || ascii_equal(name, "inverse"))
    return 7;

  if (ascii_equal(name, "hidden") || ascii_equal(name, "conceal"))
    return 8;

  if (ascii_equal(name, "strike") || ascii_equal(name, "strikethrough"))
    return 9;

  return -1;
}

/* ================================== */
/* Convert one hexadecimal RGB digit. */
/* ================================== */
static unsigned
rgb_digit(int c)
{
  int value = hex(c);

  if (value < 0)
    fail("Invalid hexadecimal RGB color");

  return (unsigned)value;
}

/* ======================= */
/* Decode exactly #RRGGBB. */
/* ======================= */
static void
rgb_color(const char *text, unsigned *red, unsigned *green, unsigned *blue)
{
  if (strlen(text) != 7 || text[0] != '#')
    fail("RGB color must have the form #RRGGBB");

  *red = rgb_digit((unsigned char)text[1]) * 16
         + rgb_digit((unsigned char)text[2]);

  *green = rgb_digit((unsigned char)text[3]) * 16
           + rgb_digit((unsigned char)text[4]);

  *blue = rgb_digit((unsigned char)text[5]) * 16
          + rgb_digit((unsigned char)text[6]);
}

/* ======================================================================== */
/* Approximate an RGB color with the nearest classic ANSI color.            */
/* Squared distance avoids floating-point arithmetic and libm dependencies. */
/* ======================================================================== */
static unsigned
rgb_to_ansi8(unsigned red, unsigned green, unsigned blue)
{
  static const unsigned palette[8][3] = { { 0, 0, 0 },     { 255, 0, 0 },
                                          { 0, 255, 0 },   { 255, 255, 0 },
                                          { 0, 0, 255 },   { 255, 0, 255 },
                                          { 0, 255, 255 }, { 255, 255, 255 } };

  unsigned best          = 0;
  uint64_t best_distance = UINT64_MAX;
  unsigned i;

  for (i = 0; i < 8; ++i)
  {
    int64_t dr = (int64_t)red - (int64_t)palette[i][0];
    int64_t dg = (int64_t)green - (int64_t)palette[i][1];
    int64_t db = (int64_t)blue - (int64_t)palette[i][2];

    uint64_t distance = (uint64_t)(dr * dr) + (uint64_t)(dg * dg)
                        + (uint64_t)(db * db);

    if (distance < best_distance)
    {
      best          = i;
      best_distance = distance;
    }
  }

  return 30 + best;
}

/* ====================================================================== */
/* Append one decimal SGR parameter, inserting a separator when required. */
/* ====================================================================== */
static void
sgr_parameter(char *output, size_t capacity, size_t *used, unsigned value)
{
  int rc;

  rc = snprintf(output + *used, capacity - *used, *used ? ";%u" : "%u", value);

  if (rc < 0 || (size_t)rc >= capacity - *used)
    fail("Colour sequence too long");

  *used += (size_t)rc;
}

/* ====================================================== */
/* Return nonzero when text is a nonempty decimal number. */
/* ====================================================== */
static int
decimal_parameter(const char *text)
{
  if (!*text)
    return 0;

  while (*text)
  {
    if (*text < '0' || *text > '9')
      return 0;

    text++;
  }

  return 1;
}

/* ================================================================== */
/* Parse \c[...] into a complete ANSI SGR sequence.                   */
/*                                                                    */
/* Numeric syntax accepts one to three raw SGR parameters.            */
/* Symbolic syntax accepts a color or #RRGGBB followed by attributes. */
/* ================================================================== */
static size_t
ansi_color_sequence(char *argument, unsigned char *output, size_t capacity)
{
  char  *fields[16];
  size_t field_count = 0;
  char  *p;
  char   parameters[ARG_CAP];
  size_t used = 0;
  size_t total;

  unsigned red;
  unsigned green;
  unsigned blue;

  int    name;
  size_t i;

  /* An empty \c[] is the standard SGR reset, ESC[0m. */
  /* """""""""""""""""""""""""""""""""""""""""""""""" */
  if (!*argument)
  {
    if (capacity < 4)
      fail("Colour sequence too long");

    output[0] = 0x1b;
    output[1] = '[';
    output[2] = '0';
    output[3] = 'm';

    return 4;
  }

  /* Split this private directive buffer in place.                           */
  /* Replacing its separators with NUL does not modify command-file storage. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  fields[field_count++] = argument;

  for (p = argument; *p; p++)
  {
    if (*p == ';' || *p == ',')
    {
      *p = '\0';

      if (!p[1])
        fail("Empty color or attribute field");

      if (field_count == ARRAY_LENGTH(fields))
        fail("Too many color attributes");

      fields[field_count++] = p + 1;
    }
  }

  /* A decimal first field selects the raw numeric form.  This preserves */
  /* commands such as \c[31;43;4] without reinterpreting their           */
  /* parameters.                                                         */
  /*                                                                     */
  /* Validate the complete raw numeric SGR before emitting parameters.   */
  /* This includes indexed and RGB foreground/background color forms.    */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (decimal_parameter(fields[0]))
  {
    unsigned long values[16];

    for (i = 0; i < field_count; ++i)
    {
      if (!decimal_parameter(fields[i]))
        fail("Numeric color syntax requires decimal ANSI parameters");

      values[i] = number(fields[i], UINT_MAX);
    }

    for (i = 0; i < field_count; ++i)
    {
      if (values[i] == 38 || values[i] == 48)
      {
        if (i + 1 >= field_count)
          fail("Incomplete ANSI extended color sequence");

        if (values[i + 1] == 5)
        {
          if (i + 2 >= field_count)
            fail("ANSI indexed color requires an index");

          if (values[i + 2] > 255)
            fail("ANSI indexed color exceeds 255");

          i += 2;
        }
        else if (values[i + 1] == 2)
        {
          if (i + 4 >= field_count)
            fail("ANSI true color requires red, green and blue");

          if (values[i + 2] > 255 || values[i + 3] > 255 || values[i + 4] > 255)
            fail("ANSI RGB component exceeds 255");

          i += 4;
        }
        else
          fail("ANSI extended color mode must be 2 or 5");
      }
    }

    for (i = 0; i < field_count; ++i)
      sgr_parameter(parameters, sizeof(parameters), &used, (unsigned)values[i]);
  }
  else
  {
    if (fields[0][0] == '#')
    {
      rgb_color(fields[0], &red, &green, &blue);

      if (terminal_truecolor())
      {
        sgr_parameter(parameters, sizeof(parameters), &used, 38);
        sgr_parameter(parameters, sizeof(parameters), &used, 2);
        sgr_parameter(parameters, sizeof(parameters), &used, red);
        sgr_parameter(parameters, sizeof(parameters), &used, green);
        sgr_parameter(parameters, sizeof(parameters), &used, blue);
      }
      else
        sgr_parameter(parameters,
                      sizeof(parameters),
                      &used,
                      rgb_to_ansi8(red, green, blue));

      i = 1;
    }
    else
    {
      name = ansi_color_name(fields[0]);

      if (name >= 0)
      {
        sgr_parameter(parameters, sizeof(parameters), &used, (unsigned)name);
        i = 1;
      }
      else
      {
        /* Permit an attribute as the first field when no color is needed.   */
        /* Every field, including the first, is then parsed as an attribute. */
        /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
        i = 0;
      }
    }

    for (; i < field_count; ++i)
    {
      int attribute = ansi_attribute(fields[i]);

      if (attribute < 0)
      {
        if (!i)
          fail("Unknown color or attribute");

        fail("Unknown color attribute");
      }

      sgr_parameter(parameters, sizeof(parameters), &used, (unsigned)attribute);
    }
  }

  if (used > capacity - 3)
    fail("Colour sequence too long");

  output[0] = 0x1b;
  output[1] = '[';
  memcpy(output + 2, parameters, used);
  output[used + 2] = 'm';

  total = used + 3;
  return total;
}

/* ===================================================================== */
/* Convert an SGR directive to printable text for Bash ANSI-C quoting or */
/* printf %b.  For example, ESC[35m becomes the text "\\033[35m".        */
/* ===================================================================== */
static size_t
shell_color_sequence(char *argument, unsigned char *output, size_t capacity)
{
  unsigned char binary[ARG_CAP + 3];
  size_t        binary_length;
  size_t        tail_length;

  /* Generate an explicit textual reset independently. This guarantees */
  /* that \c[] always becomes the seven printable bytes "\\033[0m".    */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (!*argument)
  {
    if (capacity < 7)
      fail("Shell color sequence too long");

    output[0] = '\\';
    output[1] = '0';
    output[2] = '3';
    output[3] = '3';
    output[4] = '[';
    output[5] = '0';
    output[6] = 'm';

    return 7;
  }

  binary_length = ansi_color_sequence(argument, binary, sizeof(binary));

  if (!binary_length || binary[0] != 0x1b)
    fail("Internal color sequence is invalid");

  tail_length = binary_length - 1;

  if (capacity < 4 || tail_length > capacity - 4)
    fail("Shell color sequence too long");

  output[0] = '\\';
  output[1] = '0';
  output[2] = '3';
  output[3] = '3';

  memcpy(output + 4, binary + 1, tail_length);

  return 4 + tail_length;
}

/* ========================================================== */
/* Interpret a script action independently of subtitle state. */
/* Stage child bytes.                                         */
/* ========================================================== */
static void
step(int master)
{
  int           c = next_byte();
  unsigned char b[ARG_CAP + 3];
  char          arg[ARG_CAP] = { 0 };
  size_t        n            = 1;
  size_t        unit         = 1;

  subtitle_suppress_unit = 0;

  if (c == EOF)
  {
    if (script.meta || script.control)
      fail("Incomplete modifier");

    return;
  }

  if (c == '\\')
  {
    c = file_byte(script.files[script.depth - 1]);

    if (c == EOF)
      fail("Trailing backslash");

    switch (c)
    {
      case 'k':
        if (script.meta || script.control)
          fail("Subtitle toggle cannot be a modifier operand");

        if (!subtitle_enabled)
          subtitle_open();

        /* Subtitle style state never crosses a \k boundary. */
        /* """"""""""""""""""""""""""""""""""""""""""""""""" */
        subtitle_sgr_reset();
        subtitle_enabled = !subtitle_enabled;
        return;

      case 'm':
        if (script.meta || script.control)
          fail("Subtitle map cannot be a modifier operand");

        argument(arg);

        if (!*arg)
          fail("Empty subtitle map path");

        subtitle_load(arg);
        return;

      case 'M':
        script.meta = 1;
        return;

      case 'C':
        script.control = 1;
        return;

      case 'd':
        /* Emit binary SGR on stdout without sending it through the PTY. */
        /* The sequence affects the display but not the log.             */
        /*                                                               */
        /* During subtitle capture, apply the same SGR sequence to the   */
        /* subtitle style state without adding it to the subtitle text.  */
        /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
        argument(arg);
        display_style_active = *arg != '\0';
        n                    = ansi_color_sequence(arg, b, sizeof(b));

        if (subtitle_enabled && !subtitle_sgr(b, n))
          fail("Internal subtitle color sequence is invalid");

        display_only_unit = 1;
        unit              = n;
        break;

      case 'D':
      {
        unsigned char sgr[ARG_CAP + 3];
        size_t        sgr_length;

        if (script.meta || script.control)
          fail("Subtitle color cannot be a modifier operand");

        argument(arg);

        /* Validate the directive everywhere, but apply it only while */
        /* subtitle capture is enabled.  No bytes reach the display,  */
        /* the PTY, or the log.                                       */
        /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
        sgr_length = ansi_color_sequence(arg, sgr, sizeof(sgr));

        if (subtitle_enabled && !subtitle_sgr(sgr, sgr_length))
          fail("Internal subtitle color sequence is invalid");

        return;
      }

      case '\n':
        script.due = now_ms() + script.delay;
        return;

      case 's':
      case 'S':
      {
        unsigned long ms;

        argument(arg);
        ms = number(arg, (unsigned long)INT_MAX);

        if (c == 's')
        {
          script.delay = ms < 50 ? 50 : ms;
          ms           = script.delay;
        }

        script.due = now_ms() + ms;
        return;
      }

      case 'W':
      {
        char *x;

        unsigned long w;
        unsigned long h;

        argument(arg);
        x = strchr(arg, 'x');

        if (!x)
          fail("Resize syntax is [widthxheight]");

        *x++ = '\0';

        w = number(arg, USHRT_MAX);
        h = number(x, USHRT_MAX);

        if (!w || !h)
          fail("Zero terminal dimension");

        resize_pty(master, (unsigned)w, (unsigned)h);
        script.due = now_ms() + script.delay;
        return;
      }

      case 'R':
        argument(arg);

        if (!*arg)
          fail("Empty include path");

        push_file(arg);
        return;

      case 'u':
        argument(arg);
        n    = hex_sequence(arg, b, 4);
        unit = n;
        break;

      case 'c':
      {
        char subtitle_arg[ARG_CAP];

        argument(arg);

        if (strlen(arg) >= sizeof(subtitle_arg))
          fail("Colour argument too long");

        /* Both conversion functions split their argument in place. */
        /* """""""""""""""""""""""""""""""""""""""""""""""""""""""" */
        strcpy(subtitle_arg, arg);

        /* Always send the same printable Bash representation, whether */
        /* subtitle capture is enabled or not.                         */
        /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
        n = shell_color_sequence(arg, b, sizeof(b));

        /* Inside a \k region, additionally update SRT style state from the */
        /* equivalent binary SGR.  Do not copy the printable shell sequence */
        /* itself into the subtitle file.                                   */
        /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
        if (subtitle_enabled)
        {
          unsigned char sgr[ARG_CAP + 3];
          size_t        sgr_length;

          sgr_length = ansi_color_sequence(subtitle_arg, sgr, sizeof(sgr));

          if (!subtitle_sgr(sgr, sgr_length))
            fail("Internal subtitle color sequence is invalid");

          subtitle_suppress_unit = 1;
        }

        unit = n;
        break;
      }
      case 'a':
        b[0] = '\a';
        break;

      case 'b':
        b[0] = '\b';
        break;

      case 'f':
        b[0] = '\f';
        break;

      case 'v':
        b[0] = '\v';
        break;

      case 'r':
        b[0] = '\r';
        break;

      case 'n':
        b[0] = '\n';
        break;

      case 't':
        b[0] = '\t';
        break;

      case 'e':
        b[0] = 0x1b;
        break;

      case '\\':
      case '\'':
      case '"':
      case '?':
      case '%':
        b[0] = (unsigned char)c;
        break;

      case 'x':
      {
        FILE    *f;
        unsigned value  = 0;
        int      digits = 0, d;

        if (!script.depth)
          fail("Missing hex escape argument");

        f = script.files[script.depth - 1];
        c = file_byte(f);

        if (c == '[')
        {
          if (ungetc(c, f) == EOF)
            fail("ungetc failed");

          argument(arg);
          n    = hex_sequence(arg, b, sizeof(b));
          unit = n;
          break;
        }

        while (c != EOF && (d = hex(c)) >= 0)
        {
          if (value > (UCHAR_MAX - (unsigned)d) / 16)
            fail("Hex escape exceeds one byte");

          value  = value * 16 + (unsigned)d;
          digits = 1;
          c      = file_byte(f);
        }

        if (c != EOF && ungetc(c, f) == EOF)
          fail("ungetc failed");

        if (!digits)
          fail("Empty hex escape");

        b[0] = (unsigned char)value;
        break;
      }

      default:
        if (c >= '0' && c <= '7')
        {
          unsigned value = (unsigned)(c - '0');
          int      j;
          FILE    *f = script.files[script.depth - 1];

          for (j = 1; j < 3; ++j)
          {
            c = file_byte(f);

            if (c < '0' || c > '7')
            {
              if (c != EOF && ungetc(c, f) == EOF)
                fail("ungetc failed");
              break;
            }

            value = value * 8 + (unsigned)(c - '0');
          }

          if (value > UCHAR_MAX)
            fail("Octal escape exceeds one byte");

          b[0] = (unsigned char)value;
        }
        else
          fail("Unknown directive \\%c", c);
        break;
    }
  }
  else
  {
    n    = literal_utf8(script.files[script.depth - 1], c, b);
    unit = n;
  }

  /* Apply modifiers after decoding one complete logical input unit.        */
  /* The unit may be a literal, escape, UTF-8 scalar, or grouped hex bytes. */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (script.control)
  {
    if (n != 1 || b[0] > 0x7f)
      fail("CTRL operand must be one ASCII character");

    b[0]           = b[0] == '?' ? 0x7f : (unsigned char)(b[0] & 0x1f);
    script.control = 0;
  }

  if (script.meta)
  {
    if (n == sizeof(b))
      fail("ALT operand too long");

    memmove(b + 1, b, n);
    b[0] = 0x1b;
    n++;
    unit        = n;
    script.meta = 0;
  }

  memcpy(script.bytes, b, n);

  script.length   = n;
  script.position = 0;
  script.unit     = unit;
  script.pending  = 1;
  script.sent = script.delivered = 0;
}

/* ==================================================================== */
/* Queue one unit after older input drains; bound no-byte parsing work. */
/* ==================================================================== */
static void
schedule_script(int           master,
                struct queue *input,
                struct queue *display,
                int          *timeout)
{
  uint64_t now = now_ms();

  /* Start the next pacing interval after the previous input queue drains. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (script.pending && script.sent
      && !(display_only_unit ? count(display) : count(input)))
  {
    script.sent = 0;
    script.due  = now + script.delay;

    if (script.position == script.length)
    {
      script.pending    = 0;
      display_only_unit = 0;
    }
  }

  /* Bound no-byte directives so relay and signal handling can resume. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (!script.pending && !count(input) && !count(display) && script.depth)
  {
    unsigned budget = 128;

    while (budget-- && script.depth && !script.pending
           && now_ms() >= script.due)
      step(master);

    if (script.depth && !script.pending)
      deadline(timeout, script.due, now_ms());
  }

  /* Stage one unit. UTF-8 literals and grouped hex bytes stay together. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (script.pending && !script.sent && !count(input) && !count(display))
  {
    now = now_ms();

    if (now >= script.due)
    {
      size_t n = script.length - script.position;

      if (n > script.unit)
        n = script.unit;

      append(display_only_unit ? display : input,
             script.bytes + script.position,
             n);

      script.position += n;
      script.queued          = n;
      script.sent            = 1;
      script.delivered       = subtitle_suppress_unit;
      subtitle_suppress_unit = 0;
    }
    else
      deadline(timeout, script.due, now);
  }
}

/* =================================================================== */
/* Save and make the user terminal byte-oriented, not the child slave. */
/* =================================================================== */
static void
raw_terminal(void)
{
  struct termios t;
  int            fd = isatty(0) ? 0 : (isatty(1) ? 1 : -1);

  if (fd < 0)
    return;

  if (tcgetattr(fd, &saved) < 0)
    syserr("tcgetattr");

  terminal = fd;
  t        = saved;
  t.c_iflag &= (tcflag_t) ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR
                            | ICRNL | IXON);
  t.c_oflag &= (tcflag_t)~OPOST;
  t.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t.c_cflag &= (tcflag_t) ~(CSIZE | PARENB);
  t.c_cflag |= CS8;
  t.c_cc[VMIN]  = 1;
  t.c_cc[VTIME] = 0;

  if (set_terminal_mode(fd, TCSANOW, &t) < 0)
    syserr("tcsetattr");
}

/* ===================================================================== */
/* Send native errno bytes and use _exit to avoid parent atexit cleanup. */
/* ===================================================================== */
static PTYREC_NORETURN void
child_error(int fd)
{
  int e = errno;

  (void)write_all_best_effort(fd, (const unsigned char *)&e, sizeof(e));
  _exit(127);
}

/* ======================================================================== */
/* Prefer the XSI PTY interface; use openpty only as a configured fallback. */
/* ======================================================================== */
static void
allocate_pty(int *master, int *slave)
{
  const char *name;

#if HAVE_XSI_PTY
  *master = posix_openpt(O_RDWR | O_NOCTTY);

  if (*master < 0)
    syserr("posix_openpt");

  if (grantpt(*master) < 0)
    syserr("grantpt");

  if (unlockpt(*master) < 0)
    syserr("unlockpt");

  name = ptsname(*master);

  if (!name)
    syserr("ptsname");

  /* ptsname() returns borrowed storage; copy it before further calls. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  slave_name = malloc(strlen(name) + 1);

  if (!slave_name)
    syserr("malloc");

  strcpy(slave_name, name);

  *slave = open(slave_name, O_RDWR | O_NOCTTY);

  if (*slave < 0)
    syserr("open slave");

#elif HAVE_OPENPTY
  if (openpty(master, slave, NULL, NULL, NULL) < 0)
    syserr("openpty");

  name = ttyname(*slave);

  if (!name)
    syserr("ttyname");

  /* ttyname() returns borrowed storage; copy it before further calls. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  slave_name = malloc(strlen(name) + 1);

  if (!slave_name)
    syserr("malloc");

  strcpy(slave_name, name);

#else
#error No configured PTY allocator
#endif

  cloexec(*master);
  cloexec(*slave);
  streams_setup(*slave);
}

/* ========================================= */
/* Fork, establish the controlling terminal, */
/* attach standard streams and exec.         */
/* ========================================= */
static void
launch(int             master,
       int             slave,
       int             logfd,
       int             err[2],
       char          **argv,
       const sigset_t *mask,
       const int      *signals,
       size_t          nsignals)
{
  struct sigaction sa;
  size_t           i;
  int              fd;
  int              tty;

  /* The child reports setup or exec failure through err[1].             */
  /* Successful exec closes that close-on-exec descriptor in the parent. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  child = fork();

  if (child < 0)
    syserr("fork");

  if (child)
    return;

  close(err[0]);
  close(master);
  close(logfd);
  close(wake[0]);
  close(wake[1]);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);

  for (i = 0; i < nsignals; ++i)
    if (sigaction(signals[i], &sa, NULL) < 0)
      child_error(err[1]);

  if (sigaction(SIGPIPE, &sa, NULL) < 0 || setsid() < 0)
    child_error(err[1]);

  fd = open(slave_name, O_RDWR);

  if (fd < 0)
    child_error(err[1]);

  close(slave);
#ifdef TIOCSCTTY
  tty = open("/dev/tty", O_RDWR | O_NOCTTY);

  if (tty < 0 && ioctl(fd, TIOCSCTTY, 0) < 0)
    child_error(err[1]);

  if (tty >= 0)
    close(tty);

#endif
  tty = open("/dev/tty", O_RDWR | O_NOCTTY);
  if (tty < 0)
    child_error(err[1]);

  close(tty);

  if (tcsetpgrp(fd, getpgrp()) < 0)
    child_error(err[1]);

  if (dup2(fd, 0) < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
    child_error(err[1]);

  if (fd > 2)
    close(fd);

  if (sigprocmask(SIG_SETMASK, mask, NULL) < 0)
    child_error(err[1]);

  execvp(argv[0], argv);
  child_error(err[1]);
}

/* ========================================================= */
/* Open the binary log and check command-file identity.      */
/* Truncate regular files while leaving other targets intact. */
/* ========================================================= */
static int
open_log(const char *path)
{
  struct stat st;

  int fd;
  int flags = O_RDWR | O_CREAT | O_NONBLOCK;

#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif

  fd = open(path, flags, 0644);

  if (fd < 0)
    syserr("open log");

  if (fstat(fd, &st) < 0)
    syserr("fstat log");

  /* Regular log files are writable by the owner and readable by all.       */
  /* Do not change the permissions of devices or other non-regular targets. */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  if (S_ISREG(st.st_mode) && fchmod(fd, 0644) < 0)
    syserr("set log permissions");

  if (script.depth)
  {
    struct stat cs;

    if (fstat(fileno(script.files[0]), &cs) < 0)
      syserr("fstat commands");

    if (st.st_dev == cs.st_dev && st.st_ino == cs.st_ino)
      fail("Command file and log must differ");
  }

  log_dev   = st.st_dev;
  log_ino   = st.st_ino;
  log_known = 1;

  /*
   * Truncation is meaningful only for regular files.
   * Devices, pipes, and other writable targets are left unchanged.
   */
  if (S_ISREG(st.st_mode) && ftruncate(fd, 0) < 0)
    syserr("truncate log");

  cloexec(fd);
  return fd;
}

/* ======================================================================= */
/* Print command-line syntax; -d sets subtitle duration, not input pacing. */
/* ======================================================================= */
static void
usage(const char *name)
{
  fprintf(stderr,
          "Usage: %s [-V] [-l log] [-w width] [-h height] "
          "[-i commands] [-s subtitles] [-d duration_ms] [-o offset_ms] "
          "[--] program [args...]\n",
          name);

  exit(EXIT_FAILURE);
}

/* ======================================================= */
/* Run the nonblocking relay. Child delivery never depends */
/* on subtitle toggles.                                    */
/* ======================================================= */
int
main(int argc, char **argv)
{
  int opt;
  int master;
  int slave;
  int logfd;
  int err[2];

  int status       = 0;
  int ready        = 0;
  int stdin_open   = 1;
  int pty_open     = 1;
  int terminating  = 0;
  int drain_failed = 0;

  /* The child reports setup or exec failures by writing errno to err[1]  */
  /* before calling _exit().                                              */
  /* Because the pipe is nonblocking, the parent /* accumulates the bytes */
  /* bytes in exec_error until a complete int has been received.          */
  /* EOF with no bytes means exec() succeeded and the close-on-exec       */
  /* descriptor was closed automatically.                                 */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  int    exec_error = 0;
  size_t error_used = 0;

  size_t i;

  unsigned width  = 80;
  unsigned height = 24;

  const char *path    = NULL;
  const char *logpath = "ptyrec.log";

  struct sigaction sa;

  sigset_t blocked;
  sigset_t oldmask;
  sigset_t waitmask;

  /* Keep child input, child output and display-only SGR independent. */
  /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  struct queue input   = { { 0 }, 0, 0 };
  struct queue output  = { { 0 }, 0, 0 };
  struct queue display = { { 0 }, 0, 0 };

  uint64_t kill_at  = 0;
  uint64_t drain_at = 0;

  int signals[] = { SIGINT,
                    SIGTERM,
                    SIGHUP,
                    SIGCHLD
#ifdef SIGWINCH
                    ,
                    SIGWINCH
#endif
  };
  size_t nsignals = ARRAY_LENGTH(signals);

  for (i = 0; i < 3; ++i)
    if (fcntl((int)i, F_GETFD) < 0)
      fail("Standard descriptors must be open");

  if (atexit(cleanup))
    fail("Cannot register cleanup");

  while ((opt = option(argc, argv)) != -1)
  {
    switch (opt)
    {
      case 'V':
        puts("ptyrec " VERSION);
        return 0;

      case 'l':
        logpath = opt_arg;
        break;

      case 'i':
        path = opt_arg;
        break;

      case 's':
        subtitle_path = opt_arg;
        break;

      case 'd':
        subtitle_duration = number(opt_arg, (unsigned long)INT_MAX);

        if (!subtitle_duration)
          fail("Zero subtitle duration");
        break;

      case 'o':
      {
        const char   *value = opt_arg;
        unsigned long magnitude;
        int           backward = 0;

        /* An optional F or B prefix selects the offset direction. */
        /* A prefix-free value is treated as a forward offset.     */
        /* """"""""""""""""""""""""""""""""""""""""""""""""""""""" */
        if (*value == 'b' || *value == 'B')
        {
          backward = 1;
          value++;
        }
        else if (*value == 'f' || *value == 'F')
          value++;

        magnitude = number(value, (unsigned long)INT_MAX);

        subtitle_offset = backward ? -(int64_t)magnitude : (int64_t)magnitude;
        break;
      }

      case 'w':
        width = (unsigned)number(opt_arg, USHRT_MAX);
        break;

      case 'h':
        height = (unsigned)number(opt_arg, USHRT_MAX);
        break;

      default:
        usage(argv[0]);
        break;
    }
  }

  if (arg_index >= argc || !width || !height)
    usage(argv[0]);

  if (path)
    push_file(path);

  logfd = open_log(logpath);

  allocate_pty(&master, &slave);
  resize_pty(slave, width, height);

  if (pipe(wake) < 0 || pipe(err) < 0)
    syserr("pipe");

  for (i = 0; i < 2; ++i)
  {
    cloexec(wake[i]);
    cloexec(err[i]);

    (void)nonblock(wake[i]);
  }

  (void)nonblock(err[0]);
  sigemptyset(&blocked);

  for (i = 0; i < nsignals; ++i)
    sigaddset(&blocked, signals[i]);

  if (sigprocmask(SIG_BLOCK, &blocked, &oldmask) < 0)
    syserr("block signals");

  waitmask = oldmask;

  for (i = 0; i < nsignals; ++i)
    sigdelset(&waitmask, signals[i]);

  launch(master,
         slave,
         logfd,
         err,
         argv + arg_index,
         &oldmask,
         signals,
         nsignals);

  close(slave);
  close(err[1]);

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);

  for (i = 0; i < nsignals; ++i)
    if (sigaction(signals[i], &sa, NULL) < 0)
      syserr("sigaction");

  sa.sa_handler = SIG_IGN;

  if (sigaction(SIGPIPE, &sa, NULL) < 0)
    syserr("SIGPIPE");

  raw_terminal();

  in_flags  = fcntl(0, F_GETFL);
  out_flags = fcntl(1, F_GETFL);

  if (in_flags < 0 || out_flags < 0)
    syserr("save stream flags");

  (void)nonblock(0);
  (void)nonblock(1);
  (void)nonblock(master);

  script.delay = 50;

  /* Update process state before rebuilding the descriptor sets.   */
  /* Only descriptors whose queues can make progress are selected. */
  /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
  for (;;)
  {
    fd_set r;
    fd_set w;

    int      nfds    = 0;
    int      timeout = -1;
    int      rc;
    uint64_t now = now_ms();
    pid_t    result;

    if (!reaped)
    {
      result = wait_child(child, &status, WNOHANG);

      if (result == child)
      {
        reaped      = 1;
        drain_at    = now + DRAIN_MS;
        input.first = 0;
        input.last  = 0;
      }
      else if (result < 0 && errno != EINTR)
        syserr("waitpid");
    }

    if (stopping && !terminating)
    {
      terminating = 1;
      kill_at     = now + GRACE_MS;

      if (!reaped)
        signal_child((int)stopping);

      input.first = 0;
      input.last  = 0;
    }

    if (!pty_open && !reaped && !terminating)
    {
      terminating = 1;
      kill_at     = now + GRACE_MS;
      signal_child(SIGHUP);
    }

    if (terminating == 1 && !reaped)
    {
      if (now >= kill_at)
      {
        signal_child(SIGKILL);
        terminating = 2;
      }
      else
        deadline(&timeout, kill_at, now);
    }

    if (reaped && terminating)
      deadline(&timeout, drain_at, now);

    if (resized)
    {
      struct winsize ws;
      resized = 0;
#ifdef TIOCGWINSZ
      if (terminal >= 0 && pty_open && ioctl(terminal, TIOCGWINSZ, &ws) == 0
          && ws.ws_col && ws.ws_row)

        resize_pty(master, ws.ws_col, ws.ws_row);
#else
      (void)ws;
#endif
    }

    if (reaped && !pty_open && !count(&display) && !count(&output))
      break;

    if (reaped && terminating && now >= drain_at)
    {
      drain_failed = 1;
      break;
    }

    if (ready && !reaped && !terminating && pty_open)
      schedule_script(master, &input, &display, &timeout);

    /* Watch descriptors that can make progress; queue room limits reads. */
    /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
    FD_ZERO(&r);
    FD_ZERO(&w);

    add_fd(wake[0], &r, &nfds);

    if (err[0] >= 0)
      add_fd(err[0], &r, &nfds);

    if (pty_open && room(&output))
      add_fd(master, &r, &nfds);

    if (pty_open && count(&input))
      add_fd(master, &w, &nfds);

    if (count(&display) || count(&output))
      add_fd(1, &w, &nfds);

    if (ready && stdin_open && !reaped && !terminating && pty_open
        && room(&input) >= 4096)
      add_fd(0, &r, &nfds);

    rc = wait_io(nfds, &r, &w, timeout, &waitmask);

    if (rc < 0)
    {
      if (errno == EINTR)
        continue;

      syserr("select");
    }

    if (FD_ISSET(wake[0], &r))
    {
      unsigned char b[128];

      while (read(wake[0], b, sizeof(b)) > 0)
      {
        ;
      }
    }

    /* Close-on-exec EOF confirms setup success; partial errno is an error. */
    /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
    if (err[0] >= 0 && FD_ISSET(err[0], &r))
    {
      /* The child reports setup or exec failures by writing errno to     */
      /* err[1] before calling _exit().  Because the pipe is nonblocking, */
      /* the parent accumulates the bytes in exec_error until a complete  */
      /* int has been received.  EOF with no bytes means exec() succeeded */
      /* and the close-on-exec descriptor was closed automatically.       */
      /* '''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''' */
      ssize_t n = read(err[0],
                       (unsigned char *)&exec_error + error_used,
                       sizeof(exec_error) - error_used);

      if (n > 0)
      {
        error_used += (size_t)n;

        if (error_used == sizeof(exec_error))
          fail("Child setup/exec: %s", strerror(exec_error));
      }
      else if (!n)
      {
        if (error_used)
          fail("Incomplete child error report");

        close(err[0]);
        err[0] = -1;
        ready  = 1;

        /* Record the real clock origin; apply the user offset once */
        /* when calculating each subtitle cue timestamp.            */
        /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""" */
        subtitle_origin = now_ms();

        subtitle_started = 1;
        script.due       = subtitle_origin + 100;
      }
      else if (!retry_io())
        syserr("child error pipe");
    }

    if (FD_ISSET(1, &w))
    {
      /* Drain display-only SGR before subsequently echoed child input. */
      /* This queue has no log descriptor and never enters the PTY.     */
      /* """""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
      if (count(&display))
        (void)flush(1, &display, -1, 0);
      else
        (void)flush(1, &output, -1, 0);
    }

    /* Log child output on read, independently of short stdout writes. */
    /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
    if (pty_open && FD_ISSET(master, &r))
    {
      size_t  space = room(&output);
      ssize_t n     = read(master, output.data + output.last, space);

      if (n > 0)
      {
        log_bytes(logfd, output.data + output.last, (size_t)n);
        output.last += (size_t)n;
      }
      else if (!n || (n < 0 && errno == EIO))
      {
        if (ready || reaped)
          pty_open = 0;
      }
      else if (!retry_io())
        syserr("read PTY");
    }

    if (pty_open && FD_ISSET(master, &w))
      if (!flush(master, &input, -1, 1))
        pty_open = 0;

    /* Interactive input goes to the child without subtitle mapping. */
    /* """"""""""""""""""""""""""""""""""""""""""""""""""""""""""""" */
    if (FD_ISSET(0, &r))
    {
      unsigned char b[4096];
      ssize_t       n = read(0, b, sizeof(b));

      if (n > 0)
        append(&input, b, (size_t)n);
      else if (!n)
        stdin_open = 0;
      else if (!retry_io())
        syserr("read stdin");
    }
  }

  if (subtitle_file)
  {
    FILE *f       = subtitle_file;
    subtitle_file = NULL;

    if (fclose(f))
      syserr("close subtitles");
  }

  close(master);
  close(logfd);

  if (err[0] >= 0)
    close(err[0]);

  close(wake[0]);
  close(wake[1]);

  wake[0] = -1;
  wake[1] = -1;

  if (drain_failed)
  {
    restore();
    fputs("ptyrec: output drain deadline reached; remaining output discarded\n",
          stderr);
  }

  if (stopping)
    return 128 + (int)stopping;

  if (drain_failed)
    return EXIT_FAILURE;

  if (WIFEXITED(status))
    return WEXITSTATUS(status);

  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);

  return EXIT_FAILURE;
}
