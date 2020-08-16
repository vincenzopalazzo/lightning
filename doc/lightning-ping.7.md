lightning-ping -- Command to determinate whether a remote lightning node is active.
============================================================

SYNOPSIS
--------

**ping** id \[len\] \[pongbytes\] 

DESCRIPTION
-----------

The **ping** is a RPC command wich the called can determine whether a remote lightning node is active or inactive, the command accept the following propieties:

- *id*: A string that rappresent the node id wich the node is connected yet. the caller can conncet the node with the command *connect*.
- *len*: A integer that rappresent the lenght of ping result, by default is 128 and the maximum value accepted is 65529. 
- *pongbytes*: A integer that rappresent lenght of node answer at the ping quest, by default is 128 the maximum value accepted is 65535.

EXAMPLE JSON REQUEST
------------
```json
{
  "id": 82,
  "method": "ping",
  "params": {
    "id": "some_node_id",
    "len": 128,
    "pongbytes": 128,
  }
}
```

RETURN VALUE
------------

On success, a object is return with a integer proprity *totlen*.

- *totlen*: An integer that rappresent

On failure, one of the following error codes may be returned:

- -32602. Error in given parameters.

EXAMPLE JSON RESPONSE
-----
```json
{
   "totlen": 182
}
```


AUTHOR
------

Vincenzo Palazzo <<vincenzo.palazzo@protonmail.com>> wrote the initial version of this man page, but many others did the hard work of actually implementing this rpc command.

SEE ALSO
--------

lightning-connect(7), lightning-close(7)

RESOURCES
---------

Main web site: <https://github.com/ElementsProject/lightning>
