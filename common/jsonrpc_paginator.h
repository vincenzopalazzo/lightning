/**
 * Welcome in this wanderful experience of writing
 * a paginator for the core lightning JSON RPC 2.0 in C!
 *
 *
 * I will try to keep the code as simple as possible,
 * so if you had any dout on what I'm trying to do, blame me
 * that I was not able to do my job.
 *
 *
 * In short, the goal of this paginator is offer a struct
 * that it is used to grep the paginator information by
 * pre processin the JSON RPC request.
 *
 * So let immagine a normal listnode RPC call, where
 * cli | rest | grpc -> json_listnodes -> json response {....}
 *
 * if the paginator will be enbaled on the listnodes, the
 * call will be
 *
 * cli | rest | grpc -> json_paginator_listnodes -> json_listnodes + struct jsonrpc_paginator -> sjon response { .... }
 *
 * Done, this should be all!
 *
 *
 * Now let see how this is implemented.
 * */
#ifndef JSONRPC_PAGINATOR_H
#define JSONRPC_PAGINATOR_H

#include <ccan/tal/tal.h>
#include <ccan/short_types/short_types.h>

/**
 * jsonrpc_paginator - core struct where all the paginator information
 * are stored,
 *
 * A developer that want to extend the functionality of the
 * paginator must not abuse of this struct to avoid to made the logic
 * messy, thanks!
 *
 * @batch: array string to be able to query a list of things, useful also
 * when you deal with reading stuff from file to avoid reaccess to the file
 * to search another item.
 *
 * @offset: a position (u64) that determines the number of element (in SQL row)
 * returned by request.
 * @limit: a position (u64) that give the number of element in [0,..,offset - 1] to skip skip.
 */
struct jsonrpc_paginator {
	/** reall usefult for access to gossip map */
	const char **batch;
	/** query the database */
	const u64 *offset;
	const u64 *limit;
	/* FIXME: more smarter one? like sort_by = "json key"
	 * but this required to have a mapping between json_keys and sql keys
	 * maybe we had already somethings in the sql plugin? */
};

/** new_paginator - helper function to create a new paginator from a list of parameter.
 * This is simple enought to be avoided, but write simple code inside the C macros
 * is frustating, so let use this insteand.
 *
 * BTW: I love C Macros, really!
 * */
static inline struct jsonrpc_paginator *new_paginator(const tal_t *ctx, const char **batch,
					      const u64 *limit, const u64 *offset)
{
	struct jsonrpc_paginator *paginator = NULL;
	if (batch || limit || offset) {
		paginator = tal(ctx, struct jsonrpc_paginator);
		paginator->batch = batch;
		paginator->limit = limit;
		paginator->offset = offset;
		return paginator;
	}
	return NULL;
}

#endif // JSONRPC_PAGINATOR_H
