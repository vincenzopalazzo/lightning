#ifndef LIGHTNING_LIGHTNINGD_WAIT_H
#define LIGHTNING_LIGHTNINGD_WAIT_H
#include "config.h"

struct lightningd;

/* This WAIT_SUBSYSTEM_X corresponds to listX */
enum wait_subsystem {
	WAIT_SUBSYSTEM_INVOICE
};
#define NUM_WAIT_SUBSYSTEM (WAIT_SUBSYSTEM_INVOICE+1)

enum wait_index {
	WAIT_INDEX_CREATED,
	WAIT_INDEX_UPDATED,
	WAIT_INDEX_DELETED,
};
#define NUM_WAIT_INDEX (WAIT_INDEX_DELETED+1)

/**
 * structure for keeping created/updated/deleted indexes in the db
 */
struct indexes {
	u64 i[NUM_WAIT_INDEX];
};

/* Get an index value */
u64 *index_get(struct lightningd *ld,
	       enum wait_subsystem subsystem, enum wait_index index);

/**
 * index_update - update an index, tell waiters.
 * @ld: the lightningd
 * @subsystem: subsystem for index
 * @index: which index
 * @increase: amount to increase by (usually 1!)
 * ...: name/value pairs, followed by NULL.
 *
 * Increase index, wake any waiters, give them any name/value pairs.
 * If the value is NULL, omit that name.
 * If the name starts with '=', the value is a JSON literal (and skip over the =)
 *
 * Returns the updated index value.
 */
u64 LAST_ARG_NULL index_update(struct lightningd *ld,
				enum wait_subsystem subsystem,
				enum wait_index index,
				u64 increase,
				...);

#endif /* LIGHTNING_LIGHTNINGD_WAIT_H */
