/*
 * Copyright 2007-2020 The OpenSSL Project Authors. All Rights Reserved.
 * Copyright Nokia 2007-2019
 * Copyright Siemens AG 2015-2019
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "cmp_local.h"
#include "internal/cryptlib.h"

/* explicit #includes not strictly needed since implied by the above: */
#include <openssl/bio.h>
#include <openssl/cmp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509v3.h>

#include "openssl/cmp_util.h"

#define IS_CREP(t) ((t) == OSSL_CMP_PKIBODY_IP || (t) == OSSL_CMP_PKIBODY_CP \
                        || (t) == OSSL_CMP_PKIBODY_KUP)

/*-
 * Evaluate whether there's an exception (violating the standard) configured for
 * handling negative responses without protection or with invalid protection.
 * Returns 1 on acceptance, 0 on rejection, or -1 on (internal) error.
 */
static int unprotected_exception(const OSSL_CMP_CTX *ctx,
                                 const OSSL_CMP_MSG *rep,
                                 int invalid_protection,
                                 int expected_type /* ignored here */)
{
    int rcvd_type = ossl_cmp_msg_get_bodytype(rep /* may be NULL */);
    const char *msg_type = NULL;

    if (!ossl_assert(ctx != NULL && rep != NULL))
        return -1;

    if (!ctx->unprotectedErrors)
        return 0;

    switch (rcvd_type) {
    case OSSL_CMP_PKIBODY_ERROR:
        msg_type = "error response";
        break;
    case OSSL_CMP_PKIBODY_RP:
        {
            OSSL_CMP_PKISI *si =
                ossl_cmp_revrepcontent_get_pkisi(rep->body->value.rp,
                                                 OSSL_CMP_REVREQSID);

            if (si == NULL)
                return -1;
            if (ossl_cmp_pkisi_get_status(si) == OSSL_CMP_PKISTATUS_rejection)
                msg_type = "revocation response message with rejection status";
            break;
        }
    case OSSL_CMP_PKIBODY_PKICONF:
        msg_type = "PKI Confirmation message";
        break;
    default:
        if (IS_CREP(rcvd_type)) {
            OSSL_CMP_CERTREPMESSAGE *crepmsg = rep->body->value.ip;
            OSSL_CMP_CERTRESPONSE *crep =
                ossl_cmp_certrepmessage_get0_certresponse(crepmsg,
                                                          -1 /* any rid */);

            if (sk_OSSL_CMP_CERTRESPONSE_num(crepmsg->response) > 1)
                return -1;
            /* TODO: handle potentially multiple CertResponses in CertRepMsg */
            if (crep == NULL)
                return -1;
            if (ossl_cmp_pkisi_get_status(crep->status)
                == OSSL_CMP_PKISTATUS_rejection)
                msg_type = "CertRepMessage with rejection status";
        }
    }
    if (msg_type == NULL)
        return 0;
    ossl_cmp_log2(WARN, ctx, "ignoring %s protection of %s",
                  invalid_protection ? "invalid" : "missing", msg_type);
    return 1;
}


/* Save error info from PKIStatusInfo field of a certresponse into ctx */
static int save_statusInfo(OSSL_CMP_CTX *ctx, OSSL_CMP_PKISI *si)
{
    int i;
    OSSL_CMP_PKIFREETEXT *ss;

    if (!ossl_assert(ctx != NULL && si != NULL))
        return 0;

    if ((ctx->status = ossl_cmp_pkisi_get_status(si)) < 0)
        return 0;

    ctx->failInfoCode = 0;
    if (si->failInfo != NULL) {
        for (i = 0; i <= OSSL_CMP_PKIFAILUREINFO_MAX; i++) {
            if (ASN1_BIT_STRING_get_bit(si->failInfo, i))
                ctx->failInfoCode |= (1 << i);
        }
    }

    if (!ossl_cmp_ctx_set0_statusString(ctx, sk_ASN1_UTF8STRING_new_null())
            || (ctx->statusString == NULL))
        return 0;

    ss = si->statusString; /* may be NULL */
    for (i = 0; i < sk_ASN1_UTF8STRING_num(ss); i++) {
        ASN1_UTF8STRING *str = sk_ASN1_UTF8STRING_value(ss, i);

        if (!sk_ASN1_UTF8STRING_push(ctx->statusString, ASN1_STRING_dup(str)))
            return 0;
    }
    return 1;
}

/*-
 * Perform the generic aspects of sending a request and receiving a response.
 * Returns 1 on success and provides the received PKIMESSAGE in *rep.
 * Returns 0 on error.
 * Regardless of success, caller is responsible for freeing *rep (unless NULL).
 */
static int send_receive_check(OSSL_CMP_CTX *ctx, const OSSL_CMP_MSG *req,
                              OSSL_CMP_MSG **rep, int expected_type)
{
    const char *req_type_str =
        ossl_cmp_bodytype_to_string(ossl_cmp_msg_get_bodytype(req));
    const char *expected_type_str = ossl_cmp_bodytype_to_string(expected_type);
    int msg_timeout;
    int bt;
    time_t now = time(NULL);
    int time_left;
    OSSL_CMP_transfer_cb_t transfer_cb = ctx->transfer_cb;

    if (transfer_cb == NULL)
        transfer_cb = OSSL_CMP_MSG_http_perform;

    *rep = NULL;
    msg_timeout = ctx->msg_timeout; /* backup original value */
    if ((IS_CREP(expected_type) || expected_type == OSSL_CMP_PKIBODY_POLLREP)
            && ctx->total_timeout > 0 /* timeout is not infinite */) {
        if (now >= ctx->end_time) {
            CMPerr(0, CMP_R_TOTAL_TIMEOUT);
            return 0;
        }
        if (!ossl_assert(ctx->end_time - time(NULL) < INT_MAX)) {
            /* cannot really happen due to the assignment in do_certreq_seq() */
            CMPerr(0, CMP_R_INVALID_ARGS);
            return 0;
        }
        time_left = (int)(ctx->end_time - now);
        if (ctx->msg_timeout == 0 || time_left < ctx->msg_timeout)
            ctx->msg_timeout = time_left;
    }

    /* should print error queue since transfer_cb may call ERR_clear_error() */
    OSSL_CMP_CTX_print_errors(ctx);

    ossl_cmp_log1(INFO, ctx, "sending %s", req_type_str);

    *rep = (*transfer_cb)(ctx, req);
    ctx->msg_timeout = msg_timeout; /* restore original value */

    if (*rep == NULL) {
        CMPerr(0, CMP_R_TRANSFER_ERROR); /* or receiving response */
        ERR_add_error_data(1, req_type_str);
        ERR_add_error_data(2, ", expected response: ", expected_type_str);
        return 0;
    }

    bt = ossl_cmp_msg_get_bodytype(*rep);
    /*
     * The body type in the 'bt' variable is not yet verified.
     * Still we use this preliminary value already for a progress report because
     * the following msg verification may also produce log entries and may fail.
     */
    ossl_cmp_log1(INFO, ctx, "received %s", ossl_cmp_bodytype_to_string(bt));

    if ((bt = ossl_cmp_msg_check_received(ctx, *rep, unprotected_exception,
                                          expected_type)) < 0)
        return 0;

    if (bt == expected_type
        /* as an answer to polling, there could be IP/CP/KUP: */
            || (IS_CREP(bt) && expected_type == OSSL_CMP_PKIBODY_POLLREP))
        return 1;

    /* received message type is not one of the expected ones (e.g., error) */
    CMPerr(0, bt == OSSL_CMP_PKIBODY_ERROR ? CMP_R_RECEIVED_ERROR :
           CMP_R_UNEXPECTED_PKIBODY); /* in next line for mkerr.pl */

    if (bt != OSSL_CMP_PKIBODY_ERROR) {
        ERR_add_error_data(3, "message type is '",
                           ossl_cmp_bodytype_to_string(bt), "'");
    } else {
        OSSL_CMP_ERRORMSGCONTENT *emc = (*rep)->body->value.error;
        OSSL_CMP_PKISI *si = emc->pKIStatusInfo;
        char buf[OSSL_CMP_PKISI_BUFLEN];

        if (save_statusInfo(ctx, si)
                && OSSL_CMP_CTX_snprint_PKIStatus(ctx, buf, sizeof(buf)) != NULL)
            ERR_add_error_data(1, buf);
        if (emc->errorCode != NULL
                && BIO_snprintf(buf, sizeof(buf), "; errorCode: %ld",
                                ASN1_INTEGER_get(emc->errorCode)) > 0)
            ERR_add_error_data(1, buf);
        if (emc->errorDetails != NULL) {
            char *text = sk_ASN1_UTF8STRING2text(emc->errorDetails, ", ",
                                                 OSSL_CMP_PKISI_BUFLEN - 1);

            if (text != NULL)
                ERR_add_error_data(2, "; errorDetails: ", text);
            OPENSSL_free(text);
        }
        if (ctx->status != OSSL_CMP_PKISTATUS_rejection) {
            CMPerr(0, CMP_R_UNEXPECTED_PKISTATUS);
            if (ctx->status == OSSL_CMP_PKISTATUS_waiting)
                ctx->status = OSSL_CMP_PKISTATUS_rejection;
        }
    }
    return 0;
}

/*-
 * When a 'waiting' PKIStatus has been received, this function is used to
 * poll, which should yield a pollRep or finally a CertRepMessage in ip/cp/kup.
 * On receiving a pollRep, which includes a checkAfter value, it return this
 * value if sleep == 0, else it sleeps as long as indicated and retries.
 *
 * A transaction timeout is enabled if ctx->total_timeout is > 0.
 * In this case polling will continue until the timeout is reached and then
 * polling is done a last time even if this is before the "checkAfter" time.
 *
 * Returns -1 on receiving pollRep if sleep == 0, setting the checkAfter value.
 * Returns 1 on success and provides the received PKIMESSAGE in *rep.
 *           In this case the caller is responsible for freeing *rep.
 * Returns 0 on error (which includes the case that timeout has been reached).
 */
static int poll_for_response(OSSL_CMP_CTX *ctx, int sleep, int rid,
                             OSSL_CMP_MSG **rep, int *checkAfter)
{
    OSSL_CMP_MSG *preq = NULL;
    OSSL_CMP_MSG *prep = NULL;

    ossl_cmp_info(ctx,
                  "received 'waiting' PKIStatus, starting to poll for response");
    *rep = NULL;
    for (;;) {
        /* TODO: handle potentially multiple poll requests per message */
        if ((preq = ossl_cmp_pollReq_new(ctx, rid)) == NULL)
            goto err;

        if (!send_receive_check(ctx, preq, &prep, OSSL_CMP_PKIBODY_POLLREP))
            goto err;

        /* handle potential pollRep */
        if (ossl_cmp_msg_get_bodytype(prep) == OSSL_CMP_PKIBODY_POLLREP) {
            OSSL_CMP_POLLREPCONTENT *prc = prep->body->value.pollRep;
            OSSL_CMP_POLLREP *pollRep = NULL;
            int64_t check_after;
            char str[OSSL_CMP_PKISI_BUFLEN];
            int len;

            /* TODO: handle potentially multiple elements in pollRep */
            if (sk_OSSL_CMP_POLLREP_num(prc) > 1) {
                CMPerr(0, CMP_R_MULTIPLE_RESPONSES_NOT_SUPPORTED);
                goto err;
            }
            pollRep = ossl_cmp_pollrepcontent_get0_pollrep(prc, rid);
            if (pollRep == NULL)
                goto err;

            if (!ASN1_INTEGER_get_int64(&check_after, pollRep->checkAfter)) {
                CMPerr(0, CMP_R_BAD_CHECKAFTER_IN_POLLREP);
                goto err;
            }
            if (check_after < 0 || (uint64_t)check_after
                > (sleep ? ULONG_MAX / 1000 : INT_MAX)) {
                CMPerr(0, CMP_R_CHECKAFTER_OUT_OF_RANGE);
                if (BIO_snprintf(str, OSSL_CMP_PKISI_BUFLEN, "value = %jd",
                                 check_after) >= 0)
                    ERR_add_error_data(1, str);
                goto err;
            }
            if (ctx->total_timeout > 0) { /* timeout is not infinite */
                const int exp = 5; /* expected max time per msg round trip */
                int64_t time_left = (int64_t)(ctx->end_time - exp - time(NULL));

                if (time_left <= 0) {
                    CMPerr(0, CMP_R_TOTAL_TIMEOUT);
                    goto err;
                }
                if (time_left < check_after)
                    check_after = time_left;
                /* poll one last time just when timeout was reached */
            }

            if (pollRep->reason == NULL
                    || (len = BIO_snprintf(str, OSSL_CMP_PKISI_BUFLEN,
                                           " with reason = '")) < 0) {
                *str = '\0';
            } else {
                char *text = sk_ASN1_UTF8STRING2text(pollRep->reason, ", ",
                                                     sizeof(str) - len - 2);

                if (text == NULL
                        || BIO_snprintf(str + len, sizeof(str) - len,
                                        "%s'", text) < 0)
                    *str = '\0';
                OPENSSL_free(text);
            }
            ossl_cmp_log2(INFO, ctx,
                          "received polling response%s; checkAfter = %ld seconds",
                          str, check_after);

            OSSL_CMP_MSG_free(preq);
            preq = NULL;
            OSSL_CMP_MSG_free(prep);
            prep = NULL;
            if (sleep) {
                ossl_sleep((unsigned long)(1000 * check_after));
            } else {
                if (checkAfter != NULL)
                    *checkAfter = (int)check_after;
                return -1; /* exits the loop */
            }
        } else {
            ossl_cmp_info(ctx, "received ip/cp/kup after polling");
            /* any other body type has been rejected by send_receive_check() */
            break;
        }
    }
    if (prep == NULL)
        goto err;

    OSSL_CMP_MSG_free(preq);
    *rep = prep;

    return 1;
 err:
    OSSL_CMP_MSG_free(preq);
    OSSL_CMP_MSG_free(prep);
    return 0;
}

/* Send certConf for IR, CR or KUR sequences and check response */
int ossl_cmp_exchange_certConf(OSSL_CMP_CTX *ctx, int fail_info,
                               const char *txt)
{
    OSSL_CMP_MSG *certConf;
    OSSL_CMP_MSG *PKIconf = NULL;
    int res = 0;

    /* OSSL_CMP_certConf_new() also checks if all necessary options are set */
    if ((certConf = ossl_cmp_certConf_new(ctx, fail_info, txt)) == NULL)
        goto err;

    res = send_receive_check(ctx, certConf, &PKIconf, OSSL_CMP_PKIBODY_PKICONF);

 err:
    OSSL_CMP_MSG_free(certConf);
    OSSL_CMP_MSG_free(PKIconf);
    return res;
}

/* Send given error and check response */
int ossl_cmp_exchange_error(OSSL_CMP_CTX *ctx, int status, int fail_info,
                            const char *txt, int errorCode, const char *details)
{
    OSSL_CMP_MSG *error = NULL;
    OSSL_CMP_PKISI *si = NULL;
    OSSL_CMP_MSG *PKIconf = NULL;
    int res = 0;

    if ((si = OSSL_CMP_STATUSINFO_new(status, fail_info, txt)) == NULL)
        goto err;
    /* ossl_cmp_error_new() also checks if all necessary options are set */
    if ((error = ossl_cmp_error_new(ctx, si, errorCode, details, 0)) == NULL)
        goto err;

    res = send_receive_check(ctx, error, &PKIconf, OSSL_CMP_PKIBODY_PKICONF);

 err:
    OSSL_CMP_MSG_free(error);
    OSSL_CMP_PKISI_free(si);
    OSSL_CMP_MSG_free(PKIconf);
    return res;
}

/*-
 * Retrieve a copy of the certificate, if any, from the given CertResponse.
 * Take into account PKIStatusInfo of CertResponse in ctx, report it on error.
 * Returns NULL if not found or on error.
 */
static X509 *get1_cert_status(OSSL_CMP_CTX *ctx, int bodytype,
                              OSSL_CMP_CERTRESPONSE *crep)
{
    char buf[OSSL_CMP_PKISI_BUFLEN];
    X509 *crt = NULL;
    EVP_PKEY *privkey;

    if (!ossl_assert(ctx != NULL && crep != NULL))
        return NULL;

    privkey = OSSL_CMP_CTX_get0_newPkey(ctx, 1);
    switch (ossl_cmp_pkisi_get_status(crep->status)) {
    case OSSL_CMP_PKISTATUS_waiting:
        ossl_cmp_err(ctx,
                     "received \"waiting\" status for cert when actually aiming to extract cert");
        CMPerr(0, CMP_R_ENCOUNTERED_WAITING);
        goto err;
    case OSSL_CMP_PKISTATUS_grantedWithMods:
        ossl_cmp_warn(ctx, "received \"grantedWithMods\" for certificate");
        crt = ossl_cmp_certresponse_get1_certificate(privkey, crep);
        break;
    case OSSL_CMP_PKISTATUS_accepted:
        crt = ossl_cmp_certresponse_get1_certificate(privkey, crep);
        break;
        /* get all information in case of a rejection before going to error */
    case OSSL_CMP_PKISTATUS_rejection:
        ossl_cmp_err(ctx, "received \"rejection\" status rather than cert");
        CMPerr(0, CMP_R_REQUEST_REJECTED_BY_SERVER);
        goto err;
    case OSSL_CMP_PKISTATUS_revocationWarning:
        ossl_cmp_warn(ctx,
                      "received \"revocationWarning\" - a revocation of the cert is imminent");
        crt = ossl_cmp_certresponse_get1_certificate(privkey, crep);
        break;
    case OSSL_CMP_PKISTATUS_revocationNotification:
        ossl_cmp_warn(ctx,
                      "received \"revocationNotification\" - a revocation of the cert has occurred");
        crt = ossl_cmp_certresponse_get1_certificate(privkey, crep);
        break;
    case OSSL_CMP_PKISTATUS_keyUpdateWarning:
        if (bodytype != OSSL_CMP_PKIBODY_KUR) {
            CMPerr(0, CMP_R_ENCOUNTERED_KEYUPDATEWARNING);
            goto err;
        }
        crt = ossl_cmp_certresponse_get1_certificate(privkey, crep);
        break;
    default:
        ossl_cmp_log1(ERROR, ctx,
                      "received unsupported PKIStatus %d for certificate",
                      ctx->status);
        CMPerr(0, CMP_R_UNKNOWN_PKISTATUS);
        goto err;
    }
    if (crt == NULL) /* according to PKIStatus, we can expect a cert */
        CMPerr(0, CMP_R_CERTIFICATE_NOT_FOUND);

    return crt;

 err:
    if (OSSL_CMP_CTX_snprint_PKIStatus(ctx, buf, sizeof(buf)) != NULL)
        ERR_add_error_data(1, buf);
    return NULL;
}

/*-
 * Callback fn validating that the new certificate can be verified, using
 * ctx->certConf_cb_arg, which has been initialized using opt_out_trusted, and
 * ctx->untrusted_certs, which at this point already contains ctx->extraCertsIn.
 * Returns 0 on acceptance, else a bit field reflecting PKIFailureInfo.
 * Quoting from RFC 4210 section 5.1. Overall PKI Message:
 *     The extraCerts field can contain certificates that may be useful to
 *     the recipient.  For example, this can be used by a CA or RA to
 *     present an end entity with certificates that it needs to verify its
 *     own new certificate (if, for example, the CA that issued the end
 *     entity's certificate is not a root CA for the end entity).  Note that
 *     this field does not necessarily contain a certification path; the
 *     recipient may have to sort, select from, or otherwise process the
 *     extra certificates in order to use them.
 * Note: While often handy, there is no hard requirement by CMP that
 * an EE must be able to validate the certificates it gets enrolled.
 */
int OSSL_CMP_certConf_cb(OSSL_CMP_CTX *ctx, X509 *cert, int fail_info,
                         const char **text)
{
    X509_STORE *out_trusted = OSSL_CMP_CTX_get_certConf_cb_arg(ctx);
    (void)text; /* make (artificial) use of var to prevent compiler warning */

    if (fail_info != 0) /* accept any error flagged by CMP core library */
        return fail_info;

    if (out_trusted != NULL
            && !OSSL_CMP_validate_cert_path(ctx, out_trusted, cert))
        fail_info = 1 << OSSL_CMP_PKIFAILUREINFO_incorrectData;

    return fail_info;
}

/*-
 * Perform the generic handling of certificate responses for IR/CR/KUR/P10CR.
 * Returns -1 on receiving pollRep if sleep == 0, setting the checkAfter value.
 * Returns 1 on success and provides the received PKIMESSAGE in *resp.
 * Returns 0 on error (which includes the case that timeout has been reached).
 * Regardless of success, caller is responsible for freeing *resp (unless NULL).
 */
static int cert_response(OSSL_CMP_CTX *ctx, int sleep, int rid,
                         OSSL_CMP_MSG **resp, int *checkAfter,
                         int req_type, int expected_type)
{
    EVP_PKEY *rkey = OSSL_CMP_CTX_get0_newPkey(ctx /* may be NULL */, 0);
    int fail_info = 0; /* no failure */
    const char *txt = NULL;
    OSSL_CMP_CERTREPMESSAGE *crepmsg;
    OSSL_CMP_CERTRESPONSE *crep;
    X509 *cert;
    char *subj = NULL;
    int ret = 1;

 retry:
    crepmsg = (*resp)->body->value.ip; /* same for cp and kup */
    if (sk_OSSL_CMP_CERTRESPONSE_num(crepmsg->response) > 1) {
        CMPerr(0, CMP_R_MULTIPLE_RESPONSES_NOT_SUPPORTED);
        return 0;
    }
    /* TODO: handle potentially multiple CertResponses in CertRepMsg */
    crep = ossl_cmp_certrepmessage_get0_certresponse(crepmsg, rid);
    if (crep == NULL)
        return 0;
    if (!save_statusInfo(ctx, crep->status))
        return 0;
    if (rid == -1) {
        /* for OSSL_CMP_PKIBODY_P10CR learn CertReqId from response */
        rid = ossl_cmp_asn1_get_int(crep->certReqId);
        if (rid == -1) {
            CMPerr(0, CMP_R_BAD_REQUEST_ID);
            return 0;
        }
    }

    if (ossl_cmp_pkisi_get_status(crep->status) == OSSL_CMP_PKISTATUS_waiting) {
        OSSL_CMP_MSG_free(*resp);
        *resp = NULL;
        if ((ret = poll_for_response(ctx, sleep, rid, resp, checkAfter)) != 0) {
            if (ret == -1) /* at this point implies sleep == 0 */
                return ret; /* waiting */
            goto retry; /* got ip/cp/kup, which may still indicate 'waiting' */
        } else {
            CMPerr(0, CMP_R_POLLING_FAILED);
            return 0;
        }
    }

    cert = get1_cert_status(ctx, (*resp)->body->type, crep);
    if (cert == NULL) {
        ERR_add_error_data(1, "; cannot extract certificate from response");
        return 0;
    }
    if (!ossl_cmp_ctx_set0_newCert(ctx, cert))
        return 0;

    /*
     * if the CMP server returned certificates in the caPubs field, copy them
     * to the context so that they can be retrieved if necessary
     */
    if (crepmsg->caPubs != NULL
            && !ossl_cmp_ctx_set1_caPubs(ctx, crepmsg->caPubs))
        return 0;

    /* copy received extraCerts to ctx->extraCertsIn so they can be retrieved */
    if (!ossl_cmp_ctx_set1_extraCertsIn(ctx, (*resp)->extraCerts))
        return 0;

    subj = X509_NAME_oneline(X509_get_subject_name(cert), NULL, 0);
    if (rkey != NULL
        /* X509_check_private_key() also works if rkey is just public key */
            && !(X509_check_private_key(ctx->newCert, rkey))) {
        fail_info = 1 << OSSL_CMP_PKIFAILUREINFO_incorrectData;
        txt = "public key in new certificate does not match our enrollment key";
        /*-
         * not callling (void)ossl_cmp_exchange_error(ctx,
         *                    OSSL_CMP_PKISTATUS_rejection, fail_info, txt)
         * not throwing CMP_R_CERTIFICATE_NOT_ACCEPTED with txt
         * not returning 0
         * since we better leave this for any ctx->certConf_cb to decide
         */
    }

    /*
     * Execute the certification checking callback function possibly set in ctx,
     * which can determine whether to accept a newly enrolled certificate.
     * It may overrule the pre-decision reflected in 'fail_info' and '*txt'.
     */
    if (ctx->certConf_cb
            && (fail_info = ctx->certConf_cb(ctx, ctx->newCert,
                                             fail_info, &txt)) != 0) {
        if (txt == NULL)
            txt = "CMP client application did not accept it";
    }
    if (fail_info != 0) /* immediately log error before any certConf exchange */
        ossl_cmp_log1(ERROR, ctx,
                      "rejecting newly enrolled cert with subject: %s", subj);

    /*
     * TODO: better move certConf exchange to do_certreq_seq() such that
     * also more low-level errors with CertReqMessages get reported to server
     */
    if (!ctx->disableConfirm
            && !ossl_cmp_hdr_has_implicitConfirm((*resp)->header)) {
        if (!ossl_cmp_exchange_certConf(ctx, fail_info, txt))
            ret = 0;
    }

    /* not throwing failure earlier as transfer_cb may call ERR_clear_error() */
    if (fail_info != 0) {
        CMPerr(0, CMP_R_CERTIFICATE_NOT_ACCEPTED);
        ERR_add_error_data(2, "rejecting newly enrolled cert with subject: ",
                           subj);
        if (txt != NULL)
            ERR_add_error_txt("; ", txt);
        ret = 0;
    }
    OPENSSL_free(subj);
    return ret;
}

int OSSL_CMP_try_certreq(OSSL_CMP_CTX *ctx, int req_type, int *checkAfter)
{
    OSSL_CMP_MSG *req = NULL;
    OSSL_CMP_MSG *rep = NULL;
    int is_p10 = req_type == OSSL_CMP_PKIBODY_P10CR;
    int rid = is_p10 ? -1 : OSSL_CMP_CERTREQID;
    int rep_type = is_p10 ? OSSL_CMP_PKIBODY_CP : req_type + 1;
    int res = 0;

    if (ctx == NULL) {
        CMPerr(0, CMP_R_NULL_ARGUMENT);
        return 0;
    }

    if (ctx->status != OSSL_CMP_PKISTATUS_waiting) { /* not polling already */
        ctx->status = -1;
        if (!ossl_cmp_ctx_set0_newCert(ctx, NULL))
            return 0;

        if (ctx->total_timeout > 0) /* else ctx->end_time is not used */
            ctx->end_time = time(NULL) + ctx->total_timeout;

        req = ossl_cmp_certReq_new(ctx, req_type, 0 /* req_err */);
        if (req == NULL) /* also checks if all necessary options are set */
            return 0;

        if (!send_receive_check(ctx, req, &rep, rep_type))
            goto err;
    } else {
        if (req_type < 0)
            return ossl_cmp_exchange_error(ctx, OSSL_CMP_PKISTATUS_rejection,
                                           0 /* TODO better fail_info value? */,
                                           "polling aborted", 0 /* errorCode */,
                                           "by application");
        res = poll_for_response(ctx, 0 /* no sleep */, rid, &rep, checkAfter);
        if (res <= 0) /* waiting or error */
            return res;
    }
    res = cert_response(ctx, 0 /* no sleep */, rid, &rep, checkAfter,
                        req_type, rep_type);

 err:
    OSSL_CMP_MSG_free(req);
    OSSL_CMP_MSG_free(rep);
    return res;
}

/*-
 * Do the full sequence CR/IR/KUR/P10CR, CP/IP/KUP/CP,
 * certConf, PKIconf, and polling if required.
 * Will sleep as long as indicated by the server (according to checkAfter).
 * All enrollment options need to be present in the context.
 * TODO: another function to request two certificates at once should be created.
 * Returns pointer to received certificate, or NULL if none was received.
 */
static X509 *do_certreq_seq(OSSL_CMP_CTX *ctx, int req_type, int req_err,
                            int rep_type)
{
    OSSL_CMP_MSG *req = NULL;
    OSSL_CMP_MSG *rep = NULL;
    int rid = (req_type == OSSL_CMP_PKIBODY_P10CR) ? -1 : OSSL_CMP_CERTREQID;
    X509 *result = NULL;

    if (ctx == NULL) {
        CMPerr(0, CMP_R_NULL_ARGUMENT);
        return NULL;
    }
    ctx->status = -1;
    if (!ossl_cmp_ctx_set0_newCert(ctx, NULL))
        return NULL;

    if (ctx->total_timeout > 0) /* else ctx->end_time is not used */
        ctx->end_time = time(NULL) + ctx->total_timeout;

    /* OSSL_CMP_certreq_new() also checks if all necessary options are set */
    if ((req = ossl_cmp_certReq_new(ctx, req_type, req_err)) == NULL)
        goto err;

    if (!send_receive_check(ctx, req, &rep, rep_type))
        goto err;

    if (cert_response(ctx, 1 /* sleep */, rid, &rep, NULL, req_type, rep_type)
        <= 0)
        goto err;

    result = ctx->newCert;
 err:
    OSSL_CMP_MSG_free(req);
    OSSL_CMP_MSG_free(rep);
    return result;
}

X509 *OSSL_CMP_exec_IR_ses(OSSL_CMP_CTX *ctx)
{
    return do_certreq_seq(ctx, OSSL_CMP_PKIBODY_IR,
                          CMP_R_ERROR_CREATING_IR, OSSL_CMP_PKIBODY_IP);
}

X509 *OSSL_CMP_exec_CR_ses(OSSL_CMP_CTX *ctx)
{
    return do_certreq_seq(ctx, OSSL_CMP_PKIBODY_CR,
                          CMP_R_ERROR_CREATING_CR, OSSL_CMP_PKIBODY_CP);
}

X509 *OSSL_CMP_exec_KUR_ses(OSSL_CMP_CTX *ctx)
{
    return do_certreq_seq(ctx, OSSL_CMP_PKIBODY_KUR,
                          CMP_R_ERROR_CREATING_KUR, OSSL_CMP_PKIBODY_KUP);
}

X509 *OSSL_CMP_exec_P10CR_ses(OSSL_CMP_CTX *ctx)
{
    return do_certreq_seq(ctx, OSSL_CMP_PKIBODY_P10CR,
                          CMP_R_ERROR_CREATING_P10CR, OSSL_CMP_PKIBODY_CP);
}

X509 *OSSL_CMP_exec_RR_ses(OSSL_CMP_CTX *ctx)
{
    OSSL_CMP_MSG *rr = NULL;
    OSSL_CMP_MSG *rp = NULL;
    const int num_RevDetails = 1;
    const int rsid = OSSL_CMP_REVREQSID;
    OSSL_CMP_REVREPCONTENT *rrep = NULL;
    OSSL_CMP_PKISI *si = NULL;
    char buf[OSSL_CMP_PKISI_BUFLEN];
    X509 *result = NULL;

    if (ctx == NULL) {
        CMPerr(0, CMP_R_INVALID_ARGS);
        return 0;
    }
    if (ctx->oldCert == NULL) {
        CMPerr(0, CMP_R_MISSING_REFERENCE_CERT);
        return 0;
    }
    ctx->status = -1;

    /* OSSL_CMP_rr_new() also checks if all necessary options are set */
    if ((rr = ossl_cmp_rr_new(ctx)) == NULL)
        goto end;

    if (!send_receive_check(ctx, rr, &rp, OSSL_CMP_PKIBODY_RP))
        goto end;

    rrep = rp->body->value.rp;
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (sk_OSSL_CMP_PKISI_num(rrep->status) != num_RevDetails) {
        CMPerr(0, CMP_R_WRONG_RP_COMPONENT_COUNT);
        goto end;
    }
#else
    if (sk_OSSL_CMP_PKISI_num(rrep->status) < 1) {
        CMPerr(0, CMP_R_WRONG_RP_COMPONENT_COUNT);
        goto end;
    }
#endif

    /* evaluate PKIStatus field */
    si = ossl_cmp_revrepcontent_get_pkisi(rrep, rsid);
    if (!save_statusInfo(ctx, si))
        goto err;
    switch (ossl_cmp_pkisi_get_status(si)) {
    case OSSL_CMP_PKISTATUS_accepted:
        ossl_cmp_info(ctx, "revocation accepted (PKIStatus=accepted)");
        result = ctx->oldCert;
        break;
    case OSSL_CMP_PKISTATUS_grantedWithMods:
        ossl_cmp_info(ctx, "revocation accepted (PKIStatus=grantedWithMods)");
        result = ctx->oldCert;
        break;
    case OSSL_CMP_PKISTATUS_rejection:
        CMPerr(0, CMP_R_REQUEST_REJECTED_BY_SERVER);
        goto err;
    case OSSL_CMP_PKISTATUS_revocationWarning:
        ossl_cmp_info(ctx, "revocation accepted (PKIStatus=revocationWarning)");
        result = ctx->oldCert;
        break;
    case OSSL_CMP_PKISTATUS_revocationNotification:
        /* interpretation as warning or error depends on CA */
        ossl_cmp_warn(ctx,
                      "revocation accepted (PKIStatus=revocationNotification)");
        result = ctx->oldCert;
        break;
    case OSSL_CMP_PKISTATUS_waiting:
    case OSSL_CMP_PKISTATUS_keyUpdateWarning:
        CMPerr(0, CMP_R_UNEXPECTED_PKISTATUS);
        goto err;
    default:
        CMPerr(0, CMP_R_UNKNOWN_PKISTATUS);
        goto err;
    }

    /* check any present CertId in optional revCerts field */
    if (rrep->revCerts != NULL) {
        OSSL_CRMF_CERTID *cid;
        OSSL_CRMF_CERTTEMPLATE *tmpl =
            sk_OSSL_CMP_REVDETAILS_value(rr->body->value.rr, rsid)->certDetails;
        const X509_NAME *issuer = OSSL_CRMF_CERTTEMPLATE_get0_issuer(tmpl);
        ASN1_INTEGER *serial = OSSL_CRMF_CERTTEMPLATE_get0_serialNumber(tmpl);

        if (sk_OSSL_CRMF_CERTID_num(rrep->revCerts) != num_RevDetails) {
            CMPerr(0, CMP_R_WRONG_RP_COMPONENT_COUNT);
            result = NULL;
            goto err;
        }
        if ((cid = ossl_cmp_revrepcontent_get_CertId(rrep, rsid)) == NULL) {
            result = NULL;
            goto err;
        }
        if (X509_NAME_cmp(issuer, OSSL_CRMF_CERTID_get0_issuer(cid)) != 0) {
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
            CMPerr(0, CMP_R_WRONG_CERTID_IN_RP);
            result = NULL;
            goto err;
#endif
        }
        if (ASN1_INTEGER_cmp(serial,
                             OSSL_CRMF_CERTID_get0_serialNumber(cid)) != 0) {
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
            CMPerr(0, CMP_R_WRONG_SERIAL_IN_RP);
            result = NULL;
            goto err;
#endif
        }
    }

    /* check number of any optionally present crls */
    if (rrep->crls != NULL && sk_X509_CRL_num(rrep->crls) != num_RevDetails) {
        CMPerr(0, CMP_R_WRONG_RP_COMPONENT_COUNT);
        result = NULL;
        goto err;
    }

 err:
    if (result == NULL
            && OSSL_CMP_CTX_snprint_PKIStatus(ctx, buf, sizeof(buf)) != NULL)
        ERR_add_error_data(1, buf);

 end:
    OSSL_CMP_MSG_free(rr);
    OSSL_CMP_MSG_free(rp);
    return result;
}

STACK_OF(OSSL_CMP_ITAV) *OSSL_CMP_exec_GENM_ses(OSSL_CMP_CTX *ctx)
{
    OSSL_CMP_MSG *genm;
    OSSL_CMP_MSG *genp = NULL;
    STACK_OF(OSSL_CMP_ITAV) *rcvd_itavs = NULL;

    if (ctx == NULL) {
        CMPerr(0, CMP_R_INVALID_ARGS);
        return 0;
    }

    if ((genm = ossl_cmp_genm_new(ctx)) == NULL)
        goto err;

    if (!send_receive_check(ctx, genm, &genp, OSSL_CMP_PKIBODY_GENP))
        goto err;

    /* received stack of itavs not to be freed with the genp */
    rcvd_itavs = genp->body->value.genp;
    genp->body->value.genp = NULL;

 err:
    OSSL_CMP_MSG_free(genm);
    OSSL_CMP_MSG_free(genp);

    return rcvd_itavs; /* recv_itavs == NULL indicates an error */
}
