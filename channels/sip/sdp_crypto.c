/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2006 - 2007, Mikael Magnusson
 *
 * Mikael Magnusson <mikma@users.sourceforge.net>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file sdp_crypto.c
 *
 * \brief SDP Security descriptions
 *
 * Specified in RFC 4568
 *
 * \author Mikael Magnusson <mikma@users.sourceforge.net>
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/options.h"
#include "asterisk/utils.h"
#include "include/sdp_crypto.h"

#define SRTP_MASTER_LEN 30
#define SRTP_MASTERKEY_LEN 16
#define SRTP_MASTERSALT_LEN ((SRTP_MASTER_LEN) - (SRTP_MASTERKEY_LEN))
#define SRTP_MASTER_LEN64 (((SRTP_MASTER_LEN) * 8 + 5) / 6 + 1)

extern struct ast_srtp_res *res_srtp;
extern struct ast_srtp_policy_res *res_srtp_policy;

struct sdp_crypto {
	char *a_crypto;
	unsigned char local_key[SRTP_MASTER_LEN];
	char *tag;
	char local_key64[SRTP_MASTER_LEN64];
	unsigned char remote_key[SRTP_MASTER_LEN];
	char suite[64];
};

static int set_crypto_policy(struct ast_srtp_policy *policy, int suite_val, const unsigned char *master_key, unsigned long ssrc, int inbound);

static struct sdp_crypto *sdp_crypto_alloc(void)
{
	return ast_calloc(1, sizeof(struct sdp_crypto));
}

void sdp_crypto_destroy(struct sdp_crypto *crypto)
{
	ast_free(crypto->a_crypto);
	crypto->a_crypto = NULL;
	ast_free(crypto->tag);
	crypto->tag = NULL;
	ast_free(crypto);
}

struct sdp_crypto *sdp_crypto_setup(void)
{
	struct sdp_crypto *p;
	int key_len;
	unsigned char remote_key[SRTP_MASTER_LEN];

	if (!ast_rtp_engine_srtp_is_registered()) {
		return NULL;
	}

	if (!(p = sdp_crypto_alloc())) {
		return NULL;
	}

	if (res_srtp->get_random(p->local_key, sizeof(p->local_key)) < 0) {
		sdp_crypto_destroy(p);
		return NULL;
	}

	ast_base64encode(p->local_key64, p->local_key, SRTP_MASTER_LEN, sizeof(p->local_key64));

	key_len = ast_base64decode(remote_key, p->local_key64, sizeof(remote_key));

	if (key_len != SRTP_MASTER_LEN) {
		ast_log(LOG_ERROR, "base64 encode/decode bad len %d != %d\n", key_len, SRTP_MASTER_LEN);
		ast_free(p);
		return NULL;
	}

	if (memcmp(remote_key, p->local_key, SRTP_MASTER_LEN)) {
		ast_log(LOG_ERROR, "base64 encode/decode bad key\n");
		ast_free(p);
		return NULL;
	}

	ast_debug(1 , "local_key64 %s len %zu\n", p->local_key64, strlen(p->local_key64));

	return p;
}

static int set_crypto_policy(struct ast_srtp_policy *policy, int suite_val, const unsigned char *master_key, unsigned long ssrc, int inbound)
{
	const unsigned char *master_salt = NULL;

	if (!ast_rtp_engine_srtp_is_registered()) {
		return -1;
	}

	master_salt = master_key + SRTP_MASTERKEY_LEN;
	if (res_srtp_policy->set_master_key(policy, master_key, SRTP_MASTERKEY_LEN, master_salt, SRTP_MASTERSALT_LEN) < 0) {
		return -1;
	}

	if (res_srtp_policy->set_suite(policy, suite_val)) {
		ast_log(LOG_WARNING, "Could not set remote SRTP suite\n");
		return -1;
	}

	res_srtp_policy->set_ssrc(policy, ssrc, inbound);

	return 0;
}

static int sdp_crypto_activate(struct sdp_crypto *p, int suite_val, unsigned char *remote_key, struct ast_rtp_instance *rtp)
{
	struct ast_srtp_policy *local_policy = NULL;
	struct ast_srtp_policy *remote_policy = NULL;
	struct ast_rtp_instance_stats stats = {0,};
	int res = -1;

	if (!ast_rtp_engine_srtp_is_registered()) {
		return -1;
	}

	if (!p) {
		return -1;
	}

	if (!(local_policy = res_srtp_policy->alloc())) {
		return -1;
	}

	if (!(remote_policy = res_srtp_policy->alloc())) {
		goto err;
	}

	if (ast_rtp_instance_get_stats(rtp, &stats, AST_RTP_INSTANCE_STAT_LOCAL_SSRC)) {
		goto err;
	}

	if (set_crypto_policy(local_policy, suite_val, p->local_key, stats.local_ssrc, 0) < 0) {
		goto err;
	}

	if (set_crypto_policy(remote_policy, suite_val, remote_key, 0, 1) < 0) {
		goto err;
	}

	/* Add the SRTP policies */
	if (ast_rtp_instance_add_srtp_policy(rtp, remote_policy, local_policy)) {
		ast_log(LOG_WARNING, "Could not set SRTP policies\n");
		goto err;
	}

	ast_debug(1 , "SRTP policy activated\n");
	res = 0;

err:
	if (local_policy) {
		res_srtp_policy->destroy(local_policy);
	}

	if (remote_policy) {
		res_srtp_policy->destroy(remote_policy);
	}

	return res;
}

int sdp_crypto_process(struct sdp_crypto *p, const char *attr, struct ast_rtp_instance *rtp)
{
	char *str = NULL;
	char *tag = NULL;
	char *suite = NULL;
	char *key_params = NULL;
	char *key_param = NULL;
	char *session_params = NULL;
	char *key_salt = NULL;		/* The actual master key and key salt */
	char *lifetime = NULL;		/* Key lifetime (# of RTP packets) */
	char *mki = NULL;		/* Master Key Index */
	int found = 0;
	int key_len = 0;
	int suite_val = 0;
	unsigned char remote_key[SRTP_MASTER_LEN];
	unsigned int sdeslifetime = 0;

	/* Syntax: from RFC 4568
	 a=crypto:<tag> <crypto-suite> <key-params> [<session-params>]

	for SDES the key-params starts with "inline:"

Example of a=crypto headers:
a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:PS1uQCVeeCFCanVmcjkpPywjNWhcYD0mXXtxaVBR|2^20|1:32

a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:PS1uQCVeeCFCanVmcjkpPywjNWhcYD0mXXtxaVBR|2^20|1:32

THe lifetime can be ignored as this example (also from RFC 4568)
	inline:YUJDZGVmZ2hpSktMbW9QUXJzVHVWd3l6MTIzNDU2|1066:4

There can be multiple keys with different MKI values:

a=crypto:2 F8_128_HMAC_SHA1_80
       inline:MTIzNDU2Nzg5QUJDREUwMTIzNDU2Nzg5QUJjZGVm|2^20|1:4;
       inline:QUJjZGVmMTIzNDU2Nzg5QUJDREUwMTIzNDU2Nzg5|2^20|2:4
       FEC_ORDER=FEC_SRTP

SNOM sends without lifetime or MKI:
a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:H5Yen2gCtRLey/IBGPjHeLLpbnivJDg6IjzvV3vZ

	*/

	if (!ast_rtp_engine_srtp_is_registered()) {
		return -1;
	}

	str = ast_strdupa(attr);

	strsep(&str, ":");
	tag = strsep(&str, " ");
	suite = strsep(&str, " ");
	key_params = strsep(&str, " ");
	session_params = strsep(&str, " ");

	if (!tag || !suite) {
		ast_log(LOG_WARNING, "Unrecognized a=%s", attr);
		return -1;
	}

	if (session_params) {
		ast_log(LOG_WARNING, "Unsupported crypto parameters: %s", session_params);
		return -1;
	}

	if (!strcmp(suite, "AES_CM_128_HMAC_SHA1_80")) {
		suite_val = AST_AES_CM_128_HMAC_SHA1_80;
	} else if (!strcmp(suite, "AES_CM_128_HMAC_SHA1_32")) {
		suite_val = AST_AES_CM_128_HMAC_SHA1_32;
	} else {
		ast_log(LOG_WARNING, "Unsupported crypto suite: %s\n", suite);
		return -1;
	}

	/* Separate multiple key parameters and find one that works. */
	while ((key_param = strsep(&key_params, ";"))) {
		char *method = NULL;
		char *info = NULL;

		method = strsep(&key_param, ":");
		info = strsep(&key_param, ";");

		if (!strcmp(method, "inline")) {
			/* This is a SDES key parameter. */
			key_salt = strsep(&info, "|");

			/* The next one can be either lifetime or MKI */
			lifetime = strsep(&info, "|");
			/* Is this MKI? */
			mki = strchr(lifetime, ':');
			if (mki != NULL) {
				mki = lifetime;
				lifetime = NULL;
			} else {
				mki = strsep(&info, "|");
			}
			
			ast_debug(3, "==> SRTP SDES lifetime %s MKI %s \n", lifetime ? lifetime : "-", mki?mki : "-");

			if (lifetime) {
				if (strlen(lifetime) > 2) {
					if (lifetime[0] == '2' && lifetime[1] == '^') {
						lifetime+=2;
						sdeslifetime = 2 ^ atoi(lifetime);
					} else {
						sdeslifetime = (unsigned int) atoi(lifetime);
					}
				} else {
					/* Decimal lifetime */
					sdeslifetime = (unsigned int) atoi(lifetime);
				}
				ast_log(LOG_NOTICE, "Crypto life time (unsupported): %s Lifetime %hu\n", attr, sdeslifetime);
				continue;
			}

			found = 1;
			break;
		}
	}

	if (!found) {
		ast_log(LOG_NOTICE, "SRTP crypto offer not acceptable\n");
		return -1;
	}

	if ((key_len = ast_base64decode(remote_key, key_salt, sizeof(remote_key))) != SRTP_MASTER_LEN) {
		ast_log(LOG_WARNING, "SRTP descriptions key %d != %d\n", key_len, SRTP_MASTER_LEN);
		return -1;
	}

	if (!memcmp(p->remote_key, remote_key, sizeof(p->remote_key))) {
		ast_debug(1, "SRTP remote key unchanged; maintaining current policy\n");
		return 0;
	}

	/* Set the accepted policy and remote key */
	ast_copy_string(p->suite, suite, sizeof(p->suite));
	memcpy(p->remote_key, remote_key, sizeof(p->remote_key));

	if (sdp_crypto_activate(p, suite_val, remote_key, rtp) < 0) {
		return -1;
	}

	if (!p->tag) {
		ast_log(LOG_DEBUG, "Accepting crypto tag %s\n", tag);
		p->tag = ast_strdup(tag);
		if (!p->tag) {
			ast_log(LOG_ERROR, "Could not allocate memory for tag\n");
			return -1;
		}
	}

	/* Finally, rebuild the crypto line */
	return sdp_crypto_offer(p);
}

int sdp_crypto_offer(struct sdp_crypto *p)
{
	if (ast_strlen_zero(p->suite)) {
		/* Default crypto offer */
		strcpy(p->suite, "AES_CM_128_HMAC_SHA1_80");
	}

	/* Rebuild the crypto line */
	if (p->a_crypto) {
		ast_free(p->a_crypto);
	}

	if (ast_asprintf(&p->a_crypto, "a=crypto:%s %s inline:%s\r\n",
			 p->tag ? p->tag : "1", p->suite, p->local_key64) == -1) {
			ast_log(LOG_ERROR, "Could not allocate memory for crypto line\n");
		return -1;
	}

	ast_log(LOG_DEBUG, "Crypto line: %s", p->a_crypto);

	return 0;
}

const char *sdp_crypto_attrib(struct sdp_crypto *p)
{
	return p->a_crypto;
}
