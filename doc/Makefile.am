CHAPTERS=\
	     architecture.rst \
	     core.rst \
	     chassis.rst \
	     log-domains.rst \
	     plugins.rst \
	     protocol.rst \
	     testing.rst \
	     scripting.rst 

EXTRA_DIST = \
	     lua-classes.dot \
	     architecture.dot \
	     architecture-overview.dot \
	     index.rst \
	     conf.py \
	     ${CHAPTERS}

clean-local:
	rm -f *.html

html-local: index.html 

index.html: index.rst conf.py $(CHAPTERS)
	${SPHINXBUILD} -b html ${srcdir} ${builddir}/html


