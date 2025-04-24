#include "config.h"
#include <bitcoin/address.h>
#include <bitcoin/base58.h>
#include <bitcoin/message.h>
#include <bitcoin/privkey.h>
#include <bitcoin/pubkey.h>
#include <ccan/array_size/array_size.h>
#include <ccan/json_out/json_out.h>
#include <ccan/tal/str/str.h>
#include <common/json_param.h>
#include <common/json_stream.h>
#include <common/memleak.h>
#include <common/node_id.h>
#include <common/setup.h>
#include <plugins/libplugin.h>

/*
 * BIP-0322 Message Authentication Plugin for Core Lightning
 * This plugin provides commands for signing and verifying messages
 * using the BIP-0322 generic signed message format.
 */

static struct command_result *json_signmessage(struct command *cmd,
                                             const char *buffer,
                                             const jsmntok_t *params)
{
    const char *message;
    struct json_out *json;
    struct pubkey pubkey;
    struct privkey privkey;
    char *sig;

    if (!param(cmd, buffer, params,
             p_req("message", param_string, &message),
             NULL))
        return command_param_failed();

    /* Get node private key from hsmd */
    if (!get_hsm_secret(&privkey))
        return command_fail(cmd, LIGHTNINGD,
                          "Could not get private key from hsmd");

    /* Get public key for our node */
    if (!pubkey_from_privkey(&privkey, &pubkey))
        return command_fail(cmd, LIGHTNINGD,
                          "Could not derive public key");

    /* Sign the message using BIP-0322 */
    sig = bip322_sign_message(cmd, message, &privkey);
    if (!sig)
        return command_fail(cmd, LIGHTNINGD,
                          "Could not sign message");

    /* Provide signature and related information */
    json = json_out_new(cmd);
    json_out_start(json, NULL);
    json_out_add(json, "signature", sig);
    json_out_add_hex(json, "pubkey", &pubkey, sizeof(pubkey));
    json_out_add(json, "message", message);
    json_out_end(json);

    return command_success(cmd, json_out_finished(json));
}

static struct command_result *json_verifymessage(struct command *cmd,
                                               const char *buffer,
                                               const jsmntok_t *params)
{
    const char *message;
    const char *signature;
    const char *addr_str;
    struct json_out *json;
    struct bitcoin_address address;
    struct ripemd160 hash;
    bool valid;

    if (!param(cmd, buffer, params,
             p_req("message", param_string, &message),
             p_req("signature", param_string, &signature),
             p_req("address", param_string, &addr_str),
             NULL))
        return command_param_failed();

    /* Decode the Bitcoin address */
    if (!bitcoin_from_base58(&testnet, addr_str, strlen(addr_str), &hash, NULL)) {
        if (!bitcoin_from_base58(&mainnet, addr_str, strlen(addr_str), &hash, NULL)) {
            return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
                              "Could not decode Bitcoin address");
        }
    }

    /* Copy hash to bitcoin_address struct */
    memcpy(&address.addr, &hash, sizeof(struct ripemd160));

    /* Verify the message signature */
    valid = bip322_verify_message(message, &address, signature);

    /* Return the verification result */
    json = json_out_new(cmd);
    json_out_start(json, NULL);
    json_out_add_bool(json, "valid", valid);
    json_out_add(json, "message", message);
    json_out_add(json, "address", addr_str);
    json_out_end(json);

    return command_success(cmd, json_out_finished(json));
}

/* Plugin initialization */
static const struct plugin_command commands[] = {
    {
        "signmessage",
        "utility",
        "Sign a message with this node's key using BIP-0322",
        "Sign {message} using the node's key with the BIP-0322 generic message signing format",
        json_signmessage
    },
    {
        "verifymessage",
        "utility",
        "Verify a BIP-0322 message signature",
        "Verify a {message} against a {signature} for a Bitcoin {address}",
        json_verifymessage
    }
};

int main(int argc, char *argv[])
{
    setup_locale();
    plugin_main(argv, init, PLUGIN_STATIC, true, NULL, commands, ARRAY_SIZE(commands),
               NULL, 0, NULL, 0, NULL);
}
