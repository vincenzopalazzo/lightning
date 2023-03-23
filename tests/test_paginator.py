"""
I think I'm getting to excited for this paginator API
that I think this deserve a own python test file!
"""
import unittest
from fixtures import * # noqua: F401,F403
from utils import COMPAT

def test_listnodes_paginator(node_factory):
    """
    We run 4 nodes and then we query the
    list node by batch for the other two nodes.
    """
    l1, l2, l3, _ = node_factory.line_graph(4, fundchannel=True, wait_for_announce=True)
    l1.rpc.jsonschemas = {}

    nodes = l1.rpc.call("listnodes", { "paginator": { "batch": [l3.info["id"], l2.info["id"]] } })
    nodes_nobatch = l1.rpc.listnodes()
    print(nodes)
    print(nodes_nobatch)
    assert len(nodes["nodes"]) == 2
    assert len(nodes_nobatch["nodes"]) == 4


@unittest.skipIf(os.getenv('TEST_DB_PROVIDER', 'sqlite3') != 'sqlite3', "Canned db used")
@unittest.skipIf(not COMPAT, "needs COMPAT to convert obsolete db")
@unittest.skipIf(TEST_NETWORK != 'regtest', "The DB migration is network specific due to the chain var.")
def test_filter_listforwards_from_db(bitcoind, node_factory):
    """This test is taken from the test_db and adapt to support the
    paginator API. This allow to have a static way to assert over the
    forwards without doing crazy things."""
    bitcoind.generate_block(113)
    l1 = node_factory.get_node(dbfile='v0.12.1-forward.sqlite3.xz',
                               options={'database-upgrade': True})

    assert l1.rpc.getinfo()['fees_collected_msat'] == 4
    assert len(l1.rpc.listforwards()['forwards']) == 4
    filter_forwards = l1.rpc.call("listforwards", { "paginator": { "limit": 2, "offset": 0 } })['forwards']
    assert len(filter_forwards) == 2


