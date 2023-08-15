/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*! \file */

#include <stdio.h>

#include <isc/buffer.h>
#include <isc/commandline.h>
#include <isc/fips.h>
#include <isc/mem.h>

#include <dns/dnssec.h>
#include <dns/fixedname.h>
#include <dns/keyvalues.h>
#include <dns/rdataclass.h>
#include <dns/time.h>

#include "dnssectool.h"

const char *program = "dnssec-ksr";

/*
 * Infrastructure
 */
static isc_log_t *lctx = NULL;
static isc_mem_t *mctx = NULL;
const char *engine = NULL;
/*
 * The domain we are working on
 */
static const char *namestr = NULL;
static dns_fixedname_t fname;
static dns_name_t *name = NULL;
/*
 * KSR context
 */
struct ksr_ctx {
	const char *policy;
	const char *configfile;
	const char *keydir;
	dns_keystore_t *keystore;
	isc_stdtime_t now;
	isc_stdtime_t start;
	isc_stdtime_t end;
	bool setstart;
	bool setend;
	/* keygen */
	dns_ttl_t ttl;
	dns_secalg_t alg;
	int size;
	time_t lifetime;
	time_t propagation;
	time_t publishsafety;
	time_t retiresafety;
	time_t signdelay;
	time_t ttlsig;
};
typedef struct ksr_ctx ksr_ctx_t;

/*
 * These are set here for backwards compatibility.
 * They are raised to 2048 in FIPS mode.
 */
static int min_rsa = 1024;
static int min_dh = 128;

#define CHECK(r)                    \
	ret = (r);                  \
	if (ret != ISC_R_SUCCESS) { \
		goto fail;          \
	}

static void
usage(int ret) {
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "    %s options [options] <command> <zone>\n", program);
	fprintf(stderr, "\n");
	fprintf(stderr, "Version: %s\n", PACKAGE_VERSION);
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "    -e <date/offset>: end date\n");
	fprintf(stderr, "    -E <engine>: name of an OpenSSL engine to use\n");
	fprintf(stderr, "    -F: FIPS mode\n");
	fprintf(stderr, "    -i <date/offset>: start date\n");
	fprintf(stderr, "    -K <directory>: write keys into directory\n");
	fprintf(stderr, "    -k <policy>: name of a DNSSEC policy\n");
	fprintf(stderr, "    -l <file>: file with dnssec-policy config\n");
	fprintf(stderr, "    -h: print usage and exit\n");
	fprintf(stderr, "    -v <level>: set verbosity level\n");
	fprintf(stderr, "    -V: print version information\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Commands:\n");
	fprintf(stderr, "    keygen:  pregenerate ZSKs\n");
	fprintf(stderr, "    request: create a Key Signing Request (KSR)\n");
	exit(ret);
}

static void
checkparams(ksr_ctx_t *ksr, const char *command) {
	if (ksr->configfile == NULL) {
		fatal("%s requires a configuration file", command);
	}
	if (ksr->policy == NULL) {
		fatal("%s requires a dnssec-policy", command);
	}
	if (!ksr->setend) {
		fatal("%s requires an end date", command);
	}
	if (!ksr->setstart) {
		ksr->start = ksr->now;
	}
	if (ksr->keydir == NULL) {
		ksr->keydir = ".";
	}
}

static void
getkasp(ksr_ctx_t *ksr, dns_kasp_t **kasp) {
	cfg_parser_t *parser = NULL;
	cfg_obj_t *config = NULL;

	RUNTIME_CHECK(cfg_parser_create(mctx, lctx, &parser) == ISC_R_SUCCESS);
	if (cfg_parse_file(parser, ksr->configfile, &cfg_type_namedconf,
			   &config) != ISC_R_SUCCESS)
	{
		fatal("unable to load dnssec-policy '%s' from '%s'",
		      ksr->policy, ksr->configfile);
	}
	kasp_from_conf(config, mctx, lctx, ksr->policy, ksr->keydir, engine,
		       kasp);
	if (*kasp == NULL) {
		fatal("failed to load dnssec-policy '%s'", ksr->policy);
	}
	if (ISC_LIST_EMPTY(dns_kasp_keys(*kasp))) {
		fatal("dnssec-policy '%s' has no keys configured", ksr->policy);
	}
	cfg_obj_destroy(parser, &config);
	cfg_parser_destroy(&parser);
}

static int
keytag_cmp(const void *k1, const void *k2) {
	dns_dnsseckey_t **key1 = (dns_dnsseckey_t **)k1;
	dns_dnsseckey_t **key2 = (dns_dnsseckey_t **)k2;
	if (dst_key_id((*key1)->key) < dst_key_id((*key2)->key)) {
		return (-1);
	} else if (dst_key_id((*key1)->key) > dst_key_id((*key2)->key)) {
		return (1);
	}
	return (0);
}

static void
get_dnskeys(ksr_ctx_t *ksr, dns_dnsseckeylist_t *keys) {
	dns_dnsseckeylist_t keys_read;
	dns_dnsseckey_t **keys_sorted;
	int i = 0, n = 0;
	isc_result_t ret;

	ISC_LIST_INIT(*keys);
	ISC_LIST_INIT(keys_read);
	ret = dns_dnssec_findmatchingkeys(name, NULL, ksr->keydir, NULL,
					  ksr->now, mctx, &keys_read);
	if (ret != ISC_R_SUCCESS && ret != ISC_R_NOTFOUND) {
		fatal("failed to load existing keys from %s: %s", ksr->keydir,
		      isc_result_totext(ret));
	}
	/* Sort on keytag. */
	for (dns_dnsseckey_t *dk = ISC_LIST_HEAD(keys_read); dk != NULL;
	     dk = ISC_LIST_NEXT(dk, link))
	{
		n++;
	}
	keys_sorted = isc_mem_cget(mctx, n, sizeof(dns_dnsseckey_t *));
	for (dns_dnsseckey_t *dk = ISC_LIST_HEAD(keys_read); dk != NULL;
	     dk = ISC_LIST_NEXT(dk, link), i++)
	{
		keys_sorted[i] = dk;
	}
	qsort(keys_sorted, n, sizeof(dns_dnsseckey_t *), keytag_cmp);
	while (!ISC_LIST_EMPTY(keys_read)) {
		dns_dnsseckey_t *key = ISC_LIST_HEAD(keys_read);
		ISC_LIST_UNLINK(keys_read, key, link);
	}
	/* Save sorted list in 'keys' */
	for (i = 0; i < n; i++) {
		ISC_LIST_APPEND(*keys, keys_sorted[i], link);
	}
	INSIST(ISC_LIST_EMPTY(keys_read));
	isc_mem_cput(mctx, keys_sorted, n, sizeof(dns_dnsseckey_t *));
}

static void
setcontext(ksr_ctx_t *ksr, dns_kasp_t *kasp) {
	ksr->propagation = dns_kasp_zonepropagationdelay(kasp);
	ksr->publishsafety = dns_kasp_publishsafety(kasp);
	ksr->retiresafety = dns_kasp_retiresafety(kasp);
	ksr->signdelay = dns_kasp_signdelay(kasp);
	ksr->ttl = dns_kasp_dnskeyttl(kasp);
	ksr->ttlsig = dns_kasp_zonemaxttl(kasp, true);
}

static void
cleanup(dns_dnsseckeylist_t *keys, dns_kasp_t *kasp) {
	while (!ISC_LIST_EMPTY(*keys)) {
		dns_dnsseckey_t *key = ISC_LIST_HEAD(*keys);
		ISC_LIST_UNLINK(*keys, key, link);
		dst_key_free(&key->key);
		dns_dnsseckey_destroy(mctx, &key);
	}
	dns_kasp_detach(&kasp);
}

static void
progress(int p) {
	char c = '*';
	switch (p) {
	case 0:
		c = '.';
		break;
	case 1:
		c = '+';
		break;
	case 2:
		c = '*';
		break;
	case 3:
		c = ' ';
		break;
	default:
		break;
	}
	(void)putc(c, stderr);
	(void)fflush(stderr);
}

static void
create_zsk(ksr_ctx_t *ksr, dns_kasp_key_t *kaspkey, dns_dnsseckeylist_t *keys,
	   isc_stdtime_t inception, isc_stdtime_t active,
	   isc_stdtime_t *expiration) {
	bool conflict = false;
	bool freekey = false;
	bool show_progress = true;
	char algstr[DNS_SECALG_FORMATSIZE];
	char filename[PATH_MAX + 1];
	char timestr[26]; /* Minimal buf as per ctime_r() spec. */
	dst_key_t *key = NULL;
	int options = (DST_TYPE_PRIVATE | DST_TYPE_PUBLIC | DST_TYPE_STATE);
	isc_buffer_t buf;
	isc_result_t ret;
	isc_stdtime_t prepub;

	isc_stdtime_tostring(inception, timestr, sizeof(timestr));

	/* Check algorithm and size. */
	dns_secalg_format(ksr->alg, algstr, sizeof(algstr));
	if (!dst_algorithm_supported(ksr->alg)) {
		fatal("unsupported algorithm: %s", algstr);
	}
	INSIST(ksr->size >= 0);
	switch (ksr->alg) {
	case DST_ALG_RSASHA1:
	case DST_ALG_NSEC3RSASHA1:
		if (isc_fips_mode()) {
			/* verify-only in FIPS mode */
			fatal("unsupported algorithm: %s", algstr);
		}
		FALLTHROUGH;
	case DST_ALG_RSASHA256:
	case DST_ALG_RSASHA512:
		if (ksr->size != 0 &&
		    (ksr->size < min_rsa || ksr->size > MAX_RSA))
		{
			fatal("RSA key size %d out of range", ksr->size);
		}
		break;
	case DST_ALG_ECDSA256:
		ksr->size = 256;
		break;
	case DST_ALG_ECDSA384:
		ksr->size = 384;
		break;
	case DST_ALG_ED25519:
		ksr->size = 256;
		break;
	case DST_ALG_ED448:
		ksr->size = 456;
		break;
	default:
		show_progress = false;
		break;
	}

	isc_buffer_init(&buf, filename, sizeof(filename) - 1);

	/* Check existing keys. */
	for (dns_dnsseckey_t *dk = ISC_LIST_HEAD(*keys); dk != NULL;
	     dk = ISC_LIST_NEXT(dk, link))
	{
		isc_stdtime_t act = 0, inact = 0;

		if (!dns_kasp_key_match(kaspkey, dk)) {
			continue;
		}
		(void)dst_key_gettime(dk->key, DST_TIME_ACTIVATE, &act);
		(void)dst_key_gettime(dk->key, DST_TIME_INACTIVE, &inact);
		/*
		 * If this key's activation time is set after the inception
		 * time, it is not eligble for the current bundle.
		 */
		if (act > inception) {
			continue;
		}
		/*
		 * If this key's inactive time is set before the inception
		 * time, it is not eligble for the current bundle.
		 */
		if (inact > 0 && inception >= inact) {
			continue;
		}

		/* Found matching existing key. */
		if (verbose > 0 && show_progress) {
			fprintf(stderr,
				"Selecting key pair for bundle %s: ", timestr);
			fflush(stderr);
		}
		key = dk->key;
		*expiration = inact;
		goto output;
	}

	/* No existing keys match. */
	do {
		conflict = false;

		if (verbose > 0 && show_progress) {
			fprintf(stderr,
				"Generating key pair for bundle %s: ", timestr);
		}
		if (ksr->keystore != NULL && ksr->policy != NULL) {
			ret = dns_keystore_keygen(
				ksr->keystore, name, ksr->policy,
				dns_rdataclass_in, mctx, ksr->alg, ksr->size,
				DNS_KEYOWNER_ZONE, &key);
		} else if (show_progress) {
			ret = dst_key_generate(
				name, ksr->alg, ksr->size, 0, DNS_KEYOWNER_ZONE,
				DNS_KEYPROTO_DNSSEC, dns_rdataclass_in, NULL,
				mctx, &key, &progress);
			fflush(stderr);
		} else {
			ret = dst_key_generate(
				name, ksr->alg, ksr->size, 0, DNS_KEYOWNER_ZONE,
				DNS_KEYPROTO_DNSSEC, dns_rdataclass_in, NULL,
				mctx, &key, NULL);
		}

		if (ret != ISC_R_SUCCESS) {
			fatal("failed to generate key %s/%s: %s\n", namestr,
			      algstr, isc_result_totext(ret));
		}

		/* Do not overwrite an existing key. */
		if (key_collision(key, name, ksr->keydir, mctx, NULL)) {
			conflict = true;
			if (verbose > 0) {
				isc_buffer_clear(&buf);
				ret = dst_key_buildfilename(key, 0, ksr->keydir,
							    &buf);
				if (ret == ISC_R_SUCCESS) {
					fprintf(stderr,
						"%s: %s already exists, or "
						"might collide with another "
						"key upon revokation.  "
						"Generating a new key\n",
						program, filename);
				}
			}
			dst_key_free(&key);
		}
	} while (conflict);

	freekey = true;

	/* Set key timing metadata. */
	prepub = ksr->ttl + ksr->publishsafety + ksr->propagation;
	dst_key_setttl(key, ksr->ttl);
	dst_key_setnum(key, DST_NUM_LIFETIME, ksr->lifetime);
	dst_key_setbool(key, DST_BOOL_KSK, false);
	dst_key_setbool(key, DST_BOOL_ZSK, true);
	dst_key_settime(key, DST_TIME_CREATED, ksr->now);
	dst_key_settime(key, DST_TIME_PUBLISH, (active - prepub));
	dst_key_settime(key, DST_TIME_ACTIVATE, active);
	if (ksr->lifetime > 0) {
		isc_stdtime_t inactive = (active + ksr->lifetime);
		isc_stdtime_t remove = ksr->ttlsig + ksr->propagation +
				       ksr->retiresafety + ksr->signdelay;
		dst_key_settime(key, DST_TIME_INACTIVE, inactive);
		dst_key_settime(key, DST_TIME_DELETE, (inactive + remove));
		*expiration = inactive;
	} else {
		*expiration = 0;
	}

	ret = dst_key_tofile(key, options, ksr->keydir);
	if (ret != ISC_R_SUCCESS) {
		char keystr[DST_KEY_FORMATSIZE];
		dst_key_format(key, keystr, sizeof(keystr));
		fatal("failed to write key %s: %s\n", keystr,
		      isc_result_totext(ret));
	}

output:
	isc_buffer_clear(&buf);
	ret = dst_key_buildfilename(key, 0, NULL, &buf);
	if (ret != ISC_R_SUCCESS) {
		fatal("dst_key_buildfilename returned: %s\n",
		      isc_result_totext(ret));
	}
	printf("%s\n", filename);
	fflush(stdout);
	if (freekey) {
		dst_key_free(&key);
	}
}

static isc_stdtime_t
print_dnskey(dns_kasp_key_t *kaspkey, dns_ttl_t ttl, dns_dnsseckeylist_t *keys,
	     isc_stdtime_t inception, isc_stdtime_t *next_inception) {
	bool ksk = dns_kasp_key_ksk(kaspkey);
	bool zsk = dns_kasp_key_zsk(kaspkey);
	char algstr[DNS_SECALG_FORMATSIZE];
	char classstr[10];
	char keystr[DST_KEY_MAXSIZE];
	char pubstr[DST_KEY_MAXTEXTSIZE];
	char rolestr[4];
	char timestr[26]; /* Minimal buf as per ctime_r() spec. */
	dst_key_t *key = NULL;
	isc_stdtime_t next_bundle = *next_inception;

	isc_stdtime_tostring(inception, timestr, sizeof(timestr));
	dns_secalg_format(dns_kasp_key_algorithm(kaspkey), algstr,
			  sizeof(algstr));
	if (ksk && zsk) {
		snprintf(rolestr, sizeof(rolestr), "csk");
	} else if (ksk) {
		snprintf(rolestr, sizeof(rolestr), "ksk");
	} else {
		snprintf(rolestr, sizeof(rolestr), "zsk");
	}

	/* Fetch matching key pair. */
	for (dns_dnsseckey_t *dk = ISC_LIST_HEAD(*keys); dk != NULL;
	     dk = ISC_LIST_NEXT(dk, link))
	{
		dns_rdata_t rdata = DNS_RDATA_INIT;
		isc_buffer_t classbuf;
		isc_buffer_t keybuf;
		isc_buffer_t pubbuf;
		isc_region_t r;
		isc_result_t ret;
		isc_stdtime_t pub = 0, del = 0;

		(void)dst_key_gettime(dk->key, DST_TIME_PUBLISH, &pub);
		(void)dst_key_gettime(dk->key, DST_TIME_DELETE, &del);

		/* Determine next bundle. */
		if (pub > 0 && pub > inception && pub < next_bundle) {
			next_bundle = pub;
		}
		if (del > 0 && del > inception && del < next_bundle) {
			next_bundle = del;
		}
		/* Find matching key. */
		if (!dns_kasp_key_match(kaspkey, dk)) {
			continue;
		}
		if (pub > inception) {
			continue;
		}
		if (del != 0 && inception >= del) {
			continue;
		}
		/* Found matching key pair. */
		key = dk->key;
		/* Print DNSKEY record. */
		isc_buffer_init(&classbuf, classstr, sizeof(classstr));
		isc_buffer_init(&keybuf, keystr, sizeof(keystr));
		isc_buffer_init(&pubbuf, pubstr, sizeof(pubstr));
		CHECK(dst_key_todns(key, &keybuf));
		isc_buffer_usedregion(&keybuf, &r);
		dns_rdata_fromregion(&rdata, dst_key_class(key),
				     dns_rdatatype_dnskey, &r);
		CHECK(dns_rdata_totext(&rdata, (dns_name_t *)NULL, &pubbuf));
		CHECK(dns_rdataclass_totext(dst_key_class(key), &classbuf));
		CHECK(dns_name_print(dst_key_name(key), stdout));
		fprintf(stdout, " %u ", ttl);
		isc_buffer_usedregion(&classbuf, &r);
		if ((unsigned int)fwrite(r.base, 1, r.length, stdout) !=
		    r.length)
		{
			goto fail;
		}
		fprintf(stdout, " DNSKEY ");
		isc_buffer_usedregion(&pubbuf, &r);
		if ((unsigned int)fwrite(r.base, 1, r.length, stdout) !=
		    r.length)
		{
			goto fail;
		}
		fputc('\n', stdout);
		fflush(stdout);
	}
	/* No key pair found. */
	if (key == NULL) {
		fatal("no %s/%s %s key pair found for bundle %s", namestr,
		      algstr, rolestr, timestr);
	}

	return (next_bundle);

fail:
	fatal("failed to print %s/%s %s key pair found for bundle %s", namestr,
	      algstr, rolestr, timestr);
}

static void
keygen(ksr_ctx_t *ksr) {
	dns_kasp_t *kasp = NULL;
	dns_dnsseckeylist_t keys;
	bool noop = true;

	/* Check parameters */
	checkparams(ksr, "keygen");
	/* Get the policy */
	getkasp(ksr, &kasp);
	/* Get existing keys */
	get_dnskeys(ksr, &keys);
	/* Set context */
	setcontext(ksr, kasp);
	/* Key generation */
	for (dns_kasp_key_t *kk = ISC_LIST_HEAD(dns_kasp_keys(kasp));
	     kk != NULL; kk = ISC_LIST_NEXT(kk, link))
	{
		if (dns_kasp_key_ksk(kk)) {
			/* only ZSKs allowed */
			continue;
		}
		ksr->alg = dns_kasp_key_algorithm(kk);
		ksr->lifetime = dns_kasp_key_lifetime(kk);
		ksr->keystore = dns_kasp_key_keystore(kk);
		ksr->size = dns_kasp_key_size(kk);
		noop = false;

		for (isc_stdtime_t inception = ksr->start, act = ksr->start;
		     inception < ksr->end; inception += ksr->lifetime)
		{
			create_zsk(ksr, kk, &keys, inception, act, &act);
			if (ksr->lifetime == 0) {
				/* unlimited lifetime, but not infinite loop */
				break;
			}
		}
	}
	if (noop) {
		fatal("policy '%s' has no zsks", ksr->policy);
	}
	/* Cleanup */
	cleanup(&keys, kasp);
}

static void
request(ksr_ctx_t *ksr) {
	dns_dnsseckeylist_t keys;
	dns_kasp_t *kasp = NULL;
	isc_stdtime_t next = 0;
	isc_stdtime_t inception = 0;

	/* Check parameters */
	checkparams(ksr, "request");
	/* Get the policy */
	getkasp(ksr, &kasp);
	/* Get keys */
	get_dnskeys(ksr, &keys);
	/* Set context */
	setcontext(ksr, kasp);
	/* Create request */
	inception = ksr->start;
	while (inception <= ksr->end) {
		char timestr[26]; /* Minimal buf as per ctime_r() spec. */
		char utc[sizeof("YYYYMMDDHHSSMM")];
		isc_buffer_t b;
		isc_region_t r;
		isc_result_t ret;

		isc_stdtime_tostring(inception, timestr, sizeof(timestr));
		isc_buffer_init(&b, utc, sizeof(utc));
		ret = dns_time32_totext(inception, &b);
		if (ret != ISC_R_SUCCESS) {
			fatal("failed to convert bundle time32 to text: %s",
			      isc_result_totext(ret));
		}
		isc_buffer_usedregion(&b, &r);

		fprintf(stdout, ";; KSR %s - bundle %.*s (%s)\n", namestr,
			(int)r.length, r.base, timestr);

		next = ksr->end + 1;
		for (dns_kasp_key_t *kk = ISC_LIST_HEAD(dns_kasp_keys(kasp));
		     kk != NULL; kk = ISC_LIST_NEXT(kk, link))
		{
			/*
			 * Output the DNSKEY records for the current bundle
			 * that starts at 'inception. The 'next' variable is
			 * updated to the start time of the
			 * next bundle, determined by the earliest publication
			 * or withdrawal of a key that is after the current
			 * inception.
			 */
			next = print_dnskey(kk, ksr->ttl, &keys, inception,
					    &next);
		}
		inception = next;
	}
	/* Cleanup */
	cleanup(&keys, kasp);
}

int
main(int argc, char *argv[]) {
	isc_result_t ret;
	isc_buffer_t buf;
	int ch;
	char *endp;
	bool set_fips_mode = false;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && OPENSSL_API_LEVEL >= 30000
	OSSL_PROVIDER *fips = NULL, *base = NULL;
#endif
	ksr_ctx_t ksr = {
		.now = isc_stdtime_now(),
	};

	isc_mem_create(&mctx);

	isc_commandline_errprint = false;

#define OPTIONS "E:e:Fhi:K:k:l:v:V"
	while ((ch = isc_commandline_parse(argc, argv, OPTIONS)) != -1) {
		switch (ch) {
		case 'e':
			ksr.end = strtotime(isc_commandline_argument, ksr.now,
					    ksr.now, &ksr.setend);
			break;
		case 'E':
			engine = isc_commandline_argument;
			break;
		case 'F':
			set_fips_mode = true;
			break;
		case 'h':
			usage(0);
			break;
		case 'i':
			ksr.start = strtotime(isc_commandline_argument, ksr.now,
					      ksr.now, &ksr.setstart);
			break;
		case 'K':
			ksr.keydir = isc_commandline_argument;
			ret = try_dir(ksr.keydir);
			if (ret != ISC_R_SUCCESS) {
				fatal("cannot open directory %s: %s",
				      ksr.keydir, isc_result_totext(ret));
			}
			break;
		case 'k':
			ksr.policy = isc_commandline_argument;
			break;
		case 'l':
			ksr.configfile = isc_commandline_argument;
			break;
		case 'V':
			version(program);
			break;
		case 'v':
			verbose = strtoul(isc_commandline_argument, &endp, 0);
			if (*endp != '\0') {
				fatal("-v must be followed by a number");
			}
			break;
		default:
			usage(1);
			break;
		}
	}
	argv += isc_commandline_index;
	argc -= isc_commandline_index;

	if (argc != 2) {
		fatal("must provide a command and zone name");
	}

	ret = dst_lib_init(mctx, engine);
	if (ret != ISC_R_SUCCESS) {
		fatal("could not initialize dst: %s", isc_result_totext(ret));
	}

	/*
	 * After dst_lib_init which will set FIPS mode if requested
	 * at build time.  The minumums are both raised to 2048.
	 */
	if (isc_fips_mode()) {
		min_rsa = min_dh = 2048;
	}

	setup_logging(mctx, &lctx);

	if (set_fips_mode) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L && OPENSSL_API_LEVEL >= 30000
		fips = OSSL_PROVIDER_load(NULL, "fips");
		if (fips == NULL) {
			fatal("Failed to load FIPS provider");
		}
		base = OSSL_PROVIDER_load(NULL, "base");
		if (base == NULL) {
			OSSL_PROVIDER_unload(fips);
			fatal("Failed to load base provider");
		}
#endif
		if (!isc_fips_mode()) {
			if (isc_fips_set_mode(1) != ISC_R_SUCCESS) {
				fatal("setting FIPS mode failed");
			}
		}
	}

	/* zone */
	namestr = argv[1];
	name = dns_fixedname_initname(&fname);
	isc_buffer_init(&buf, argv[1], strlen(argv[1]));
	isc_buffer_add(&buf, strlen(argv[1]));
	ret = dns_name_fromtext(name, &buf, dns_rootname, 0, NULL);
	if (ret != ISC_R_SUCCESS) {
		fatal("invalid zone name %s: %s", argv[1],
		      isc_result_totext(ret));
	}

	/* command */
	if (strcmp(argv[0], "keygen") == 0) {
		keygen(&ksr);
	} else if (strcmp(argv[0], "request") == 0) {
		request(&ksr);
	} else {
		fatal("unknown command '%s'", argv[0]);
	}

	exit(0);
}
