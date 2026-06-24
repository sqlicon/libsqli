#include "sqlicon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

/* ---------------------------------------------------------------- */
/* Machine identifier (for key derivation)                          */
/* ---------------------------------------------------------------- */

static int read_machine_identifier(char *out, size_t out_cap)
{
    const char *paths[] = {
        "/etc/machine-id",
        "/var/lib/dbus/machine-id",
        "/sys/class/dmi/id/product_uuid"
    };
    char line[256];
    for (size_t i = 0; i < (sizeof(paths) / sizeof(paths[0])); i++) {
        FILE *fp = fopen(paths[i], "r");
        if (fp == NULL)
            continue;
        if (fgets(line, sizeof(line), fp) != NULL) {
            fclose(fp);
            strip_trailing_inplace(line);
            if (line[0] != '\0') {
                if (snprintf(out, out_cap, "%s", line) >= (int)out_cap)
                    return -1;
                return 0;
            }
        } else {
            fclose(fp);
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- */
/* Key derivation                                                   */
/* ---------------------------------------------------------------- */

int derive_profile_key_v1(unsigned char key_out[32])
{
    char machine_id[256];
    if (read_machine_identifier(machine_id, sizeof(machine_id)) != 0)
        return -1;

    char uid_buf[64];
    if (snprintf(uid_buf, sizeof(uid_buf), "%lu", (unsigned long)getuid()) >= (int)sizeof(uid_buf))
        return -1;

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (md == NULL)
        return -1;
    unsigned int out_len = 0;
    int ok = 0;
    do {
        if (EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1)
            break;
        if (EVP_DigestUpdate(md, "sqlicon:kdf:uuid_uid_v1:", 24) != 1)
            break;
        if (EVP_DigestUpdate(md, machine_id, strlen(machine_id)) != 1)
            break;
        if (EVP_DigestUpdate(md, ":", 1) != 1)
            break;
        if (EVP_DigestUpdate(md, uid_buf, strlen(uid_buf)) != 1)
            break;
        if (EVP_DigestFinal_ex(md, key_out, &out_len) != 1)
            break;
        if (out_len != 32)
            break;
        ok = 1;
    } while (0);
    EVP_MD_CTX_free(md);
    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------- */
/* Base64 helpers                                                   */
/* ---------------------------------------------------------------- */

static int base64_encode_alloc(const unsigned char *in, size_t in_len, char **out_b64)
{
    if (in_len > (size_t)((INT_MAX / 4) * 3))
        return -1;
    size_t out_cap = 4 * ((in_len + 2) / 3) + 1;
    char *out = malloc(out_cap);
    if (out == NULL)
        return -1;
    int enc_len = EVP_EncodeBlock((unsigned char *)out, in, (int)in_len);
    if (enc_len < 0) {
        free(out);
        return -1;
    }
    out[(size_t)enc_len] = '\0';
    *out_b64 = out;
    return 0;
}

static int base64_decode_alloc(const char *b64, unsigned char **out, size_t *out_len)
{
    size_t in_len = strlen(b64);
    if (in_len == 0 || (in_len % 4) != 0)
        return -1;
    if (in_len > (size_t)INT_MAX)
        return -1;

    size_t cap = (in_len / 4) * 3;
    unsigned char *buf = malloc(cap + 1);
    if (buf == NULL)
        return -1;

    int dec = EVP_DecodeBlock(buf, (const unsigned char *)b64, (int)in_len);
    if (dec < 0) {
        free(buf);
        return -1;
    }
    size_t actual = (size_t)dec;
    if (in_len >= 2 && b64[in_len - 1] == '=')
        actual--;
    if (in_len >= 2 && b64[in_len - 2] == '=')
        actual--;
    *out = buf;
    *out_len = actual;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Encrypt / Decrypt profile secrets                                */
/* ---------------------------------------------------------------- */

int encrypt_profile_secret(const char *plain, char **out_b64)
{
    if (plain == NULL || plain[0] == '\0') {
        *out_b64 = strdup("");
        return (*out_b64 != NULL) ? 0 : -1;
    }

    unsigned char key[32];
    if (derive_profile_key_v1(key) != 0)
        return -1;

    size_t plain_len = strlen(plain);
    unsigned char nonce[12];
    if (RAND_bytes(nonce, (int)sizeof(nonce)) != 1)
        return -1;

    unsigned char *cipher = malloc(plain_len + 16);
    if (cipher == NULL)
        return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        free(cipher);
        return -1;
    }

    int ok = 0;
    int len = 0;
    int cipher_len = 0;
    unsigned char tag[16];
    unsigned char *packed = NULL;
    size_t packed_len = 0;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)sizeof(nonce), NULL) != 1)
            break;
        if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
            break;
        if (EVP_EncryptUpdate(ctx, cipher, &len, (const unsigned char *)plain, (int)plain_len) != 1)
            break;
        cipher_len = len;
        if (EVP_EncryptFinal_ex(ctx, cipher + cipher_len, &len) != 1)
            break;
        cipher_len += len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)sizeof(tag), tag) != 1)
            break;

        packed_len = sizeof(nonce) + sizeof(tag) + (size_t)cipher_len;
        packed = malloc(packed_len);
        if (packed == NULL)
            break;
        memcpy(packed, nonce, sizeof(nonce));
        memcpy(packed + sizeof(nonce), tag, sizeof(tag));
        memcpy(packed + sizeof(nonce) + sizeof(tag), cipher, (size_t)cipher_len);
        if (base64_encode_alloc(packed, packed_len, out_b64) != 0)
            break;
        ok = 1;
    } while (0);

    free(packed);
    EVP_CIPHER_CTX_free(ctx);
    memset(key, 0, sizeof(key));
    free(cipher);
    return ok ? 0 : -1;
}

int decrypt_profile_secret(const char *b64, char **out_plain)
{
    if (b64 == NULL || b64[0] == '\0') {
        *out_plain = strdup("");
        return (*out_plain != NULL) ? 0 : -1;
    }

    unsigned char *packed = NULL;
    size_t packed_len = 0;
    if (base64_decode_alloc(b64, &packed, &packed_len) != 0)
        return -1;
    if (packed_len < (12 + 16)) {
        free(packed);
        return -1;
    }

    const unsigned char *nonce = packed;
    const unsigned char *tag = packed + 12;
    const unsigned char *cipher = packed + 28;
    size_t cipher_len = packed_len - 28;

    unsigned char key[32];
    if (derive_profile_key_v1(key) != 0) {
        free(packed);
        return -1;
    }

    unsigned char *plain = malloc(cipher_len + 1);
    if (plain == NULL) {
        memset(key, 0, sizeof(key));
        free(packed);
        return -1;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        free(plain);
        memset(key, 0, sizeof(key));
        free(packed);
        return -1;
    }

    int ok = 0;
    int len = 0;
    int plain_len = 0;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1)
            break;
        if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
            break;
        if (EVP_DecryptUpdate(ctx, plain, &len, cipher, (int)cipher_len) != 1)
            break;
        plain_len = len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) != 1)
            break;
        if (EVP_DecryptFinal_ex(ctx, plain + plain_len, &len) != 1)
            break;
        plain_len += len;
        plain[plain_len] = '\0';
        *out_plain = (char *)plain;
        ok = 1;
    } while (0);

    EVP_CIPHER_CTX_free(ctx);
    memset(key, 0, sizeof(key));
    free(packed);
    if (!ok) {
        free(plain);
        return -1;
    }
    return 0;
}
