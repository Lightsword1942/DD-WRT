include $(top_srcdir)/src/Common.am

libexec_SCRIPTS	= ext_sql_session_acl
CLEANFILES += ext_sql_session_acl ext_sql_session_acl.8
man_MANS = ext_sql_session_acl.8
EXTRA_DIST= \
	ext_sql_session_acl.8 \
	ext_sql_session_acl.pl.in \
	config.test

ext_sql_session_acl.8: ext_sql_session_acl
	pod2man ext_sql_session_acl ext_sql_session_acl.8

ext_sql_session_acl: ext_sql_session_acl.pl.in
	$(subst_perlshell)
