======
ptyrec
======

-------------------------------------------------
Terminal automation with optional SRT subtitles
-------------------------------------------------

:Author: Pierre Gentile <p.gen.progs@gmail.com>
:Copyright: Copyright 2018 Pierre Gentile
:Manual section: 1
:Manual group: Text Processing

SYNOPSIS
========

::

  ptyrec [-V] [-l log] [-w width] [-h height] [-i commands]
         [-s subtitles] [-d duration_ms] [-o offset_ms]
         [--] program [args...]

DESCRIPTION
===========

``ptyrec`` starts a program in a pseudo-terminal (PTY), relays standard
input, interprets a command file, displays PTY output, and records PTY
output in a binary log.

Scripts are interpreted and sent to the child in the same way whether or
not subtitles are enabled.
While subtitle capture is active, each delivered script unit is also written
to the SRT file, after applying any matching map entry.

OPTIONS
=======

``-V``
  Print the program version and exit.

``-l log``
  Select the binary PTY-output log destination.
  The default value is ``ptyrec.log``.

  Regular files are truncated when opened.
  Other writable targets such as character devices or ``/dev/null``
  are accepted but are not truncated.

``-w width``
  Set the initial terminal width in columns.
  The default value is 80.
  Zero is rejected.

``-h height``
  Set the initial terminal height in rows.
  The default value is 24.
  Zero is rejected.

  This is a height option, not a help option.

``-i commands``
  Read and interpret the specified command file.
  Command files must be regular files.

  Includes are supported through the ``R`` directive.

``-s subtitles``
  Select the SRT output path.
  The default value is ``ptyrec.log``.

  The SRT file is opened lazily when the first enabling ``k`` directive
  is encountered.

``-d duration_ms``
  Set the positive subtitle duration in milliseconds.
  The default value is 300 milliseconds.

  This option does not change script input pacing.

``-o offset_ms``
  Adjust subtitle timestamps by the specified number of milliseconds.

  A value without a prefix, or with an ``f`` or ``F`` prefix, moves
  timestamps forward.
  A ``b`` or ``B`` prefix moves timestamps backward.

  Backward offsets that would produce negative timestamps are clamped to
  zero.

  For example, ``-o b2000`` moves subtitle timestamps backward by two
  seconds.

``--``
  End option processing.
  The remaining arguments specify the child program and its arguments.

Standard file descriptors 0, 1, and 2 must be open when ``ptyrec`` starts.

INPUT, OUTPUT, AND LOGGING
==========================

Standard input is relayed as binary bytes without command interpretation.
The command file is interpreted separately.
Script bytes are normally queued for the child.
The ``\d`` directive is the exception because its color sequence is written
directly to standard output.

PTY output is logged when read and then queued for display.
Sequences produced by ``\d`` bypass the PTY and are not written to the log.
Script input and interactive input are not added separately to the log.
If the child terminal echoes input, that echo arrives as PTY output and is
therefore logged normally.

EOF on standard input simply stops further reads.
It does not send an EOF control character to the child.
The child program's terminal mode determines how input bytes are processed.

The user's terminal is made byte-oriented and is restored during normal
cleanup and handled fatal exits.

COMMAND-FILE SYNTAX
===================

Literal characters
------------------

ASCII literal bytes are delivered as individual units.
Literal non-ASCII UTF-8 is validated and delivered one scalar encoding at a
time.
The parser rejects invalid leading bytes, invalid continuation bytes,
overlong encodings, surrogate code points, out-of-range scalar values, and
truncated literal sequences.

UTF-8 continuation bytes are read from the same command file as their leading
byte.
Raw-byte escapes bypass literal UTF-8 validation.

Byte escapes
------------

The following escapes are supported::

  \a  \b  \f  \v  \r  \n  \t
  \e  \\  \'  \"  \?  \%

The ``\e`` escape produces ESC, byte ``0x1b``.
Octal escapes consume up to three octal digits and must fit in one byte.
Traditional unbracketed ``\x`` escapes consume hexadecimal digits greedily
and must also fit in one byte.

Examples::

  \x18
  \030

Use bracketed hexadecimal syntax when a following literal hexadecimal
character must not be consumed as part of an unbracketed escape.

Grouped raw-byte sequences
--------------------------

Examples::

  \x[e9e4]
  \x[e9 e4]
  \u[c3a9]

Bracketed ``\x`` accepts raw byte pairs as one delivery unit.
The first two examples send bytes ``e9`` and ``e4`` consecutively, without
an inter-byte pacing delay.

Bracketed ``\u`` accepts one to four raw bytes as one delivery unit.
Its argument is not interpreted as a Unicode scalar number.
To send U+00E9 using ``\u``, supply its UTF-8 representation, ``c3a9``, not
``e9``.

Whitespace may separate byte pairs, but it may not split the two digits of a
pair.
Empty, odd-length, invalid, and overlong sequences are rejected.

Pacing and sleeps
-----------------

Examples::

  \s[100]
  \S[1000]

Lowercase ``s`` changes the configured input delay and waits once.
Values below 50 milliseconds are clamped to 50 milliseconds.
Uppercase ``S`` waits once without changing subsequent input pacing.
The default pacing delay is 50 milliseconds.

A backslash followed by a literal line feed is a delay-only action.
An ordinary literal line feed is delivered to the child.

The pacing delay starts after the shared input queue drains.
For grouped ``\x`` and ``\u`` sequences, no pacing delay is inserted between
bytes.
Queue drain means that the write accepted the queued bytes, not that the child
application consumed them.

Terminal geometry
-----------------

Example::

  \W[100x30]

Set the child terminal's width and height, and then wait for the configured
input delay.
Both dimensions must be positive and must fit in the supported ``unsigned
short`` range.
Resize signals from the user's terminal are propagated when usable terminal
geometry is available.

Command-file includes
---------------------

Example::

  \R[commands.inc]

Push another regular command file.
At its EOF, interpretation resumes in the parent file.
Include paths are resolved relative to the process's current working
directory, not to the including file.

The include stack is bounded by ``DEPTH``.
A backslash and its following directive character must belong to the same
file.
A trailing backslash in an included file is an error and cannot consume a
character from the parent file.
Bracketed directive arguments must also remain within one file.

Color sequences sent to the child
---------------------------------

Examples::

  \c[31]
  \c[0]
  \c[]
  \c[red]
  \c[bold]
  \c[bold;underline]
  \c[italic,strike]
  \c[normal]
  \c[magenta;bold]
  \c[#12AB34;italic;underline]
  \c[38;5;196]
  \c[38;2;255;0;0]

``c`` constructs an ANSI Select Graphic Rendition (SGR) representation.
Its terminal representation does not depend on whether subtitles are enabled.
Inside and outside ``k`` regions, the directive sends printable shell escape
text such as ``\\033[31m`` rather than inserting a binary ESC byte directly
into the interactive shell's input editor.

The generated text only becomes a color sequence when the shell context
interprets backslash escapes.
Bash ANSI-C quoting and ``printf %b`` are suitable examples::

  printf $'x\c[red;bold]test\c[]x\\n'
  printf '%b\\n' 'x\c[38;5;196]test\c[]x'

The first example is delivered to Bash as the equivalent of::

  printf $'x\\033[31;1mtest\\033[0mx\\n'

A plain ``echo`` command is not recommended because interpretation of
backslash escapes is shell-dependent.
A single command-file ``\n`` directive produces an actual line feed before
Bash interprets the command.
Use ``\\n`` when the two printable characters backslash and ``n`` must reach
Bash inside ANSI-C quoting or a ``printf`` operand.

An empty argument is an explicit complete reset::

  \c[]    ->    \\033[0m

The following input forms are supported:

* raw decimal SGR parameters separated by semicolons or commas;
* the case-insensitive names ``black``, ``red``, ``green``, ``yellow``,
  ``blue``, ``magenta``, ``cyan``, and ``white``;
* ``#RRGGBB`` colors;
* the symbolic attributes ``reset``, ``normal``, ``bold``, ``bright``,
  ``light``, ``faint``, ``dim``, ``italic``, ``underline``, ``underlined``,
  ``blink``, ``reverse``, ``inverse``, ``hidden``, ``conceal``, ``strike``,
  and ``strikethrough``.

A symbolic attribute may be the first field when no color is required.
When the first symbolic field is a color name or ``#RRGGBB``, later fields
are interpreted as attributes.
Unknown fields are rejected.

When the terminal advertises true-color support through ``COLORTERM`` or
``TERM``, ``#RRGGBB`` is emitted as ``38;2;R;G;B``.
Otherwise, it is approximated with the nearest classic eight-color ANSI
foreground value.

Raw extended-color syntax is validated before delivery:

* ``38;5;N`` and ``48;5;N`` require an index from 0 through 255;
* ``38;2;R;G;B`` and ``48;2;R;G;B`` require three components from 0 through
  255;
* extended-color modes other than 2 and 5 are rejected;
* incomplete indexed or RGB forms are rejected.

Inside an enabled ``k`` region, ``c`` also updates the subtitle style from an
independently generated binary SGR sequence.
The printable ``\\033[...]`` representation is suppressed from SRT text.
The directive itself does not produce a subtitle cue.

Display-only color sequences
-----------------------------

``\d[...]`` uses the same color syntax as ``\c[...]``, but writes the binary
ANSI SGR sequence directly to standard output instead of sending it to the
child.
The sequence affects the display but does not appear in the PTY log.

Examples::

  \d[red]
  \d[blue;bold]
  \d[#12AB34;italic]
  \d[38;5;196]
  \d[]

For example::

  \d[red]text\d[]

If the child echoes its input, ``text`` is displayed in red.
The log contains the echoed text, but not the color sequences generated by
``\d``.

Unlike ``\c``, this directive emits a binary SGR sequence and does not require
shell quoting or ``printf %b``.
An empty argument, ``\d[]``, resets the display with ``ESC[0m``.

While subtitle capture is enabled between ``\k`` directives, ``\d[...]`` also
updates the subtitle style without adding the binary SGR sequence to subtitle
text.
Outside a subtitle-capture region, it affects only the display.

The display sequence is written before the next scripted input unit, so the
color is active when echoed input is shown.
If the program exits with a display style still active, cleanup makes a
best-effort attempt to reset the terminal.

Subtitle-only color sequences
------------------------------

``\D[...]`` uses the same color syntax as ``\c[...]`` and ``\d[...]``, but
changes only the subtitle style.
It is effective only while subtitle capture is enabled between ``\k``
directives.
It does not modify the displayed input stream, enter the PTY, or appear in the
log.
Outside a subtitle-capture region, the argument is still checked, but no
style is changed.

Examples::

  \k
  \D[red]
  subtitle text
  \D[]
  \k

``\D[]`` resets only the subtitle style.
Neither ``\D[...]`` nor ``\D[]`` produces a subtitle cue.

CTRL and ALT modifiers
----------------------

Examples::

  \CA
  \C?
  \Mx

``C`` applies CTRL to the next generated operand, which must be one ASCII
character.
A question-mark operand generates DEL.
``M`` prefixes the next generated operand with ESC and retains the resulting
operand as one unit.
CTRL is applied before ALT when both modifiers are pending.

Modifiers remain pending across actions that generate no bytes until an
operand is produced.
Subtitle toggles, subtitle-only color directives, and map-loading directives
reject pending modifiers.
A modifier without a final operand is rejected at script EOF.

Subtitle toggles and map selection
----------------------------------

Examples::

  \m[keys.map]
  \khello\k

``m`` replaces the active subtitle map.
``k`` toggles subtitle generation.
Neither directive generates child input or adds a pacing delay.
Subtitles start disabled.

An unmatched enabling ``k`` leaves subtitles enabled through the remainder of
the script.
The parser does not require a closing toggle.

SUBTITLE SEMANTICS
==================

Between enabling and disabling ``k`` directives, each delivered child-input
unit also produces one SRT cue.
The ``c``, ``d``, and ``D`` directives can update subtitle style without
producing cue text.
Outside those regions, command interpretation and child delivery are
unchanged, but no subtitle cue is produced.

The ``k`` toggle affects only the additional subtitle copy.
It does not change the child representation of ordinary input or ``c`` color
directives.
A ``d`` directive bypasses the child and writes to the display.
A ``D`` directive bypasses both the child and the display.
Subtitle style state is reset whenever a ``k`` boundary is crossed, preventing
color or attributes from leaking between subtitle regions.

A cue uses elapsed time from exec-pipe EOF, adjusted by ``-o``, and the fixed
duration selected with ``-d``.
The default duration is 300 milliseconds.
Fixed-duration cues may overlap.
A backward offset that would place a cue before zero is clamped to zero.

Map lookup does not match across separate delivery units.
A whole-unit map entry can match grouped ``\x`` or ``\u`` bytes, or one
literal UTF-8 scalar.
Subtitles are generated from command-file input, not from the child
program's output.

The SRT writer supports foreground color, bold, italic, underline, and
strike-through.
It recognises classic colors 30 through 37, bright colors 90 through 97,
true color ``38;2;R;G;B``, indexed color ``38;5;N``, and these resets:

* 0 resets every retained subtitle style;
* 22 disables bold;
* 23 disables italic;
* 24 disables underline;
* 29 disables strike-through;
* 39 resets the foreground color.

Background forms beginning with 48 are validated and delivered where
applicable, but the current SRT state does not retain a background color.
The attributes dim, blink, reverse, and hidden have no corresponding SRT
markup.

MAP-FILE FORMAT
===============

Each nonblank line contains a key token, whitespace, and replacement text.
Leading whitespace is skipped.
Replacement text may contain spaces.
Later duplicate keys take precedence.
CRLF line endings are accepted.

Examples::

  v_space [SPACE]
  \t [TAB]
  \n [ENTER]
  \u[20 09] SPACE_TAB
  \u[c3a9] e acute
  \u18 [CTRL-X]

Bracketed ``\u`` and ``\x`` keys may contain spaces or tabs between byte
pairs.
Map fields accept C byte escapes, raw ``\u`` hexadecimal pairs with optional
brackets, and bracketed ``\x`` pairs.
Unbracketed ``\x`` is a greedy single-byte escape.
The key alias ``v_space`` applies only to keys, not to replacement text.

A literal NUL in a map source line is rejected.
Escape decoding can produce embedded NUL bytes because field lengths are
stored explicitly.
Empty keys, malformed escapes, missing closing brackets, and oversized fields
are rejected.
A missing replacement suppresses subtitle text for that key.

Mapping precedence
------------------

Mapping uses this precedence:

#. An exact map-file match for the complete delivered unit.
#. For an unmatched unit, an explicit map-file match for each byte.
#. The built-in premapping for that byte.
#. The original byte, unchanged.

Built-in premappings
--------------------

Space, byte ``0x20``
  OPEN BOX, encoded as UTF-8 ``e2 90 a3``.

Horizontal tab, byte ``0x09``
  UTF-8 ``e2 87 a5``.

Line feed, byte ``0x0a``
  UTF-8 ``e2 8f 8e``.

Carriage return, byte ``0x0d``
  UTF-8 ``e2 90 8d``.

Backspace, byte ``0x08``
  UTF-8 ``e2 8c ab``.

ESC, byte ``0x1b``
  The three ASCII bytes ``ESC``.

These defaults apply only to the subtitle copy and only when an explicit map
does not override them.
They never replace child input.

TIMING AND RELAY LIMITATIONS
============================

Successful child execution is detected by EOF on the close-on-exec child
error pipe.
This event starts the subtitle clock and an initial 100-millisecond script
wait.
It is not an application-readiness signal.
Add an explicit ``S`` sleep when an application requires additional
initialization time.

Script scheduling and subtitle emission use the shared input queue.
Interactive bytes appended after a script unit can postpone queue drain,
subtitle timestamping, and further script progress.

PTY and standard-output writes retain short-write suffixes and retry transient
errors.
Log and SRT writes are synchronous; stalled file I/O can delay the relay and
handled termination.

PORTABILITY AND RESOURCE HANDLING
=================================

The configuration prefers the XSI pseudo-terminal interface through
``posix_openpt()``, ``grantpt()``, ``unlockpt()``, and ``ptsname()``.
When that complete interface is unavailable, ``openpty()`` may be used as a
configuration-time fallback.
The implementation does not switch allocators at run time after a selected
allocator fails.

After PTY allocation, ``streams_setup()`` pushes the ``ptem`` and ``ldterm``
modules when STREAMS support is configured and the modules are required.

The implementation uses ``select()`` with a self-pipe for signal wakeups.
Monotonic time is used when available; otherwise, wall-clock time is clamped
against backward jumps.
Descriptors outside ``FD_SETSIZE`` are rejected.
Internal descriptors are marked close-on-exec.

Log, SRT, command-file, and active-map aliases are checked using known file
identities before relevant output truncation.
These checks reduce the risk of accidental aliases, but they do not cover
every filesystem race or aliases created after validation.

BUILDING
========

Configure and build with::

  ./build.sh

Alternatively::

  sh -n configure
  ./configure
  make

To get help::

  build.sh --help
  configure --help

Compiler settings may be supplied through the environment::

  CFLAGS='-O2 -std=c99 -Wall -Wextra -Wpedantic' ./configure
  make

INSTALLATION
============

Select the installation prefix while configuring::

  make install

For a staged installation, use::

  make install DESTDIR=/staging/root

``ptyrec`` should not be installed setuid or setgid.

