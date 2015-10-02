include Makefile.sources
include $(top_srcdir)/src/gallium/Automake.inc

AM_CPPFLAGS = \
	$(LIBDRM_CFLAGS) \
	$(GALLIUM_CFLAGS)

noinst_LTLIBRARIES = librenderonly.la

librenderonly_la_SOURCES = \
	$(C_SOURCES)
