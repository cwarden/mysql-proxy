# SUBDIRS=chapter

EXTRA_DIST = \
	     lua-classes.dot \
	     architecture.dot \
	     architecture-overview.dot \
	     architecture.rst \
	     core.rst \
	     chassis.rst \
	     log-domains.rst \
	     plugins.rst \
	     protocol.rst \
	     tests.rst \
	     scripting.rst \
	     lifecycle.msc

clean-local:
	rm -f *.html

html-local: index.html 

index.html: index.rst scripting.rst protocol.rst
	sphinx-build-2.6 -b html ${srcdir} ${builddir}


