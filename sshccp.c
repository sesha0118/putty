/*
 * ChaCha20-Poly1305 Implementation for SSH-2
 *
 * Protocol spec:
 *  http://cvsweb.openbsd.org/cgi-bin/cvsweb/src/usr.bin/ssh/PROTOCOL.chacha20poly1305?rev=1.2&content-type=text/x-cvsweb-markup
 *
 * ChaCha20 spec:
 *  http://cr.yp.to/chacha/chacha-20080128.pdf
 *
 * Salsa20 spec:
 *  http://cr.yp.to/snuffle/spec.pdf
 *
 * Poly1305-AES spec:
 *  http://cr.yp.to/mac/poly1305-20050329.pdf
 *
 * The nonce for the Poly1305 is the second part of the key output
 * from the first round of ChaCha20. This removes the AES requirement.
 * This is undocumented!
 *
 * This has an intricate link between the cipher and the MAC. The
 * keying of both is done in by the cipher and setting of the IV is
 * done by the MAC. One cannot operate without the other. The
 * configuration of the ssh2_cipher structure ensures that the MAC is
 * set (and others ignored) if this cipher is chosen.
 *
 * This cipher also encrypts the length using a different
 * instantiation of the cipher using a different key and IV made from
 * the sequence number which is passed in addition when calling
 * encrypt/decrypt on it.
 */

#include "ssh.h"

#ifndef INLINE
#define INLINE
#endif

/* ChaCha20 implementation, only supporting 256-bit keys */

/* State for each ChaCha20 instance */
struct chacha20 {
    /* Current context, usually with the count incremented
     * 0-3 are the static constant
     * 4-11 are the key
     * 12-13 are the counter
     * 14-15 are the IV */
    uint32 state[16];
    /* The output of the state above ready to xor */
    unsigned char current[64];
    /* The index of the above currently used to allow a true streaming cipher */
    int currentIndex;
};

static INLINE void chacha20_round(struct chacha20 *ctx)
{
    int i;
    uint32 copy[16];

    /* Take a copy */
    memcpy(copy, ctx->state, sizeof(copy));

    /* A circular rotation for a 32bit number */
#define rotl(x, shift) x = ((x << shift) | (x >> (32 - shift)))

    /* What to do for each quarter round operation */
#define qrop(a, b, c, d)                        \
    copy[a] += copy[b];                         \
    copy[c] ^= copy[a];                         \
    rotl(copy[c], d)

    /* A quarter round */
#define quarter(a, b, c, d)                     \
    qrop(a, b, d, 16);                          \
    qrop(c, d, b, 12);                          \
    qrop(a, b, d, 8);                           \
    qrop(c, d, b, 7)

    /* Do 20 rounds, in pairs because every other is different */
    for (i = 0; i < 20; i += 2) {
        /* A round */
        quarter(0, 4, 8, 12);
        quarter(1, 5, 9, 13);
        quarter(2, 6, 10, 14);
        quarter(3, 7, 11, 15);
        /* Another slightly different round */
        quarter(0, 5, 10, 15);
        quarter(1, 6, 11, 12);
        quarter(2, 7, 8, 13);
        quarter(3, 4, 9, 14);
    }

    /* Dump the macros, don't need them littering */
#undef rotl
#undef qrop
#undef quarter

    /* Add the initial state */
    for (i = 0; i < 16; ++i) {
        copy[i] += ctx->state[i];
    }

    /* Update the content of the xor buffer */
    for (i = 0; i < 16; ++i) {
        ctx->current[i * 4 + 0] = copy[i] >> 0;
        ctx->current[i * 4 + 1] = copy[i] >> 8;
        ctx->current[i * 4 + 2] = copy[i] >> 16;
        ctx->current[i * 4 + 3] = copy[i] >> 24;
    }
    /* State full, reset pointer to beginning */
    ctx->currentIndex = 0;
    smemclr(copy, sizeof(copy));

    /* Increment round counter */
    ++ctx->state[12];
    /* Check for overflow, not done in one line so the 32 bits are chopped by the type */
    if (!(uint32)(ctx->state[12])) {
        ++ctx->state[13];
    }
}

/* Initialise context with 256bit key */
static void chacha20_key(struct chacha20 *ctx, const unsigned char *key)
{
    static const char constant[16] = "expand 32-byte k";

    /* Add the fixed string to the start of the state */
    ctx->state[0] = GET_32BIT_LSB_FIRST(constant + 0);
    ctx->state[1] = GET_32BIT_LSB_FIRST(constant + 4);
    ctx->state[2] = GET_32BIT_LSB_FIRST(constant + 8);
    ctx->state[3] = GET_32BIT_LSB_FIRST(constant + 12);

    /* Add the key */
    ctx->state[4]  = GET_32BIT_LSB_FIRST(key + 0);
    ctx->state[5]  = GET_32BIT_LSB_FIRST(key + 4);
    ctx->state[6]  = GET_32BIT_LSB_FIRST(key + 8);
    ctx->state[7]  = GET_32BIT_LSB_FIRST(key + 12);
    ctx->state[8]  = GET_32BIT_LSB_FIRST(key + 16);
    ctx->state[9]  = GET_32BIT_LSB_FIRST(key + 20);
    ctx->state[10] = GET_32BIT_LSB_FIRST(key + 24);
    ctx->state[11] = GET_32BIT_LSB_FIRST(key + 28);

    /* New key, dump context */
    ctx->currentIndex = 64;
}

static void chacha20_iv(struct chacha20 *ctx, const unsigned char *iv)
{
    ctx->state[12] = 0;
    ctx->state[13] = 0;
    ctx->state[14] = GET_32BIT_MSB_FIRST(iv);
    ctx->state[15] = GET_32BIT_MSB_FIRST(iv + 4);

    /* New IV, dump context */
    ctx->currentIndex = 64;
}

static void chacha20_encrypt(struct chacha20 *ctx, unsigned char *blk, int len)
{
    while (len) {
        /* If we don't have any state left, then cycle to the next */
        if (ctx->currentIndex >= 64) {
            chacha20_round(ctx);
        }

        /* Do the xor while there's some state left and some plaintext left */
        while (ctx->currentIndex < 64 && len) {
            *blk++ ^= ctx->current[ctx->currentIndex++];
            --len;
        }
    }
}

/* Decrypt is encrypt... It's xor against a PRNG... */
static INLINE void chacha20_decrypt(struct chacha20 *ctx,
                                    unsigned char *blk, int len)
{
    chacha20_encrypt(ctx, blk, len);
}

/* Poly1305 implementation (no AES, nonce is not encrypted) */

struct poly1305 {
    unsigned char nonce[16];
    Bignum modulo;
    Bignum r;
    Bignum h;

    /* Buffer in case we get less that a multiple of 16 bytes */
    unsigned char buffer[16];
    int bufferIndex;
};

static void poly1305_make(struct poly1305 *ctx)
{
    static const unsigned char p[] = {
        0x03,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfb
    };

    ctx->modulo = bignum_from_bytes(p, sizeof(p));
    ctx->r = NULL;
    ctx->h = NULL;
    memset(ctx->nonce, 0, 16);
    ctx->bufferIndex = 0;
}

static void poly1305_free(struct poly1305 *ctx)
{
    if (ctx->modulo) {
        freebn(ctx->modulo);
    }
    if (ctx->r) {
        freebn(ctx->r);
    }
    if (ctx->h) {
        freebn(ctx->h);
    }
    smemclr(ctx, sizeof(struct poly1305));
}

/* Takes a 256 bit key */
static void poly1305_key(struct poly1305 *ctx, const unsigned char *key)
{
    unsigned char key_copy[16];
    memcpy(key_copy, key, 16);

    /* Key the MAC itself
     * bytes 4, 8, 12 and 16 are required to have their top four bits clear */
    key_copy[3] &= 0x0f;
    key_copy[7] &= 0x0f;
    key_copy[11] &= 0x0f;
    key_copy[15] &= 0x0f;
    /* bytes 5, 9 and 13 are required to have their bottom two bits clear */
    key_copy[4] &= 0xfc;
    key_copy[8] &= 0xfc;
    key_copy[12] &= 0xfc;
    if (ctx->r) {
        freebn(ctx->r);
    }
    ctx->r = bignum_from_bytes_le(key_copy, 16);
    smemclr(key_copy, sizeof(key_copy));

    /* Use second 128 bits are the nonce */
    memcpy(ctx->nonce, key+16, 16);
}

/* Feed up to 16 bytes (should only be less for the last chunk) */
static void poly1305_feed_chunk(struct poly1305 *ctx,
                                const unsigned char *chunk, int len)
{
    Bignum tmp, tmp2;
    Bignum c = bignum_from_bytes_le(chunk, len);
    tmp = bignum_lshift(One, 8 * len);
    tmp2 = bigadd(c, tmp);
    freebn(tmp);
    freebn(c);
    if (ctx->h) {
        tmp = bigadd(ctx->h, tmp2);
        freebn(tmp2);
        freebn(ctx->h);
    } else {
        tmp = tmp2;
    }
    ctx->h = modmul(tmp, ctx->r, ctx->modulo);
    freebn(tmp);
}

static void poly1305_feed(struct poly1305 *ctx,
                          const unsigned char *buf, int len)
{
    /* Check for stuff left in the buffer from last time */
    if (ctx->bufferIndex) {
        /* Try to fill up to 16 */
        while (ctx->bufferIndex < 16 && len) {
            ctx->buffer[ctx->bufferIndex++] = *buf++;
            --len;
        }
        if (ctx->bufferIndex == 16) {
            poly1305_feed_chunk(ctx, ctx->buffer, 16);
            ctx->bufferIndex = 0;
        }
    }

    /* Process 16 byte whole chunks */
    while (len >= 16) {
        poly1305_feed_chunk(ctx, buf, 16);
        len -= 16;
        buf += 16;
    }

    /* Cache stuff that's left over */
    if (len) {
        memcpy(ctx->buffer, buf, len);
        ctx->bufferIndex = len;
    }
}

/* Finalise and populate buffer with 16 byte with MAC */
static void poly1305_finalise(struct poly1305 *ctx, unsigned char *mac)
{
    Bignum tmp, tmp2;
    int i;

    if (ctx->bufferIndex) {
        poly1305_feed_chunk(ctx, ctx->buffer, ctx->bufferIndex);
    }

    tmp = bignum_from_bytes_le(ctx->nonce, 16);

    tmp2 = bigadd(ctx->h, tmp);
    freebn(tmp);
    for (i = 0; i < 16; ++i) {
        mac[i] = bignum_byte(tmp2, i);
    }
    freebn(tmp2);
}

/* SSH-2 wrapper */

struct ccp_context {
    struct chacha20 a_cipher; /* Used for length */
    struct chacha20 b_cipher; /* Used for content */

    /* Cache of the first 4 bytes because they are the sequence number */
    /* Kept in 8 bytes with the top as zero to allow easy passing to setiv */
    int mac_initialised; /* Where we have got to in filling mac_iv */
    unsigned char mac_iv[8];

    struct poly1305 mac;
};

static void *poly_make_context(void *ctx)
{
    return ctx;
}

static void poly_free_context(void *ctx)
{
    /* Not allocated, just forwarded, no need to free */
}

static void poly_setkey(void *ctx, unsigned char *key)
{
    /* Uses the same context as ChaCha20, so ignore */
}

static void poly_start(void *handle)
{
    struct ccp_context *ctx = (struct ccp_context *)handle;

    ctx->mac_initialised = 0;
    memset(ctx->mac_iv, 0, 8);
    poly1305_free(&ctx->mac);
    poly1305_make(&ctx->mac);
}

static void poly_bytes(void *handle, unsigned char const *blk, int len)
{
    struct ccp_context *ctx = (struct ccp_context *)handle;

    /* First 4 bytes are the IV */
    while (ctx->mac_initialised < 4 && len) {
        ctx->mac_iv[7 - ctx->mac_initialised] = *blk++;
        ++ctx->mac_initialised;
        --len;
    }

    /* Initialise the IV if needed */
    if (ctx->mac_initialised == 4) {
        chacha20_iv(&ctx->b_cipher, ctx->mac_iv);
        ++ctx->mac_initialised;  /* Don't do it again */

        /* Do first rotation */
        chacha20_round(&ctx->b_cipher);

        /* Set the poly key */
        poly1305_key(&ctx->mac, ctx->b_cipher.current);

        /* Set the first round as used */
        ctx->b_cipher.currentIndex = 64;
    }

    /* Update the MAC with anything left */
    if (len) {
        poly1305_feed(&ctx->mac, blk, len);
    }
}

static void poly_genresult(void *handle, unsigned char *blk)
{
    struct ccp_context *ctx = (struct ccp_context *)handle;
    poly1305_finalise(&ctx->mac, blk);
}

static int poly_verresult(void *handle, unsigned char const *blk)
{
    struct ccp_context *ctx = (struct ccp_context *)handle;
    int res;
    unsigned char mac[16];
    poly1305_finalise(&ctx->mac, mac);
    res = smemeq(blk, mac, 16);
    return res;
}

/* The generic poly operation used before generate and verify */
static void poly_op(void *handle, unsigned char *blk, int len, unsigned long seq)
{
    unsigned char iv[4];
    poly_start(handle);
    PUT_32BIT_MSB_FIRST(iv, seq);
    /* poly_bytes expects the first 4 bytes to be the IV */
    poly_bytes(handle, iv, 4);
    smemclr(iv, sizeof(iv));
    poly_bytes(handle, blk, len);
}

static void poly_generate(void *handle, unsigned char *blk, int len, unsigned long seq)
{
    poly_op(handle, blk, len, seq);
    poly_genresult(handle, blk+len);
}

static int poly_verify(void *handle, unsigned char *blk, int len, unsigned long seq)
{
    poly_op(handle, blk, len, seq);
    return poly_verresult(handle, blk+len);
}

static const struct ssh_mac ssh2_poly1305 = {
    poly_make_context, poly_free_context,
    poly_setkey,

    /* whole-packet operations */
    poly_generate, poly_verify,

    /* partial-packet operations */
    poly_start, poly_bytes, poly_genresult, poly_verresult,

    "", "", /* Not selectable individually, just part of ChaCha20-Poly1305 */
    16, "<implicit>"
};

static void *ccp_make_context(void)
{
    struct ccp_context *ctx = snew(struct ccp_context);
    if (ctx) {
        poly1305_make(&ctx->mac);
    }
    return ctx;
}

static void ccp_free_context(void *vctx)
{
    struct ccp_context *ctx = (struct ccp_context *)vctx;
    smemclr(&ctx->a_cipher, sizeof(ctx->a_cipher));
    smemclr(&ctx->b_cipher, sizeof(ctx->b_cipher));
    poly1305_free(&ctx->mac);
    sfree(ctx);
}

static void ccp_iv(void *vctx, unsigned char *iv)
{
    /* struct ccp_context *ctx = (struct ccp_context *)vctx; */
    /* IV is set based on the sequence number */
}

static void ccp_key(void *vctx, unsigned char *key)
{
    struct ccp_context *ctx = (struct ccp_context *)vctx;
    /* Initialise the a_cipher (for decrypting lengths) with the first 256 bits */
    chacha20_key(&ctx->a_cipher, key + 32);
    /* Initialise the b_cipher (for content and MAC) with the second 256 bits */
    chacha20_key(&ctx->b_cipher, key);
}

static void ccp_encrypt(void *vctx, unsigned char *blk, int len)
{
    struct ccp_context *ctx = (struct ccp_context *)vctx;
    chacha20_encrypt(&ctx->b_cipher, blk, len);
}

static void ccp_decrypt(void *vctx, unsigned char *blk, int len)
{
    struct ccp_context *ctx = (struct ccp_context *)vctx;
    chacha20_decrypt(&ctx->b_cipher, blk, len);
}

static void ccp_length_op(struct ccp_context *ctx, unsigned char *blk, int len,
                          unsigned long seq)
{
    unsigned char iv[8];
    PUT_32BIT_LSB_FIRST(iv, seq >> 32);
    PUT_32BIT_LSB_FIRST(iv + 4, seq);
    chacha20_iv(&ctx->a_cipher, iv);
    chacha20_iv(&ctx->b_cipher, iv);
    /* Reset content block count to 1, as the first is the key for Poly1305 */
    ++ctx->b_cipher.state[12];
    smemclr(iv, sizeof(iv));
}

static void ccp_encrypt_length(void *vctx, unsigned char *blk, int len,
                               unsigned long seq)
{
    struct ccp_context *ctx = (struct ccp_context *)vctx;
    ccp_length_op(ctx, blk, len, seq);
    chacha20_encrypt(&ctx->a_cipher, blk, len);
}

static void ccp_decrypt_length(void *vctx, unsigned char *blk, int len,
                               unsigned long seq)
{
    struct ccp_context *ctx = (struct ccp_context *)vctx;
    ccp_length_op(ctx, blk, len, seq);
    chacha20_decrypt(&ctx->a_cipher, blk, len);
}

static const struct ssh2_cipher ssh2_chacha20_poly1305 = {

    ccp_make_context,
    ccp_free_context,
    ccp_iv,
    ccp_key,
    ccp_encrypt,
    ccp_decrypt,
    ccp_encrypt_length,
    ccp_decrypt_length,

    "chacha20-poly1305@openssh.com",
    1, 512, SSH_CIPHER_SEPARATE_LENGTH, "ChaCha20 Poly1305",

    &ssh2_poly1305
};

static const struct ssh2_cipher *const ccp_list[] = {
    &ssh2_chacha20_poly1305
};

const struct ssh2_ciphers ssh2_ccp = {
    sizeof(ccp_list) / sizeof(*ccp_list),
    ccp_list
};
