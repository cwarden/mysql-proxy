pkglib_LTLIBRARIES = libmaster.la
libmaster_la_LDFLAGS  = -export-dynamic -no-undefined -no-version -dynamic
libmaster_la_SOURCES  = master-plugin.c
libmaster_la_LIBADD   = $(EVENT_LIBS) $(GLIB_LIBS) $(GMODULE_LIBS) $(top_builddir)/src/libmysql-proxy.la
libmaster_la_CPPFLAGS = $(MYSQL_CFLAGS) $(GLIB_CFLAGS) $(LUA_CFLAGS) $(GMODULE_CFLAGS) -I$(top_srcdir)/src/

# EXTRA_DIST=CMakeLists.txt

