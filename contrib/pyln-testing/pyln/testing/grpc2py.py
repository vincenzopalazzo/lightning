# This file was automatically derived from the JSON-RPC schemas in
# `doc/schemas`. Do not edit this file manually as it would get
# overwritten.

import json


def hexlify(b):
    return b if b is None else b.hex()

def amount2msat(a):
    return a.msat

def amount_or_all2msat(a):
    breakpoint()


def remove_default(d):
    # grpc is really not good at empty values, they get replaced with the type's default value...
    return {k: v for k, v in d.items() if v is not None and v != ""}


def getinfo_our_features2py(m):
    return remove_default({
        "init": hexlify(m.init),  # PrimitiveField in generate_composite
        "node": hexlify(m.node),  # PrimitiveField in generate_composite
        "channel": hexlify(m.channel),  # PrimitiveField in generate_composite
        "invoice": hexlify(m.invoice),  # PrimitiveField in generate_composite
    })


def getinfo_address2py(m):
    return remove_default({
        "type": str(m.item_type),  # EnumField in generate_composite
        "port": m.port,  # PrimitiveField in generate_composite
        "address": m.address,  # PrimitiveField in generate_composite
    })


def getinfo_binding2py(m):
    return remove_default({
        "type": str(m.item_type),  # EnumField in generate_composite
        "address": m.address,  # PrimitiveField in generate_composite
        "port": m.port,  # PrimitiveField in generate_composite
        "socket": m.socket,  # PrimitiveField in generate_composite
    })


def getinfo2py(m):
    return remove_default({
        "id": hexlify(m.id),  # PrimitiveField in generate_composite
        "alias": m.alias,  # PrimitiveField in generate_composite
        "color": hexlify(m.color),  # PrimitiveField in generate_composite
        "num_peers": m.num_peers,  # PrimitiveField in generate_composite
        "num_pending_channels": m.num_pending_channels,  # PrimitiveField in generate_composite
        "num_active_channels": m.num_active_channels,  # PrimitiveField in generate_composite
        "num_inactive_channels": m.num_inactive_channels,  # PrimitiveField in generate_composite
        "version": m.version,  # PrimitiveField in generate_composite
        "lightning_dir": m.lightning_dir,  # PrimitiveField in generate_composite
        "blockheight": m.blockheight,  # PrimitiveField in generate_composite
        "network": m.network,  # PrimitiveField in generate_composite
        "fees_collected_msat": amount2msat(m.fees_collected_msat),  # PrimitiveField in generate_composite
        "address": [getinfo_address2py(i) for i in m.address],  # ArrayField[composite] in generate_composite
        "binding": [getinfo_binding2py(i) for i in m.binding],  # ArrayField[composite] in generate_composite
        "warning_bitcoind_sync": m.warning_bitcoind_sync,  # PrimitiveField in generate_composite
        "warning_lightningd_sync": m.warning_lightningd_sync,  # PrimitiveField in generate_composite
    })


def listpeers_peers_log2py(m):
    return remove_default({
        "type": str(m.item_type),  # EnumField in generate_composite
        "num_skipped": m.num_skipped,  # PrimitiveField in generate_composite
        "time": m.time,  # PrimitiveField in generate_composite
        "source": m.source,  # PrimitiveField in generate_composite
        "log": m.log,  # PrimitiveField in generate_composite
        "node_id": hexlify(m.node_id),  # PrimitiveField in generate_composite
        "data": hexlify(m.data),  # PrimitiveField in generate_composite
    })


def listpeers_peers2py(m):
    return remove_default({
        "id": hexlify(m.id),  # PrimitiveField in generate_composite
        "connected": m.connected,  # PrimitiveField in generate_composite
        "log": [listpeers_peers_log2py(i) for i in m.log],  # ArrayField[composite] in generate_composite
        "netaddr": [m.netaddr for i in m.netaddr], # ArrayField[primitive] in generate_composite
        "remote_addr": m.remote_addr,  # PrimitiveField in generate_composite
        "features": hexlify(m.features),  # PrimitiveField in generate_composite
    })


def listpeers2py(m):
    return remove_default({
        "peers": [listpeers_peers2py(i) for i in m.peers],  # ArrayField[composite] in generate_composite
    })


def listfunds_outputs2py(m):
    return remove_default({
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
        "output": m.output,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "scriptpubkey": hexlify(m.scriptpubkey),  # PrimitiveField in generate_composite
        "address": m.address,  # PrimitiveField in generate_composite
        "redeemscript": hexlify(m.redeemscript),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "reserved": m.reserved,  # PrimitiveField in generate_composite
        "blockheight": m.blockheight,  # PrimitiveField in generate_composite
    })


def listfunds_channels2py(m):
    return remove_default({
        "peer_id": hexlify(m.peer_id),  # PrimitiveField in generate_composite
        "our_amount_msat": amount2msat(m.our_amount_msat),  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "funding_txid": hexlify(m.funding_txid),  # PrimitiveField in generate_composite
        "funding_output": m.funding_output,  # PrimitiveField in generate_composite
        "connected": m.connected,  # PrimitiveField in generate_composite
        "state": str(m.state),  # EnumField in generate_composite
        "short_channel_id": m.short_channel_id,  # PrimitiveField in generate_composite
    })


def listfunds2py(m):
    return remove_default({
        "outputs": [listfunds_outputs2py(i) for i in m.outputs],  # ArrayField[composite] in generate_composite
        "channels": [listfunds_channels2py(i) for i in m.channels],  # ArrayField[composite] in generate_composite
    })


def sendpay2py(m):
    return remove_default({
        "id": m.id,  # PrimitiveField in generate_composite
        "groupid": m.groupid,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "destination": hexlify(m.destination),  # PrimitiveField in generate_composite
        "created_at": m.created_at,  # PrimitiveField in generate_composite
        "amount_sent_msat": amount2msat(m.amount_sent_msat),  # PrimitiveField in generate_composite
        "label": m.label,  # PrimitiveField in generate_composite
        "partid": m.partid,  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
        "message": m.message,  # PrimitiveField in generate_composite
    })


def listchannels_channels2py(m):
    return remove_default({
        "source": hexlify(m.source),  # PrimitiveField in generate_composite
        "destination": hexlify(m.destination),  # PrimitiveField in generate_composite
        "short_channel_id": m.short_channel_id,  # PrimitiveField in generate_composite
        "public": m.public,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "message_flags": m.message_flags,  # PrimitiveField in generate_composite
        "channel_flags": m.channel_flags,  # PrimitiveField in generate_composite
        "active": m.active,  # PrimitiveField in generate_composite
        "last_update": m.last_update,  # PrimitiveField in generate_composite
        "base_fee_millisatoshi": m.base_fee_millisatoshi,  # PrimitiveField in generate_composite
        "fee_per_millionth": m.fee_per_millionth,  # PrimitiveField in generate_composite
        "delay": m.delay,  # PrimitiveField in generate_composite
        "htlc_minimum_msat": amount2msat(m.htlc_minimum_msat),  # PrimitiveField in generate_composite
        "htlc_maximum_msat": amount2msat(m.htlc_maximum_msat),  # PrimitiveField in generate_composite
        "features": hexlify(m.features),  # PrimitiveField in generate_composite
    })


def listchannels2py(m):
    return remove_default({
        "channels": [listchannels_channels2py(i) for i in m.channels],  # ArrayField[composite] in generate_composite
    })


def addgossip2py(m):
    return remove_default({
    })


def autocleaninvoice2py(m):
    return remove_default({
        "enabled": m.enabled,  # PrimitiveField in generate_composite
        "expired_by": m.expired_by,  # PrimitiveField in generate_composite
        "cycle_seconds": m.cycle_seconds,  # PrimitiveField in generate_composite
    })


def checkmessage2py(m):
    return remove_default({
        "verified": m.verified,  # PrimitiveField in generate_composite
        "pubkey": hexlify(m.pubkey),  # PrimitiveField in generate_composite
    })


def close2py(m):
    return remove_default({
        "type": str(m.item_type),  # EnumField in generate_composite
        "tx": hexlify(m.tx),  # PrimitiveField in generate_composite
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
    })


def connect_address2py(m):
    return remove_default({
        "type": str(m.item_type),  # EnumField in generate_composite
        "socket": m.socket,  # PrimitiveField in generate_composite
        "address": m.address,  # PrimitiveField in generate_composite
        "port": m.port,  # PrimitiveField in generate_composite
    })


def connect2py(m):
    return remove_default({
        "id": hexlify(m.id),  # PrimitiveField in generate_composite
        "features": hexlify(m.features),  # PrimitiveField in generate_composite
        "direction": str(m.direction),  # EnumField in generate_composite
    })


def createinvoice2py(m):
    return remove_default({
        "label": m.label,  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "description": m.description,  # PrimitiveField in generate_composite
        "expires_at": m.expires_at,  # PrimitiveField in generate_composite
        "pay_index": m.pay_index,  # PrimitiveField in generate_composite
        "amount_received_msat": amount2msat(m.amount_received_msat),  # PrimitiveField in generate_composite
        "paid_at": m.paid_at,  # PrimitiveField in generate_composite
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
        "local_offer_id": hexlify(m.local_offer_id),  # PrimitiveField in generate_composite
        "payer_note": m.payer_note,  # PrimitiveField in generate_composite
    })


def datastore2py(m):
    return remove_default({
        "key": [m.key for i in m.key], # ArrayField[primitive] in generate_composite
        "generation": m.generation,  # PrimitiveField in generate_composite
        "hex": hexlify(m.hex),  # PrimitiveField in generate_composite
        "string": m.string,  # PrimitiveField in generate_composite
    })


def createonion2py(m):
    return remove_default({
        "onion": hexlify(m.onion),  # PrimitiveField in generate_composite
        "shared_secrets": [hexlify(m.shared_secrets) for i in hexlify(m.shared_secrets)], # ArrayField[primitive] in generate_composite
    })


def deldatastore2py(m):
    return remove_default({
        "key": [m.key for i in m.key], # ArrayField[primitive] in generate_composite
        "generation": m.generation,  # PrimitiveField in generate_composite
        "hex": hexlify(m.hex),  # PrimitiveField in generate_composite
        "string": m.string,  # PrimitiveField in generate_composite
    })


def delexpiredinvoice2py(m):
    return remove_default({
    })


def delinvoice2py(m):
    return remove_default({
        "label": m.label,  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "description": m.description,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "expires_at": m.expires_at,  # PrimitiveField in generate_composite
        "local_offer_id": hexlify(m.local_offer_id),  # PrimitiveField in generate_composite
        "payer_note": m.payer_note,  # PrimitiveField in generate_composite
    })


def invoice2py(m):
    return remove_default({
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "payment_secret": hexlify(m.payment_secret),  # PrimitiveField in generate_composite
        "expires_at": m.expires_at,  # PrimitiveField in generate_composite
        "warning_capacity": m.warning_capacity,  # PrimitiveField in generate_composite
        "warning_offline": m.warning_offline,  # PrimitiveField in generate_composite
        "warning_deadends": m.warning_deadends,  # PrimitiveField in generate_composite
        "warning_private_unused": m.warning_private_unused,  # PrimitiveField in generate_composite
        "warning_mpp": m.warning_mpp,  # PrimitiveField in generate_composite
    })


def listdatastore_datastore2py(m):
    return remove_default({
        "key": [m.key for i in m.key], # ArrayField[primitive] in generate_composite
        "generation": m.generation,  # PrimitiveField in generate_composite
        "hex": hexlify(m.hex),  # PrimitiveField in generate_composite
        "string": m.string,  # PrimitiveField in generate_composite
    })


def listdatastore2py(m):
    return remove_default({
        "datastore": [listdatastore_datastore2py(i) for i in m.datastore],  # ArrayField[composite] in generate_composite
    })


def listinvoices_invoices2py(m):
    return remove_default({
        "label": m.label,  # PrimitiveField in generate_composite
        "description": m.description,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "expires_at": m.expires_at,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "local_offer_id": hexlify(m.local_offer_id),  # PrimitiveField in generate_composite
        "payer_note": m.payer_note,  # PrimitiveField in generate_composite
        "pay_index": m.pay_index,  # PrimitiveField in generate_composite
        "amount_received_msat": amount2msat(m.amount_received_msat),  # PrimitiveField in generate_composite
        "paid_at": m.paid_at,  # PrimitiveField in generate_composite
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
    })


def listinvoices2py(m):
    return remove_default({
        "invoices": [listinvoices_invoices2py(i) for i in m.invoices],  # ArrayField[composite] in generate_composite
    })


def sendonion2py(m):
    return remove_default({
        "id": m.id,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "destination": hexlify(m.destination),  # PrimitiveField in generate_composite
        "created_at": m.created_at,  # PrimitiveField in generate_composite
        "amount_sent_msat": amount2msat(m.amount_sent_msat),  # PrimitiveField in generate_composite
        "label": m.label,  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "partid": m.partid,  # PrimitiveField in generate_composite
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
        "message": m.message,  # PrimitiveField in generate_composite
    })


def listsendpays_payments2py(m):
    return remove_default({
        "id": m.id,  # PrimitiveField in generate_composite
        "groupid": m.groupid,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "destination": hexlify(m.destination),  # PrimitiveField in generate_composite
        "created_at": m.created_at,  # PrimitiveField in generate_composite
        "amount_sent_msat": amount2msat(m.amount_sent_msat),  # PrimitiveField in generate_composite
        "label": m.label,  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "description": m.description,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
        "erroronion": hexlify(m.erroronion),  # PrimitiveField in generate_composite
    })


def listsendpays2py(m):
    return remove_default({
        "payments": [listsendpays_payments2py(i) for i in m.payments],  # ArrayField[composite] in generate_composite
    })


def listtransactions_transactions_inputs2py(m):
    return remove_default({
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
        "index": m.index,  # PrimitiveField in generate_composite
        "sequence": m.sequence,  # PrimitiveField in generate_composite
        "type": str(m.item_type),  # EnumField in generate_composite
        "channel": m.channel,  # PrimitiveField in generate_composite
    })


def listtransactions_transactions_outputs2py(m):
    return remove_default({
        "index": m.index,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "script_pub_key": hexlify(m.script_pub_key),  # PrimitiveField in generate_composite
        "type": str(m.item_type),  # EnumField in generate_composite
        "channel": m.channel,  # PrimitiveField in generate_composite
    })


def listtransactions_transactions2py(m):
    return remove_default({
        "hash": hexlify(m.hash),  # PrimitiveField in generate_composite
        "rawtx": hexlify(m.rawtx),  # PrimitiveField in generate_composite
        "blockheight": m.blockheight,  # PrimitiveField in generate_composite
        "txindex": m.txindex,  # PrimitiveField in generate_composite
        "type": [str(i) for i in m.type],  # ArrayField[composite] in generate_composite
        "channel": m.channel,  # PrimitiveField in generate_composite
        "locktime": m.locktime,  # PrimitiveField in generate_composite
        "version": m.version,  # PrimitiveField in generate_composite
        "inputs": [listtransactions_transactions_inputs2py(i) for i in m.inputs],  # ArrayField[composite] in generate_composite
        "outputs": [listtransactions_transactions_outputs2py(i) for i in m.outputs],  # ArrayField[composite] in generate_composite
    })


def listtransactions2py(m):
    return remove_default({
        "transactions": [listtransactions_transactions2py(i) for i in m.transactions],  # ArrayField[composite] in generate_composite
    })


def pay2py(m):
    return remove_default({
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
        "destination": hexlify(m.destination),  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "created_at": m.created_at,  # PrimitiveField in generate_composite
        "parts": m.parts,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "amount_sent_msat": amount2msat(m.amount_sent_msat),  # PrimitiveField in generate_composite
        "warning_partial_completion": m.warning_partial_completion,  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
    })


def listnodes_nodes_addresses2py(m):
    return remove_default({
        "type": str(m.item_type),  # EnumField in generate_composite
        "port": m.port,  # PrimitiveField in generate_composite
        "address": m.address,  # PrimitiveField in generate_composite
    })


def listnodes_nodes2py(m):
    return remove_default({
        "nodeid": hexlify(m.nodeid),  # PrimitiveField in generate_composite
        "last_timestamp": m.last_timestamp,  # PrimitiveField in generate_composite
        "alias": m.alias,  # PrimitiveField in generate_composite
        "color": hexlify(m.color),  # PrimitiveField in generate_composite
        "features": hexlify(m.features),  # PrimitiveField in generate_composite
        "addresses": [listnodes_nodes_addresses2py(i) for i in m.addresses],  # ArrayField[composite] in generate_composite
    })


def listnodes2py(m):
    return remove_default({
        "nodes": [listnodes_nodes2py(i) for i in m.nodes],  # ArrayField[composite] in generate_composite
    })


def waitanyinvoice2py(m):
    return remove_default({
        "label": m.label,  # PrimitiveField in generate_composite
        "description": m.description,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "expires_at": m.expires_at,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "pay_index": m.pay_index,  # PrimitiveField in generate_composite
        "amount_received_msat": amount2msat(m.amount_received_msat),  # PrimitiveField in generate_composite
        "paid_at": m.paid_at,  # PrimitiveField in generate_composite
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
    })


def waitinvoice2py(m):
    return remove_default({
        "label": m.label,  # PrimitiveField in generate_composite
        "description": m.description,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "expires_at": m.expires_at,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "pay_index": m.pay_index,  # PrimitiveField in generate_composite
        "amount_received_msat": amount2msat(m.amount_received_msat),  # PrimitiveField in generate_composite
        "paid_at": m.paid_at,  # PrimitiveField in generate_composite
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
    })


def waitsendpay2py(m):
    return remove_default({
        "id": m.id,  # PrimitiveField in generate_composite
        "groupid": m.groupid,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "destination": hexlify(m.destination),  # PrimitiveField in generate_composite
        "created_at": m.created_at,  # PrimitiveField in generate_composite
        "amount_sent_msat": amount2msat(m.amount_sent_msat),  # PrimitiveField in generate_composite
        "label": m.label,  # PrimitiveField in generate_composite
        "partid": m.partid,  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
    })


def newaddr2py(m):
    return remove_default({
        "bech32": m.bech32,  # PrimitiveField in generate_composite
        "p2sh_segwit": m.p2sh_segwit,  # PrimitiveField in generate_composite
    })


def withdraw2py(m):
    return remove_default({
        "tx": hexlify(m.tx),  # PrimitiveField in generate_composite
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
        "psbt": m.psbt,  # PrimitiveField in generate_composite
    })


def keysend2py(m):
    return remove_default({
        "payment_preimage": hexlify(m.payment_preimage),  # PrimitiveField in generate_composite
        "destination": hexlify(m.destination),  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "created_at": m.created_at,  # PrimitiveField in generate_composite
        "parts": m.parts,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "amount_sent_msat": amount2msat(m.amount_sent_msat),  # PrimitiveField in generate_composite
        "warning_partial_completion": m.warning_partial_completion,  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
    })


def fundpsbt_reservations2py(m):
    return remove_default({
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
        "vout": m.vout,  # PrimitiveField in generate_composite
        "was_reserved": m.was_reserved,  # PrimitiveField in generate_composite
        "reserved": m.reserved,  # PrimitiveField in generate_composite
        "reserved_to_block": m.reserved_to_block,  # PrimitiveField in generate_composite
    })


def fundpsbt2py(m):
    return remove_default({
        "psbt": m.psbt,  # PrimitiveField in generate_composite
        "feerate_per_kw": m.feerate_per_kw,  # PrimitiveField in generate_composite
        "estimated_final_weight": m.estimated_final_weight,  # PrimitiveField in generate_composite
        "excess_msat": amount2msat(m.excess_msat),  # PrimitiveField in generate_composite
        "change_outnum": m.change_outnum,  # PrimitiveField in generate_composite
        "reservations": [fundpsbt_reservations2py(i) for i in m.reservations],  # ArrayField[composite] in generate_composite
    })


def sendpsbt2py(m):
    return remove_default({
        "tx": hexlify(m.tx),  # PrimitiveField in generate_composite
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
    })


def signpsbt2py(m):
    return remove_default({
        "signed_psbt": m.signed_psbt,  # PrimitiveField in generate_composite
    })


def utxopsbt_reservations2py(m):
    return remove_default({
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
        "vout": m.vout,  # PrimitiveField in generate_composite
        "was_reserved": m.was_reserved,  # PrimitiveField in generate_composite
        "reserved": m.reserved,  # PrimitiveField in generate_composite
        "reserved_to_block": m.reserved_to_block,  # PrimitiveField in generate_composite
    })


def utxopsbt2py(m):
    return remove_default({
        "psbt": m.psbt,  # PrimitiveField in generate_composite
        "feerate_per_kw": m.feerate_per_kw,  # PrimitiveField in generate_composite
        "estimated_final_weight": m.estimated_final_weight,  # PrimitiveField in generate_composite
        "excess_msat": amount2msat(m.excess_msat),  # PrimitiveField in generate_composite
        "change_outnum": m.change_outnum,  # PrimitiveField in generate_composite
        "reservations": [utxopsbt_reservations2py(i) for i in m.reservations],  # ArrayField[composite] in generate_composite
    })


def txdiscard2py(m):
    return remove_default({
        "unsigned_tx": hexlify(m.unsigned_tx),  # PrimitiveField in generate_composite
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
    })


def txprepare2py(m):
    return remove_default({
        "psbt": m.psbt,  # PrimitiveField in generate_composite
        "unsigned_tx": hexlify(m.unsigned_tx),  # PrimitiveField in generate_composite
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
    })


def txsend2py(m):
    return remove_default({
        "psbt": m.psbt,  # PrimitiveField in generate_composite
        "tx": hexlify(m.tx),  # PrimitiveField in generate_composite
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
    })


def disconnect2py(m):
    return remove_default({
    })


def feerates_perkb2py(m):
    return remove_default({
        "min_acceptable": m.min_acceptable,  # PrimitiveField in generate_composite
        "max_acceptable": m.max_acceptable,  # PrimitiveField in generate_composite
        "opening": m.opening,  # PrimitiveField in generate_composite
        "mutual_close": m.mutual_close,  # PrimitiveField in generate_composite
        "unilateral_close": m.unilateral_close,  # PrimitiveField in generate_composite
        "delayed_to_us": m.delayed_to_us,  # PrimitiveField in generate_composite
        "htlc_resolution": m.htlc_resolution,  # PrimitiveField in generate_composite
        "penalty": m.penalty,  # PrimitiveField in generate_composite
    })


def feerates_perkw2py(m):
    return remove_default({
        "min_acceptable": m.min_acceptable,  # PrimitiveField in generate_composite
        "max_acceptable": m.max_acceptable,  # PrimitiveField in generate_composite
        "opening": m.opening,  # PrimitiveField in generate_composite
        "mutual_close": m.mutual_close,  # PrimitiveField in generate_composite
        "unilateral_close": m.unilateral_close,  # PrimitiveField in generate_composite
        "delayed_to_us": m.delayed_to_us,  # PrimitiveField in generate_composite
        "htlc_resolution": m.htlc_resolution,  # PrimitiveField in generate_composite
        "penalty": m.penalty,  # PrimitiveField in generate_composite
    })


def feerates_onchain_fee_estimates2py(m):
    return remove_default({
        "opening_channel_satoshis": m.opening_channel_satoshis,  # PrimitiveField in generate_composite
        "mutual_close_satoshis": m.mutual_close_satoshis,  # PrimitiveField in generate_composite
        "unilateral_close_satoshis": m.unilateral_close_satoshis,  # PrimitiveField in generate_composite
        "htlc_timeout_satoshis": m.htlc_timeout_satoshis,  # PrimitiveField in generate_composite
        "htlc_success_satoshis": m.htlc_success_satoshis,  # PrimitiveField in generate_composite
    })


def feerates2py(m):
    return remove_default({
        "warning_missing_feerates": m.warning_missing_feerates,  # PrimitiveField in generate_composite
    })


def fundchannel2py(m):
    return remove_default({
        "tx": hexlify(m.tx),  # PrimitiveField in generate_composite
        "txid": hexlify(m.txid),  # PrimitiveField in generate_composite
        "outnum": m.outnum,  # PrimitiveField in generate_composite
        "channel_id": hexlify(m.channel_id),  # PrimitiveField in generate_composite
        "close_to": hexlify(m.close_to),  # PrimitiveField in generate_composite
        "mindepth": m.mindepth,  # PrimitiveField in generate_composite
    })


def getroute_route2py(m):
    return remove_default({
        "id": hexlify(m.id),  # PrimitiveField in generate_composite
        "channel": m.channel,  # PrimitiveField in generate_composite
        "direction": m.direction,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "delay": m.delay,  # PrimitiveField in generate_composite
        "style": str(m.style),  # EnumField in generate_composite
    })


def getroute2py(m):
    return remove_default({
        "route": [getroute_route2py(i) for i in m.route],  # ArrayField[composite] in generate_composite
    })


def listforwards_forwards2py(m):
    return remove_default({
        "in_channel": m.in_channel,  # PrimitiveField in generate_composite
        "in_msat": amount2msat(m.in_msat),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "received_time": m.received_time,  # PrimitiveField in generate_composite
        "out_channel": m.out_channel,  # PrimitiveField in generate_composite
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "style": str(m.style),  # EnumField in generate_composite
        "fee_msat": amount2msat(m.fee_msat),  # PrimitiveField in generate_composite
        "out_msat": amount2msat(m.out_msat),  # PrimitiveField in generate_composite
    })


def listforwards2py(m):
    return remove_default({
        "forwards": [listforwards_forwards2py(i) for i in m.forwards],  # ArrayField[composite] in generate_composite
    })


def listpays_pays2py(m):
    return remove_default({
        "payment_hash": hexlify(m.payment_hash),  # PrimitiveField in generate_composite
        "status": str(m.status),  # EnumField in generate_composite
        "destination": hexlify(m.destination),  # PrimitiveField in generate_composite
        "created_at": m.created_at,  # PrimitiveField in generate_composite
        "label": m.label,  # PrimitiveField in generate_composite
        "bolt11": m.bolt11,  # PrimitiveField in generate_composite
        "description": m.description,  # PrimitiveField in generate_composite
        "bolt12": m.bolt12,  # PrimitiveField in generate_composite
        "amount_msat": amount2msat(m.amount_msat),  # PrimitiveField in generate_composite
        "amount_sent_msat": amount2msat(m.amount_sent_msat),  # PrimitiveField in generate_composite
        "erroronion": hexlify(m.erroronion),  # PrimitiveField in generate_composite
    })


def listpays2py(m):
    return remove_default({
        "pays": [listpays_pays2py(i) for i in m.pays],  # ArrayField[composite] in generate_composite
    })


def ping2py(m):
    return remove_default({
        "totlen": m.totlen,  # PrimitiveField in generate_composite
    })


def signmessage2py(m):
    return remove_default({
        "signature": hexlify(m.signature),  # PrimitiveField in generate_composite
        "recid": hexlify(m.recid),  # PrimitiveField in generate_composite
        "zbase": m.zbase,  # PrimitiveField in generate_composite
    })


def stop2py(m):
    return remove_default({
    })
