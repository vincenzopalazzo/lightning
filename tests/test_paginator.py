"""
I think I'm getting to excited for this paginator API
that I think this deserve a own python test file!
"""
from fixtures import * # noqua: F401,F403


def test_listnodes_paginator(node_factory):
    """
    We run 4 nodes and then we query the
    list node by batch for the other two nodes.
    """
    l1, l2, l3, _ = node_factory.line_graph(4, fundchannel=True, wait_for_announce=True)
    l1.rpc.jsonschemas = {}

    nodes = l1.rpc.call("listnodes", { "batch": [l3.info["id"], l2.info["id"]] })
    nodes_nobatch = l1.rpc.listnodes()
    print(nodes)
    print(nodes_nobatch)
    assert len(nodes["nodes"]) == 2
    assert len(nodes_nobatch["nodes"]) == 4
