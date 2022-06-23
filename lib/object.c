
#include <stdio.h>

#include "files.h"
#include "log.h"
#include "object.h"
#include "tool_rc.h"
#include "tpm2.h"
#include "tpm2_auth_util.h"
#include <tss2/tss2_mu.h>

#define NULL_OBJECT "null"
#define NULL_OBJECT_LEN (sizeof(NULL_OBJECT) - 1)

ASN1_SEQUENCE(TSSPRIVKEY_OBJ) = {
    ASN1_SIMPLE(TSSPRIVKEY_OBJ, type, ASN1_OBJECT),
    ASN1_EXP_OPT(TSSPRIVKEY_OBJ, emptyAuth, ASN1_BOOLEAN, 0),
    ASN1_SIMPLE(TSSPRIVKEY_OBJ, parent, ASN1_INTEGER),
    ASN1_SIMPLE(TSSPRIVKEY_OBJ, pubkey, ASN1_OCTET_STRING),
    ASN1_SIMPLE(TSSPRIVKEY_OBJ, privkey, ASN1_OCTET_STRING)
} ASN1_SEQUENCE_END(TSSPRIVKEY_OBJ)

#define OID_loadableKey "2.23.133.10.1.3"
#define TSSPRIVKEY_PEM_STRING "TSS2 PRIVATE KEY"

IMPLEMENT_ASN1_FUNCTIONS(TSSPRIVKEY_OBJ);
IMPLEMENT_PEM_write_bio(TSSPRIVKEY_OBJ, TSSPRIVKEY_OBJ, TSSPRIVKEY_PEM_STRING, TSSPRIVKEY_OBJ);
IMPLEMENT_PEM_read_bio(TSSPRIVKEY_OBJ, TSSPRIVKEY_OBJ, TSSPRIVKEY_PEM_STRING, TSSPRIVKEY_OBJ);

tool_rc select_primary_template_alg(ESYS_CONTEXT *esys_ctx, TPM2_ALG_ID *alg_id) {
    tool_rc rc;
    TPMS_CAPABILITY_DATA *capability_data = NULL;
    TPM2_ALG_ID id = TPM2_ALG_NULL;

    TSS2_RC r = Esys_GetCapability(esys_ctx, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
        TPM2_CAP_ALGS, 0, TPM2_MAX_CAP_ALGS, NULL, &capability_data);
    if (r != TSS2_RC_SUCCESS) {
        LOG_ERR("Unable to fetch TPM supported algorithms");
        rc = tool_rc_general_error;
        goto ret;
    }

    for (uint32_t index = 0; index < capability_data->data.algorithms.count; index++) {
        if (capability_data->data.algorithms.algProperties[index].alg == TPM2_ALG_ECC) {
            id = TPM2_ALG_ECC;
            break;
        }
    }
    if (id == TPM2_ALG_NULL) {
        id = TPM2_ALG_RSA;
    }
    *alg_id = id;
    rc = tool_rc_success;

ret:
    Esys_Free(capability_data);
    return rc;
}

tool_rc setup_primary(ESYS_CONTEXT *esys_ctx, ESYS_TR *parent) {
    tool_rc rc;
    TSS2_RC r;
    const TPM2B_PUBLIC *primary_template = NULL;

    const TPM2B_SENSITIVE_CREATE primary_sensitive = { 0 };

    const TPM2B_DATA all_outside_info = {
        .size = 0,
    };

    const TPML_PCR_SELECTION all_creation_PCR = {
        .count = 0,
    };

    static const TPM2B_PUBLIC primary_rsa_template = {
        .publicArea = {
            .type = TPM2_ALG_RSA,
            .nameAlg = TPM2_ALG_SHA256,
            .objectAttributes = (TPMA_OBJECT_USERWITHAUTH |
                                TPMA_OBJECT_RESTRICTED |
                                TPMA_OBJECT_DECRYPT |
                                TPMA_OBJECT_NODA |
                                TPMA_OBJECT_FIXEDTPM |
                                TPMA_OBJECT_FIXEDPARENT |
                                TPMA_OBJECT_SENSITIVEDATAORIGIN),
            .authPolicy = {
                .size = 0,
            },
            .parameters.rsaDetail = {
                .symmetric = {
                    .algorithm = TPM2_ALG_AES,
                    .keyBits.aes = 128,
                    .mode.aes = TPM2_ALG_CFB,
                },
                .scheme = {
                    .scheme = TPM2_ALG_NULL,
                    .details = {}
                },
                .keyBits = 2048,
                .exponent = 0,
            },
            .unique.rsa = {
                .size = 0,
            }
        }
    };

    static const TPM2B_PUBLIC primary_ecc_template = {
        .publicArea = {
            .type = TPM2_ALG_ECC,
            .nameAlg = TPM2_ALG_SHA256,
            .objectAttributes = (TPMA_OBJECT_USERWITHAUTH |
                                TPMA_OBJECT_RESTRICTED |
                                TPMA_OBJECT_DECRYPT |
                                TPMA_OBJECT_NODA |
                                TPMA_OBJECT_FIXEDTPM |
                                TPMA_OBJECT_FIXEDPARENT |
                                TPMA_OBJECT_SENSITIVEDATAORIGIN),
            .authPolicy = {
                .size = 0,
            },
            .parameters.eccDetail = {
                .symmetric = {
                    .algorithm = TPM2_ALG_AES,
                    .keyBits.aes = 128,
                    .mode.aes = TPM2_ALG_CFB,
                },
                .scheme = {
                    .scheme = TPM2_ALG_NULL,
                    .details = {}
                },
                .curveID = TPM2_ECC_NIST_P256,
                .kdf = {
                    .scheme = TPM2_ALG_NULL,
                    .details = {}
                },
            },
            .unique.ecc = {
                .x.size = 0,
                .y.size = 0
            }
        }
    };

    TPM2_ALG_ID alg_id;

    rc = select_primary_template_alg(esys_ctx, &alg_id);
    if (rc != tool_rc_success) {
        LOG_ERR("Unable to correctly select primary template");
        goto ret;
    }

    if (alg_id == TPM2_ALG_ECC) {
        primary_template = &primary_ecc_template;
    } else {
        primary_template = &primary_rsa_template;
    }

    r = Esys_CreatePrimary(esys_ctx, ESYS_TR_RH_OWNER, ESYS_TR_PASSWORD,
        ESYS_TR_NONE, ESYS_TR_NONE, &primary_sensitive, primary_template,
        &all_outside_info, &all_creation_PCR, parent, NULL, NULL, NULL, NULL);
    if (r != TSS2_RC_SUCCESS) {
        LOG_ERR("Unable to run create primary");
        rc = tool_rc_general_error;
        goto ret;
    }
    rc = tool_rc_success;

ret:
    return rc;
}

static tool_rc do_ctx_file(ESYS_CONTEXT *ctx, const char *objectstr, FILE *f,
        tpm2_loaded_object *outobject) {
    /* assign a dummy transient handle */
    outobject->handle = TPM2_TRANSIENT_FIRST;
    outobject->path = objectstr;
    return files_load_tpm_context_from_file(ctx, &outobject->tr_handle, f);
}

tool_rc fetch_tpk(const char *objectstr, TSSPRIVKEY_OBJ **tpk) {
    tool_rc rc = tool_rc_success;
    BIO *input_bio = BIO_new_file(objectstr, "rb");
    if (!input_bio) {
        LOG_ERR("Unable to read as BIO file");
        rc = tool_rc_general_error;
        goto ret;
    }

    *tpk = PEM_read_bio_TSSPRIVKEY_OBJ(input_bio, NULL, NULL, NULL);
    if (*tpk == NULL) {
        LOG_ERR("Unable to read PEM from provided BIO/file");
        rc = tool_rc_general_error;
        goto ret;
    }

ret:
    if (input_bio) {
        BIO_free(input_bio);
    }
    return rc;
}

tool_rc fetch_priv_pub_from_tpk(TSSPRIVKEY_OBJ *tpk, TPM2B_PUBLIC *pub,
        TPM2B_PRIVATE *priv) {
    tool_rc rc;
    int pub_len = tpk->pubkey->length;
    int priv_len = tpk->privkey->length;
            
    rc = Tss2_MU_TPM2B_PUBLIC_Unmarshal(tpk->pubkey->data, pub_len,
        NULL, pub);
    if (rc != tool_rc_success) {
        LOG_ERR("Error deserializing public portion of object");
        goto error;
    }

    rc = Tss2_MU_TPM2B_PRIVATE_Unmarshal(tpk->privkey->data, priv_len,
        NULL, priv);
    if (rc != tool_rc_success) {
        LOG_ERR("Error deserializing private portion of object");
        goto error;
    }
    return tool_rc_success;

error:
    return tool_rc_general_error;
}

static tool_rc tpm2_util_object_load2(ESYS_CONTEXT *ctx, const char *objectstr,
        const char *auth, bool do_auth, tpm2_loaded_object *outobject,
        bool is_restricted_pswd_session, tpm2_handle_flags flags,
        TPM2B_PUBLIC *pub, TPM2B_PRIVATE *priv) {

    ESYS_CONTEXT *tmp_ctx = is_restricted_pswd_session ? NULL : ctx;

    if (do_auth) {
        tpm2_session *s = NULL;
        tool_rc rc = tpm2_auth_util_from_optarg(tmp_ctx, auth, &s,
                is_restricted_pswd_session);
        if (rc != tool_rc_success) {
            return rc;
        }

        outobject->session = s;
    }

    if (!objectstr) {
        LOG_ERR("object string is empty");
        return tool_rc_general_error;
    }

    // 1. Always attempt file
    tool_rc rc = tool_rc_general_error;
    FILE *f = fopen(objectstr, "rb");
    if (f) {
        rc = do_ctx_file(ctx, objectstr, f, outobject);
        fclose(f);
        if (rc != tool_rc_success) {
            /*
             * 2. if not parent context, then try for PEM
             * Start by converting file to a openssl BIO object
             */
            TSSPRIVKEY_OBJ *tpk = NULL;
            rc = fetch_tpk(objectstr, &tpk);
            if (rc != tool_rc_success) {
                LOG_ERR("Unable to fetch TSS PRIVKEY");
                goto error;
            }

            rc = fetch_priv_pub_from_tpk(tpk, pub, priv);
            if (rc != tool_rc_success) {
                LOG_ERR("Unable to fetch public/private portion of tss privkey");
                goto error;
            }

            uint64_t val;
            int ret = ASN1_INTEGER_get_uint64(&val, tpk->parent);
            if (ret == 0 || val > UINT32_MAX) {
                LOG_ERR("Unable to convert parent to integer/value too large");
                goto error;
            }
            
            bool is_persistent_parent = (val != TPM2_RH_OWNER && val != 0);
            if (!is_persistent_parent) {
                ESYS_TR obj_parent = ESYS_TR_NONE;
                rc = setup_primary(ctx, &obj_parent);
                if (rc != tool_rc_success) {
                    LOG_ERR("Unable to create parent using createprimary");
                    goto error;
                }

                outobject->tr_handle = obj_parent;
                rc = Esys_TR_GetTpmHandle(ctx, obj_parent,
                    &outobject->handle);
                if (rc != TSS2_RC_SUCCESS) {
                    LOG_ERR("Unable to fetch TPM handle from ESYS TR handle");
                    goto error;
                }
            } else {
                ESYS_TR tr_parent = ESYS_TR_NONE;
                rc = tpm2_from_tpm_public(ctx, val, ESYS_TR_NONE, ESYS_TR_NONE,
                    ESYS_TR_NONE, &tr_parent);
                if (rc != tool_rc_success) {
                    LOG_ERR("Unable to fetch TR Handle for persistent parent");
                    goto error;
                }

                outobject->tr_handle = tr_parent;
            }

            TSSPRIVKEY_OBJ_free(tpk);
            rc = tool_rc_success;
            goto ret;
error:
            if (tpk) {
                TSSPRIVKEY_OBJ_free(tpk);
            }
            rc = tool_rc_general_error;
            goto ret;
        }
ret:
        return rc;
    }

    // 3. Try to convert a hierarchy or raw handle
    TPMI_RH_PROVISION handle;
    bool result = tpm2_util_handle_from_optarg(objectstr, &handle, flags);
    if (result) {
        outobject->handle = handle;
        outobject->path = NULL;
        return tpm2_util_sys_handle_to_esys_handle(ctx, outobject->handle,
                &outobject->tr_handle);
    }

    LOG_ERR("Cannot make sense of object context \"%s\"", objectstr);

    return tool_rc_general_error;
}

tool_rc tpm2_util_object_load(ESYS_CONTEXT *ctx, const char *objectstr,
        tpm2_loaded_object *outobject, tpm2_handle_flags flags) {

    return tpm2_util_object_load2(ctx, objectstr, NULL, false, outobject,
        false, flags, NULL, NULL);
}

tool_rc tpm2_util_object_load_auth(ESYS_CONTEXT *ctx, const char *objectstr,
        const char *auth, tpm2_loaded_object *outobject,
        bool is_restricted_pswd_session, tpm2_handle_flags flags) {

    return tpm2_util_object_load2(ctx, objectstr, auth, true, outobject,
            is_restricted_pswd_session, flags, NULL, NULL);
}

tool_rc tpm2_util_object_load_auth_pub_priv(ESYS_CONTEXT *ctx,
        const char *objectstr, const char *auth, tpm2_loaded_object *outobject,
        bool is_restricted_pswd_session, tpm2_handle_flags flags,
        TPM2B_PUBLIC *pub, TPM2B_PRIVATE *priv) {

    return tpm2_util_object_load2(ctx, objectstr, auth, true, outobject,
        is_restricted_pswd_session, flags, pub, priv);
}
