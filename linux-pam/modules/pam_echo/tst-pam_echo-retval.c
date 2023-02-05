/*
 * Check pam_echo return values.
 *
 * Copyright (c) 2020 Dmitry V. Levin <ldv@altlinux.org>
 */

#include "test_assert.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <security/pam_appl.h>

#define MODULE_NAME "pam_echo"
#define TEST_NAME "tst-" MODULE_NAME "-retval"

static const char service_file[] = TEST_NAME ".service";
static const char user_name[] = "";
static struct pam_conv conv;

int
main(void)
{
	pam_handle_t *pamh = NULL;
	FILE *fp;
	char cwd[PATH_MAX];

	ASSERT_NE(NULL, getcwd(cwd, sizeof(cwd)));

	/* PAM_SUCCESS -> PAM_SUCCESS, PAM_IGNORE -> PAM_PERM_DENIED */
	ASSERT_NE(NULL, fp = fopen(service_file, "w"));
	ASSERT_LT(0, fprintf(fp, "#%%PAM-1.0\n"
			     "auth required %s/.libs/%s.so\n"
			     "account required %s/.libs/%s.so\n"
			     "password required %s/.libs/%s.so\n"
			     "session required %s/.libs/%s.so\n",
			     cwd, MODULE_NAME,
			     cwd, MODULE_NAME,
			     cwd, MODULE_NAME,
			     cwd, MODULE_NAME));
	ASSERT_EQ(0, fclose(fp));

	ASSERT_EQ(PAM_SUCCESS,
		  pam_start_confdir(service_file, user_name, &conv, ".", &pamh));
	ASSERT_NE(NULL, pamh);
	ASSERT_EQ(PAM_SUCCESS, pam_authenticate(pamh, 0));
	ASSERT_EQ(PAM_PERM_DENIED, pam_setcred(pamh, 0));
	ASSERT_EQ(PAM_SUCCESS, pam_acct_mgmt(pamh, 0));
	ASSERT_EQ(PAM_PERM_DENIED, pam_chauthtok(pamh, 0));
	ASSERT_EQ(PAM_SUCCESS, pam_open_session(pamh, 0));
	ASSERT_EQ(PAM_PERM_DENIED, pam_close_session(pamh, 0));
	ASSERT_EQ(PAM_SUCCESS, pam_end(pamh, 0));
	pamh = NULL;

	/* PAM_SILENT: PAM_IGNORE -> PAM_PERM_DENIED */
	ASSERT_EQ(PAM_SUCCESS,
		  pam_start_confdir(service_file, user_name, &conv, ".", &pamh));
	ASSERT_NE(NULL, pamh);
	ASSERT_EQ(PAM_PERM_DENIED, pam_authenticate(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_PERM_DENIED, pam_setcred(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_PERM_DENIED, pam_acct_mgmt(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_PERM_DENIED, pam_chauthtok(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_PERM_DENIED, pam_open_session(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_PERM_DENIED, pam_close_session(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_SUCCESS, pam_end(pamh, 0));
	pamh = NULL;

	/* PAM_IGNORE -> PAM_SUCCESS */
	ASSERT_NE(NULL, fp = fopen(service_file, "w"));
	ASSERT_LT(0, fprintf(fp, "#%%PAM-1.0\n"
			     "auth required %s/.libs/%s.so\n"
			     "auth required %s/../pam_permit/.libs/pam_permit.so\n"
			     "account required %s/.libs/%s.so\n"
			     "account required %s/../pam_permit/.libs/pam_permit.so\n"
			     "password required %s/.libs/%s.so\n"
			     "password required %s/../pam_permit/.libs/pam_permit.so\n"
			     "session required %s/.libs/%s.so\n"
			     "session required %s/../pam_permit/.libs/pam_permit.so\n",
			     cwd, MODULE_NAME, cwd,
			     cwd, MODULE_NAME, cwd,
			     cwd, MODULE_NAME, cwd,
			     cwd, MODULE_NAME, cwd));
	ASSERT_EQ(0, fclose(fp));

	ASSERT_EQ(PAM_SUCCESS,
		  pam_start_confdir(service_file, user_name, &conv, ".", &pamh));
	ASSERT_NE(NULL, pamh);
	ASSERT_EQ(PAM_SUCCESS, pam_authenticate(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_SUCCESS, pam_setcred(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_SUCCESS, pam_acct_mgmt(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_SUCCESS, pam_chauthtok(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_SUCCESS, pam_open_session(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_SUCCESS, pam_close_session(pamh, PAM_SILENT));
	ASSERT_EQ(PAM_SUCCESS, pam_end(pamh, 0));
	pamh = NULL;

	ASSERT_EQ(0, unlink(service_file));

	return 0;
}
