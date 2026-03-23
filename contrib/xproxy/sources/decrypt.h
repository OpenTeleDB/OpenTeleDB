//
// Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
//

#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#define DECRYPT_BUFFER_SIZE 1024
#define DECRYPT_BLOCK_SIZE 16
#define MY_KEY ""

static  inline void generate_key(const unsigned char *input, int inlen, unsigned char *output) {
    unsigned char digest1[SHA_DIGEST_LENGTH];
    unsigned char digest2[SHA_DIGEST_LENGTH];
    
    // firt SHA1
    SHA1(input, inlen, digest1);
    // twice SHA1
    SHA1(digest1, SHA_DIGEST_LENGTH, digest2);
    memcpy(output, digest2, 16);
}

static inline int my_hex_decode(const char *in, unsigned char *out) {
    size_t len = strlen(in);
    if (len % 2 != 0) return -1;
    
    for(size_t i=0; i<len; i+=2) {
        unsigned int byte;
        sscanf(in+i, "%2x", &byte);
        out[i/2] = (unsigned char)byte;
    }
    return len/2;
}

static  inline int decrypt_string(const char *ciphertext_hex, const char *key, char *plaintext) {
    EVP_CIPHER_CTX *ctx = NULL;
    unsigned char aes_key[16];
    unsigned char ciphertext[DECRYPT_BUFFER_SIZE];
    int cipher_len = my_hex_decode(ciphertext_hex, ciphertext);
    int plain_len = 0, final_len = 0;

    /* Not crypted */
    if(cipher_len <= 0 || (cipher_len % DECRYPT_BLOCK_SIZE) != 0) 
    {
        strcpy(plaintext, ciphertext_hex);
        return 0;
    }

    // 生成密钥（两次SHA1）
    generate_key((const unsigned char*)key, strlen(key), aes_key);

    if(!(ctx = EVP_CIPHER_CTX_new())) goto err;

    if(1 != EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, aes_key, NULL))
        goto err;

    if(1 != EVP_DecryptUpdate(ctx, (unsigned char*)plaintext, &plain_len,
                            ciphertext, cipher_len))
        goto err;

    if(1 != EVP_DecryptFinal_ex(ctx, (unsigned char*)plaintext + plain_len, &final_len))
        goto err;

    plain_len += final_len;
    plaintext[plain_len] = '\0';

    EVP_CIPHER_CTX_free(ctx);
    return 0;

err:
    strcpy(plaintext, ciphertext_hex);
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}
