#include "config.h"
#include <assert.h>
#include <bitcoin/address.h>
#include <bitcoin/message.h>
#include <bitcoin/privkey.h>
#include <bitcoin/pubkey.h>
#include <ccan/str/hex/hex.h>
#include <common/setup.h>
#include <common/utils.h>
#include <stdio.h>
#include <string.h>

/* Test vectors for BIP-0322 */
static void test_bip322_sign_verify(void)
{
    struct privkey privkey;
    struct pubkey pubkey;
    struct bitcoin_address address;
    char *sig;
    const char *msg = "Hello BIP-0322 from Core Lightning!";

    /* Setup a private key for testing */
    memset(&privkey, 'a', sizeof(privkey));

    /* Derive public key from private key */
    if (!pubkey_from_privkey(&privkey, &pubkey))
        abort();

    /* Get the Bitcoin address for this pubkey */
    pubkey_to_hash160(&pubkey, &address.addr);

    /* Sign the message */
    sig = bip322_sign_message(NULL, msg, &privkey);
    if (!sig)
        abort();

    printf("Message: %s\n", msg);
    printf("Signature: %s\n", sig);

    /* Verify the message signature */
    bool verified = bip322_verify_message(msg, &address, sig);

    /* Ensure verification passed */
    assert(verified);

    /* Test with modified message to ensure verification fails */
    verified = bip322_verify_message("Wrong message", &address, sig);
    assert(!verified);

    tal_free(sig);
}

int main(void)
{
    setup_locale();

    /* Initialize secp256k1 context */
    secp256k1_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY |
                                            SECP256K1_CONTEXT_SIGN);

    /* Run the test */
    test_bip322_sign_verify();

    /* Clean up */
    secp256k1_context_destroy(secp256k1_ctx);

    printf("All BIP-0322 message signature tests passed!\n");
    return 0;
}
