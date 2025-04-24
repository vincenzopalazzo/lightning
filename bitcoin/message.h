#ifndef LIGHTNING_BITCOIN_MESSAGE_H
#define LIGHTNING_BITCOIN_MESSAGE_H
#include "config.h"
#include <ccan/short_types/short_types.h>
#include <ccan/tal/tal.h>
#include <stdbool.h>

struct bitcoin_address;
struct privkey;
struct pubkey;

/**
 * BIP-0322: Generic Signed Message Format
 *
 * This implements a generic signed message format as defined in BIP-0322.
 * It allows signing arbitrary messages with Bitcoin addresses and scripts
 * in a standardized format that's compatible with the BIP.
 */

/**
 * bip322_sign_message - Sign a message using BIP-322 format
 * @ctx: context to allocate signature from
 * @msg: the message to sign
 * @privkey: the private key to sign with
 *
 * Signs a message using the BIP-0322 standard format.
 * Returns the signature string on success, or NULL on error.
 */
char *bip322_sign_message(const tal_t *ctx,
                         const char *msg,
                         const struct privkey *privkey);

/**
 * bip322_verify_message - Verify a BIP-0322 formatted message signature
 * @msg: the message that was signed
 * @address: the Bitcoin address to verify against
 * @sig: the signature to verify
 *
 * Verifies that a signature for a message was created by the owner of
 * the private key corresponding to the given address.
 * Returns true if signature is valid, false otherwise.
 */
bool bip322_verify_message(const char *msg,
                          const struct bitcoin_address *address,
                          const char *sig);

#endif /* LIGHTNING_BITCOIN_MESSAGE_H */
