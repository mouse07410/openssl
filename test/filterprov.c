/*
 * Copyright 2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/*
 * A filtering provider for test purposes. We pass all calls through to the
 * default provider except where we want other behaviour for a test.
 */

#include <string.h>
#include <openssl/core.h>
#include <openssl/core_numbers.h>
#include <openssl/provider.h>
#include <openssl/crypto.h>

OSSL_provider_init_fn filter_provider_init;

int filter_provider_set_filter(int operation, const char *name);

#define MAX_FILTERS     10
#define MAX_ALG_FILTERS 5

struct filter_prov_globals_st {
    OPENSSL_CTX *libctx;
    OSSL_PROVIDER *deflt;
    struct {
        int operation;
        OSSL_ALGORITHM alg[MAX_ALG_FILTERS + 1];
    } dispatch[MAX_FILTERS];
    int num_dispatch;
};

static struct filter_prov_globals_st ourglobals;

static struct filter_prov_globals_st *get_globals(void)
{
    /*
     * Ideally we'd like to store this in the OPENSSL_CTX so that we can have
     * more than one instance of the filter provider at a time. But for now we
     * just make it simple.
     */
    return &ourglobals;
}

static OSSL_provider_gettable_params_fn filter_gettable_params;
static OSSL_provider_get_params_fn filter_get_params;
static OSSL_provider_query_operation_fn filter_query;
static OSSL_provider_teardown_fn filter_teardown;

static const OSSL_PARAM *filter_gettable_params(void *provctx)
{
    struct filter_prov_globals_st *globs = get_globals();

    return OSSL_PROVIDER_gettable_params(globs->deflt);
}

static int filter_get_params(void *provctx, OSSL_PARAM params[])
{
    struct filter_prov_globals_st *globs = get_globals();

    return OSSL_PROVIDER_get_params(globs->deflt, params);
}

static const OSSL_ALGORITHM *filter_query(void *provctx,
                                          int operation_id,
                                          int *no_cache)
{
    struct filter_prov_globals_st *globs = get_globals();
    int i;

    for (i = 0; i < globs->num_dispatch; i++) {
        if (globs->dispatch[i].operation == operation_id) {
            *no_cache = 0;
            return globs->dispatch[i].alg;
        }
    }

    /* No filter set, so pass it down to the chained provider */
    return OSSL_PROVIDER_query_operation(globs->deflt, operation_id, no_cache);
}

static void filter_teardown(void *provctx)
{
    struct filter_prov_globals_st *globs = get_globals();

    OSSL_PROVIDER_unload(globs->deflt);
    OPENSSL_CTX_free(globs->libctx);
}

/* Functions we provide to the core */
static const OSSL_DISPATCH filter_dispatch_table[] = {
    { OSSL_FUNC_PROVIDER_GETTABLE_PARAMS, (void (*)(void))filter_gettable_params },
    { OSSL_FUNC_PROVIDER_GET_PARAMS, (void (*)(void))filter_get_params },
    { OSSL_FUNC_PROVIDER_QUERY_OPERATION, (void (*)(void))filter_query },
    { OSSL_FUNC_PROVIDER_TEARDOWN, (void (*)(void))filter_teardown },
    { 0, NULL }
};

int filter_provider_init(const OSSL_CORE_HANDLE *handle,
                         const OSSL_DISPATCH *in,
                         const OSSL_DISPATCH **out,
                         void **provctx)
{
    memset(&ourglobals, 0, sizeof(ourglobals));
    ourglobals.libctx = OPENSSL_CTX_new();
    if (ourglobals.libctx == NULL)
        goto err;

    ourglobals.deflt = OSSL_PROVIDER_load(ourglobals.libctx, "default");
    if (ourglobals.deflt == NULL)
        goto err;

    *provctx = OSSL_PROVIDER_get0_provider_ctx(ourglobals.deflt);
    *out = filter_dispatch_table;
    return 1;

 err:
    OSSL_PROVIDER_unload(ourglobals.deflt);
    OPENSSL_CTX_free(ourglobals.libctx);
    return 0;
}

/*
 * Set a filter for the given operation id. The filter string is a colon
 * separated list of algorithms that will be made available by this provider.
 * Anything not in the filter will be suppressed. If a filter is not set for
 * a given operation id then all algorithms are made available.
 */
int filter_provider_set_filter(int operation, const char *filterstr)
{
    int no_cache = 0;
    int algnum = 0, last = 0, ret = 0;
    struct filter_prov_globals_st *globs = get_globals();
    size_t namelen;
    char *filterstrtmp = OPENSSL_strdup(filterstr);
    char *name, *sep;
    const OSSL_ALGORITHM *provalgs = OSSL_PROVIDER_query_operation(globs->deflt,
                                                                   operation,
                                                                   &no_cache);
    const OSSL_ALGORITHM *algs;

    if (filterstrtmp == NULL)
        goto err;

    /* We don't support no_cache */
    if (no_cache)
        goto err;

    /* Nothing to filter */
    if (provalgs == NULL)
        goto err;

    if (globs->num_dispatch >= MAX_FILTERS)
        goto err;

    for (name = filterstrtmp; !last; name = sep + 1) {
        sep = strstr(name, ":");
        if (sep != NULL)
            *sep = '\0';
        else
            last = 1;
        namelen = strlen(name);

        for (algs = provalgs; algs->algorithm_names != NULL; algs++) {
            const char *found = strstr(algs->algorithm_names, name);

            if (found == NULL)
                continue;
            if (found[namelen] != '\0' && found[namelen] != ':')
                continue;
            if (found != algs->algorithm_names && found[-1] != ':')
                continue;

            /* We found a match */
            if (algnum >= MAX_ALG_FILTERS)
                goto err;

            globs->dispatch[globs->num_dispatch].alg[algnum++] = *algs;
            break;
        }
        if (algs->algorithm_names == NULL) {
            /* No match found */
            goto err;
        }
    }

    globs->dispatch[globs->num_dispatch].operation = operation;
    globs->num_dispatch++;

    ret = 1;
 err:
    OPENSSL_free(filterstrtmp);
    return ret;
}
