
# AM_CFLAGS = -static
AM_CFLAGS = -I$(srcdir)/src/include
AM_LDFLAGS = -T$(srcdir)/tasklet.lds
AUTOMAKE_OPTIONS = subdir-objects
bin_PROGRAMS = early-ramdisk-init
early_ramdisk_init_SOURCES = src/init/init.c
early_ramdisk_init_SOURCES += src/init/fast-modules-load.c
early_ramdisk_init_SOURCES += src/utils/thread_pool.c
early_ramdisk_init_SOURCES += src/utils/strlcpy.c
early_ramdisk_init_SOURCES += src/utils/log.c
early_ramdisk_init_SOURCES += src/utils/tasklet.c
