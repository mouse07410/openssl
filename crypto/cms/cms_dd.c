/*
 * Copyright 2008-2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "internal/cryptlib.h"
#include <openssl/asn1t.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/cms.h>
#include "cms_local.h"

/* CMS DigestedData Utilities */

CMS_ContentInfo *cms_DigestedData_create(const EVP_MD *md,
                                         OSSL_LIB_CTX *libctx,
                                         const char *propq)
{
    CMS_ContentInfo *cms;
    CMS_DigestedData *dd;

    cms = CMS_ContentInfo_new_ex(libctx, propq);
    if (cms == NULL)
        return NULL;

    dd = M_ASN1_new_of(CMS_DigestedData);

    if (dd == NULL)
        goto err;

    cms->contentType = OBJ_nid2obj(NID_pkcs7_digest);
    cms->d.digestedData = dd;

    dd->version = 0;
    dd->encapContentInfo->eContentType = OBJ_nid2obj(NID_pkcs7_data);

    X509_ALGOR_set_md(dd->digestAlgorithm, md);

    return cms;

 err:
    CMS_ContentInfo_free(cms);
    return NULL;
}

BIO *cms_DigestedData_init_bio(const CMS_ContentInfo *cms)
{
    CMS_DigestedData *dd = cms->d.digestedData;

    return cms_DigestAlgorithm_init_bio(dd->digestAlgorithm, cms_get0_cmsctx(cms));
}

int cms_DigestedData_do_final(const CMS_ContentInfo *cms, BIO *chain, int verify)
{
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen;
    int r = 0;
    CMS_DigestedData *dd;

    if (mctx == NULL) {
        CMSerr(CMS_F_CMS_DIGESTEDDATA_DO_FINAL, ERR_R_MALLOC_FAILURE);
        goto err;
    }

    dd = cms->d.digestedData;

    if (!cms_DigestAlgorithm_find_ctx(mctx, chain, dd->digestAlgorithm))
        goto err;

    if (EVP_DigestFinal_ex(mctx, md, &mdlen) <= 0)
        goto err;

    if (verify) {
        if (mdlen != (unsigned int)dd->digest->length) {
            CMSerr(CMS_F_CMS_DIGESTEDDATA_DO_FINAL,
                   CMS_R_MESSAGEDIGEST_WRONG_LENGTH);
            goto err;
        }

        if (memcmp(md, dd->digest->data, mdlen))
            CMSerr(CMS_F_CMS_DIGESTEDDATA_DO_FINAL,
                   CMS_R_VERIFICATION_FAILURE);
        else
            r = 1;
    } else {
        if (!ASN1_STRING_set(dd->digest, md, mdlen))
            goto err;
        r = 1;
    }

 err:
    EVP_MD_CTX_free(mctx);

    return r;

}
