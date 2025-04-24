#include "config.h"
#include <bitcoin/address.h>
#include <bitcoin/base58.h>
#include <bitcoin/message.h>
#include <bitcoin/privkey.h>
#include <bitcoin/pubkey.h>
#include <bitcoin/script.h>
#include <bitcoin/shadouble.h>
#include <bitcoin/signature.h>
#include <bitcoin/tx.h>
#include <ccan/crypto/sha256/sha256.h>
#include <ccan/mem/mem.h>
#include <ccan/str/hex/hex.h>
#include <ccan/tal/str/str.h>
#include <common/utils.h>
#include <string.h>

/* BIP-0322 magic bytes for message signing */
static const unsigned char MAGIC_BYTES[] = {"\x18Bitcoin Signed Message:\n"};

/*
 * Implementation of BIP-0322 Generic Signed Message Format
 * https://github.com/bitcoin/bips/blob/master/bip-0322.mediawiki
 */

/* Hash a message for signing according to BIP-0322 */
static void bip322_hash_message(const char *msg, struct sha256_double *hash)
{
    struct sha256_ctx ctx;
    size_t msg_len = strlen(msg);

    /* Initialize SHA256 hash with first round */
    sha256_init(&ctx);

    /* Add magic bytes */
    sha256_update(&ctx, MAGIC_BYTES + 1, MAGIC_BYTES[0]);

    /* Add message length as varint */
    u8 varint[9];
    size_t varint_len = 0;

    if (msg_len < 253) {
        varint[0] = msg_len;
        varint_len = 1;
    } else if (msg_len <= 0xFFFF) {
        varint[0] = 253;
        varint[1] = (msg_len >> 0) & 0xFF;
        varint[2] = (msg_len >> 8) & 0xFF;
        varint_len = 3;
    } else if (msg_len <= 0xFFFFFFFF) {
        varint[0] = 254;
        varint[1] = (msg_len >> 0) & 0xFF;
        varint[2] = (msg_len >> 8) & 0xFF;
        varint[3] = (msg_len >> 16) & 0xFF;
        varint[4] = (msg_len >> 24) & 0xFF;
        varint_len = 5;
    } else {
        varint[0] = 255;
        varint[1] = (msg_len >> 0) & 0xFF;
        varint[2] = (msg_len >> 8) & 0xFF;
        varint[3] = (msg_len >> 16) & 0xFF;
        varint[4] = (msg_len >> 24) & 0xFF;
        varint[5] = (msg_len >> 32) & 0xFF;
        varint[6] = (msg_len >> 40) & 0xFF;
        varint[7] = (msg_len >> 48) & 0xFF;
        varint[8] = (msg_len >> 56) & 0xFF;
        varint_len = 9;
    }

    /* Add the varint length prefix */
    sha256_update(&ctx, varint, varint_len);

    /* Add message */
    sha256_update(&ctx, msg, msg_len);

    /* Get the first SHA256 hash */
    struct sha256 first_sha;
    sha256_done(&ctx, &first_sha);

    /* Double SHA256 */
    sha256(&hash->sha, &first_sha, sizeof(first_sha));
}

/*
 * BIP-0322 lays out a very specific transaction format for message signing
 * that uses a spend-to-verify template.
 */
static struct bitcoin_tx *bip322_build_sign_tx(const tal_t *ctx,
                                              const struct sha256_double *hash,
                                              const struct pubkey *pubkey)
{
    struct bitcoin_tx *tx;
    u8 *scriptPubKey;

    /* Create a new transaction */
    tx = bitcoin_tx(ctx, 2, 1);

    /* Create P2WPKH scriptPubKey for this pubkey */
    scriptPubKey = scriptpubkey_p2wpkh(NULL, pubkey);

    /* First input is a dummy with the hash of the message as outpoint */
    tx->wtx->inputs[0].txhash = hash->sha.u.u8;
    tx->wtx->inputs[0].index = 0;
    tx->wtx->inputs[0].script = NULL;
    tx->wtx->inputs[0].script_len = 0;
    tx->wtx->inputs[0].witness = NULL;

    /* Second input is the signature from key */
    tx->wtx->inputs[1].txhash = hash->sha.u.u8; /* Same hash for both inputs */
    tx->wtx->inputs[1].index = 1;
    tx->wtx->inputs[1].script = NULL;
    tx->wtx->inputs[1].script_len = 0;
    /* Witness will be added during signing */

    /* Output is an OP_RETURN with magic "BIP0322" and hash */
    u8 *output_script = tal_arr(ctx, u8, 6 + sizeof(struct sha256));
    output_script[0] = OP_RETURN;
    output_script[1] = 0x24; /* Push 36 bytes */
    memcpy(output_script + 2, "BIP0322", 7);
    memcpy(output_script + 9, hash->sha.u.u8, sizeof(struct sha256));

    tx->wtx->outputs[0].script = output_script;
    tx->wtx->outputs[0].script_len = tal_count(output_script);
    tx->wtx->outputs[0].satoshi = 0; /* Zero value output */

    tal_free(scriptPubKey);
    return tx;
}

/*
 * Sign a message using the BIP-0322 generic signed message format
 */
char *bip322_sign_message(const tal_t *ctx,
                         const char *msg,
                         const struct privkey *privkey)
{
    struct sha256_double hash;
    struct pubkey pubkey;
    struct bitcoin_tx *tx;
    struct bitcoin_signature sig;
    secp256k1_ecdsa_signature signature;
    u8 *witness_script;
    char *result;

    /* Get the hash of the message according to BIP-0322 */
    bip322_hash_message(msg, &hash);

    /* Derive public key from private key */
    if (!pubkey_from_privkey(privkey, &pubkey))
        return NULL;

    /* Build the transaction template for signing */
    tx = bip322_build_sign_tx(ctx, &hash, &pubkey);

    /* Create the P2WPKH witness script (scriptCode) for this pubkey */
    struct ripemd160 pubkey_hash;
    pubkey_to_hash160(&pubkey, &pubkey_hash);
    witness_script = bitcoin_redeem_p2wpkh(ctx, &pubkey);

    /* Sign the transaction (second input only) */
    sign_tx_input(tx, 1, NULL, witness_script,
                 privkey, &pubkey, SIGHASH_ALL, &sig);

    /* Create the witness stack for the signature */
    u8 **witness = tal_arr(ctx, u8*, 2);
    u8 der[73];
    size_t der_len = signature_to_der(der, &sig);
    witness[0] = tal_arr(witness, u8, der_len);
    memcpy(witness[0], der, der_len);

    witness[1] = tal_arr(witness, u8, PUBKEY_CMPR_LEN);
    pubkey_to_der(witness[1], &pubkey);

    /* Attach witness to transaction */
    tx->wtx->inputs[1].witness = witness;

    /* BIP-0322 signature is base64 encoded transaction */
    u8 *tx_bytes = linearize_tx(ctx, tx);
    result = tal_arr(ctx, char, base64_encoded_length(tal_count(tx_bytes)));
    base64_encode(tx_bytes, tal_count(tx_bytes), result, base64_encoded_length(tal_count(tx_bytes)));

    return result;
}

/*
 * Verify a BIP-0322 formatted message signature
 */
bool bip322_verify_message(const char *msg,
                          const struct bitcoin_address *address,
                          const char *sig)
{
    struct sha256_double hash;
    u8 *tx_bytes = NULL;
    struct bitcoin_tx *tx = NULL;
    size_t tx_bytes_len;
    bool valid = false;
    const tal_t *ctx = NULL;

    /* Create a temporary context for allocations */
    ctx = tal(NULL, char);

    /* Get the hash of the message according to BIP-0322 */
    bip322_hash_message(msg, &hash);

    /* Decode the base64 signature back to raw transaction bytes */
    tx_bytes_len = base64_decoded_length(strlen(sig));
    tx_bytes = tal_arr(ctx, u8, tx_bytes_len);
    if (!base64_decode(sig, strlen(sig), tx_bytes, tx_bytes_len))
        goto cleanup;

    /* Parse the transaction */
    tx = pull_bitcoin_tx(ctx, &tx_bytes, &tx_bytes_len);
    if (!tx)
        goto cleanup;

    /* Basic BIP-0322 verification checks */

    /* 1. Check tx has exactly 2 inputs and 1 output */
    if (tx->wtx->num_inputs != 2 || tx->wtx->num_outputs != 1)
        goto cleanup;

    /* 2. Check the first outpoint matches our message hash */
    if (memcmp(tx->wtx->inputs[0].txhash, hash.sha.u.u8, sizeof(hash.sha.u.u8)) != 0 ||
        tx->wtx->inputs[0].index != 0)
        goto cleanup;

    /* 3. Check the second outpoint uses the same transaction hash */
    if (memcmp(tx->wtx->inputs[1].txhash, hash.sha.u.u8, sizeof(hash.sha.u.u8)) != 0 ||
        tx->wtx->inputs[1].index != 1)
        goto cleanup;

    /* 4. Check the output is an OP_RETURN with our expected data */
    if (tx->wtx->outputs[0].satoshi != 0 ||
        tx->wtx->outputs[0].script_len < 9 ||
        tx->wtx->outputs[0].script[0] != OP_RETURN)
        goto cleanup;

    /* 5. Check the OP_RETURN contains "BIP0322" and the message hash */
    if (memcmp(tx->wtx->outputs[0].script + 2, "BIP0322", 7) != 0 ||
        memcmp(tx->wtx->outputs[0].script + 9, hash.sha.u.u8, sizeof(struct sha256)) != 0)
        goto cleanup;

    /* 6. Verify the witness data for the second input */
    if (!tx->wtx->inputs[1].witness ||
        !tx->wtx->inputs[1].witness[0] ||
        !tx->wtx->inputs[1].witness[1])
        goto cleanup;

    /* Extract pubkey from witness */
    struct pubkey pubkey;
    if (!pubkey_from_der(tx->wtx->inputs[1].witness[1],
                         tal_bytelen(tx->wtx->inputs[1].witness[1]),
                         &pubkey))
        goto cleanup;

    /* Convert pubkey to address and check against the provided address */
    struct ripemd160 pubkey_hash;
    pubkey_to_hash160(&pubkey, &pubkey_hash);

    /* Check if the derived address matches the input address */
    if (memcmp(&pubkey_hash, &address->addr, sizeof(struct ripemd160)) != 0)
        goto cleanup;

    /* Extract signature from witness */
    struct bitcoin_signature sig_check;
    if (!signature_from_der(tx->wtx->inputs[1].witness[0],
                           tal_bytelen(tx->wtx->inputs[1].witness[0]),
                           &sig_check))
        goto cleanup;

    /* Create P2WPKH witness script for verification */
    u8 *witness_script = bitcoin_redeem_p2wpkh(ctx, &pubkey);

    /* Verify the signature */
    valid = check_tx_sig(tx, 1, NULL, witness_script, &pubkey, &sig_check);

cleanup:
    tal_free(ctx);
    return valid;
}
