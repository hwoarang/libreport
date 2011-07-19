-include ../../config.mak

pluginslibdir = $(PLUGINS_LIB_DIR)

bin_PROGRAMS = \
    reporter-bugzilla \
    reporter-rhtsupport \
    reporter-kerneloops \
    reporter-upload \
    reporter-mailx \
    reporter-print

pluginsconfdir = $(PLUGINS_CONF_DIR)

reportpluginsconfdir = $(REPORT_PLUGINS_CONF_DIR)

dist_reportpluginsconf_DATA = \
    Bugzilla.conf

eventsdir = $(EVENTS_DIR)

dist_events_DATA = \
    report_Bugzilla.xml \
    report_Bugzilla.conf \
    report_Logger.conf \
    report_Mailx.xml \
    report_RHTSupport.xml \
    report_Kerneloops.xml

@INTLTOOL_XML_RULE@

eventsconfdir = $(EVENTS_CONF_DIR)

dist_eventsconf_DATA = \
    mailx_event.conf \
    print_event.conf \
    rhtsupport_event.conf

MAN_TXT = \
    reporter-print.txt \
    reporter-mailx.txt \
    reporter-bugzilla.txt \
    reporter-kerneloops.txt \
    reporter-upload.txt

# Manual pages are generated from .txt via Docbook
man1_MANS = ${MAN_TXT:%.txt=%.1}

%.1 %.5 %.7: %.xml
	$(XMLTO_SILENT) xmlto man $< 2>&1 | sed '/Note/d'

%.xml: %.txt ../../asciidoc.conf
	$(ASCIIDOC_SILENT) asciidoc --backend=docbook --doctype=manpage --conf-file ../../asciidoc.conf -aabrt_version=$(PACKAGE_VERSION) -o $@ $<


CLEANFILES = $(man1_MANS)

EXTRA_DIST = \
    $(man_MANS) \
    $(MAN_TXT) \
    $(man1_MANS) \
    report_Bugzilla.xml.in \
    report_Bugzilla.conf \
    report_Logger.conf \
    report_Mailx.xml.in \
    report_RHTSupport.xml.in \
    report_Kerneloops.xml.in

$(DESTDIR)/$(DEBUG_INFO_DIR):
	$(mkdir_p) '$@'

reporter_bugzilla_SOURCES = \
    reporter-bugzilla.c rhbz.c rhbz.h
reporter_bugzilla_CPPFLAGS = \
    -I$(srcdir)/../include \
    -I$(srcdir)/../lib \
    -DBIN_DIR=\"$(bindir)\" \
    -DVAR_RUN=\"$(VAR_RUN)\" \
    -DCONF_DIR=\"$(CONF_DIR)\" \
    -DLOCALSTATEDIR='"$(localstatedir)"' \
    -DDEBUG_DUMPS_DIR=\"$(DEBUG_DUMPS_DIR)\" \
    -DDEBUG_INFO_DIR=\"$(DEBUG_INFO_DIR)\" \
    -DPLUGINS_LIB_DIR=\"$(PLUGINS_LIB_DIR)\" \
    -DPLUGINS_CONF_DIR=\"$(REPORT_PLUGINS_CONF_DIR)\" \
    $(GLIB_CFLAGS) \
    $(LIBREPORT_CFLAGS) \
    -D_GNU_SOURCE \
    -Wall -Wwrite-strings
reporter_bugzilla_LDADD = \
    $(GLIB_LIBS) \
    ../lib/libabrt_web.la \
    ../lib/libreport.la

reporter_rhtsupport_SOURCES = \
    abrt_rh_support.h abrt_rh_support.c \
    reporter-rhtsupport.c
reporter_rhtsupport_CPPFLAGS = \
    -I$(srcdir)/../include \
    -I$(srcdir)/../lib \
    -DBIN_DIR=\"$(bindir)\" \
    -DVAR_RUN=\"$(VAR_RUN)\" \
    -DCONF_DIR=\"$(CONF_DIR)\" \
    -DLOCALSTATEDIR='"$(localstatedir)"' \
    -DDEBUG_DUMPS_DIR=\"$(DEBUG_DUMPS_DIR)\" \
    -DDEBUG_INFO_DIR=\"$(DEBUG_INFO_DIR)\" \
    -DPLUGINS_LIB_DIR=\"$(PLUGINS_LIB_DIR)\" \
    -DPLUGINS_CONF_DIR=\"$(REPORT_PLUGINS_CONF_DIR)\" \
    $(GLIB_CFLAGS) \
    $(LIBREPORT_CFLAGS) \
    $(XMLRPC_CFLAGS) $(XMLRPC_CLIENT_CFLAGS) \
    -D_GNU_SOURCE \
    -Wall -Wwrite-strings -Werror
reporter_rhtsupport_LDFLAGS = -ltar
reporter_rhtsupport_LDADD = \
    $(GLIB_LIBS) \
    $(XMLRPC_LIBS) $(XMLRPC_CLIENT_LIBS) \
    ../lib/libabrt_web.la \
    ../lib/libreport.la

reporter_upload_SOURCES = \
    reporter-upload.c
reporter_upload_CPPFLAGS = \
    -I$(srcdir)/../include \
    -I$(srcdir)/../lib \
    -DBIN_DIR=\"$(bindir)\" \
    -DVAR_RUN=\"$(VAR_RUN)\" \
    -DCONF_DIR=\"$(CONF_DIR)\" \
    -DLOCALSTATEDIR='"$(localstatedir)"' \
    -DDEBUG_DUMPS_DIR=\"$(DEBUG_DUMPS_DIR)\" \
    -DDEBUG_INFO_DIR=\"$(DEBUG_INFO_DIR)\" \
    -DPLUGINS_LIB_DIR=\"$(PLUGINS_LIB_DIR)\" \
    -DPLUGINS_CONF_DIR=\"$(REPORT_PLUGINS_CONF_DIR)\" \
    $(GLIB_CFLAGS) \
    $(CURL_CFLAGS) \
    $(LIBREPORT_CFLAGS) \
    -D_GNU_SOURCE \
    -Wall -Wwrite-strings -Werror
reporter_upload_LDFLAGS = -ltar
reporter_upload_LDADD = \
    $(GLIB_LIBS) \
    $(CURL_LIBS) \
    ../lib/libreport.la

reporter_kerneloops_SOURCES = \
    reporter-kerneloops.c
reporter_kerneloops_CPPFLAGS = \
    -I$(srcdir)/../include \
    -I$(srcdir)/../lib \
    -DBIN_DIR=\"$(bindir)\" \
    -DVAR_RUN=\"$(VAR_RUN)\" \
    -DCONF_DIR=\"$(CONF_DIR)\" \
    -DLOCALSTATEDIR='"$(localstatedir)"' \
    -DDEBUG_DUMPS_DIR=\"$(DEBUG_DUMPS_DIR)\" \
    -DDEBUG_INFO_DIR=\"$(DEBUG_INFO_DIR)\" \
    -DPLUGINS_LIB_DIR=\"$(PLUGINS_LIB_DIR)\" \
    -DPLUGINS_CONF_DIR=\"$(REPORT_PLUGINS_CONF_DIR)\" \
    $(GLIB_CFLAGS) \
    $(LIBREPORT_CFLAGS) \
    -D_GNU_SOURCE \
    -Wall -Wwrite-strings -Werror
reporter_kerneloops_LDADD = \
    ../lib/libabrt_web.la \
    ../lib/libreport.la

reporter_mailx_SOURCES = \
    reporter-mailx.c
reporter_mailx_CPPFLAGS = \
    -I$(srcdir)/../libreport/src/include/ -I$(srcdir)/../include \
    -I$(srcdir)/../libreport/src/lib \
    -I$(srcdir)/../lib \
    -DBIN_DIR=\"$(bindir)\" \
    -DVAR_RUN=\"$(VAR_RUN)\" \
    -DCONF_DIR=\"$(CONF_DIR)\" \
    -DLOCALSTATEDIR='"$(localstatedir)"' \
    -DDEBUG_DUMPS_DIR=\"$(DEBUG_DUMPS_DIR)\" \
    -DDEBUG_INFO_DIR=\"$(DEBUG_INFO_DIR)\" \
    -DPLUGINS_LIB_DIR=\"$(PLUGINS_LIB_DIR)\" \
    -DPLUGINS_CONF_DIR=\"$(REPORT_PLUGINS_CONF_DIR)\" \
    $(GLIB_CFLAGS) \
    $(LIBREPORT_CFLAGS) \
    -D_GNU_SOURCE \
    -Wall -Wwrite-strings -Werror
reporter_mailx_LDADD = \
    ../lib/libreport.la

reporter_print_SOURCES = \
    reporter-print.c
reporter_print_CPPFLAGS = \
    -I$(srcdir)/../include \
    -I$(srcdir)/../lib \
    -DBIN_DIR=\"$(bindir)\" \
    -DVAR_RUN=\"$(VAR_RUN)\" \
    -DCONF_DIR=\"$(CONF_DIR)\" \
    -DLOCALSTATEDIR='"$(localstatedir)"' \
    -DDEBUG_DUMPS_DIR=\"$(DEBUG_DUMPS_DIR)\" \
    -DDEBUG_INFO_DIR=\"$(DEBUG_INFO_DIR)\" \
    -DPLUGINS_LIB_DIR=\"$(PLUGINS_LIB_DIR)\" \
    -DPLUGINS_CONF_DIR=\"$(REPORT_PLUGINS_CONF_DIR)\" \
    $(GLIB_CFLAGS) \
    $(LIBREPORT_CFLAGS) \
    -D_GNU_SOURCE \
    -Wall -Wwrite-strings -Werror
reporter_print_LDADD = \
    ../lib/libreport.la

DEFS = -DLOCALEDIR=\"$(localedir)\" @DEFS@