FORMATS = html pdf

EXTRA_DIST = conf.py woodchuck.rst $(FORMATS)

all: $(FORMATS)

# Everything but the main document.
aux_files = $(srcdir)/org.woodchuck.rst \
	$(srcdir)/org.woodchuck.manager.rst \
	$(srcdir)/org.woodchuck.stream.rst \
	$(srcdir)/org.woodchuck.object.rst \
	$(srcdir)/org.woodchuck.upcall.rst

BUILT_SOURCES = $(aux_files)

VPATH += $(srcdir)/../src

EXTRA_DIST += dbus2rst.py
$(srcdir)/%.rst: %.xml
	python $(srcdir)/dbus2rst.py -d $(builddir) $<

# The builders depend only on the main document.
woodchuck.rst: dbus2rst.py $(aux_files)

install-data-am: $(foreach format,$(FORMATS),install-$(format))

# Standard targets.
pdf: pdf-builder
ps: ps-builder
dvi: ps-builder
html: html-builder

# -W: Turn warnings into errors.
SPHINX_BUILD_OPTS = -W -d build-cache $(srcdir) 

# Known sphinx builders.  See Sphinx documentation for more details,
# in particular: http://sphinx.pocoo.org/invocation.html
#
# To use a particular builder, depend on BUILDER-builder
#
# We also provide pdf-builder, ps-builder and dvi-builder targets to
# generate pdf, ps and dvi output using the latex builder.
SPHINX_TARGETS=html dirhtml singlehtml htmlhelp qthelp devhelp epub \
	latex man text doctest linkcheck

# Extract configuration parameters from the conf.py file.  e.g.:
#  master_doc = $(call sphinx_conf,master_doc)
sphinx_conf = $(shell cd $(srcdir) && python -c 'import conf; print conf.$(1)')

# Things that the user needn't build are built in the source directory.
builders_loc = $(if $(filter $(1),$(FORMATS)), $(srcdir), $(builddir))/$(1)

master_doc = $(call sphinx_conf,master_doc).rst

.PHONY: $(foreach target,$(SPHINX_TARGETS),$(target)-builder)
$(foreach target,$(SPHINX_TARGETS),$(target)-builder): $(master_doc)
	if test x"$(SPHINX_BUILD)" == x:; \
	then \
	  echo "Warning: sphinx-builder was not found." \
	    "Not building $(@:-builder=) documentation."; \
	else \
	  $(SPHINX_BUILD) -b $(@:-builder=) $(SPHINX_BUILD_OPTS) \
	    $(call builders_loc,$(@:-builder=)); \
	fi

latex_file = $(call sphinx_conf,latex_documents[0][1])
pdf_file = $(latex_file:.tex=.pdf)
ps_file = $(latex_file:.tex=.pdf)
dvi_file = $(latex_file:.tex=.dvi)
INDIRECT_FILES = $(pdf_file) $(ps_file) $(dvi_file)

# Create a pdf, ps or dvi file from the generated latex source.
pdf-builder ps-builder dvi-builder: latex-builder
	if test x"$(SPHINX_BUILD)" == x:; \
	then \
	  echo "Warning: sphinx-builder was not found." \
	    "Not building $(@:-builder=) documentation."; \
	else \
	  ( cd latex && $(MAKE) $(AM_MAKEFLAGS) all-$(@:-builder=) ); \
	  $(INSTALL_DATA) -D $(call builders_loc,latex)/$($(@:-builder=)_file) \
	    $(call builders_loc,$(@:-builder=))/$($(@:-builder=)_file); \
	fi

# Install targets for the various builders.  We just copy the
# directory to the data doc directory.
$(foreach target,$(SPHINX_TARGETS),install-$(target)): install-%: %-builder
	find $(call builders_loc,$(@:install-%=%)) -type f| while read -r file; \
	do \
	  if ! echo "$$file" | grep -q -e "$(@:install-%=%)/.buildinfo$$"; \
	  then \
	    $(INSTALL_DATA) -D $$file $(DESTDIR)/$(docdir)/$$file; \
	  fi; \
	done

install-pdf install-ps install-dvi: install-%: %-builder $(DESTDIR)/$(docdir)
	$(INSTALL_DATA) -D \
	  $(call builders_loc,$*)/$($*_file) \
	  $(DESTDIR)/$(docdir)/$($*_file)

maintainer-clean-local:
	-rm -rf $(foreach f,$(FORMATS),$(srcdir)/$f)

# Any thing not in FORMAT was explicitly built by the user.  Remove it.
clean-local:
	-rm -rf $(filter-out $(FORMATS),$(SPHINX_TARGETS)) \
	  build-cache
