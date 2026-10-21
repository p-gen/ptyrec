m4_define([my_version], [m4_esyscmd([tr -d '\n' < version])])

AC_PREREQ([2.73])
AC_INIT([ptyrec], [my_version], [BUG-REPORT-ADDRESS])
AC_CONFIG_SRCDIR([ptyrec.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([no-dist-gzip dist-bzip2 foreign -Wall])
AM_SILENT_RULES([yes])

AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC
AC_LANG([C])

AC_MSG_CHECKING([for C99 support])

saved_CFLAGS=$CFLAGS
have_c99=no

for flag in '' -std=c99 -xc99=all -qlanglvl=extc99; do
    CFLAGS="$saved_CFLAGS $flag"

    AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L
# error C99 required
#endif

int main(void)
{
    for (int i = 0; i < 1; ++i) {
    }
    return 0;
}
    ]])], [
        have_c99=yes
        break
    ], [])
done

AS_IF([test "$have_c99" != yes], [
    AC_MSG_ERROR([C99 compiler required])
])

AC_MSG_RESULT([yes])

AC_CHECK_HEADERS([sys/termio.h stropts.h sys/stropts.h pty.h util.h libutil.h])

AC_CHECK_DECLS(
    [posix_openpt, grantpt, unlockpt, ptsname],
    [], [],
    [[
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
    ]]
)

have_xsi_pty=no

AC_LINK_IFELSE(
    [AC_LANG_PROGRAM([[
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef HAVE_DECL_POSIX_OPENPT
# error missing declaration
#endif
#ifndef HAVE_DECL_GRANTPT
# error missing declaration
#endif
#ifndef HAVE_DECL_UNLOCKPT
# error missing declaration
#endif
#ifndef HAVE_DECL_PTSNAME
# error missing declaration
#endif
    ]], [[
        int master;
        master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master >= 0) {
            grantpt(master);
            unlockpt(master);
            ptsname(master);
        }
        return 0;
    ]])],
    [
        have_xsi_pty=yes
        AC_DEFINE([HAVE_XSI_PTY],[1],[Define when the complete XSI PTY interface is usable.])
    ],
    []
)

AS_IF([test "$have_xsi_pty" != yes], [

AC_CHECK_DECLS(
    [openpty],
    [], [],
    [[
#include <sys/types.h>
#include <termios.h>
#include <sys/ioctl.h>
#if defined(HAVE_PTY_H)
# include <pty.h>
#elif defined(HAVE_UTIL_H)
# include <util.h>
#elif defined(HAVE_LIBUTIL_H)
# include <libutil.h>
#endif
    ]]
)

saved_LIBS=$LIBS
have_openpty=no

AC_SEARCH_LIBS([openpty], [util bsd], [
    AC_LINK_IFELSE(
        [AC_LANG_PROGRAM([[
#include <sys/types.h>
#include <stddef.h>
#include <termios.h>
#include <sys/ioctl.h>
#if defined(HAVE_PTY_H)
# include <pty.h>
#elif defined(HAVE_UTIL_H)
# include <util.h>
#elif defined(HAVE_LIBUTIL_H)
# include <libutil.h>
#endif
#ifndef HAVE_DECL_OPENPTY
# error missing declaration
#endif
        ]], [[
            int master, slave;
            return openpty(&master, &slave, NULL, NULL, NULL);
        ]])],
        [
            have_openpty=yes
            AC_DEFINE([HAVE_OPENPTY],[1],[Define when openpty is declared and links.])
        ],
        []
    )
])

AS_IF([test "$have_openpty" != yes], [
    LIBS=$saved_LIBS
    AC_MSG_ERROR([no usable XSI PTY allocator or openpty])
])
])

AC_CHECK_HEADERS([sys/select.h])
AC_CHECK_FUNCS([select])
AS_IF([test "$ac_cv_func_select" != yes], [
    AC_MSG_ERROR([select required for descriptor waits])
])

AC_SEARCH_LIBS([clock_gettime], [rt])

AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <time.h>
]], [[
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t);
]])], [
    AC_DEFINE([HAVE_MONOTONIC], [1],[Define when CLOCK_MONOTONIC links.])
], [])

AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <sys/types.h>
#include <sys/ioctl.h>
#if defined(HAVE_STROPTS_H)
# include <stropts.h>
#elif defined(HAVE_SYS_STROPTS_H)
# include <sys/stropts.h>
#endif
]], [[
    return isastream(0)+ioctl(0, I_FIND, "ptem")+ioctl(0, I_PUSH, "ptem");
]])], [
    AC_DEFINE([HAVE_STREAMS],[1],[Define when STREAMS terminal interfaces link.])
], [])

AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <sys/types.h>
#include <sys/ioctl.h>
#ifdef HAVE_SYS_TERMIO_H
# include <sys/termio.h>
#endif
]], [[
struct winsize w; w.ws_row=24; w.ws_col=80; return ioctl(0,TIOCSWINSZ,&w);
]])], [], [AC_MSG_ERROR([terminal window-size interfaces required])])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
