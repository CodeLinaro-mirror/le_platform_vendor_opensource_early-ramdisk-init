#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([early-ramdisk-init], [1.0])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_CONFIG_SRCDIR([src/init/init.c])
AC_CONFIG_HEADERS([config.h])

AC_PREFIX_DEFAULT([/sbin])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_CHECK_LIB([kmod], [kmod_new])
AC_CHECK_LIB([pthread], [pthread_mutex_init])

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h limits.h stdint.h stdlib.h string.h sys/mount.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_SIZE_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_CHECK_FUNCS([dup2 memset strerror strstr uname])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
