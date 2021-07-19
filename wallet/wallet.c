#include "invoices.h"
#include "wallet.h"

#include <bitcoin/psbt.h>
#include <bitcoin/script.h>
#include <ccan/array_size/array_size.h>
#include <ccan/mem/mem.h>
#include <ccan/tal/str/str.h>
#include <common/blockheight_states.h>
#include <common/fee_states.h>
#include <common/key_derive.h>
#include <common/memleak.h>
#include <common/onionreply.h>
#include <common/status.h>
#include <common/wireaddr.h>
#include <inttypes.h>
#include <lightningd/coin_mvts.h>
#include <lightningd/lightningd.h>
#include <lightningd/notification.h>
#include <lightningd/peer_control.h>
#include <onchaind/onchaind_wiregen.h>
#include <string.h>
#include <wallet/db_common.h>

#define SQLITE_MAX_UINT 0x7FFFFFFFFFFFFFFF
#define DIRECTION_INCOMING 0
#define DIRECTION_OUTGOING 1
/* How many blocks must a UTXO entry be buried under to be considered old enough
 * to prune? */
#define UTXO_PRUNE_DEPTH 144

/* 12 hours is usually enough reservation time */
#define RESERVATION_INC (6 * 12)

/* Possible channel state */
enum channel_state_bucket {
	IN_OFFERED = 0,
	IN_FULLFILLED = 1,
	OUT_OFFERED = 2,
	OUT_FULLFILLED = 3,
};

/* channel state identifier */
struct channel_state_param {
	const char *dir_key;
	const char *type_key;
	const enum channel_state_bucket state;
};

static void outpointfilters_init(struct wallet *w)
{
	struct db_stmt *stmt;
	struct utxo **utxos = wallet_get_utxos(NULL, w, OUTPUT_STATE_ANY);
	struct bitcoin_txid txid;
	u32 outnum;

	w->owned_outpoints = outpointfilter_new(w);
	for (size_t i = 0; i < tal_count(utxos); i++)
		outpointfilter_add(w->owned_outpoints, &utxos[i]->txid, utxos[i]->outnum);

	tal_free(utxos);

	w->utxoset_outpoints = outpointfilter_new(w);
	stmt = db_prepare_v2(
	    w->db,
	    SQL("SELECT txid, outnum FROM utxoset WHERE spendheight is NULL"));
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		db_column_sha256d(stmt, 0, &txid.shad);
		outnum = db_column_int(stmt, 1);
		outpointfilter_add(w->utxoset_outpoints, &txid, outnum);
	}
	tal_free(stmt);
}

struct wallet *wallet_new(struct lightningd *ld, struct timers *timers,
			  struct ext_key *bip32_base STEALS)
{
	struct wallet *wallet = tal(ld, struct wallet);
	wallet->ld = ld;
	wallet->log = new_log(wallet, ld->log_book, NULL, "wallet");
	wallet->bip32_base = tal_steal(wallet, bip32_base);
	wallet->keyscan_gap = 50;
	list_head_init(&wallet->unstored_payments);
	wallet->db = db_setup(wallet, ld, wallet->bip32_base);

	db_begin_transaction(wallet->db);
	wallet->invoices = invoices_new(wallet, wallet->db, timers);
	outpointfilters_init(wallet);
	db_commit_transaction(wallet->db);
	return wallet;
}

/**
 * wallet_add_utxo - Register an UTXO which we (partially) own
 *
 * Add an UTXO to the set of outputs we care about.
 *
 * This can fail if we've already seen UTXO.
 */
static bool wallet_add_utxo(struct wallet *w, struct utxo *utxo,
			    enum wallet_output_type type)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(w->db, SQL("SELECT * from outputs WHERE "
					"prev_out_tx=? AND prev_out_index=?"));
	db_bind_txid(stmt, 0, &utxo->txid);
	db_bind_int(stmt, 1, utxo->outnum);
	db_query_prepared(stmt);

	/* If we get a result, that means a clash. */
	if (db_step(stmt)) {
		tal_free(stmt);
		return false;
	}
	tal_free(stmt);

	stmt = db_prepare_v2(
	    w->db, SQL("INSERT INTO outputs ("
		       "  prev_out_tx"
		       ", prev_out_index"
		       ", value"
		       ", type"
		       ", status"
		       ", keyindex"
		       ", channel_id"
		       ", peer_id"
		       ", commitment_point"
		       ", option_anchor_outputs"
		       ", confirmation_height"
		       ", spend_height"
		       ", scriptpubkey"
		       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));
	db_bind_txid(stmt, 0, &utxo->txid);
	db_bind_int(stmt, 1, utxo->outnum);
	db_bind_amount_sat(stmt, 2, &utxo->amount);
	db_bind_int(stmt, 3, wallet_output_type_in_db(type));
	db_bind_int(stmt, 4, OUTPUT_STATE_AVAILABLE);
	db_bind_int(stmt, 5, utxo->keyindex);
	if (utxo->close_info) {
		db_bind_u64(stmt, 6, utxo->close_info->channel_id);
		db_bind_node_id(stmt, 7, &utxo->close_info->peer_id);
		if (utxo->close_info->commitment_point)
			db_bind_pubkey(stmt, 8, utxo->close_info->commitment_point);
		else
			db_bind_null(stmt, 8);
		db_bind_int(stmt, 9, utxo->close_info->option_anchor_outputs);
	} else {
		db_bind_null(stmt, 6);
		db_bind_null(stmt, 7);
		db_bind_null(stmt, 8);
		db_bind_null(stmt, 9);
	}

	if (utxo->blockheight) {
		db_bind_int(stmt, 10, *utxo->blockheight);
	} else
		db_bind_null(stmt, 10);

	if (utxo->spendheight)
		db_bind_int(stmt, 11, *utxo->spendheight);
	else
		db_bind_null(stmt, 11);

	db_bind_blob(stmt, 12, utxo->scriptPubkey,
			  tal_bytelen(utxo->scriptPubkey));

	db_exec_prepared_v2(take(stmt));
	return true;
}

/**
 * wallet_stmt2output - Extract data from stmt and fill an UTXO
 */
static struct utxo *wallet_stmt2output(const tal_t *ctx, struct db_stmt *stmt)
{
	struct utxo *utxo = tal(ctx, struct utxo);
	u32 *blockheight, *spendheight;
	db_column_txid(stmt, 0, &utxo->txid);
	utxo->outnum = db_column_int(stmt, 1);
	db_column_amount_sat(stmt, 2, &utxo->amount);
	utxo->is_p2sh = db_column_int(stmt, 3) == p2sh_wpkh;
	utxo->status = db_column_int(stmt, 4);
	utxo->keyindex = db_column_int(stmt, 5);
	if (!db_column_is_null(stmt, 6)) {
		utxo->close_info = tal(utxo, struct unilateral_close_info);
		utxo->close_info->channel_id = db_column_u64(stmt, 6);
		db_column_node_id(stmt, 7, &utxo->close_info->peer_id);
		if (!db_column_is_null(stmt, 8)) {
			utxo->close_info->commitment_point
				= tal(utxo->close_info, struct pubkey);
			db_column_pubkey(stmt, 8,
					 utxo->close_info->commitment_point);
		} else
			utxo->close_info->commitment_point = NULL;
		utxo->close_info->option_anchor_outputs
			= db_column_int(stmt, 9);
		utxo->close_info->csv = db_column_int(stmt, 14);
	} else {
		utxo->close_info = NULL;
	}

	utxo->scriptPubkey =
	    tal_dup_arr(utxo, u8, db_column_blob(stmt, 12),
			db_column_bytes(stmt, 12), 0);

	utxo->blockheight = NULL;
	utxo->spendheight = NULL;

	if (!db_column_is_null(stmt, 10)) {
		blockheight = tal(utxo, u32);
		*blockheight = db_column_int(stmt, 10);
		utxo->blockheight = blockheight;
	}

	if (!db_column_is_null(stmt, 11)) {
		spendheight = tal(utxo, u32);
		*spendheight = db_column_int(stmt, 11);
		utxo->spendheight = spendheight;
	}

	/* This column can be null if 0.9.1 db or below. */
	utxo->reserved_til = db_column_int_or_default(stmt, 13, 0);

	return utxo;
}

bool wallet_update_output_status(struct wallet *w,
				 const struct bitcoin_txid *txid,
				 const u32 outnum, enum output_status oldstatus,
				 enum output_status newstatus)
{
	struct db_stmt *stmt;
	size_t changes;
	if (oldstatus != OUTPUT_STATE_ANY) {
		stmt = db_prepare_v2(
		    w->db, SQL("UPDATE outputs SET status=? WHERE status=? AND "
			       "prev_out_tx=? AND prev_out_index=?"));
		db_bind_int(stmt, 0, output_status_in_db(newstatus));
		db_bind_int(stmt, 1, output_status_in_db(oldstatus));
		db_bind_txid(stmt, 2, txid);
		db_bind_int(stmt, 3, outnum);
	} else {
		stmt = db_prepare_v2(w->db,
				     SQL("UPDATE outputs SET status=? WHERE "
					 "prev_out_tx=? AND prev_out_index=?"));
		db_bind_int(stmt, 0, output_status_in_db(newstatus));
		db_bind_txid(stmt, 1, txid);
		db_bind_int(stmt, 2, outnum);
	}
	db_exec_prepared_v2(stmt);
	changes = db_count_changes(stmt);
	tal_free(stmt);
	return changes > 0;
}

struct utxo **wallet_get_utxos(const tal_t *ctx, struct wallet *w, const enum output_status state)
{
	struct utxo **results;
	int i;
	struct db_stmt *stmt;

	if (state == OUTPUT_STATE_ANY) {
		stmt = db_prepare_v2(w->db, SQL("SELECT"
						"  prev_out_tx"
						", prev_out_index"
						", value"
						", type"
						", status"
						", keyindex"
						", channel_id"
						", peer_id"
						", commitment_point"
						", option_anchor_outputs"
						", confirmation_height"
						", spend_height"
						", scriptpubkey "
						", reserved_til "
						", csv_lock "
						"FROM outputs"));
	} else {
		stmt = db_prepare_v2(w->db, SQL("SELECT"
						"  prev_out_tx"
						", prev_out_index"
						", value"
						", type"
						", status"
						", keyindex"
						", channel_id"
						", peer_id"
						", commitment_point"
						", option_anchor_outputs"
						", confirmation_height"
						", spend_height"
						", scriptpubkey "
						", reserved_til "
						", csv_lock "
						"FROM outputs "
						"WHERE status= ? "));
		db_bind_int(stmt, 0, output_status_in_db(state));
	}
	db_query_prepared(stmt);

	results = tal_arr(ctx, struct utxo*, 0);
	for (i=0; db_step(stmt); i++) {
		struct utxo *u = wallet_stmt2output(results, stmt);
		tal_arr_expand(&results, u);
	}
	tal_free(stmt);

	return results;
}

struct utxo **wallet_get_unconfirmed_closeinfo_utxos(const tal_t *ctx,
						     struct wallet *w)
{
	struct db_stmt *stmt;
	struct utxo **results;
	int i;

	stmt = db_prepare_v2(w->db, SQL("SELECT"
					"  prev_out_tx"
					", prev_out_index"
					", value"
					", type"
					", status"
					", keyindex"
					", channel_id"
					", peer_id"
					", commitment_point"
					", option_anchor_outputs"
					", confirmation_height"
					", spend_height"
					", scriptpubkey"
					", reserved_til"
					", csv_lock"
					" FROM outputs"
					" WHERE channel_id IS NOT NULL AND "
					"confirmation_height IS NULL"));
	db_query_prepared(stmt);

	results = tal_arr(ctx, struct utxo *, 0);
	for (i = 0; db_step(stmt); i++) {
		struct utxo *u = wallet_stmt2output(results, stmt);
		tal_arr_expand(&results, u);
	}
	tal_free(stmt);

	return results;
}

struct utxo *wallet_utxo_get(const tal_t *ctx, struct wallet *w,
			     const struct bitcoin_txid *txid,
			     u32 outnum)
{
	struct db_stmt *stmt;
	struct utxo *utxo;

	stmt = db_prepare_v2(w->db, SQL("SELECT"
					"  prev_out_tx"
					", prev_out_index"
					", value"
					", type"
					", status"
					", keyindex"
					", channel_id"
					", peer_id"
					", commitment_point"
					", option_anchor_outputs"
					", confirmation_height"
					", spend_height"
					", scriptpubkey"
					", reserved_til"
					", csv_lock"
					" FROM outputs"
					" WHERE prev_out_tx = ?"
					" AND prev_out_index = ?"));

	db_bind_sha256d(stmt, 0, &txid->shad);
	db_bind_int(stmt, 1, outnum);

	db_query_prepared(stmt);

	if (!db_step(stmt)) {
		tal_free(stmt);
		return NULL;
	}

	utxo = wallet_stmt2output(ctx, stmt);
	tal_free(stmt);

	return utxo;
}

bool wallet_unreserve_output(struct wallet *w,
			     const struct bitcoin_txid *txid,
			     const u32 outnum)
{
	return wallet_update_output_status(w, txid, outnum,
					   OUTPUT_STATE_RESERVED,
					   OUTPUT_STATE_AVAILABLE);
}

/**
 * unreserve_utxo - Mark a reserved UTXO as available again
 */
static void unreserve_utxo(struct wallet *w, const struct utxo *unres)
{
	if (!wallet_update_output_status(w, &unres->txid, unres->outnum,
					 OUTPUT_STATE_RESERVED,
					 OUTPUT_STATE_AVAILABLE)) {
		fatal("Unable to unreserve output");
	}
}

/**
 * destroy_utxos - Destructor for an array of pointers to utxo
 */
static void destroy_utxos(const struct utxo **utxos, struct wallet *w)
{
	for (size_t i = 0; i < tal_count(utxos); i++)
		unreserve_utxo(w, utxos[i]);
}

void wallet_persist_utxo_reservation(struct wallet *w, const struct utxo **utxos)
{
	tal_del_destructor2(utxos, destroy_utxos, w);
}

void wallet_confirm_utxos(struct wallet *w, const struct utxo **utxos)
{
	tal_del_destructor2(utxos, destroy_utxos, w);
	for (size_t i = 0; i < tal_count(utxos); i++) {
		if (!wallet_update_output_status(
			w, &utxos[i]->txid, utxos[i]->outnum,
			OUTPUT_STATE_RESERVED, OUTPUT_STATE_SPENT)) {
			fatal("Unable to mark output as spent");
		}
	}
}

static void db_set_utxo(struct db *db, const struct utxo *utxo)
{
	struct db_stmt *stmt;

	if (utxo->status == OUTPUT_STATE_RESERVED)
		assert(utxo->reserved_til);
	else
		assert(!utxo->reserved_til);

	stmt = db_prepare_v2(
		db, SQL("UPDATE outputs SET status=?, reserved_til=? "
			"WHERE prev_out_tx=? AND prev_out_index=?"));
	db_bind_int(stmt, 0, output_status_in_db(utxo->status));
	db_bind_int(stmt, 1, utxo->reserved_til);
	db_bind_txid(stmt, 2, &utxo->txid);
	db_bind_int(stmt, 3, utxo->outnum);
	db_exec_prepared_v2(take(stmt));
}

bool wallet_reserve_utxo(struct wallet *w, struct utxo *utxo,
			 u32 current_height,
			 u32 reserve)
{
	switch (utxo->status) {
	case OUTPUT_STATE_SPENT:
		return false;
	case OUTPUT_STATE_AVAILABLE:
	case OUTPUT_STATE_RESERVED:
		break;
	case OUTPUT_STATE_ANY:
		abort();
	}

	/* We simple increase existing reservations, which DTRT if we unreserve */
	if (utxo->reserved_til >= current_height)
		utxo->reserved_til += reserve;
	else
		utxo->reserved_til = current_height + reserve;

	utxo->status = OUTPUT_STATE_RESERVED;

	db_set_utxo(w->db, utxo);

	return true;
}

void wallet_unreserve_utxo(struct wallet *w, struct utxo *utxo,
			   u32 current_height,
			   u32 unreserve)
{
	if (utxo->status != OUTPUT_STATE_RESERVED)
		fatal("UTXO %s:%u is not reserved",
		      type_to_string(tmpctx, struct bitcoin_txid, &utxo->txid),
		      utxo->outnum);

	if (utxo->reserved_til <= current_height + unreserve) {
		utxo->status = OUTPUT_STATE_AVAILABLE;
		utxo->reserved_til = 0;
	} else
		utxo->reserved_til -= unreserve;

	db_set_utxo(w->db, utxo);
}

static bool excluded(const struct utxo **excludes,
		     const struct utxo *utxo)
{
	for (size_t i = 0; i < tal_count(excludes); i++) {
		if (bitcoin_txid_eq(&excludes[i]->txid, &utxo->txid)
		    && excludes[i]->outnum == utxo->outnum)
			return true;
	}
	return false;
}

static bool deep_enough(u32 maxheight, const struct utxo *utxo,
			u32 current_blockheight)
{
	/* If we require confirmations check that we have a
	 * confirmation height and that it is below the required
	 * maxheight (current_height - minconf) */
	if (maxheight == 0)
		return true;
	if (!utxo->blockheight)
		return false;
	/* Check that CSV-lock is now free! */
	if (utxo->close_info) {
		u32 csv_free = *utxo->blockheight + utxo->close_info->csv;
		assert(csv_free > *utxo->blockheight);
		if (current_blockheight < csv_free)
			return false;
	}
	return *utxo->blockheight <= maxheight;
}

/* FIXME: Make this wallet_find_utxos, and branch and bound and I've
 * left that to @niftynei to do, who actually read the paper! */
struct utxo *wallet_find_utxo(const tal_t *ctx, struct wallet *w,
			      unsigned current_blockheight,
			      struct amount_sat *amount_hint,
			      unsigned feerate_per_kw,
			      u32 maxheight,
			      const struct utxo **excludes)
{
	struct db_stmt *stmt;
	struct utxo *utxo;

	stmt = db_prepare_v2(w->db, SQL("SELECT"
					"  prev_out_tx"
					", prev_out_index"
					", value"
					", type"
					", status"
					", keyindex"
					", channel_id"
					", peer_id"
					", commitment_point"
					", option_anchor_outputs"
					", confirmation_height"
					", spend_height"
					", scriptpubkey "
					", reserved_til"
					", csv_lock"
					" FROM outputs"
					" WHERE status = ?"
					" OR (status = ? AND reserved_til <= ?)"
					"ORDER BY RANDOM();"));
	db_bind_int(stmt, 0, output_status_in_db(OUTPUT_STATE_AVAILABLE));
	db_bind_int(stmt, 1, output_status_in_db(OUTPUT_STATE_RESERVED));
	db_bind_u64(stmt, 2, current_blockheight);

	/* FIXME: Use feerate + estimate of input cost to establish
	 * range for amount_hint */

	db_query_prepared(stmt);

	utxo = NULL;
	while (!utxo && db_step(stmt)) {
		utxo = wallet_stmt2output(ctx, stmt);
		if (excluded(excludes, utxo)
		    || !deep_enough(maxheight, utxo, current_blockheight))
			utxo = tal_free(utxo);

	}
	tal_free(stmt);
	return utxo;
}

bool wallet_add_onchaind_utxo(struct wallet *w,
			      const struct bitcoin_txid *txid,
			      u32 outnum,
			      const u8 *scriptpubkey,
			      u32 blockheight,
			      struct amount_sat amount,
			      const struct channel *channel,
			      /* NULL if option_static_remotekey */
			      const struct pubkey *commitment_point,
			      u32 csv_lock)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(w->db, SQL("SELECT * from outputs WHERE "
					"prev_out_tx=? AND prev_out_index=?"));
	db_bind_txid(stmt, 0, txid);
	db_bind_int(stmt, 1, outnum);
	db_query_prepared(stmt);

	/* If we get a result, that means a clash. */
	if (db_step(stmt)) {
		tal_free(stmt);
		return false;
	}
	tal_free(stmt);

	stmt = db_prepare_v2(
	    w->db, SQL("INSERT INTO outputs ("
		       "  prev_out_tx"
		       ", prev_out_index"
		       ", value"
		       ", type"
		       ", status"
		       ", keyindex"
		       ", channel_id"
		       ", peer_id"
		       ", commitment_point"
		       ", option_anchor_outputs"
		       ", confirmation_height"
		       ", spend_height"
		       ", scriptpubkey"
		       ", csv_lock"
		       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));
	db_bind_txid(stmt, 0, txid);
	db_bind_int(stmt, 1, outnum);
	db_bind_amount_sat(stmt, 2, &amount);
	db_bind_int(stmt, 3, wallet_output_type_in_db(p2wpkh));
	db_bind_int(stmt, 4, OUTPUT_STATE_AVAILABLE);
	db_bind_int(stmt, 5, 0);
	db_bind_u64(stmt, 6, channel->dbid);
	db_bind_node_id(stmt, 7, &channel->peer->id);
	if (commitment_point)
		db_bind_pubkey(stmt, 8, commitment_point);
	else
		db_bind_null(stmt, 8);

	db_bind_int(stmt, 9, channel->option_anchor_outputs);
	db_bind_int(stmt, 10, blockheight);

	/* spendheight */
	db_bind_null(stmt, 11);
	db_bind_blob(stmt, 12, scriptpubkey, tal_bytelen(scriptpubkey));

	db_bind_int(stmt, 13, csv_lock);

	db_exec_prepared_v2(take(stmt));
	return true;
}

bool wallet_can_spend(struct wallet *w, const u8 *script,
		      u32 *index, bool *output_is_p2sh)
{
	struct ext_key ext;
	u64 bip32_max_index = db_get_intvar(w->db, "bip32_max_index", 0);
	u32 i;

	/* If not one of these, can't be for us. */
	if (is_p2sh(script, NULL))
		*output_is_p2sh = true;
	else if (is_p2wpkh(script, NULL))
		*output_is_p2sh = false;
	else
		return false;

	for (i = 0; i <= bip32_max_index + w->keyscan_gap; i++) {
		u8 *s;

		if (bip32_key_from_parent(w->bip32_base, i,
					  BIP32_FLAG_KEY_PUBLIC, &ext)
		    != WALLY_OK) {
			abort();
		}
		s = scriptpubkey_p2wpkh_derkey(w, ext.pub_key);
		if (*output_is_p2sh) {
			u8 *p2sh = scriptpubkey_p2sh(w, s);
			tal_free(s);
			s = p2sh;
		}
		if (scripteq(s, script)) {
			/* If we found a used key in the keyscan_gap we should
			 * remember that. */
			if (i > bip32_max_index)
				db_set_intvar(w->db, "bip32_max_index", i);
			tal_free(s);
			*index = i;
			return true;
		}
		tal_free(s);
	}
	return false;
}

s64 wallet_get_newindex(struct lightningd *ld)
{
	u64 newidx = db_get_intvar(ld->wallet->db, "bip32_max_index", 0) + 1;

	if (newidx == BIP32_INITIAL_HARDENED_CHILD)
		return -1;

	db_set_intvar(ld->wallet->db, "bip32_max_index", newidx);
	return newidx;
}

static void wallet_shachain_init(struct wallet *wallet,
				 struct wallet_shachain *chain)
{
	struct db_stmt *stmt;

	assert(chain->id == 0);

	/* Create shachain */
	shachain_init(&chain->chain);
	stmt = db_prepare_v2(
	    wallet->db,
	    SQL("INSERT INTO shachains (min_index, num_valid) VALUES (?, 0);"));
	db_bind_u64(stmt, 0, chain->chain.min_index);
	db_exec_prepared_v2(stmt);

	chain->id = db_last_insert_id_v2(stmt);
	tal_free(stmt);
}

/* TODO(cdecker) Stolen from shachain, move to some appropriate location */
static unsigned int count_trailing_zeroes(uint64_t index)
{
#if HAVE_BUILTIN_CTZLL
	return index ? (unsigned int)__builtin_ctzll(index) : SHACHAIN_BITS;
#else
	unsigned int i;

	for (i = 0; i < SHACHAIN_BITS; i++) {
		if (index & (1ULL << i))
			break;
	}
	return i;
#endif
}

bool wallet_shachain_add_hash(struct wallet *wallet,
			      struct wallet_shachain *chain,
			      uint64_t index,
			      const struct secret *hash)
{
	struct db_stmt *stmt;
	u32 pos = count_trailing_zeroes(index);
	struct sha256 s;
	bool updated;

	BUILD_ASSERT(sizeof(s) == sizeof(*hash));
	memcpy(&s, hash, sizeof(s));

	assert(index < SQLITE_MAX_UINT);
	if (!shachain_add_hash(&chain->chain, index, &s)) {
		return false;
	}

	stmt = db_prepare_v2(
	    wallet->db,
	    SQL("UPDATE shachains SET num_valid=?, min_index=? WHERE id=?"));
	db_bind_int(stmt, 0, chain->chain.num_valid);
	db_bind_u64(stmt, 1, index);
	db_bind_u64(stmt, 2, chain->id);
	db_exec_prepared_v2(take(stmt));

	stmt = db_prepare_v2(wallet->db,
			     SQL("UPDATE shachain_known SET idx=?, hash=? "
				 "WHERE shachain_id=? AND pos=?"));
	db_bind_u64(stmt, 0, index);
	db_bind_secret(stmt, 1, hash);
	db_bind_u64(stmt, 2, chain->id);
	db_bind_int(stmt, 3, pos);
	db_exec_prepared_v2(stmt);
	updated = db_count_changes(stmt) == 1;
	tal_free(stmt);

	if (!updated) {
		stmt = db_prepare_v2(
		    wallet->db, SQL("INSERT INTO shachain_known (shachain_id, "
				    "pos, idx, hash) VALUES (?, ?, ?, ?);"));
		db_bind_u64(stmt, 0, chain->id);
		db_bind_int(stmt, 1, pos);
		db_bind_u64(stmt, 2, index);
		db_bind_secret(stmt, 3, hash);
		db_exec_prepared_v2(take(stmt));
	}

	return true;
}

bool wallet_shachain_load(struct wallet *wallet, u64 id,
			  struct wallet_shachain *chain)
{
	struct db_stmt *stmt;
	chain->id = id;
	shachain_init(&chain->chain);

	/* Load shachain metadata */
	stmt = db_prepare_v2(
	    wallet->db,
	    SQL("SELECT min_index, num_valid FROM shachains WHERE id=?"));
	db_bind_u64(stmt, 0, id);
	db_query_prepared(stmt);

	if (!db_step(stmt)) {
		tal_free(stmt);
		return false;
	}

	chain->chain.min_index = db_column_u64(stmt, 0);
	chain->chain.num_valid = db_column_u64(stmt, 1);
	tal_free(stmt);

	/* Load shachain known entries */
	stmt = db_prepare_v2(wallet->db,
			     SQL("SELECT idx, hash, pos FROM shachain_known "
				 "WHERE shachain_id=?"));
	db_bind_u64(stmt, 0, id);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		int pos = db_column_int(stmt, 2);
		chain->chain.known[pos].index = db_column_u64(stmt, 0);
		db_column_sha256(stmt, 1, &chain->chain.known[pos].hash);
	}
	tal_free(stmt);
	return true;
}

static struct peer *wallet_peer_load(struct wallet *w, const u64 dbid)
{
	const unsigned char *addrstr;
	struct peer *peer = NULL;
	struct node_id id;
	struct wireaddr_internal addr;
	struct db_stmt *stmt;

	stmt = db_prepare_v2(
	    w->db, SQL("SELECT id, node_id, address FROM peers WHERE id=?;"));
	db_bind_u64(stmt, 0, dbid);
	db_query_prepared(stmt);

	if (!db_step(stmt))
		goto done;

	if (db_column_is_null(stmt, 1))
		goto done;

	db_column_node_id(stmt, 1, &id);

	addrstr = db_column_text(stmt, 2);
	if (!parse_wireaddr_internal((const char*)addrstr, &addr, DEFAULT_PORT,
				     false, false, true, true, NULL))
		goto done;

	/* FIXME: save incoming in db! */
	peer = new_peer(w->ld, db_column_u64(stmt, 0), &id, &addr, false);

done:
	tal_free(stmt);
	return peer;
}

static struct bitcoin_signature *
wallet_htlc_sigs_load(const tal_t *ctx, struct wallet *w, u64 channelid,
		      bool option_anchor_outputs)
{
	struct db_stmt *stmt;
	struct bitcoin_signature *htlc_sigs = tal_arr(ctx, struct bitcoin_signature, 0);

	stmt = db_prepare_v2(
	    w->db, SQL("SELECT signature FROM htlc_sigs WHERE channelid = ?"));
	db_bind_u64(stmt, 0, channelid);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		struct bitcoin_signature sig;
		db_column_signature(stmt, 0, &sig.s);
		/* BOLT #3:
		 * ## HTLC-Timeout and HTLC-Success Transactions
		 *...
		 * * if `option_anchor_outputs` applies to this commitment
		 *   transaction, `SIGHASH_SINGLE|SIGHASH_ANYONECANPAY` is
		 *   used.
		 */
		if (option_anchor_outputs)
			sig.sighash_type = SIGHASH_SINGLE|SIGHASH_ANYONECANPAY;
		else
			sig.sighash_type = SIGHASH_ALL;
		tal_arr_expand(&htlc_sigs, sig);
	}
	tal_free(stmt);

	log_debug(w->log, "Loaded %zu HTLC signatures from DB",
		  tal_count(htlc_sigs));
	return htlc_sigs;
}

bool wallet_remote_ann_sigs_load(const tal_t *ctx, struct wallet *w, u64 id,
				 secp256k1_ecdsa_signature **remote_ann_node_sig,
				 secp256k1_ecdsa_signature **remote_ann_bitcoin_sig)
{
	struct db_stmt *stmt;
	bool res;
	stmt = db_prepare_v2(
	    w->db, SQL("SELECT remote_ann_node_sig, remote_ann_bitcoin_sig"
		       " FROM channels WHERE id = ?"));
	db_bind_u64(stmt, 0, id);
	db_query_prepared(stmt);

	res = db_step(stmt);

	/* This must succeed, since we know the channel exists */
	assert(res);

	/* if only one sig exists, forget the sig and hope peer send new ones*/
	if (db_column_is_null(stmt, 0) || db_column_is_null(stmt, 1)) {
		*remote_ann_node_sig = *remote_ann_bitcoin_sig = NULL;
		tal_free(stmt);
		return true;
	}

	/* the case left over is both sigs exist */
	*remote_ann_node_sig = tal(ctx, secp256k1_ecdsa_signature);
	*remote_ann_bitcoin_sig = tal(ctx, secp256k1_ecdsa_signature);

	if (!db_column_signature(stmt, 0, *remote_ann_node_sig))
		goto fail;

	if (!db_column_signature(stmt, 1, *remote_ann_bitcoin_sig))
		goto fail;

	tal_free(stmt);
	return true;

fail:
	*remote_ann_node_sig = tal_free(*remote_ann_node_sig);
	*remote_ann_bitcoin_sig = tal_free(*remote_ann_bitcoin_sig);
	tal_free(stmt);
	return false;
}

static struct fee_states *wallet_channel_fee_states_load(struct wallet *w,
							 const u64 id,
							 enum side opener)
{
	struct fee_states *fee_states;
	struct db_stmt *stmt;

	stmt = db_prepare_v2(w->db, SQL("SELECT hstate, feerate_per_kw FROM channel_feerates WHERE channel_id = ?"));
	db_bind_u64(stmt, 0, id);
	db_query_prepared(stmt);

	/* Start with blank slate. */
	fee_states = new_fee_states(w, opener, NULL);
	while (db_step(stmt)) {
		enum htlc_state hstate = db_column_int(stmt, 0);
		u32 feerate = db_column_int(stmt, 1);

		if (fee_states->feerate[hstate] != NULL) {
			log_broken(w->log,
				   "duplicate channel_feerates for %s id %"PRIu64,
				   htlc_state_name(hstate), id);
			fee_states = tal_free(fee_states);
			break;
		}
		fee_states->feerate[hstate] = tal_dup(fee_states, u32, &feerate);
	}
	tal_free(stmt);

	if (fee_states && !fee_states_valid(fee_states, opener)) {
		log_broken(w->log,
			   "invalid channel_feerates for id %"PRIu64, id);
		fee_states = tal_free(fee_states);
	}
	return fee_states;
}

static struct height_states *wallet_channel_height_states_load(struct wallet *w,
							       const u64 id,
							       enum side opener)
{
	struct height_states *states;
	struct db_stmt *stmt;

	stmt = db_prepare_v2(w->db, SQL("SELECT hstate, blockheight FROM channel_blockheights WHERE channel_id = ?"));
	db_bind_u64(stmt, 0, id);
	db_query_prepared(stmt);

	/* Start with blank slate. */
	states = new_height_states(w, opener, NULL);
	while (db_step(stmt)) {
		enum htlc_state hstate = db_column_int(stmt, 0);
		u32 blockheight = db_column_int(stmt, 1);

		if (states->height[hstate] != NULL) {
			log_broken(w->log,
				   "duplicate channel_blockheights for %s id %"PRIu64,
				   htlc_state_name(hstate), id);
			states = tal_free(states);
			break;
		}
		states->height[hstate] = tal_dup(states, u32, &blockheight);
	}
	tal_free(stmt);

	if (states && !height_states_valid(states, opener)) {
		log_broken(w->log,
			   "invalid channel_blockheight for id %"PRIu64, id);
		states = tal_free(states);
	}
	return states;
}

void wallet_inflight_add(struct wallet *w, struct channel_inflight *inflight)
{
	struct db_stmt *stmt;
	stmt = db_prepare_v2(w->db,
			     SQL("INSERT INTO channel_funding_inflights ("
				 "  channel_id" // 0
				 ", funding_tx_id" // 1
				 ", funding_tx_outnum" // 2
				 ", funding_feerate" // 3
				 ", funding_satoshi" // 4
				 ", our_funding_satoshi" // 5
				 ", funding_psbt" // 6
				 ", last_tx" // 7
				 ", last_sig" // 8
				 ", lease_commit_sig" // 9
				 ", lease_chan_max_msat" // 10
				 ", lease_chan_max_ppt" // 11
				 ", lease_expiry" // 12
				 ", lease_blockheight_start" // 13
				 ") VALUES ("
				 "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));

	db_bind_u64(stmt, 0, inflight->channel->dbid);
	db_bind_txid(stmt, 1, &inflight->funding->txid);
	db_bind_int(stmt, 2, inflight->funding->outnum);
	db_bind_int(stmt, 3, inflight->funding->feerate);
	db_bind_amount_sat(stmt, 4, &inflight->funding->total_funds);
	db_bind_amount_sat(stmt, 5, &inflight->funding->our_funds);
	db_bind_psbt(stmt, 6, inflight->funding_psbt);
	db_bind_psbt(stmt, 7, inflight->last_tx->psbt);
	db_bind_signature(stmt, 8, &inflight->last_sig.s);

	if (inflight->lease_expiry != 0) {
		db_bind_signature(stmt, 9, inflight->lease_commit_sig);
		db_bind_int(stmt, 10, inflight->lease_chan_max_msat);
		db_bind_int(stmt, 11, inflight->lease_chan_max_ppt);
		db_bind_int(stmt, 12, inflight->lease_expiry);
		db_bind_int(stmt, 13, inflight->lease_blockheight_start);
	} else {
		db_bind_null(stmt, 9);
		db_bind_null(stmt, 10);
		db_bind_null(stmt, 11);
		db_bind_int(stmt, 12, 0);
		db_bind_null(stmt, 13);
	}

	db_exec_prepared_v2(stmt);
	assert(!stmt->error);
	tal_free(stmt);
}

void wallet_inflight_save(struct wallet *w,
			  struct channel_inflight *inflight)
{
	struct db_stmt *stmt;
	/* The *only* thing you can update on an
	 * inflight is the funding PSBT (to add sigs)
	 * ((and maybe later the last_tx/last_sig if this is for
	 * a splice */
	stmt = db_prepare_v2(w->db,
			     SQL("UPDATE channel_funding_inflights SET"
				 "  funding_psbt=?" // 0
				 ", funding_tx_remote_sigs_received=?" // 1
				 " WHERE"
				 "  channel_id=?" // 2
				 " AND funding_tx_id=?" // 3
				 " AND funding_tx_outnum=?")); // 4
	db_bind_psbt(stmt, 0, inflight->funding_psbt);
	db_bind_int(stmt, 1, inflight->remote_tx_sigs);
	db_bind_u64(stmt, 2, inflight->channel->dbid);
	db_bind_txid(stmt, 3, &inflight->funding->txid);
	db_bind_int(stmt, 4, inflight->funding->outnum);

	db_exec_prepared_v2(take(stmt));
}

void wallet_channel_clear_inflights(struct wallet *w,
				    struct channel *chan)
{
	struct db_stmt *stmt;
	struct channel_inflight *inflight;

	/* Remove all the inflights for the channel */
	stmt = db_prepare_v2(w->db, SQL("DELETE FROM channel_funding_inflights"
					" WHERE channel_id = ?"));
	db_bind_u64(stmt, 0, chan->dbid);
	db_exec_prepared_v2(take(stmt));

	/* Empty out the list too */
	while ((inflight = list_tail(&chan->inflights,
				     struct channel_inflight, list)))
		tal_free(inflight);
}

static struct channel_inflight *
wallet_stmt2inflight(struct wallet *w, struct db_stmt *stmt,
		     struct channel *chan)
{
	struct amount_sat funding_sat, our_funding_sat;
	struct bitcoin_txid funding_txid;
	struct bitcoin_signature last_sig;
	struct channel_inflight *inflight;

	secp256k1_ecdsa_signature *lease_commit_sig;
	u32 lease_chan_max_msat, lease_blockheight_start;
	u16 lease_chan_max_ppt;

	db_column_txid(stmt, 0, &funding_txid);
	db_column_amount_sat(stmt, 3, &funding_sat);
	db_column_amount_sat(stmt, 4, &our_funding_sat);
	if (!db_column_signature(stmt, 7, &last_sig.s))
		return NULL;

	last_sig.sighash_type = SIGHASH_ALL;

	if (!db_column_is_null(stmt, 10)) {
		lease_commit_sig = tal(tmpctx, secp256k1_ecdsa_signature);
		db_column_signature(stmt, 10, lease_commit_sig);
		lease_chan_max_msat = db_column_int(stmt, 11);
		lease_chan_max_ppt = db_column_int(stmt, 12);
		lease_blockheight_start = db_column_int(stmt, 13);
	} else {
		lease_commit_sig = NULL;
		lease_chan_max_msat = 0;
		lease_chan_max_ppt = 0;
		lease_blockheight_start = 0;
	}

	inflight = new_inflight(chan, funding_txid,
				db_column_int(stmt, 1),
				db_column_int(stmt, 2),
				funding_sat,
				our_funding_sat,
				db_column_psbt(tmpctx, stmt, 5),
				db_column_psbt_to_tx(tmpctx, stmt, 6),
				last_sig,
				db_column_int(stmt, 9),
				lease_commit_sig,
				lease_chan_max_msat,
				lease_chan_max_ppt,
				lease_blockheight_start);

	/* Pull out the serialized tx-sigs-received-ness */
	inflight->remote_tx_sigs = db_column_int(stmt, 8);
	return inflight;
}

static bool wallet_channel_load_inflights(struct wallet *w,
					  struct channel *chan)
{
	bool ok = true;
	struct db_stmt *stmt;

	stmt = db_prepare_v2(w->db, SQL("SELECT"
					"  funding_tx_id" // 0
					", funding_tx_outnum" // 1
					", funding_feerate" // 2
					", funding_satoshi" // 3
					", our_funding_satoshi" // 4
					", funding_psbt" // 5
					", last_tx" // 6
					", last_sig" // 7
					", funding_tx_remote_sigs_received" //8
					", lease_expiry" // 9
					", lease_commit_sig" // 10
					", lease_chan_max_msat" // 11
					", lease_chan_max_ppt" // 12
					", lease_blockheight_start" // 13
					" FROM channel_funding_inflights"
					" WHERE channel_id = ?" // ?0
					" ORDER BY funding_feerate"));

	db_bind_u64(stmt, 0, chan->dbid);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		struct channel_inflight *inflight;
		inflight = wallet_stmt2inflight(w, stmt, chan);
		if (!inflight) {
			ok = false;
			break;
		}

	}
	tal_free(stmt);
	return ok;
}

/**
 * wallet_stmt2channel - Helper to populate a wallet_channel from a `db_stmt`
 */
static struct channel *wallet_stmt2channel(struct wallet *w, struct db_stmt *stmt)
{
	bool ok = true;
	struct channel_info channel_info;
	struct fee_states *fee_states;
	struct height_states *height_states;
	struct short_channel_id *scid;
	struct channel_id cid;
	struct channel *chan;
	u64 peer_dbid;
	struct peer *peer;
	struct wallet_shachain wshachain;
	struct channel_config our_config;
	struct bitcoin_txid funding_txid;
	struct bitcoin_outpoint *shutdown_wrong_funding;
	struct bitcoin_signature last_sig;
	u8 *remote_shutdown_scriptpubkey;
	u8 *local_shutdown_scriptpubkey;
	struct changed_htlc *last_sent_commit;
	s64 final_key_idx, channel_config_id;
	struct basepoints local_basepoints;
	struct pubkey local_funding_pubkey;
	struct pubkey *future_per_commitment_point;
	struct amount_sat funding_sat, our_funding_sat;
	struct amount_msat push_msat, our_msat, msat_to_us_min, msat_to_us_max;
	secp256k1_ecdsa_signature *lease_commit_sig;
	u32 lease_chan_max_msat;
	u16 lease_chan_max_ppt;

	peer_dbid = db_column_u64(stmt, 1);
	peer = find_peer_by_dbid(w->ld, peer_dbid);
	if (!peer) {
		peer = wallet_peer_load(w, peer_dbid);
		if (!peer) {
			return NULL;
		}
	}

	if (!db_column_is_null(stmt, 2)) {
		scid = tal(tmpctx, struct short_channel_id);
		if (!db_column_short_channel_id(stmt, 2, scid))
			return NULL;
	} else {
		scid = NULL;
	}

	ok &= wallet_shachain_load(w, db_column_u64(stmt, 29), &wshachain);

	remote_shutdown_scriptpubkey = db_column_arr(tmpctx, stmt, 30, u8);
	local_shutdown_scriptpubkey = db_column_arr(tmpctx, stmt, 50, u8);

	/* Do we have a last_sent_commit, if yes, populate */
	if (!db_column_is_null(stmt, 43)) {
		const u8 *cursor = db_column_blob(stmt, 43);
		size_t len = db_column_bytes(stmt, 43);
		size_t n = 0;
		last_sent_commit = tal_arr(tmpctx, struct changed_htlc, n);
		while (len) {
			tal_resize(&last_sent_commit, n+1);
			fromwire_changed_htlc(&cursor, &len,
					      &last_sent_commit[n++]);
		}
	} else
		last_sent_commit = NULL;

#ifdef COMPAT_V060
	if (!last_sent_commit && !db_column_is_null(stmt, 32)) {
		last_sent_commit = tal(tmpctx, struct changed_htlc);
		last_sent_commit->newstate = db_column_u64(stmt, 32);
		last_sent_commit->id = db_column_u64(stmt, 33);
	}
#endif

	if (!db_column_is_null(stmt, 42)) {
		future_per_commitment_point = tal(tmpctx, struct pubkey);
		db_column_pubkey(stmt, 42, future_per_commitment_point);
	} else
		future_per_commitment_point = NULL;

	db_column_channel_id(stmt, 3, &cid);
	channel_config_id = db_column_u64(stmt, 4);
	ok &= wallet_channel_config_load(w, channel_config_id, &our_config);
	db_column_sha256d(stmt, 13, &funding_txid.shad);
	ok &= db_column_signature(stmt, 35, &last_sig.s);
	last_sig.sighash_type = SIGHASH_ALL;

	/* Populate channel_info */
	db_column_pubkey(stmt, 20, &channel_info.remote_fundingkey);
	db_column_pubkey(stmt, 21, &channel_info.theirbase.revocation);
	db_column_pubkey(stmt, 22, &channel_info.theirbase.payment);
	db_column_pubkey(stmt, 23, &channel_info.theirbase.htlc);
	db_column_pubkey(stmt, 24, &channel_info.theirbase.delayed_payment);
	db_column_pubkey(stmt, 25, &channel_info.remote_per_commit);
	db_column_pubkey(stmt, 26, &channel_info.old_remote_per_commit);

	wallet_channel_config_load(w, db_column_u64(stmt, 5),
				   &channel_info.their_config);

	fee_states
		= wallet_channel_fee_states_load(w,
						 db_column_u64(stmt, 0),
						 db_column_int(stmt, 7));
	if (!fee_states)
		ok = false;

	if (!ok) {
		tal_free(fee_states);
		return NULL;
	}

	/* Blockheight states for the channel! */
	height_states
		= wallet_channel_height_states_load(w,
						    db_column_u64(stmt, 0),
						    db_column_int(stmt, 7));
	if (!height_states)
		ok = false;

	if (!ok) {
		tal_free(height_states);
		return NULL;
	}

	final_key_idx = db_column_u64(stmt, 31);
	if (final_key_idx < 0) {
		tal_free(fee_states);
		log_broken(w->log, "%s: Final key < 0", __func__);
		return NULL;
	}

	db_column_pubkey(stmt, 53, &local_basepoints.revocation);
	db_column_pubkey(stmt, 54, &local_basepoints.payment);
	db_column_pubkey(stmt, 55, &local_basepoints.htlc);
	db_column_pubkey(stmt, 56, &local_basepoints.delayed_payment);
	db_column_pubkey(stmt, 57, &local_funding_pubkey);
	if (db_column_is_null(stmt, 58))
		shutdown_wrong_funding = NULL;
	else {
		shutdown_wrong_funding = tal(tmpctx, struct bitcoin_outpoint);
		db_column_txid(stmt, 58, &shutdown_wrong_funding->txid);
		shutdown_wrong_funding->n = db_column_int(stmt, 59);
	}

	db_column_amount_sat(stmt, 15, &funding_sat);
	db_column_amount_sat(stmt, 16, &our_funding_sat);
	db_column_amount_msat(stmt, 18, &push_msat);
	db_column_amount_msat(stmt, 19, &our_msat);
	db_column_amount_msat(stmt, 40, &msat_to_us_min);
	db_column_amount_msat(stmt, 41, &msat_to_us_max);

	if (!db_column_is_null(stmt, 61)) {
		lease_commit_sig = tal(w, secp256k1_ecdsa_signature);
		db_column_signature(stmt, 61, lease_commit_sig);
		lease_chan_max_msat = db_column_int(stmt, 62);
		lease_chan_max_ppt = db_column_int(stmt, 63);
	} else {
		lease_commit_sig = NULL;
		lease_chan_max_msat = 0;
		lease_chan_max_ppt = 0;
	}

	chan = new_channel(peer, db_column_u64(stmt, 0),
			   &wshachain,
			   db_column_int(stmt, 6),
			   db_column_int(stmt, 7),
			   NULL, /* Set up fresh log */
			   "Loaded from database",
			   db_column_int(stmt, 8),
			   &our_config,
			   db_column_int(stmt, 9),
			   db_column_u64(stmt, 10),
			   db_column_u64(stmt, 11),
			   db_column_u64(stmt, 12),
			   &funding_txid,
			   db_column_int(stmt, 14),
			   funding_sat,
			   push_msat,
			   our_funding_sat,
			   db_column_int(stmt, 17) != 0,
			   scid,
			   &cid,
			   our_msat,
			   msat_to_us_min, /* msatoshi_to_us_min */
			   msat_to_us_max, /* msatoshi_to_us_max */
			   db_column_psbt_to_tx(tmpctx, stmt, 34),
			   &last_sig,
			   wallet_htlc_sigs_load(tmpctx, w,
						 db_column_u64(stmt, 0),
						 db_column_int(stmt, 49)),
			   &channel_info,
			   take(fee_states),
			   remote_shutdown_scriptpubkey,
			   local_shutdown_scriptpubkey,
			   final_key_idx,
			   db_column_int(stmt, 36) != 0,
			   last_sent_commit,
			   db_column_u64(stmt, 37),
			   db_column_int(stmt, 38),
			   db_column_int(stmt, 39),
			   /* Not connected */
			   false,
			   &local_basepoints, &local_funding_pubkey,
			   future_per_commitment_point,
			   db_column_int(stmt, 44),
			   db_column_int(stmt, 45),
			   db_column_arr(tmpctx, stmt, 46, u8),
			   db_column_u64(stmt, 47),
			   db_column_u64(stmt, 48),
			   db_column_int(stmt, 49),
			   db_column_int(stmt, 51),
			   db_column_int(stmt, 52),
			   shutdown_wrong_funding,
			   take(height_states),
			   db_column_int(stmt, 60),
			   lease_commit_sig,
			   lease_chan_max_msat,
			   lease_chan_max_ppt);

	if (!wallet_channel_load_inflights(w, chan)) {
		tal_free(chan);
		return NULL;
	}

	return chan;
}

static void set_max_channel_dbid(struct wallet *w)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(w->db, SQL("SELECT id FROM channels ORDER BY id DESC LIMIT 1;"));
	db_query_prepared(stmt);
	w->max_channel_dbid = 0;

	if (db_step(stmt))
		w->max_channel_dbid = db_column_u64(stmt, 0);

	tal_free(stmt);
}

static bool wallet_channels_load_active(struct wallet *w)
{
	bool ok = true;
	struct db_stmt *stmt;
	int count = 0;

	/* We load all channels */
	stmt = db_prepare_v2(w->db, SQL("SELECT"
					"  id" // 0
					", peer_id" // 1
					", short_channel_id" // 2
					", full_channel_id" // 3
					", channel_config_local" // 4
					", channel_config_remote" // 5
					", state" // 6
					", funder" // 7
					", channel_flags" // 8
					", minimum_depth" // 9
					", next_index_local" // 10
					", next_index_remote" // 11
					", next_htlc_id" // 12
					", funding_tx_id" // 13
					", funding_tx_outnum" // 14
					", funding_satoshi" // 15
					", our_funding_satoshi" // 16
					", funding_locked_remote" // 17
					", push_msatoshi" // 18
					", msatoshi_local" // 19
					", fundingkey_remote" // 20
					", revocation_basepoint_remote" // 21
					", payment_basepoint_remote" // 22
					", htlc_basepoint_remote" // 23
					", delayed_payment_basepoint_remote"
					", per_commit_remote" // 25
					", old_per_commit_remote" // 26
					", local_feerate_per_kw" // 27
					", remote_feerate_per_kw" // 28
					", shachain_remote_id"  // 29
					", shutdown_scriptpubkey_remote" // 30
					", shutdown_keyidx_local" // 31
					", last_sent_commit_state" // 32
					", last_sent_commit_id" // 33
					", last_tx" // 34
					", last_sig" // 35
					", last_was_revoke" // 36
					", first_blocknum" // 37
					", min_possible_feerate" // 38
					", max_possible_feerate" // 39
					", msatoshi_to_us_min" // 40
					", msatoshi_to_us_max" // 41
					", future_per_commitment_point" // 42
					", last_sent_commit" // 43
					", feerate_base" // 44
					", feerate_ppm" // 45
					", remote_upfront_shutdown_script" // 46
					", local_static_remotekey_start" // 47
					", remote_static_remotekey_start" // 48
					", option_anchor_outputs" // 49
					", shutdown_scriptpubkey_local" // 50
					", closer" // 51
					", state_change_reason" // 52
					", revocation_basepoint_local" // 53
					", payment_basepoint_local" // 54
					", htlc_basepoint_local" // 55
					", delayed_payment_basepoint_local" // 56
					", funding_pubkey_local" // 57
					", shutdown_wrong_txid" // 58
					", shutdown_wrong_outnum" // 59
					", lease_expiry" // 60
					", lease_commit_sig" // 61
					", lease_chan_max_msat" // 62
					", lease_chan_max_ppt" // 63
					" FROM channels"
                                        " WHERE state != ?;")); //? 0
	db_bind_int(stmt, 0, CLOSED);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		struct channel *c = wallet_stmt2channel(w, stmt);
		if (!c) {
			ok = false;
			break;
		}
		count++;
	}
	log_debug(w->log, "Loaded %d channels from DB", count);
	tal_free(stmt);
	return ok;
}

bool wallet_init_channels(struct wallet *w)
{
	/* We set the max channel database id separately */
	set_max_channel_dbid(w);
	return wallet_channels_load_active(w);
}

static enum channel_state_bucket get_state_channel_db(const char *dir, const char *typ)
{
	enum channel_state_bucket channel_state = IN_OFFERED;
	if (streq(dir, "out"))
		channel_state += 2;
	if (streq(typ, "fulfilled"))
		channel_state += 1;
	return channel_state;
}

static
void wallet_channel_stats_incr_x(struct wallet *w,
				 char const *dir,
				 char const *typ,
				 u64 cdbid,
				 struct amount_msat msat)
{
	struct db_stmt *stmt;
	const char *query = NULL;

	switch (get_state_channel_db(dir, typ)) {
	case IN_OFFERED:
		query = SQL("UPDATE channels"
			    "   SET in_payments_offered = COALESCE(in_payments_offered, 0) + 1"
			    "     , in_msatoshi_offered = COALESCE(in_msatoshi_offered, 0) + ?"
			    " WHERE id = ?;");
		break;
	case IN_FULLFILLED:
		query = SQL("UPDATE channels"
			    "   SET in_payments_fulfilled = COALESCE(in_payments_fulfilled, 0) + 1"
			    "     , in_msatoshi_fulfilled = COALESCE(in_msatoshi_fulfilled, 0) + ?"
			    " WHERE id = ?;");
		break;
	case OUT_OFFERED:
		query = SQL("UPDATE channels"
			    "   SET out_payments_offered = COALESCE(out_payments_offered, 0) + 1"
			    "     , out_msatoshi_offered = COALESCE(out_msatoshi_offered, 0) + ?"
			    " WHERE id = ?;");
		break;
	case OUT_FULLFILLED:
		query = SQL("UPDATE channels"
			    "   SET out_payments_fulfilled = COALESCE(out_payments_fulfilled, 0) + 1"
			    "     , out_msatoshi_fulfilled = COALESCE(out_msatoshi_fulfilled, 0) + ?"
			    " WHERE id = ?;");
		break;
	}

	// Sanity check!
	if (!query)
		fatal("Unknown channel state key (direction %s, type %s)", dir, typ);

	stmt = db_prepare_v2(w->db, query);
	db_bind_amount_msat(stmt, 0, &msat);
	db_bind_u64(stmt, 1, cdbid);

	db_exec_prepared_v2(take(stmt));
}
void wallet_channel_stats_incr_in_offered(struct wallet *w, u64 id,
					  struct amount_msat m)
{
	wallet_channel_stats_incr_x(w, "in", "offered", id, m);
}
void wallet_channel_stats_incr_in_fulfilled(struct wallet *w, u64 id,
					    struct amount_msat m)
{
	wallet_channel_stats_incr_x(w, "in", "fulfilled", id, m);
}
void wallet_channel_stats_incr_out_offered(struct wallet *w, u64 id,
					    struct amount_msat m)
{
	wallet_channel_stats_incr_x(w, "out", "offered", id, m);
}
void wallet_channel_stats_incr_out_fulfilled(struct wallet *w, u64 id,
					    struct amount_msat m)
{
	wallet_channel_stats_incr_x(w, "out", "fulfilled", id, m);
}

void wallet_channel_stats_load(struct wallet *w,
			       u64 id,
			       struct channel_stats *stats)
{
	struct db_stmt *stmt;
	int res;
	stmt = db_prepare_v2(w->db, SQL(
				     "SELECT"
				     "   in_payments_offered,  in_payments_fulfilled"
				     ",  in_msatoshi_offered,  in_msatoshi_fulfilled"
				     ", out_payments_offered, out_payments_fulfilled"
				     ", out_msatoshi_offered, out_msatoshi_fulfilled"
				     "  FROM channels"
				     " WHERE id = ?"));
	db_bind_u64(stmt, 0, id);
	db_query_prepared(stmt);

	res = db_step(stmt);

	/* This must succeed, since we know the channel exists */
	assert(res);

	stats->in_payments_offered = db_column_int_or_default(stmt, 0, 0);
	stats->in_payments_fulfilled = db_column_int_or_default(stmt, 1, 0);
	db_column_amount_msat_or_default(stmt, 2, &stats->in_msatoshi_offered, AMOUNT_MSAT(0));
	db_column_amount_msat_or_default(stmt, 3, &stats->in_msatoshi_fulfilled, AMOUNT_MSAT(0));
	stats->out_payments_offered = db_column_int_or_default(stmt, 4, 0);
	stats->out_payments_fulfilled = db_column_int_or_default(stmt, 5, 0);
	db_column_amount_msat_or_default(stmt, 6, &stats->out_msatoshi_offered, AMOUNT_MSAT(0));
	db_column_amount_msat_or_default(stmt, 7, &stats->out_msatoshi_fulfilled, AMOUNT_MSAT(0));
	tal_free(stmt);
}

void wallet_blocks_heights(struct wallet *w, u32 def, u32 *min, u32 *max)
{
	assert(min != NULL && max != NULL);
	struct db_stmt *stmt = db_prepare_v2(w->db, SQL("SELECT MIN(height), MAX(height) FROM blocks;"));
	db_query_prepared(stmt);
	*min = def;
	*max = def;

	/* If we ever processed a block we'll get the latest block in the chain */
	if (db_step(stmt)) {
		if (!db_column_is_null(stmt, 0)) {
			*min = db_column_int(stmt, 0);
			*max = db_column_int(stmt, 1);
		}
	}
	tal_free(stmt);
}

static void wallet_channel_config_insert(struct wallet *w,
					 struct channel_config *cc)
{
	struct db_stmt *stmt;

	assert(cc->id == 0);

	stmt = db_prepare_v2(w->db, SQL("INSERT INTO channel_configs DEFAULT VALUES;"));
	db_exec_prepared_v2(stmt);
	cc->id = db_last_insert_id_v2(stmt);
	tal_free(stmt);
}

static void wallet_channel_config_save(struct wallet *w,
				       const struct channel_config *cc)
{
	struct db_stmt *stmt;

	assert(cc->id != 0);
	stmt = db_prepare_v2(w->db, SQL("UPDATE channel_configs SET"
					"  dust_limit_satoshis=?,"
					"  max_htlc_value_in_flight_msat=?,"
					"  channel_reserve_satoshis=?,"
					"  htlc_minimum_msat=?,"
					"  to_self_delay=?,"
					"  max_accepted_htlcs=?"
					" WHERE id=?;"));
	db_bind_amount_sat(stmt, 0, &cc->dust_limit);
	db_bind_amount_msat(stmt, 1, &cc->max_htlc_value_in_flight);
	db_bind_amount_sat(stmt, 2, &cc->channel_reserve);
	db_bind_amount_msat(stmt, 3, &cc->htlc_minimum);
	db_bind_int(stmt, 4, cc->to_self_delay);
	db_bind_int(stmt, 5, cc->max_accepted_htlcs);
	db_bind_u64(stmt, 6, cc->id);
	db_exec_prepared_v2(take(stmt));
}

bool wallet_channel_config_load(struct wallet *w, const u64 id,
				struct channel_config *cc)
{
	bool ok = true;
	int col = 1;
	const char *query = SQL(
	    "SELECT id, dust_limit_satoshis, max_htlc_value_in_flight_msat, "
	    "channel_reserve_satoshis, htlc_minimum_msat, to_self_delay, "
	    "max_accepted_htlcs FROM channel_configs WHERE id= ? ;");
	struct db_stmt *stmt = db_prepare_v2(w->db, query);
	db_bind_u64(stmt, 0, id);
	db_query_prepared(stmt);

	if (!db_step(stmt))
		return false;

	cc->id = id;
	db_column_amount_sat(stmt, col++, &cc->dust_limit);
	db_column_amount_msat(stmt, col++, &cc->max_htlc_value_in_flight);
	db_column_amount_sat(stmt, col++, &cc->channel_reserve);
	db_column_amount_msat(stmt, col++, &cc->htlc_minimum);
	cc->to_self_delay = db_column_int(stmt, col++);
	cc->max_accepted_htlcs = db_column_int(stmt, col++);
	assert(col == 7);
	tal_free(stmt);
	return ok;
}

u64 wallet_get_channel_dbid(struct wallet *wallet)
{
	return ++wallet->max_channel_dbid;
}

/* When we receive the remote announcement message, we will also call this function */
void wallet_announcement_save(struct wallet *w, u64 id,
			      secp256k1_ecdsa_signature *remote_ann_node_sig,
			      secp256k1_ecdsa_signature *remote_ann_bitcoin_sig)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(w->db, SQL("UPDATE channels SET"
					"  remote_ann_node_sig=?,"
					"  remote_ann_bitcoin_sig=?"
					" WHERE id=?"));

	db_bind_signature(stmt, 0, remote_ann_node_sig);
	db_bind_signature(stmt, 1, remote_ann_bitcoin_sig);
	db_bind_u64(stmt, 2, id);
	db_exec_prepared_v2(take(stmt));
}

void wallet_channel_save(struct wallet *w, struct channel *chan)
{
	struct db_stmt *stmt;
	u8 *last_sent_commit;
	assert(chan->first_blocknum);

	wallet_channel_config_save(w, &chan->our_config);

	stmt = db_prepare_v2(w->db, SQL("UPDATE channels SET"
					"  shachain_remote_id=?," // 0
					"  short_channel_id=?," // 1
					"  full_channel_id=?," // 2
					"  state=?," // 3
					"  funder=?," // 4
					"  channel_flags=?," // 5
					"  minimum_depth=?," // 6
					"  next_index_local=?," // 7
					"  next_index_remote=?," // 8
					"  next_htlc_id=?," // 9
					"  funding_tx_id=?," // 10
					"  funding_tx_outnum=?," // 11
					"  funding_satoshi=?," // 12
					"  our_funding_satoshi=?," // 13
					"  funding_locked_remote=?," // 14
					"  push_msatoshi=?," // 15
					"  msatoshi_local=?," // 16
					"  shutdown_scriptpubkey_remote=?,"
					"  shutdown_keyidx_local=?," // 18
					"  channel_config_local=?," // 19
					"  last_tx=?, last_sig=?," // 20 + 21
					"  last_was_revoke=?," // 22
					"  min_possible_feerate=?," // 23
					"  max_possible_feerate=?," // 24
					"  msatoshi_to_us_min=?," // 25
					"  msatoshi_to_us_max=?," // 26
					"  feerate_base=?," // 27
					"  feerate_ppm=?," // 28
					"  remote_upfront_shutdown_script=?," // 29
					"  local_static_remotekey_start=?," // 30
					"  remote_static_remotekey_start=?," // 31
					"  option_anchor_outputs=?," // 32
					"  shutdown_scriptpubkey_local=?," // 33
					"  closer=?," // 34
					"  state_change_reason=?," // 35
					"  shutdown_wrong_txid=?," // 36
					"  shutdown_wrong_outnum=?," // 37
					"  lease_expiry=?," // 38
					"  lease_commit_sig=?," // 39
					"  lease_chan_max_msat=?," // 40
					"  lease_chan_max_ppt=?" // 41
					" WHERE id=?")); // 42
	db_bind_u64(stmt, 0, chan->their_shachain.id);
	if (chan->scid)
		db_bind_short_channel_id(stmt, 1, chan->scid);
	else
		db_bind_null(stmt, 1);

	db_bind_channel_id(stmt, 2, &chan->cid);
	db_bind_int(stmt, 3, chan->state);
	db_bind_int(stmt, 4, chan->opener);
	db_bind_int(stmt, 5, chan->channel_flags);
	db_bind_int(stmt, 6, chan->minimum_depth);

	db_bind_u64(stmt, 7, chan->next_index[LOCAL]);
	db_bind_u64(stmt, 8, chan->next_index[REMOTE]);
	db_bind_u64(stmt, 9, chan->next_htlc_id);

	db_bind_sha256d(stmt, 10, &chan->funding_txid.shad);

	db_bind_int(stmt, 11, chan->funding_outnum);
	db_bind_amount_sat(stmt, 12, &chan->funding);
	db_bind_amount_sat(stmt, 13, &chan->our_funds);
	db_bind_int(stmt, 14, chan->remote_funding_locked);
	db_bind_amount_msat(stmt, 15, &chan->push);
	db_bind_amount_msat(stmt, 16, &chan->our_msat);

	db_bind_talarr(stmt, 17, chan->shutdown_scriptpubkey[REMOTE]);
	db_bind_u64(stmt, 18, chan->final_key_idx);
	db_bind_u64(stmt, 19, chan->our_config.id);
	db_bind_psbt(stmt, 20, chan->last_tx->psbt);
	db_bind_signature(stmt, 21, &chan->last_sig.s);
	db_bind_int(stmt, 22, chan->last_was_revoke);
	db_bind_int(stmt, 23, chan->min_possible_feerate);
	db_bind_int(stmt, 24, chan->max_possible_feerate);
	db_bind_amount_msat(stmt, 25, &chan->msat_to_us_min);
	db_bind_amount_msat(stmt, 26, &chan->msat_to_us_max);
	db_bind_int(stmt, 27, chan->feerate_base);
	db_bind_int(stmt, 28, chan->feerate_ppm);
	db_bind_talarr(stmt, 29, chan->remote_upfront_shutdown_script);
	db_bind_u64(stmt, 30, chan->static_remotekey_start[LOCAL]);
	db_bind_u64(stmt, 31, chan->static_remotekey_start[REMOTE]);
	db_bind_int(stmt, 32, chan->option_anchor_outputs);
	db_bind_talarr(stmt, 33, chan->shutdown_scriptpubkey[LOCAL]);
	db_bind_int(stmt, 34, chan->closer);
	db_bind_int(stmt, 35, chan->state_change_cause);
	if (chan->shutdown_wrong_funding) {
		db_bind_txid(stmt, 36, &chan->shutdown_wrong_funding->txid);
		db_bind_int(stmt, 37, chan->shutdown_wrong_funding->n);
	} else {
		db_bind_null(stmt, 36);
		db_bind_null(stmt, 37);
	}

	db_bind_int(stmt, 38, chan->lease_expiry);
	if (chan->lease_commit_sig) {
		db_bind_signature(stmt, 39, chan->lease_commit_sig);
		db_bind_int(stmt, 40, chan->lease_chan_max_msat);
		db_bind_int(stmt, 41, chan->lease_chan_max_ppt);
	} else {
		db_bind_null(stmt, 39);
		db_bind_null(stmt, 40);
		db_bind_null(stmt, 41);
	}
	db_bind_u64(stmt, 42, chan->dbid);
	db_exec_prepared_v2(take(stmt));

	wallet_channel_config_save(w, &chan->channel_info.their_config);
	stmt = db_prepare_v2(w->db, SQL("UPDATE channels SET"
					"  fundingkey_remote=?,"
					"  revocation_basepoint_remote=?,"
					"  payment_basepoint_remote=?,"
					"  htlc_basepoint_remote=?,"
					"  delayed_payment_basepoint_remote=?,"
					"  per_commit_remote=?,"
					"  old_per_commit_remote=?,"
					"  channel_config_remote=?,"
					"  future_per_commitment_point=?"
					" WHERE id=?"));
	db_bind_pubkey(stmt, 0,  &chan->channel_info.remote_fundingkey);
	db_bind_pubkey(stmt, 1,  &chan->channel_info.theirbase.revocation);
	db_bind_pubkey(stmt, 2,  &chan->channel_info.theirbase.payment);
	db_bind_pubkey(stmt, 3,  &chan->channel_info.theirbase.htlc);
	db_bind_pubkey(stmt, 4,  &chan->channel_info.theirbase.delayed_payment);
	db_bind_pubkey(stmt, 5,  &chan->channel_info.remote_per_commit);
	db_bind_pubkey(stmt, 6,  &chan->channel_info.old_remote_per_commit);
	db_bind_u64(stmt, 7, chan->channel_info.their_config.id);
	if (chan->future_per_commitment_point)
		db_bind_pubkey(stmt, 8, chan->future_per_commitment_point);
	else
		db_bind_null(stmt, 8);
	db_bind_u64(stmt, 9, chan->dbid);
	db_exec_prepared_v2(take(stmt));

	/* FIXME: Updates channel_feerates by discarding and rewriting. */
	stmt = db_prepare_v2(w->db, SQL("DELETE FROM channel_feerates "
					"WHERE channel_id=?"));
	db_bind_u64(stmt, 0, chan->dbid);
	db_exec_prepared_v2(take(stmt));

	for (enum htlc_state i = 0;
	     i < ARRAY_SIZE(chan->fee_states->feerate);
	     i++) {
		if (!chan->fee_states->feerate[i])
			continue;
		stmt = db_prepare_v2(w->db, SQL("INSERT INTO channel_feerates "
						" VALUES(?, ?, ?)"));
		db_bind_u64(stmt, 0, chan->dbid);
		db_bind_int(stmt, 1, i);
		db_bind_int(stmt, 2, *chan->fee_states->feerate[i]);
		db_exec_prepared_v2(take(stmt));
	}

	/* FIXME: Updates channel_blockheights by discarding and rewriting. */
	stmt = db_prepare_v2(w->db, SQL("DELETE FROM channel_blockheights "
					"WHERE channel_id=?"));
	db_bind_u64(stmt, 0, chan->dbid);
	db_exec_prepared_v2(take(stmt));

	for (enum htlc_state i = 0;
	     i < ARRAY_SIZE(chan->blockheight_states->height);
	     i++) {
		if (!chan->blockheight_states->height[i])
			continue;
		stmt = db_prepare_v2(w->db, SQL("INSERT INTO channel_blockheights "
						" VALUES(?, ?, ?)"));
		db_bind_u64(stmt, 0, chan->dbid);
		db_bind_int(stmt, 1, i);
		db_bind_int(stmt, 2, *chan->blockheight_states->height[i]);
		db_exec_prepared_v2(take(stmt));
	}

	/* If we have a last_sent_commit, store it */
	last_sent_commit = tal_arr(tmpctx, u8, 0);
	for (size_t i = 0; i < tal_count(chan->last_sent_commit); i++)
		towire_changed_htlc(&last_sent_commit,
				    &chan->last_sent_commit[i]);
	/* Make it null in db if it's empty */
	if (tal_count(last_sent_commit) == 0)
		last_sent_commit = tal_free(last_sent_commit);

	stmt = db_prepare_v2(w->db, SQL("UPDATE channels SET"
					"  last_sent_commit=?"
					" WHERE id=?"));
	/* Update the inflights also */
	struct channel_inflight *inflight;
	list_for_each(&chan->inflights, inflight, list)
		wallet_inflight_save(w, inflight);

	db_bind_talarr(stmt, 0, last_sent_commit);
	db_bind_u64(stmt, 1, chan->dbid);
	db_exec_prepared_v2(take(stmt));
}

void wallet_state_change_add(struct wallet *w,
			     const u64 channel_id,
			     struct timeabs *timestamp,
			     enum channel_state old_state,
			     enum channel_state new_state,
			     enum state_change cause,
			     char *message)
{
	struct db_stmt *stmt;
	stmt = db_prepare_v2(w->db,
			     SQL("INSERT INTO channel_state_changes ("
				 "  channel_id"
				 ", timestamp"
				 ", old_state"
				 ", new_state"
				 ", cause"
				 ", message"
				 ") VALUES (?, ?, ?, ?, ?, ?);"));

	db_bind_u64(stmt, 0, channel_id);
	db_bind_timeabs(stmt, 1, *timestamp);
	db_bind_int(stmt, 2, old_state);
	db_bind_int(stmt, 3, new_state);
	db_bind_int(stmt, 4, cause);
	db_bind_text(stmt, 5, message);

	db_exec_prepared_v2(take(stmt));
}

struct state_change_entry *wallet_state_change_get(struct wallet *w,
						   const tal_t *ctx,
						   u64 channel_id)
{
	struct db_stmt *stmt;
	struct state_change_entry tmp;
	struct state_change_entry *res = tal_arr(ctx,
						 struct state_change_entry, 0);
	stmt = db_prepare_v2(
	    w->db, SQL("SELECT"
		       " timestamp,"
		       " old_state,"
		       " new_state,"
		       " cause,"
		       " message "
		       "FROM channel_state_changes "
		       "WHERE channel_id = ? "
		       "ORDER BY timestamp ASC;"));
	db_bind_int(stmt, 0, channel_id);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		tmp.timestamp = db_column_timeabs(stmt, 0);
		tmp.old_state = db_column_int(stmt, 1);
		tmp.new_state = db_column_int(stmt, 2);
		tmp.cause = db_column_int(stmt, 3);
		tmp.message = tal_strdup(res, (const char *)db_column_text(stmt, 4));
		tal_arr_expand(&res, tmp);
	}
	tal_free(stmt);
	return res;
}

static void wallet_peer_save(struct wallet *w, struct peer *peer)
{
	const char *addr =
	    type_to_string(tmpctx, struct wireaddr_internal, &peer->addr);
	struct db_stmt *stmt =
	    db_prepare_v2(w->db, SQL("SELECT id FROM peers WHERE node_id = ?"));

	db_bind_node_id(stmt, 0, &peer->id);
	db_query_prepared(stmt);

	if (db_step(stmt)) {
		/* So we already knew this peer, just return its dbid */
		peer->dbid = db_column_u64(stmt, 0);
		tal_free(stmt);

		/* Since we're at it update the wireaddr */
		stmt = db_prepare_v2(
		    w->db, SQL("UPDATE peers SET address = ? WHERE id = ?"));
		db_bind_text(stmt, 0, addr);
		db_bind_u64(stmt, 1, peer->dbid);
		db_exec_prepared_v2(take(stmt));

	} else {
		/* Unknown peer, create it from scratch */
		tal_free(stmt);
		stmt = db_prepare_v2(w->db,
				     SQL("INSERT INTO peers (node_id, address) VALUES (?, ?);")
			);
		db_bind_node_id(stmt, 0, &peer->id);
		db_bind_text(stmt, 1,addr);
		db_exec_prepared_v2(stmt);
		peer->dbid = db_last_insert_id_v2(take(stmt));
	}
}

void wallet_channel_insert(struct wallet *w, struct channel *chan)
{
	struct db_stmt *stmt;

	assert(chan->dbid != 0);
	assert(chan->unsaved_dbid == 0);

	if (chan->peer->dbid == 0)
		wallet_peer_save(w, chan->peer);

	/* Insert a stub, that we update, unifies INSERT and UPDATE paths */
	stmt = db_prepare_v2(
	    w->db, SQL("INSERT INTO channels ("
		       "  peer_id"
		       ", first_blocknum"
		       ", id"
		       ", revocation_basepoint_local"
		       ", payment_basepoint_local"
		       ", htlc_basepoint_local"
		       ", delayed_payment_basepoint_local"
		       ", funding_pubkey_local"
		       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?);"));
	db_bind_u64(stmt, 0, chan->peer->dbid);
	db_bind_int(stmt, 1, chan->first_blocknum);
	db_bind_int(stmt, 2, chan->dbid);

	db_bind_pubkey(stmt, 3, &chan->local_basepoints.revocation);
	db_bind_pubkey(stmt, 4, &chan->local_basepoints.payment);
	db_bind_pubkey(stmt, 5, &chan->local_basepoints.htlc);
	db_bind_pubkey(stmt, 6, &chan->local_basepoints.delayed_payment);
	db_bind_pubkey(stmt, 7, &chan->local_funding_pubkey);

	db_exec_prepared_v2(take(stmt));

	wallet_channel_config_insert(w, &chan->our_config);
	wallet_channel_config_insert(w, &chan->channel_info.their_config);
	wallet_shachain_init(w, &chan->their_shachain);

	/* Now save path as normal */
	wallet_channel_save(w, chan);
}

void wallet_channel_close(struct wallet *w, u64 wallet_id)
{
	/* We keep a couple of dependent tables around as well, such as the
	 * channel_configs table, since that might help us debug some issues,
	 * and it is rather limited in size. Tables that can grow quite
	 * considerably and that are of limited use after channel closure will
	 * be pruned as well. */

	struct db_stmt *stmt;

	/* Delete entries from `channel_htlcs` */
	stmt = db_prepare_v2(w->db, SQL("DELETE FROM channel_htlcs "
					"WHERE channel_id=?"));
	db_bind_u64(stmt, 0, wallet_id);
	db_exec_prepared_v2(take(stmt));

	/* Delete entries from `htlc_sigs` */
	stmt = db_prepare_v2(w->db, SQL("DELETE FROM htlc_sigs "
					"WHERE channelid=?"));
	db_bind_u64(stmt, 0, wallet_id);
	db_exec_prepared_v2(take(stmt));

	/* Delete entries from `htlc_sigs` */
	stmt = db_prepare_v2(w->db, SQL("DELETE FROM channeltxs "
					"WHERE channel_id=?"));
	db_bind_u64(stmt, 0, wallet_id);
	db_exec_prepared_v2(take(stmt));

	/* Delete any entries from 'inflights' */
	stmt = db_prepare_v2(w->db,
			     SQL("DELETE FROM channel_funding_inflights "
				 " WHERE channel_id=?"));
	db_bind_u64(stmt, 0, wallet_id);
	db_exec_prepared_v2(take(stmt));

	/* Delete shachains */
	stmt = db_prepare_v2(w->db, SQL("DELETE FROM shachains "
					"WHERE id IN ("
					"  SELECT shachain_remote_id "
					"  FROM channels "
					"  WHERE channels.id=?"
					")"));
	db_bind_u64(stmt, 0, wallet_id);
	db_exec_prepared_v2(take(stmt));

	/* Set the channel to closed and disassociate with peer */
	stmt = db_prepare_v2(w->db, SQL("UPDATE channels "
					"SET state=?, peer_id=? "
					"WHERE channels.id=?"));
	db_bind_u64(stmt, 0, CLOSED);
	db_bind_null(stmt, 1);
	db_bind_u64(stmt, 2, wallet_id);
	db_exec_prepared_v2(take(stmt));
}

//TODO: Vincenzo Working here
struct channel **wallet_get_channels_closed(struct wallet *w, const tal_t *ctx)
{
	struct db_stmt *stmt;
	struct channel **channels;


	stmt = db_prepare_v2(w->db, SQL("SELECT * FROM channels WHERE state=?;"));
	db_bind_u64(stmt, 0, CLOSED);
	db_query_prepared(stmt);


	channels = tal_arr(ctx, struct channel*, 0);
	while (db_step(stmt)) {
		struct channel *c = wallet_stmt2channel(w, stmt);
		tal_arr_expand(&channels, c);
	}
	tal_free(stmt);
	return channels;
}

void wallet_peer_delete(struct wallet *w, u64 peer_dbid)
{
	struct db_stmt *stmt;

	/* Must not have any channels still using this peer */
	stmt = db_prepare_v2(w->db, SQL("SELECT * FROM channels WHERE peer_id = ?;"));
	db_bind_u64(stmt, 0, peer_dbid);
	db_query_prepared(stmt);

	if (db_step(stmt))
		fatal("We have channels using peer %"PRIu64, peer_dbid);
	tal_free(stmt);

	stmt = db_prepare_v2(w->db, SQL("DELETE FROM peers WHERE id=?"));
	db_bind_u64(stmt, 0, peer_dbid);
	db_exec_prepared_v2(take(stmt));
}

void wallet_confirm_tx(struct wallet *w,
		       const struct bitcoin_txid *txid,
		       const u32 confirmation_height)
{
	struct db_stmt *stmt;
	assert(confirmation_height > 0);
	stmt = db_prepare_v2(w->db, SQL("UPDATE outputs "
					"SET confirmation_height = ? "
					"WHERE prev_out_tx = ?"));
	db_bind_int(stmt, 0, confirmation_height);
	db_bind_sha256d(stmt, 1, &txid->shad);

	db_exec_prepared_v2(take(stmt));
}

int wallet_extract_owned_outputs(struct wallet *w, const struct wally_tx *wtx,
				 const u32 *blockheight,
				 struct amount_sat *total)
{
	int num_utxos = 0;

	*total = AMOUNT_SAT(0);
	for (size_t output = 0; output < wtx->num_outputs; output++) {
		struct utxo *utxo;
		u32 index;
		bool is_p2sh;
		const u8 *script;
		struct amount_asset asset =
			wally_tx_output_get_amount(&wtx->outputs[output]);
		struct chain_coin_mvt *mvt;

		if (!amount_asset_is_main(&asset))
			continue;

		script = wally_tx_output_get_script(tmpctx,
						    &wtx->outputs[output]);
		if (!script)
			continue;

		if (!wallet_can_spend(w, script, &index, &is_p2sh))
			continue;

		utxo = tal(w, struct utxo);
		utxo->keyindex = index;
		utxo->is_p2sh = is_p2sh;
		utxo->amount = amount_asset_to_sat(&asset);
		utxo->status = OUTPUT_STATE_AVAILABLE;
		wally_txid(wtx, &utxo->txid);
		utxo->outnum = output;
		utxo->close_info = NULL;

		utxo->blockheight = blockheight ? blockheight : NULL;
		utxo->spendheight = NULL;
		utxo->scriptPubkey = tal_dup_talarr(utxo, u8, script);

		log_debug(w->log, "Owning output %zu %s (%s) txid %s%s",
			  output,
			  type_to_string(tmpctx, struct amount_sat,
					 &utxo->amount),
			  is_p2sh ? "P2SH" : "SEGWIT",
			  type_to_string(tmpctx, struct bitcoin_txid,
					 &utxo->txid), blockheight ? " CONFIRMED" : "");

		/* We only record final ledger movements */
		if (blockheight) {
			mvt = new_coin_deposit_sat(utxo, "wallet", &utxo->txid, utxo->outnum,
						   blockheight ? *blockheight : 0,
						   utxo->amount);
			notify_chain_mvt(w->ld, mvt);
		}

		if (!wallet_add_utxo(w, utxo, is_p2sh ? p2sh_wpkh : our_change)) {
			/* In case we already know the output, make
			 * sure we actually track its
			 * blockheight. This can happen when we grab
			 * the output from a transaction we created
			 * ourselves. */
			if (blockheight)
				wallet_confirm_tx(w, &utxo->txid, *blockheight);
			tal_free(utxo);
			continue;
		}

		/* This is an unconfirmed change output, we should track it */
		if (!is_p2sh && !blockheight)
			txfilter_add_scriptpubkey(w->ld->owned_txfilter, script);

		outpointfilter_add(w->owned_outpoints, &utxo->txid, utxo->outnum);

		if (!amount_sat_add(total, *total, utxo->amount))
			fatal("Cannot add utxo output %zu/%zu %s + %s",
			      output, wtx->num_outputs,
			      type_to_string(tmpctx, struct amount_sat, total),
			      type_to_string(tmpctx, struct amount_sat,
					     &utxo->amount));

		wallet_annotate_txout(w, &utxo->txid, output, TX_WALLET_DEPOSIT, 0);
		tal_free(utxo);
		num_utxos++;
	}
	return num_utxos;
}

void wallet_htlc_save_in(struct wallet *wallet,
			 const struct channel *chan, struct htlc_in *in)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(wallet->db,
			     SQL("INSERT INTO channel_htlcs ("
				 " channel_id,"
				 " channel_htlc_id, "
				 " direction,"
				 " msatoshi,"
				 " cltv_expiry,"
				 " payment_hash, "
				 " payment_key,"
				 " hstate,"
				 " shared_secret,"
				 " routing_onion,"
				 " received_time) VALUES "
				 "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));

	db_bind_u64(stmt, 0, chan->dbid);
	db_bind_u64(stmt, 1, in->key.id);
	db_bind_int(stmt, 2, DIRECTION_INCOMING);
	db_bind_amount_msat(stmt, 3, &in->msat);
	db_bind_int(stmt, 4, in->cltv_expiry);
	db_bind_sha256(stmt, 5, &in->payment_hash);

	if (in->preimage)
		db_bind_preimage(stmt, 6, in->preimage);
	else
		db_bind_null(stmt, 6);
	db_bind_int(stmt, 7, in->hstate);

	if (!in->shared_secret)
		db_bind_null(stmt, 8);
	else
		db_bind_secret(stmt, 8, in->shared_secret);

	db_bind_blob(stmt, 9, in->onion_routing_packet,
		     sizeof(in->onion_routing_packet));

	db_bind_timeabs(stmt, 10, in->received_time);

	db_exec_prepared_v2(stmt);
	in->dbid = db_last_insert_id_v2(take(stmt));
}

void wallet_htlc_save_out(struct wallet *wallet,
			  const struct channel *chan,
			  struct htlc_out *out)
{
	struct db_stmt *stmt;

	/* We absolutely need the incoming HTLC to be persisted before
	 * we can persist it's dependent */
	assert(out->in == NULL || out->in->dbid != 0);

	stmt = db_prepare_v2(
	    wallet->db,
	    SQL("INSERT INTO channel_htlcs ("
		" channel_id,"
		" channel_htlc_id,"
		" direction,"
		" origin_htlc,"
		" msatoshi,"
		" cltv_expiry,"
		" payment_hash,"
		" payment_key,"
		" hstate,"
		" routing_onion,"
		" malformed_onion,"
		" partid) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?);"));

	db_bind_u64(stmt, 0, chan->dbid);
	db_bind_u64(stmt, 1, out->key.id);
	db_bind_int(stmt, 2, DIRECTION_OUTGOING);
	if (out->in)
		db_bind_u64(stmt, 3, out->in->dbid);
	else
		db_bind_null(stmt, 3);
	db_bind_amount_msat(stmt, 4, &out->msat);
	db_bind_int(stmt, 5, out->cltv_expiry);
	db_bind_sha256(stmt, 6, &out->payment_hash);

	if (out->preimage)
		db_bind_preimage(stmt, 7, out->preimage);
	else
		db_bind_null(stmt, 7);
	db_bind_int(stmt, 8, out->hstate);

	db_bind_blob(stmt, 9, out->onion_routing_packet,
		     sizeof(out->onion_routing_packet));
	if (!out->am_origin)
		db_bind_null(stmt, 10);
	else
		db_bind_u64(stmt, 10, out->partid);

	db_exec_prepared_v2(stmt);
	out->dbid = db_last_insert_id_v2(stmt);
	tal_free(stmt);
}

/* input htlcs use failcode & failonion & we_filled, output htlcs use failmsg & failonion */
void wallet_htlc_update(struct wallet *wallet, const u64 htlc_dbid,
			const enum htlc_state new_state,
			const struct preimage *payment_key,
			enum onion_wire badonion,
			const struct onionreply *failonion,
			const u8 *failmsg,
			bool *we_filled)
{
	struct db_stmt *stmt;

	/* We should only use this for badonion codes */
	assert(!badonion || (badonion & BADONION));

	/* The database ID must be set by a previous call to
	 * `wallet_htlc_save_*` */
	assert(htlc_dbid);
	stmt = db_prepare_v2(
	    wallet->db, SQL("UPDATE channel_htlcs SET hstate=?, payment_key=?, "
			    "malformed_onion=?, failuremsg=?, localfailmsg=?, "
			    "we_filled=?"
			    " WHERE id=?"));

	/* FIXME: htlc_state_in_db */
	db_bind_int(stmt, 0, new_state);
	db_bind_u64(stmt, 6, htlc_dbid);

	if (payment_key)
		db_bind_preimage(stmt, 1, payment_key);
	else
		db_bind_null(stmt, 1);

	db_bind_int(stmt, 2, badonion);

	if (failonion)
		db_bind_onionreply(stmt, 3, failonion);
	else
		db_bind_null(stmt, 3);

	db_bind_talarr(stmt, 4, failmsg);

	if (we_filled)
		db_bind_int(stmt, 5, *we_filled);
	else
		db_bind_null(stmt, 5);

	db_exec_prepared_v2(take(stmt));
}

static bool wallet_stmt2htlc_in(struct channel *channel,
				struct db_stmt *stmt, struct htlc_in *in)
{
	bool ok = true;
	in->dbid = db_column_u64(stmt, 0);
	in->key.id = db_column_u64(stmt, 1);
	in->key.channel = channel;
	db_column_amount_msat(stmt, 2, &in->msat);
	in->cltv_expiry = db_column_int(stmt, 3);
	in->hstate = db_column_int(stmt, 4);
	in->status = NULL;
	/* FIXME: save blinding in db !*/
	in->blinding = NULL;

	db_column_sha256(stmt, 5, &in->payment_hash);

	if (!db_column_is_null(stmt, 6)) {
		in->preimage = tal(in, struct preimage);
		db_column_preimage(stmt, 6, in->preimage);
	} else {
		in->preimage = NULL;
	}

	assert(db_column_bytes(stmt, 7) == sizeof(in->onion_routing_packet));
	memcpy(&in->onion_routing_packet, db_column_blob(stmt, 7),
	       sizeof(in->onion_routing_packet));

	if (db_column_is_null(stmt, 8))
		in->failonion = NULL;
	else
		in->failonion = db_column_onionreply(in, stmt, 8);
	in->badonion = db_column_int(stmt, 9);
	if (db_column_is_null(stmt, 11)) {
		in->shared_secret = NULL;
	} else {
		assert(db_column_bytes(stmt, 11) == sizeof(struct secret));
		in->shared_secret = tal(in, struct secret);
		memcpy(in->shared_secret, db_column_blob(stmt, 11),
		       sizeof(struct secret));
#ifdef COMPAT_V062
		if (memeqzero(in->shared_secret, sizeof(*in->shared_secret)))
			in->shared_secret = tal_free(in->shared_secret);
#endif
	}

#ifdef COMPAT_V072
	if (db_column_is_null(stmt, 12)) {
		in->received_time.ts.tv_sec = 0;
		in->received_time.ts.tv_nsec = 0;
	} else
#endif /* COMPAT_V072 */
	in->received_time = db_column_timeabs(stmt, 12);

#ifdef COMPAT_V080
	/* This field is now reserved for badonion codes: the rest should
	 * use the failonion field. */
	if (in->badonion && !(in->badonion & BADONION)) {
		log_broken(channel->log,
			   "Replacing incoming HTLC %"PRIu64" error "
			   "%s with WIRE_TEMPORARY_NODE_FAILURE",
			   in->key.id, onion_wire_name(in->badonion));
		in->badonion = 0;
		in->failonion = create_onionreply(in,
						  in->shared_secret,
						  towire_temporary_node_failure(tmpctx));
	}
#endif

	if (!db_column_is_null(stmt, 13)) {
		in->we_filled = tal(in, bool);
		*in->we_filled = db_column_int(stmt, 13);
	} else
		in->we_filled = NULL;

	return ok;
}

/* Removes matching htlc from unconnected_htlcs_in */
static bool wallet_stmt2htlc_out(struct wallet *wallet,
				 struct channel *channel,
				 struct db_stmt *stmt, struct htlc_out *out,
				 struct htlc_in_map *unconnected_htlcs_in)
{
	bool ok = true;
	out->dbid = db_column_u64(stmt, 0);
	out->key.id = db_column_u64(stmt, 1);
	out->key.channel = channel;
	db_column_amount_msat(stmt, 2, &out->msat);
	out->cltv_expiry = db_column_int(stmt, 3);
	out->hstate = db_column_int(stmt, 4);
	db_column_sha256(stmt, 5, &out->payment_hash);
	/* FIXME: save blinding in db !*/
	out->blinding = NULL;

	if (!db_column_is_null(stmt, 6)) {
		out->preimage = tal(out, struct preimage);
		db_column_preimage(stmt, 6, out->preimage);
	} else {
		out->preimage = NULL;
	}

	assert(db_column_bytes(stmt, 7) == sizeof(out->onion_routing_packet));
	memcpy(&out->onion_routing_packet, db_column_blob(stmt, 7),
	       sizeof(out->onion_routing_packet));

	if (db_column_is_null(stmt, 8))
		out->failonion = NULL;
	else
		out->failonion = db_column_onionreply(out, stmt, 8);

	if (db_column_is_null(stmt, 14))
		out->failmsg = NULL;
	else
		out->failmsg = tal_dup_arr(out, u8, db_column_blob(stmt, 14),
					   db_column_bytes(stmt, 14), 0);

	out->in = NULL;

	if (!db_column_is_null(stmt, 10)) {
		u64 in_id = db_column_u64(stmt, 10);
		struct htlc_in *hin;

		hin = remove_htlc_in_by_dbid(unconnected_htlcs_in, in_id);
		if (hin)
			htlc_out_connect_htlc_in(out, hin);
		out->am_origin = false;
		if (!out->in && !out->preimage) {
#ifdef COMPAT_V061
			log_broken(wallet->log,
				   "Missing preimage for orphaned HTLC; replacing with zeros");
			out->preimage = talz(out, struct preimage);
#else
			fatal("Unable to find corresponding htlc_in %"PRIu64
			      " for unfulfilled htlc_out %"PRIu64,
			      in_id, out->dbid);
#endif
		}
	} else {
		out->partid = db_column_u64(stmt, 13);
		out->am_origin = true;
	}

	return ok;
}

static void fixup_hin(struct wallet *wallet, struct htlc_in *hin)
{
	/* We didn't used to save failcore, failonion... */
#ifdef COMPAT_V061
	/* We care about HTLCs being removed only, not those being added. */
	if (hin->hstate < SENT_REMOVE_HTLC)
		return;

	/* Successful ones are fine. */
	if (hin->preimage)
		return;

	/* Failed ones (only happens after db fixed!) OK. */
	if (hin->badonion || hin->failonion)
		return;

	hin->failonion = create_onionreply(hin,
					   hin->shared_secret,
					   towire_temporary_node_failure(tmpctx));

	log_broken(wallet->log, "HTLC #%"PRIu64" (%s) "
		   " for amount %s"
		   " from %s"
		   " is missing a resolution:"
		   " subsituting temporary node failure",
		   hin->key.id, htlc_state_name(hin->hstate),
		   type_to_string(tmpctx, struct amount_msat, &hin->msat),
		   type_to_string(tmpctx, struct node_id,
				  &hin->key.channel->peer->id));
#endif
}

bool wallet_htlcs_load_in_for_channel(struct wallet *wallet,
				      struct channel *chan,
				      struct htlc_in_map *htlcs_in)
{
	struct db_stmt *stmt;
	bool ok = true;
	int incount = 0;

	log_debug(wallet->log, "Loading in HTLCs for channel %"PRIu64, chan->dbid);
	stmt = db_prepare_v2(wallet->db, SQL("SELECT"
					     "  id"
					     ", channel_htlc_id"
					     ", msatoshi"
					     ", cltv_expiry"
					     ", hstate"
					     ", payment_hash"
					     ", payment_key"
					     ", routing_onion"
					     ", failuremsg"
					     ", malformed_onion"
					     ", origin_htlc"
					     ", shared_secret"
					     ", received_time"
					     ", we_filled"
					     " FROM channel_htlcs"
					     " WHERE direction= ?"
					     " AND channel_id= ?"
					     " AND hstate != ?"));
	db_bind_int(stmt, 0, DIRECTION_INCOMING);
	db_bind_u64(stmt, 1, chan->dbid);
	db_bind_int(stmt, 2, SENT_REMOVE_ACK_REVOCATION);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		struct htlc_in *in = tal(chan, struct htlc_in);
		ok &= wallet_stmt2htlc_in(chan, stmt, in);
		connect_htlc_in(htlcs_in, in);
		fixup_hin(wallet, in);
		ok &= htlc_in_check(in, NULL) != NULL;
		incount++;
	}
	tal_free(stmt);

	log_debug(wallet->log, "Restored %d incoming HTLCS", incount);
	return ok;
}

bool wallet_htlcs_load_out_for_channel(struct wallet *wallet,
				       struct channel *chan,
				       struct htlc_out_map *htlcs_out,
				       struct htlc_in_map *unconnected_htlcs_in)
{
	struct db_stmt *stmt;
	bool ok = true;
	int outcount = 0;

	stmt = db_prepare_v2(wallet->db, SQL("SELECT"
					     "  id"
					     ", channel_htlc_id"
					     ", msatoshi"
					     ", cltv_expiry"
					     ", hstate"
					     ", payment_hash"
					     ", payment_key"
					     ", routing_onion"
					     ", failuremsg"
					     ", malformed_onion"
					     ", origin_htlc"
					     ", shared_secret"
					     ", received_time"
					     ", partid"
					     ", localfailmsg"
					     " FROM channel_htlcs"
					     " WHERE direction = ?"
					     " AND channel_id = ?"
					     " AND hstate != ?"));
	db_bind_int(stmt, 0, DIRECTION_OUTGOING);
	db_bind_u64(stmt, 1, chan->dbid);
	db_bind_int(stmt, 2, RCVD_REMOVE_ACK_REVOCATION);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		struct htlc_out *out = tal(chan, struct htlc_out);
		ok &= wallet_stmt2htlc_out(wallet, chan, stmt, out,
					   unconnected_htlcs_in);
		connect_htlc_out(htlcs_out, out);
		/* Cannot htlc_out_check because we haven't wired the
		 * dependencies in yet */
		outcount++;
	}
	tal_free(stmt);

	log_debug(wallet->log, "Restored %d outgoing HTLCS", outcount);

	return ok;
}

bool wallet_invoice_create(struct wallet *wallet,
			   struct invoice *pinvoice,
			   const struct amount_msat *msat TAKES,
			   const struct json_escape *label TAKES,
			   u64 expiry,
			   const char *b11enc,
			   const char *description,
			   const u8 *features,
			   const struct preimage *r,
			   const struct sha256 *rhash,
			   const struct sha256 *local_offer_id)
{
	return invoices_create(wallet->invoices, pinvoice, msat, label, expiry, b11enc, description, features, r, rhash, local_offer_id);
}
bool wallet_invoice_find_by_label(struct wallet *wallet,
				  struct invoice *pinvoice,
				  const struct json_escape *label)
{
	return invoices_find_by_label(wallet->invoices, pinvoice, label);
}
bool wallet_invoice_find_by_rhash(struct wallet *wallet,
				  struct invoice *pinvoice,
				  const struct sha256 *rhash)
{
	return invoices_find_by_rhash(wallet->invoices, pinvoice, rhash);
}
bool wallet_invoice_find_unpaid(struct wallet *wallet,
				struct invoice *pinvoice,
				const struct sha256 *rhash)
{
	return invoices_find_unpaid(wallet->invoices, pinvoice, rhash);
}
bool wallet_invoice_delete(struct wallet *wallet,
			   struct invoice invoice)
{
	return invoices_delete(wallet->invoices, invoice);
}
void wallet_invoice_delete_expired(struct wallet *wallet, u64 e)
{
	invoices_delete_expired(wallet->invoices, e);
}
bool wallet_invoice_iterate(struct wallet *wallet,
			    struct invoice_iterator *it)
{
	return invoices_iterate(wallet->invoices, it);
}
const struct invoice_details *
wallet_invoice_iterator_deref(const tal_t *ctx, struct wallet *wallet,
			      const struct invoice_iterator *it)
{
	return invoices_iterator_deref(ctx, wallet->invoices, it);
}
bool wallet_invoice_resolve(struct wallet *wallet,
			    struct invoice invoice,
			    struct amount_msat msatoshi_received)
{
	return invoices_resolve(wallet->invoices, invoice, msatoshi_received);
}
void wallet_invoice_waitany(const tal_t *ctx,
			    struct wallet *wallet,
			    u64 lastpay_index,
			    void (*cb)(const struct invoice *, void*),
			    void *cbarg)
{
	invoices_waitany(ctx, wallet->invoices, lastpay_index, cb, cbarg);
}
void wallet_invoice_waitone(const tal_t *ctx,
			    struct wallet *wallet,
			    struct invoice invoice,
			    void (*cb)(const struct invoice *, void*),
			    void *cbarg)
{
	invoices_waitone(ctx, wallet->invoices, invoice, cb, cbarg);
}

const struct invoice_details *wallet_invoice_details(const tal_t *ctx,
						     struct wallet *wallet,
						     struct invoice invoice)
{
	return invoices_get_details(ctx, wallet->invoices, invoice);
}

struct htlc_stub *wallet_htlc_stubs(const tal_t *ctx, struct wallet *wallet,
				    struct channel *chan)
{
	struct htlc_stub *stubs;
	struct sha256 payment_hash;
	struct db_stmt *stmt;

	stmt = db_prepare_v2(wallet->db,
			     SQL("SELECT channel_id, direction, cltv_expiry, "
				 "channel_htlc_id, payment_hash "
				 "FROM channel_htlcs WHERE channel_id = ?;"));

	db_bind_u64(stmt, 0, chan->dbid);
	db_query_prepared(stmt);

	stubs = tal_arr(ctx, struct htlc_stub, 0);

	while (db_step(stmt)) {
		struct htlc_stub stub;

		assert(db_column_u64(stmt, 0) == chan->dbid);

		/* FIXME: merge these two enums */
		stub.owner = db_column_int(stmt, 1)==DIRECTION_INCOMING?REMOTE:LOCAL;
		stub.cltv_expiry = db_column_int(stmt, 2);
		stub.id = db_column_u64(stmt, 3);

		db_column_sha256(stmt, 4, &payment_hash);
		ripemd160(&stub.ripemd, payment_hash.u.u8, sizeof(payment_hash.u));
		tal_arr_expand(&stubs, stub);
	}
	tal_free(stmt);
	return stubs;
}

void wallet_local_htlc_out_delete(struct wallet *wallet,
				  struct channel *chan,
				  const struct sha256 *payment_hash,
				  u64 partid)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(wallet->db, SQL("DELETE FROM channel_htlcs"
					     " WHERE direction = ?"
					     " AND origin_htlc = ?"
					     " AND payment_hash = ?"
					     " AND partid = ?;"));
	db_bind_int(stmt, 0, DIRECTION_OUTGOING);
	db_bind_int(stmt, 1, 0);
	db_bind_sha256(stmt, 2, payment_hash);
	db_bind_u64(stmt, 3, partid);
	db_exec_prepared_v2(take(stmt));
}

static struct wallet_payment *
find_unstored_payment(struct wallet *wallet,
		      const struct sha256 *payment_hash,
		      u64 partid)
{
	struct wallet_payment *i;

	list_for_each(&wallet->unstored_payments, i, list) {
		if (sha256_eq(payment_hash, &i->payment_hash)
		    && i->partid == partid)
			return i;
	}
	return NULL;
}

static void destroy_unstored_payment(struct wallet_payment *payment)
{
	list_del(&payment->list);
}

void wallet_payment_setup(struct wallet *wallet, struct wallet_payment *payment)
{
	assert(!find_unstored_payment(wallet, &payment->payment_hash,
				      payment->partid));

	list_add_tail(&wallet->unstored_payments, &payment->list);
	tal_add_destructor(payment, destroy_unstored_payment);
}

void wallet_payment_store(struct wallet *wallet,
			  struct wallet_payment *payment TAKES)
{
	struct db_stmt *stmt;
	if (!find_unstored_payment(wallet, &payment->payment_hash, payment->partid)) {
		/* Already stored on-disk */
#if DEVELOPER
		/* Double-check that it is indeed stored to disk
		 * (catch bug, where we call this on a payment_hash
		 * we never paid to) */
		bool res;
		stmt =
		    db_prepare_v2(wallet->db, SQL("SELECT status FROM payments"
						  " WHERE payment_hash=?"
						  " AND partid = ?;"));
		db_bind_sha256(stmt, 0, &payment->payment_hash);
		db_bind_u64(stmt, 1, payment->partid);
		db_query_prepared(stmt);
		res = db_step(stmt);
		assert(res);
		tal_free(stmt);
#endif
		return;
	}

        /* Don't attempt to add the same payment twice */
	assert(!payment->id);

	stmt = db_prepare_v2(
		wallet->db,
		SQL("INSERT INTO payments ("
		    "  status,"
		    "  payment_hash,"
		    "  destination,"
		    "  msatoshi,"
		    "  timestamp,"
		    "  path_secrets,"
		    "  route_nodes,"
		    "  route_channels,"
		    "  msatoshi_sent,"
		    "  description,"
		    "  bolt11,"
		    "  total_msat,"
		    "  partid,"
		    "  local_offer_id"
		    ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));

	db_bind_int(stmt, 0, payment->status);
	db_bind_sha256(stmt, 1, &payment->payment_hash);

	if (payment->destination != NULL)
		db_bind_node_id(stmt, 2, payment->destination);
	else
		db_bind_null(stmt, 2);

	db_bind_amount_msat(stmt, 3, &payment->msatoshi);
	db_bind_int(stmt, 4, payment->timestamp);

	if (payment->path_secrets != NULL)
		db_bind_secret_arr(stmt, 5, payment->path_secrets);
	else
		db_bind_null(stmt, 5);

	assert((payment->route_channels == NULL) == (payment->route_nodes == NULL));
	if (payment->route_nodes) {
		db_bind_node_id_arr(stmt, 6, payment->route_nodes);
		db_bind_short_channel_id_arr(stmt, 7, payment->route_channels);
	} else {
		db_bind_null(stmt, 6);
		db_bind_null(stmt, 7);
	}

	db_bind_amount_msat(stmt, 8, &payment->msatoshi_sent);

	if (payment->label != NULL)
		db_bind_text(stmt, 9, payment->label);
	else
		db_bind_null(stmt, 9);

	if (payment->invstring != NULL)
		db_bind_text(stmt, 10, payment->invstring);
	else
		db_bind_null(stmt, 10);

	db_bind_amount_msat(stmt, 11, &payment->total_msat);
	db_bind_u64(stmt, 12, payment->partid);

	if (payment->local_offer_id != NULL)
		db_bind_sha256(stmt, 13, payment->local_offer_id);
	else
		db_bind_null(stmt, 13);

	db_exec_prepared_v2(stmt);
	payment->id = db_last_insert_id_v2(stmt);
	assert(payment->id > 0);
	tal_free(stmt);

	if (taken(payment)) {
		tal_free(payment);
	}  else {
		list_del(&payment->list);
		tal_del_destructor(payment, destroy_unstored_payment);
	}
}

void wallet_payment_delete(struct wallet *wallet,
			   const struct sha256 *payment_hash,
			   u64 partid)
{
	struct db_stmt *stmt;
	struct wallet_payment *payment;

	payment = find_unstored_payment(wallet, payment_hash, partid);
	if (payment) {
		tal_free(payment);
		return;
	}

	stmt = db_prepare_v2(
	    wallet->db, SQL("DELETE FROM payments WHERE payment_hash = ?"
			    " AND partid = ?"));

	db_bind_sha256(stmt, 0, payment_hash);
	db_bind_u64(stmt, 1, partid);

	db_exec_prepared_v2(take(stmt));
}

void wallet_payment_delete_by_hash(struct wallet *wallet,
				   const struct sha256 *payment_hash)
{
	struct db_stmt *stmt;
	stmt = db_prepare_v2(
		wallet->db, SQL("DELETE FROM payments WHERE payment_hash = ?"));

	db_bind_sha256(stmt, 0, payment_hash);
	db_exec_prepared_v2(take(stmt));
}

static struct wallet_payment *wallet_stmt2payment(const tal_t *ctx,
						  struct db_stmt *stmt)
{
	struct wallet_payment *payment = tal(ctx, struct wallet_payment);
	payment->id = db_column_u64(stmt, 0);
	payment->status = db_column_int(stmt, 1);

	if (!db_column_is_null(stmt, 2)) {
		payment->destination = tal(payment, struct node_id);
		db_column_node_id(stmt, 2, payment->destination);
	} else {
		payment->destination = NULL;
	}

	db_column_amount_msat(stmt, 3, &payment->msatoshi);
	db_column_sha256(stmt, 4, &payment->payment_hash);

	payment->timestamp = db_column_int(stmt, 5);
	if (!db_column_is_null(stmt, 6)) {
		payment->payment_preimage = tal(payment, struct preimage);
		db_column_preimage(stmt, 6, payment->payment_preimage);
	} else
		payment->payment_preimage = NULL;

	/* We either used `sendpay` or `sendonion` with the `shared_secrets`
	 * argument. */
	if (!db_column_is_null(stmt, 7))
		payment->path_secrets = db_column_secret_arr(payment, stmt, 7);
	else
		payment->path_secrets = NULL;

	/* Either none, or both are set */
	assert(db_column_is_null(stmt, 8) == db_column_is_null(stmt, 9));
	if (!db_column_is_null(stmt, 8)) {
		payment->route_nodes = db_column_node_id_arr(payment, stmt, 8);
		payment->route_channels =
		    db_column_short_channel_id_arr(payment, stmt, 9);
	} else {
		payment->route_nodes = NULL;
		payment->route_channels = NULL;
	}

	db_column_amount_msat(stmt, 10, &payment->msatoshi_sent);

	if (!db_column_is_null(stmt, 11) && db_column_text(stmt, 11) != NULL)
		payment->label =
		    tal_strdup(payment, (const char *)db_column_text(stmt, 11));
	else
		payment->label = NULL;

	if (!db_column_is_null(stmt, 12) && db_column_text(stmt, 12) != NULL)
		payment->invstring = tal_strdup(
		    payment, (const char *)db_column_text(stmt, 12));
	else
		payment->invstring = NULL;

	if (!db_column_is_null(stmt, 13))
		payment->failonion =
		    tal_dup_arr(payment, u8, db_column_blob(stmt, 13),
				db_column_bytes(stmt, 13), 0);
	else
		payment->failonion = NULL;

	if (!db_column_is_null(stmt, 14))
		db_column_amount_msat(stmt, 14, &payment->total_msat);
	else
		payment->total_msat = AMOUNT_MSAT(0);

	if (!db_column_is_null(stmt, 15))
		payment->partid = db_column_u64(stmt, 15);
	else
		payment->partid = 0;

	if (!db_column_is_null(stmt, 16)) {
		payment->local_offer_id = tal(payment, struct sha256);
		db_column_sha256(stmt, 16, payment->local_offer_id);
	} else
		payment->local_offer_id = NULL;

	return payment;
}

struct wallet_payment *
wallet_payment_by_hash(const tal_t *ctx, struct wallet *wallet,
		       const struct sha256 *payment_hash,
		       u64 partid)
{
	struct db_stmt *stmt;
	struct wallet_payment *payment;

	/* Present the illusion that it's in the db... */
	payment = find_unstored_payment(wallet, payment_hash, partid);
	if (payment)
		return payment;

	stmt = db_prepare_v2(wallet->db, SQL("SELECT"
					     "  id"
					     ", status"
					     ", destination"
					     ", msatoshi"
					     ", payment_hash"
					     ", timestamp"
					     ", payment_preimage"
					     ", path_secrets"
					     ", route_nodes"
					     ", route_channels"
					     ", msatoshi_sent"
					     ", description"
					     ", bolt11"
					     ", failonionreply"
					     ", total_msat"
					     ", partid"
					     ", local_offer_id"
					     " FROM payments"
					     " WHERE payment_hash = ?"
					     " AND partid = ?"));

	db_bind_sha256(stmt, 0, payment_hash);
	db_bind_u64(stmt, 1, partid);
	db_query_prepared(stmt);
	if (db_step(stmt)) {
		payment = wallet_stmt2payment(ctx, stmt);
	}
	tal_free(stmt);
	return payment;
}

void wallet_payment_set_status(struct wallet *wallet,
			       const struct sha256 *payment_hash,
			       u64 partid,
			       const enum wallet_payment_status newstatus,
			       const struct preimage *preimage)
{
	struct db_stmt *stmt;
	struct wallet_payment *payment;

	/* We can only fail an unstored payment! */
	payment = find_unstored_payment(wallet, payment_hash, partid);
	if (payment) {
		assert(newstatus == PAYMENT_FAILED);
		tal_free(payment);
		return;
	}

	stmt = db_prepare_v2(wallet->db,
			     SQL("UPDATE payments SET status=? "
				 "WHERE payment_hash=? AND partid=?"));

	db_bind_int(stmt, 0, wallet_payment_status_in_db(newstatus));
	db_bind_sha256(stmt, 1, payment_hash);
	db_bind_u64(stmt, 2, partid);
	db_exec_prepared_v2(take(stmt));

	if (preimage) {
		stmt = db_prepare_v2(wallet->db,
				     SQL("UPDATE payments SET payment_preimage=? "
					 "WHERE payment_hash=? AND partid=?"));

		db_bind_preimage(stmt, 0, preimage);
		db_bind_sha256(stmt, 1, payment_hash);
		db_bind_u64(stmt, 2, partid);
		db_exec_prepared_v2(take(stmt));
	}
	if (newstatus != PAYMENT_PENDING) {
		stmt =
		    db_prepare_v2(wallet->db, SQL("UPDATE payments"
						  "   SET path_secrets = NULL"
						  "     , route_nodes = NULL"
						  "     , route_channels = NULL"
						  " WHERE payment_hash = ?"
						  " AND partid = ?;"));
		db_bind_sha256(stmt, 0, payment_hash);
		db_bind_u64(stmt, 1, partid);
		db_exec_prepared_v2(take(stmt));
	}
}

void wallet_payment_get_failinfo(const tal_t *ctx,
				 struct wallet *wallet,
				 const struct sha256 *payment_hash,
				 u64 partid,
				 /* outputs */
				 struct onionreply **failonionreply,
				 bool *faildestperm,
				 int *failindex,
				 enum onion_wire *failcode,
				 struct node_id **failnode,
				 struct short_channel_id **failchannel,
				 u8 **failupdate,
				 char **faildetail,
				 int *faildirection)
{
	struct db_stmt *stmt;
	bool resb;
	size_t len;

	stmt = db_prepare_v2(wallet->db,
			     SQL("SELECT failonionreply, faildestperm"
				 ", failindex, failcode"
				 ", failnode, failchannel"
				 ", failupdate, faildetail, faildirection"
				 "  FROM payments"
				 " WHERE payment_hash=? AND partid=?;"));
	db_bind_sha256(stmt, 0, payment_hash);
	db_bind_u64(stmt, 1, partid);
	db_query_prepared(stmt);
	resb = db_step(stmt);
	assert(resb);

	if (db_column_is_null(stmt, 0))
		*failonionreply = NULL;
	else {
		*failonionreply = db_column_onionreply(ctx, stmt, 0);
	}
	*faildestperm = db_column_int(stmt, 1) != 0;
	*failindex = db_column_int(stmt, 2);
	*failcode = (enum onion_wire) db_column_int(stmt, 3);
	if (db_column_is_null(stmt, 4))
		*failnode = NULL;
	else {
		*failnode = tal(ctx, struct node_id);
		db_column_node_id(stmt, 4, *failnode);
	}
	if (db_column_is_null(stmt, 5))
		*failchannel = NULL;
	else {
		*failchannel = tal(ctx, struct short_channel_id);
		resb = db_column_short_channel_id(stmt, 5, *failchannel);
		assert(resb);

		/* For pre-0.6.2 dbs, direction will be 0 */
		*faildirection = db_column_int(stmt, 8);
	}
	if (db_column_is_null(stmt, 6))
		*failupdate = NULL;
	else {
		len = db_column_bytes(stmt, 6);
		*failupdate = tal_arr(ctx, u8, len);
		memcpy(*failupdate, db_column_blob(stmt, 6), len);
	}
	if (!db_column_is_null(stmt, 7))
		*faildetail = tal_strndup(ctx, db_column_blob(stmt, 7),
					  db_column_bytes(stmt, 7));
	else
		*faildetail = NULL;

	tal_free(stmt);
}

void wallet_payment_set_failinfo(struct wallet *wallet,
				 const struct sha256 *payment_hash,
				 u64 partid,
				 const struct onionreply *failonionreply,
				 bool faildestperm,
				 int failindex,
				 enum onion_wire failcode,
				 const struct node_id *failnode,
				 const struct short_channel_id *failchannel,
				 const u8 *failupdate /*tal_arr*/,
				 const char *faildetail,
				 int faildirection)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(wallet->db, SQL("UPDATE payments"
					     "   SET failonionreply=?"
					     "     , faildestperm=?"
					     "     , failindex=?"
					     "     , failcode=?"
					     "     , failnode=?"
					     "     , failchannel=?"
					     "     , failupdate=?"
					     "     , faildetail=?"
					     "     , faildirection=?"
					     " WHERE payment_hash=?"
					     " AND partid=?;"));
	if (failonionreply)
		db_bind_talarr(stmt, 0, failonionreply->contents);
	else
		db_bind_null(stmt, 0);
	db_bind_int(stmt, 1, faildestperm ? 1 : 0);
	db_bind_int(stmt, 2, failindex);
	db_bind_int(stmt, 3, (int) failcode);

	if (failnode)
		db_bind_node_id(stmt, 4, failnode);
	else
		db_bind_null(stmt, 4);

	if (failchannel) {
		db_bind_short_channel_id(stmt, 5, failchannel);
		db_bind_int(stmt, 8, faildirection);
	} else {
		db_bind_null(stmt, 5);
		db_bind_null(stmt, 8);
	}

	db_bind_talarr(stmt, 6, failupdate);

	if (faildetail != NULL)
		db_bind_text(stmt, 7, faildetail);
	else
		db_bind_null(stmt, 7);

	db_bind_sha256(stmt, 9, payment_hash);
	db_bind_u64(stmt, 10, partid);

	db_exec_prepared_v2(take(stmt));
}

const struct wallet_payment **
wallet_payment_list(const tal_t *ctx,
		    struct wallet *wallet,
		    const struct sha256 *payment_hash)
{
	const struct wallet_payment **payments;
	struct db_stmt *stmt;
	struct wallet_payment *p;
	size_t i;

	payments = tal_arr(ctx, const struct wallet_payment *, 0);
	if (payment_hash) {
		stmt =
		    db_prepare_v2(wallet->db, SQL("SELECT"
						  "  id"
						  ", status"
						  ", destination"
						  ", msatoshi"
						  ", payment_hash"
						  ", timestamp"
						  ", payment_preimage"
						  ", path_secrets"
						  ", route_nodes"
						  ", route_channels"
						  ", msatoshi_sent"
						  ", description"
						  ", bolt11"
						  ", failonionreply"
						  ", total_msat"
						  ", partid"
						  ", local_offer_id"
						  " FROM payments"
						  " WHERE payment_hash = ?"
						  " ORDER BY id;"));
		db_bind_sha256(stmt, 0, payment_hash);
	} else {
		stmt = db_prepare_v2(wallet->db, SQL("SELECT"
						     "  id"
						     ", status"
						     ", destination"
						     ", msatoshi"
						     ", payment_hash"
						     ", timestamp"
						     ", payment_preimage"
						     ", path_secrets"
						     ", route_nodes"
						     ", route_channels"
						     ", msatoshi_sent"
						     ", description"
						     ", bolt11"
						     ", failonionreply"
						     ", total_msat"
						     ", partid"
						     ", local_offer_id"
						     " FROM payments"
						     " ORDER BY id;"));
	}
	db_query_prepared(stmt);

	for (i = 0; db_step(stmt); i++) {
		tal_resize(&payments, i+1);
		payments[i] = wallet_stmt2payment(payments, stmt);
	}
	tal_free(stmt);

	/* Now attach payments not yet in db. */
	list_for_each(&wallet->unstored_payments, p, list) {
		if (payment_hash && !sha256_eq(&p->payment_hash, payment_hash))
			continue;
		tal_resize(&payments, i+1);
		payments[i++] = p;
	}

	return payments;
}

const struct wallet_payment **
wallet_payments_by_offer(const tal_t *ctx,
			 struct wallet *wallet,
			 const struct sha256 *local_offer_id)
{
	const struct wallet_payment **payments;
	struct db_stmt *stmt;
	struct wallet_payment *p;
	size_t i;

	payments = tal_arr(ctx, const struct wallet_payment *, 0);
	stmt = db_prepare_v2(wallet->db, SQL("SELECT"
					     "  id"
					     ", status"
					     ", destination"
					     ", msatoshi"
					     ", payment_hash"
					     ", timestamp"
					     ", payment_preimage"
					     ", path_secrets"
					     ", route_nodes"
					     ", route_channels"
					     ", msatoshi_sent"
					     ", description"
					     ", bolt11"
					     ", failonionreply"
					     ", total_msat"
					     ", partid"
					     ", local_offer_id"
					     " FROM payments"
					     " WHERE local_offer_id = ?;"));
	db_bind_sha256(stmt, 0, local_offer_id);
	db_query_prepared(stmt);

	for (i = 0; db_step(stmt); i++) {
		tal_resize(&payments, i+1);
		payments[i] = wallet_stmt2payment(payments, stmt);
	}
	tal_free(stmt);

	/* Now attach payments not yet in db. */
	list_for_each(&wallet->unstored_payments, p, list) {
		if (!p->local_offer_id || !sha256_eq(p->local_offer_id, local_offer_id))
			continue;
		tal_resize(&payments, i+1);
		payments[i++] = p;
	}

	return payments;
}

void wallet_htlc_sigs_save(struct wallet *w, u64 channel_id,
			   const struct bitcoin_signature *htlc_sigs)
{
	/* Clear any existing HTLC sigs for this channel */
	struct db_stmt *stmt = db_prepare_v2(
	    w->db, SQL("DELETE FROM htlc_sigs WHERE channelid = ?"));
	db_bind_u64(stmt, 0, channel_id);
	db_exec_prepared_v2(take(stmt));

	/* Now insert the new ones */
	for (size_t i=0; i<tal_count(htlc_sigs); i++) {
		stmt = db_prepare_v2(w->db,
				     SQL("INSERT INTO htlc_sigs (channelid, "
					 "signature) VALUES (?, ?)"));
		db_bind_u64(stmt, 0, channel_id);
		db_bind_signature(stmt, 1, &htlc_sigs[i].s);
		db_exec_prepared_v2(take(stmt));
	}
}

bool wallet_network_check(struct wallet *w)
{
	struct bitcoin_blkid chainhash;
	struct db_stmt *stmt = db_prepare_v2(
	    w->db, SQL("SELECT blobval FROM vars WHERE name='genesis_hash'"));
	db_query_prepared(stmt);

	if (db_step(stmt)) {
		db_column_sha256d(stmt, 0, &chainhash.shad);
		tal_free(stmt);
		if (!bitcoin_blkid_eq(&chainhash,
				      &chainparams->genesis_blockhash)) {
			log_broken(w->log, "Wallet blockchain hash does not "
					   "match network blockchain hash: %s "
					   "!= %s. "
					   "Are you on the right network? "
					   "(--network={one of %s})",
				   type_to_string(w, struct bitcoin_blkid,
						  &chainhash),
				   type_to_string(w, struct bitcoin_blkid,
						  &chainparams->genesis_blockhash),
				   chainparams_get_network_names(tmpctx));
			return false;
		}
	} else {
		tal_free(stmt);
		/* Still a pristine wallet, claim it for the chain
		 * that we are running */
		stmt = db_prepare_v2(w->db, SQL("INSERT INTO vars (name, blobval) "
						"VALUES ('genesis_hash', ?);"));
		db_bind_sha256d(stmt, 0, &chainparams->genesis_blockhash.shad);
		db_exec_prepared_v2(take(stmt));
	}
	return true;
}

/**
 * wallet_utxoset_prune -- Remove spent UTXO entries that are old
 */
static void wallet_utxoset_prune(struct wallet *w, const u32 blockheight)
{
	struct db_stmt *stmt;
	struct bitcoin_txid txid;

	stmt = db_prepare_v2(
	    w->db,
	    SQL("SELECT txid, outnum FROM utxoset WHERE spendheight < ?"));
	db_bind_int(stmt, 0, blockheight - UTXO_PRUNE_DEPTH);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		db_column_sha256d(stmt, 0, &txid.shad);
		outpointfilter_remove(w->utxoset_outpoints, &txid,
				      db_column_int(stmt, 1));
	}
	tal_free(stmt);

	stmt = db_prepare_v2(w->db,
			     SQL("DELETE FROM utxoset WHERE spendheight < ?"));
	db_bind_int(stmt, 0, blockheight - UTXO_PRUNE_DEPTH);
	db_exec_prepared_v2(take(stmt));
}

void wallet_block_add(struct wallet *w, struct block *b)
{
	struct db_stmt *stmt =
	    db_prepare_v2(w->db, SQL("INSERT INTO blocks "
				     "(height, hash, prev_hash) "
				     "VALUES (?, ?, ?);"));
	db_bind_int(stmt, 0, b->height);
	db_bind_sha256d(stmt, 1, &b->blkid.shad);
	if (b->prev) {
		db_bind_sha256d(stmt, 2, &b->prev->blkid.shad);
	}else {
		db_bind_null(stmt, 2);
	}
	db_exec_prepared_v2(take(stmt));

	/* Now cleanup UTXOs that we don't care about anymore */
	wallet_utxoset_prune(w, b->height);
}

void wallet_block_remove(struct wallet *w, struct block *b)
{
	struct db_stmt *stmt =
		db_prepare_v2(w->db, SQL("DELETE FROM blocks WHERE hash = ?"));
	db_bind_sha256d(stmt, 0, &b->blkid.shad);
	db_exec_prepared_v2(take(stmt));

	/* Make sure that all descendants of the block are also deleted */
	stmt = db_prepare_v2(w->db,
			     SQL("SELECT * FROM blocks WHERE height >= ?;"));
	db_bind_int(stmt, 0, b->height);
	db_query_prepared(stmt);
	assert(!db_step(stmt));
	tal_free(stmt);
}

void wallet_blocks_rollback(struct wallet *w, u32 height)
{
	struct db_stmt *stmt = db_prepare_v2(w->db, SQL("DELETE FROM blocks "
							"WHERE height > ?"));
	db_bind_int(stmt, 0, height);
	db_exec_prepared_v2(take(stmt));
}

bool wallet_outpoint_spend(struct wallet *w, const tal_t *ctx, const u32 blockheight,
		      const struct bitcoin_txid *txid, const u32 outnum)
{
	struct db_stmt *stmt;
	bool our_spend;
	if (outpointfilter_matches(w->owned_outpoints, txid, outnum)) {
		stmt = db_prepare_v2(w->db, SQL("UPDATE outputs "
						"SET spend_height = ?, "
						" status = ? "
						"WHERE prev_out_tx = ?"
						" AND prev_out_index = ?"));

		db_bind_int(stmt, 0, blockheight);
		db_bind_int(stmt, 1, output_status_in_db(OUTPUT_STATE_SPENT));
		db_bind_sha256d(stmt, 2, &txid->shad);
		db_bind_int(stmt, 3, outnum);

		db_exec_prepared_v2(take(stmt));

		our_spend = true;
	} else
		our_spend = false;

	if (outpointfilter_matches(w->utxoset_outpoints, txid, outnum)) {
		stmt = db_prepare_v2(w->db, SQL("UPDATE utxoset "
						"SET spendheight = ? "
						"WHERE txid = ?"
						" AND outnum = ?"));

		db_bind_int(stmt, 0, blockheight);
		db_bind_sha256d(stmt, 1, &txid->shad);
		db_bind_int(stmt, 2, outnum);
		db_exec_prepared_v2(stmt);
		tal_free(stmt);
	}
	return our_spend;
}

void wallet_utxoset_add(struct wallet *w, const struct bitcoin_tx *tx,
			const u32 outnum, const u32 blockheight,
			const u32 txindex, const u8 *scriptpubkey,
			struct amount_sat sat)
{
	struct db_stmt *stmt;
	struct bitcoin_txid txid;
	bitcoin_txid(tx, &txid);

	stmt = db_prepare_v2(w->db, SQL("INSERT INTO utxoset ("
					" txid,"
					" outnum,"
					" blockheight,"
					" spendheight,"
					" txindex,"
					" scriptpubkey,"
					" satoshis"
					") VALUES(?, ?, ?, ?, ?, ?, ?);"));
	db_bind_sha256d(stmt, 0, &txid.shad);
	db_bind_int(stmt, 1, outnum);
	db_bind_int(stmt, 2, blockheight);
	db_bind_null(stmt, 3);
	db_bind_int(stmt, 4, txindex);
	db_bind_talarr(stmt, 5, scriptpubkey);
	db_bind_amount_sat(stmt, 6, &sat);
	db_exec_prepared_v2(take(stmt));

	outpointfilter_add(w->utxoset_outpoints, &txid, outnum);
}

void wallet_filteredblock_add(struct wallet *w, const struct filteredblock *fb)
{
	struct db_stmt *stmt;
	if (wallet_have_block(w, fb->height))
		return;

	stmt = db_prepare_v2(w->db, SQL("INSERT INTO blocks "
					"(height, hash, prev_hash) "
					"VALUES (?, ?, ?);"));
	db_bind_int(stmt, 0, fb->height);
	db_bind_sha256d(stmt, 1, &fb->id.shad);
	db_bind_sha256d(stmt, 2, &fb->prev_hash.shad);
	db_exec_prepared_v2(take(stmt));

	for (size_t i = 0; i < tal_count(fb->outpoints); i++) {
		struct filteredblock_outpoint *o = fb->outpoints[i];
		stmt =
		    db_prepare_v2(w->db, SQL("INSERT INTO utxoset ("
					     " txid,"
					     " outnum,"
					     " blockheight,"
					     " spendheight,"
					     " txindex,"
					     " scriptpubkey,"
					     " satoshis"
					     ") VALUES(?, ?, ?, ?, ?, ?, ?);"));
		db_bind_sha256d(stmt, 0, &o->txid.shad);
		db_bind_int(stmt, 1, o->outnum);
		db_bind_int(stmt, 2, fb->height);
		db_bind_null(stmt, 3);
		db_bind_int(stmt, 4, o->txindex);
		db_bind_talarr(stmt, 5, o->scriptPubKey);
		db_bind_amount_sat(stmt, 6, &o->amount);
		db_exec_prepared_v2(take(stmt));

		outpointfilter_add(w->utxoset_outpoints, &o->txid, o->outnum);
	}
}

bool wallet_have_block(struct wallet *w, u32 blockheight)
{
	bool result;
	struct db_stmt *stmt = db_prepare_v2(
	    w->db, SQL("SELECT height FROM blocks WHERE height = ?"));
	db_bind_int(stmt, 0, blockheight);
	db_query_prepared(stmt);
	result = db_step(stmt);
	tal_free(stmt);
	return result;
}

struct outpoint *wallet_outpoint_for_scid(struct wallet *w, tal_t *ctx,
					  const struct short_channel_id *scid)
{
	struct db_stmt *stmt;
	struct outpoint *op;
	stmt = db_prepare_v2(w->db, SQL("SELECT"
					" txid,"
					" spendheight,"
					" scriptpubkey,"
					" satoshis "
					"FROM utxoset "
					"WHERE blockheight = ?"
					" AND txindex = ?"
					" AND outnum = ?"
					" AND spendheight IS NULL"));
	db_bind_int(stmt, 0, short_channel_id_blocknum(scid));
	db_bind_int(stmt, 1, short_channel_id_txnum(scid));
	db_bind_int(stmt, 2, short_channel_id_outnum(scid));
	db_query_prepared(stmt);

	if (!db_step(stmt)) {
		tal_free(stmt);
		return NULL;
	}

	op = tal(ctx, struct outpoint);
	op->blockheight = short_channel_id_blocknum(scid);
	op->txindex = short_channel_id_txnum(scid);
	op->outnum = short_channel_id_outnum(scid);
	db_column_sha256d(stmt, 0, &op->txid.shad);
	if (db_column_is_null(stmt, 1))
		op->spendheight = 0;
	else
		op->spendheight = db_column_int(stmt, 1);
	op->scriptpubkey = tal_arr(op, u8, db_column_bytes(stmt, 2));
	memcpy(op->scriptpubkey, db_column_blob(stmt, 2), db_column_bytes(stmt, 2));
	db_column_amount_sat(stmt, 3, &op->sat);
	tal_free(stmt);

	return op;
}

/* Turns "SELECT blockheight, txindex, outnum" into scids */
static const struct short_channel_id *db_scids(const tal_t *ctx,
					       struct db_stmt *stmt STEALS)
{
	struct short_channel_id *res = tal_arr(ctx, struct short_channel_id, 0);

	while (db_step(stmt)) {
		struct short_channel_id scid;
		u64 blocknum, txnum, outnum;
		bool ok;
		blocknum = db_column_int(stmt, 0);
		txnum = db_column_int(stmt, 1);
		outnum = db_column_int(stmt, 2);
		ok = mk_short_channel_id(&scid, blocknum, txnum, outnum);

		assert(ok);
		tal_arr_expand(&res, scid);
	}
	tal_free(stmt);
	return res;
}

const struct short_channel_id *
wallet_utxoset_get_spent(const tal_t *ctx, struct wallet *w,
			 u32 blockheight)
{
	struct db_stmt *stmt;
	stmt = db_prepare_v2(w->db, SQL("SELECT"
					" blockheight,"
					" txindex,"
					" outnum "
					"FROM utxoset "
					"WHERE spendheight = ?"));
	db_bind_int(stmt, 0, blockheight);
	db_query_prepared(stmt);

	return db_scids(ctx, stmt);
}

const struct short_channel_id *
wallet_utxoset_get_created(const tal_t *ctx, struct wallet *w,
			   u32 blockheight)
{
	struct db_stmt *stmt;
	stmt = db_prepare_v2(w->db, SQL("SELECT"
					" blockheight,"
					" txindex,"
					" outnum "
					"FROM utxoset "
					"WHERE blockheight = ?"));
	db_bind_int(stmt, 0, blockheight);
	db_query_prepared(stmt);

	return db_scids(ctx, stmt);
}

void wallet_transaction_add(struct wallet *w, const struct wally_tx *tx,
			    const u32 blockheight, const u32 txindex)
{
	struct bitcoin_txid txid;
	struct db_stmt *stmt = db_prepare_v2(
	    w->db, SQL("SELECT blockheight FROM transactions WHERE id=?"));

	wally_txid(tx, &txid);
	db_bind_txid(stmt, 0, &txid);
	db_query_prepared(stmt);

	if (!db_step(stmt)) {
		tal_free(stmt);
		/* This transaction is still unknown, insert */
		stmt = db_prepare_v2(w->db,
				     SQL("INSERT INTO transactions ("
					 "  id"
					 ", blockheight"
					 ", txindex"
					 ", rawtx) VALUES (?, ?, ?, ?);"));
		db_bind_txid(stmt, 0, &txid);
		if (blockheight) {
			db_bind_int(stmt, 1, blockheight);
			db_bind_int(stmt, 2, txindex);
		} else {
			db_bind_null(stmt, 1);
			db_bind_null(stmt, 2);
		}
		db_bind_tx(stmt, 3, tx);
		db_exec_prepared_v2(take(stmt));
	} else {
		tal_free(stmt);

		if (blockheight) {
			/* We know about the transaction, update */
			stmt = db_prepare_v2(w->db,
					     SQL("UPDATE transactions "
						 "SET blockheight = ?, txindex = ? "
						 "WHERE id = ?"));
			db_bind_int(stmt, 0, blockheight);
			db_bind_int(stmt, 1, txindex);
			db_bind_txid(stmt, 2, &txid);
			db_exec_prepared_v2(take(stmt));
		}
	}
}

static void wallet_annotation_add(struct wallet *w, const struct bitcoin_txid *txid, int num,
				  enum wallet_tx_annotation_type annotation_type, enum wallet_tx_type type, u64 channel)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(
		w->db,SQL("INSERT INTO transaction_annotations "
			  "(txid, idx, location, type, channel) "
			  "VALUES (?, ?, ?, ?, ?) ON CONFLICT(txid,idx) DO NOTHING;"));

	db_bind_txid(stmt, 0, txid);
	db_bind_int(stmt, 1, num);
	db_bind_int(stmt, 2, annotation_type);
	db_bind_int(stmt, 3, type);
	if (channel != 0)
		db_bind_u64(stmt, 4, channel);
	else
		db_bind_null(stmt, 4);
	db_exec_prepared_v2(take(stmt));
}

void wallet_annotate_txout(struct wallet *w, const struct bitcoin_txid *txid,
			   int outnum, enum wallet_tx_type type, u64 channel)
{
	wallet_annotation_add(w, txid, outnum, OUTPUT_ANNOTATION, type, channel);
}

void wallet_annotate_txin(struct wallet *w, const struct bitcoin_txid *txid,
			  int innum, enum wallet_tx_type type, u64 channel)
{
	wallet_annotation_add(w, txid, innum, INPUT_ANNOTATION, type, channel);
}

void wallet_transaction_annotate(struct wallet *w,
				 const struct bitcoin_txid *txid, enum wallet_tx_type type,
				 u64 channel_id)
{
	struct db_stmt *stmt = db_prepare_v2(
	    w->db, SQL("SELECT type, channel_id FROM transactions WHERE id=?"));
	db_bind_txid(stmt, 0, txid);
	db_query_prepared(stmt);

	if (!db_step(stmt))
		fatal("Attempting to annotate a transaction we don't have: %s",
		      type_to_string(tmpctx, struct bitcoin_txid, txid));

	if (!db_column_is_null(stmt, 0))
		type |= db_column_u64(stmt, 0);

	if (channel_id == 0 && !db_column_is_null(stmt, 1))
		channel_id = db_column_u64(stmt, 1);

	tal_free(stmt);

	stmt = db_prepare_v2(w->db, SQL("UPDATE transactions "
					"SET type = ?"
					", channel_id = ? "
					"WHERE id = ?"));

	db_bind_u64(stmt, 0, type);

	if (channel_id)
		db_bind_int(stmt, 1, channel_id);
	else
		db_bind_null(stmt, 1);

	db_bind_txid(stmt, 2, txid);
	db_exec_prepared_v2(take(stmt));
}

bool wallet_transaction_type(struct wallet *w, const struct bitcoin_txid *txid,
			     enum wallet_tx_type *type)
{
	struct db_stmt *stmt = db_prepare_v2(w->db, SQL("SELECT type FROM transactions WHERE id=?"));
	db_bind_sha256(stmt, 0, &txid->shad.sha);
	db_query_prepared(stmt);

	if (!db_step(stmt)) {
		tal_free(stmt);
		return false;
	}

	if (!db_column_is_null(stmt, 0))
		*type = db_column_u64(stmt, 0);
	else
		*type = 0;

	tal_free(stmt);
	return true;
}

struct bitcoin_tx *wallet_transaction_get(const tal_t *ctx, struct wallet *w,
					  const struct bitcoin_txid *txid)
{
	struct bitcoin_tx *tx;
	struct db_stmt *stmt = db_prepare_v2(
	    w->db, SQL("SELECT rawtx FROM transactions WHERE id=?"));
	db_bind_txid(stmt, 0, txid);
	db_query_prepared(stmt);

	if (!db_step(stmt)) {
		tal_free(stmt);
		return NULL;
	}

	if (!db_column_is_null(stmt, 0))
		tx = db_column_tx(ctx, stmt, 0);
	else
		tx = NULL;

	tal_free(stmt);
	return tx;
}

u32 wallet_transaction_height(struct wallet *w, const struct bitcoin_txid *txid)
{
	u32 blockheight;
	struct db_stmt *stmt = db_prepare_v2(
	    w->db, SQL("SELECT blockheight FROM transactions WHERE id=?"));
	db_bind_txid(stmt, 0, txid);
	db_query_prepared(stmt);

	if (!db_step(stmt)) {
		tal_free(stmt);
		return 0;
	}

	if (!db_column_is_null(stmt, 0))
		blockheight = db_column_int(stmt, 0);
	else
		blockheight = 0;
	tal_free(stmt);
	return blockheight;
}

struct txlocator *wallet_transaction_locate(const tal_t *ctx, struct wallet *w,
					    const struct bitcoin_txid *txid)
{
	struct txlocator *loc;
	struct db_stmt *stmt;

	stmt = db_prepare_v2(
		w->db, SQL("SELECT blockheight, txindex FROM transactions WHERE id=?"));
	db_bind_txid(stmt, 0, txid);
	db_query_prepared(stmt);

	if (!db_step(stmt)) {
		tal_free(stmt);
		return NULL;
	}

	if (db_column_is_null(stmt, 0))
		loc = NULL;
	else {
		loc = tal(ctx, struct txlocator);
		loc->blkheight = db_column_int(stmt, 0);
		loc->index = db_column_int(stmt, 1);
	}
	tal_free(stmt);
	return loc;
}

struct bitcoin_txid *wallet_transactions_by_height(const tal_t *ctx,
						   struct wallet *w,
						   const u32 blockheight)
{
	struct db_stmt *stmt;
	struct bitcoin_txid *txids = tal_arr(ctx, struct bitcoin_txid, 0);
	int count = 0;
	stmt = db_prepare_v2(
	    w->db, SQL("SELECT id FROM transactions WHERE blockheight=?"));
	db_bind_int(stmt, 0, blockheight);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		count++;
		tal_resize(&txids, count);
		db_column_txid(stmt, 0, &txids[count-1]);
	}
	tal_free(stmt);

	return txids;
}

void wallet_channeltxs_add(struct wallet *w, struct channel *chan,
			   const int type, const struct bitcoin_txid *txid,
			   const u32 input_num, const u32 blockheight)
{
	struct db_stmt *stmt;
	stmt = db_prepare_v2(w->db, SQL("INSERT INTO channeltxs ("
					"  channel_id"
					", type"
					", transaction_id"
					", input_num"
					", blockheight"
					") VALUES (?, ?, ?, ?, ?);"));
	db_bind_int(stmt, 0, chan->dbid);
	db_bind_int(stmt, 1, type);
	db_bind_sha256(stmt, 2, &txid->shad.sha);
	db_bind_int(stmt, 3, input_num);
	db_bind_int(stmt, 4, blockheight);

	db_exec_prepared_v2(take(stmt));
}

u32 *wallet_onchaind_channels(struct wallet *w,
			      const tal_t *ctx)
{
	struct db_stmt *stmt;
	size_t count = 0;
	u32 *channel_ids = tal_arr(ctx, u32, 0);
	stmt = db_prepare_v2(
	    w->db,
	    SQL("SELECT DISTINCT(channel_id) FROM channeltxs WHERE type = ?;"));
	db_bind_int(stmt, 0, WIRE_ONCHAIND_INIT);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		count++;
		tal_resize(&channel_ids, count);
			channel_ids[count-1] = db_column_u64(stmt, 0);
	}
	tal_free(stmt);

	return channel_ids;
}

struct channeltx *wallet_channeltxs_get(struct wallet *w, const tal_t *ctx,
					u32 channel_id)
{
	struct db_stmt *stmt;
	size_t count = 0;
	struct channeltx *res = tal_arr(ctx, struct channeltx, 0);
	stmt = db_prepare_v2(
	    w->db, SQL("SELECT"
		       "  c.type"
		       ", c.blockheight"
		       ", t.rawtx"
		       ", c.input_num"
		       ", c.blockheight - t.blockheight + 1 AS depth"
		       ", t.id as txid "
		       "FROM channeltxs c "
		       "JOIN transactions t ON t.id = c.transaction_id "
		       "WHERE c.channel_id = ? "
		       "ORDER BY c.id ASC;"));
	db_bind_int(stmt, 0, channel_id);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		count++;
		tal_resize(&res, count);

		res[count-1].channel_id = channel_id;
		res[count-1].type = db_column_int(stmt, 0);
		res[count-1].blockheight = db_column_int(stmt, 1);
		res[count-1].tx = db_column_tx(ctx, stmt, 2);
		res[count-1].input_num = db_column_int(stmt, 3);
		res[count-1].depth = db_column_int(stmt, 4);
		db_column_txid(stmt, 5, &res[count-1].txid);
	}
	tal_free(stmt);
	return res;
}

static bool wallet_forwarded_payment_update(struct wallet *w,
					    const struct htlc_in *in,
					    const struct htlc_out *out,
					    enum forward_status state,
					    enum onion_wire failcode,
					    struct timeabs *resolved_time)
{
	struct db_stmt *stmt;
	bool changed;

	/* We update based solely on the htlc_in since an HTLC cannot be
	 * associated with more than one forwarded payment. This saves us from
	 * having to have two versions of the update statement (one with and
	 * one without the htlc_out restriction).*/
	stmt = db_prepare_v2(w->db,
			     SQL("UPDATE forwarded_payments SET"
				 "  in_msatoshi=?"
				 ", out_msatoshi=?"
				 ", state=?"
				 ", resolved_time=?"
				 ", failcode=?"
				 " WHERE in_htlc_id=?"));
	db_bind_amount_msat(stmt, 0, &in->msat);

	if (out) {
		db_bind_amount_msat(stmt, 1, &out->msat);
	} else {
		db_bind_null(stmt, 1);
	}

	db_bind_int(stmt, 2, wallet_forward_status_in_db(state));

	if (resolved_time != NULL) {
		db_bind_timeabs(stmt, 3, *resolved_time);
	} else {
		db_bind_null(stmt, 3);
	}

	if (failcode != 0) {
		assert(state == FORWARD_FAILED || state == FORWARD_LOCAL_FAILED);
		db_bind_int(stmt, 4, (int)failcode);
	} else {
		db_bind_null(stmt, 4);
	}

	db_bind_u64(stmt, 5, in->dbid);
	db_exec_prepared_v2(stmt);
	changed = db_count_changes(stmt) != 0;
	tal_free(stmt);

	return changed;
}

void wallet_forwarded_payment_add(struct wallet *w, const struct htlc_in *in,
				  const struct short_channel_id *scid_out,
				  const struct htlc_out *out,
				  enum forward_status state,
				  enum onion_wire failcode)
{
	struct db_stmt *stmt;
	struct timeabs *resolved_time;

	if (state == FORWARD_SETTLED || state == FORWARD_FAILED) {
		resolved_time = tal(tmpctx, struct timeabs);
		*resolved_time = time_now();
	} else {
		resolved_time = NULL;
	}

	if (wallet_forwarded_payment_update(w, in, out, state, failcode, resolved_time))
		goto notify;

	stmt = db_prepare_v2(w->db,
			     SQL("INSERT INTO forwarded_payments ("
				 "  in_htlc_id"
				 ", out_htlc_id"
				 ", in_channel_scid"
				 ", out_channel_scid"
				 ", in_msatoshi"
				 ", out_msatoshi"
				 ", state"
				 ", received_time"
				 ", resolved_time"
				 ", failcode"
				 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));
	db_bind_u64(stmt, 0, in->dbid);

	if (out) {
		db_bind_u64(stmt, 1, out->dbid);
		db_bind_u64(stmt, 3, out->key.channel->scid->u64);
		db_bind_amount_msat(stmt, 5, &out->msat);
	} else {
		/* FORWARD_LOCAL_FAILED may occur before we get htlc_out */
		assert(failcode != 0);
		assert(state == FORWARD_LOCAL_FAILED);
		db_bind_null(stmt, 1);
		db_bind_null(stmt, 3);
		db_bind_null(stmt, 5);
	}

	db_bind_u64(stmt, 2, in->key.channel->scid->u64);

	db_bind_amount_msat(stmt, 4, &in->msat);

	db_bind_int(stmt, 6, wallet_forward_status_in_db(state));
	db_bind_timeabs(stmt, 7, in->received_time);

	if (resolved_time != NULL)
		db_bind_timeabs(stmt, 8, *resolved_time);
	else
		db_bind_null(stmt, 8);

	if (failcode != 0) {
		assert(state == FORWARD_FAILED || state == FORWARD_LOCAL_FAILED);
		db_bind_int(stmt, 9, (int)failcode);
	} else {
		db_bind_null(stmt, 9);
	}

	db_exec_prepared_v2(take(stmt));

notify:
	notify_forward_event(w->ld, in, scid_out, out ? &out->msat : NULL,
			     state, failcode, resolved_time);
}

struct amount_msat wallet_total_forward_fees(struct wallet *w)
{
	struct db_stmt *stmt;
	struct amount_msat total;
	bool res;

	stmt = db_prepare_v2(w->db, SQL("SELECT"
					" CAST(COALESCE(SUM(in_msatoshi - out_msatoshi), 0) AS BIGINT)"
					"FROM forwarded_payments "
					"WHERE state = ?;"));
	db_bind_int(stmt, 0, wallet_forward_status_in_db(FORWARD_SETTLED));
	db_query_prepared(stmt);

	res = db_step(stmt);
	assert(res);

	db_column_amount_msat(stmt, 0, &total);
	tal_free(stmt);

	return total;
}

bool string_to_forward_status(const char *status_str, enum forward_status *status)
{
	if (streq(status_str, "offered")) {
		*status = FORWARD_OFFERED;
		return true;
	} else if (streq(status_str, "settled")) {
		*status = FORWARD_SETTLED;
		return true;
	} else if (streq(status_str, "failed")) {
		*status = FORWARD_FAILED;
		return true;
	} else if (streq(status_str, "local_failed")) {
		*status = FORWARD_LOCAL_FAILED;
		return true;
	}
	return false;
}

const struct forwarding *wallet_forwarded_payments_get(struct wallet *w,
						       const tal_t *ctx,
						       enum forward_status status,
						       const struct short_channel_id *chan_in,
						       const struct short_channel_id *chan_out)
{
	struct forwarding *results = tal_arr(ctx, struct forwarding, 0);
	size_t count = 0;
	struct db_stmt *stmt;

	// placeholder for any parameter, the value doesn't matter because it's discarded by sql
	const int any = -1;

	stmt = db_prepare_v2(
	    w->db,
	    SQL("SELECT"
		"  f.state"
		", in_msatoshi"
		", out_msatoshi"
		", hin.payment_hash as payment_hash"
		", in_channel_scid"
		", out_channel_scid"
		", f.received_time"
		", f.resolved_time"
		", f.failcode "
		"FROM forwarded_payments f "
		"LEFT JOIN channel_htlcs hin ON (f.in_htlc_id = hin.id) "
		"WHERE (1 = ? OR f.state = ?) AND "
		"(1 = ? OR f.in_channel_scid = ?) AND "
		"(1 = ? OR f.out_channel_scid = ?)"));

	if (status == FORWARD_ANY) {
		// any status
		db_bind_int(stmt, 0, 1);
		db_bind_int(stmt, 1, any);
	} else {
		// specific forward status
		db_bind_int(stmt, 0, 0);
		db_bind_int(stmt, 1, status);
	}

	if (chan_in) {
		// specific in_channel
		db_bind_int(stmt, 2, 0);
		db_bind_u64(stmt, 3, chan_in->u64);
	} else {
		// any in_channel
		db_bind_int(stmt, 2, 1);
		db_bind_int(stmt, 3, any);
	}

	if (chan_out) {
		// specific out_channel
		db_bind_int(stmt, 4, 0);
		db_bind_u64(stmt, 5, chan_out->u64);
	} else {
		// any out_channel
		db_bind_int(stmt, 4, 1);
		db_bind_int(stmt, 5, any);
	}

	db_query_prepared(stmt);

	for (count=0; db_step(stmt); count++) {
		tal_resize(&results, count+1);
		struct forwarding *cur = &results[count];
		cur->status = db_column_int(stmt, 0);
		db_column_amount_msat(stmt, 1, &cur->msat_in);

		if (!db_column_is_null(stmt, 2)) {
			db_column_amount_msat(stmt, 2, &cur->msat_out);
			if (!amount_msat_sub(&cur->fee, cur->msat_in, cur->msat_out)) {
				log_broken(w->log, "Forwarded in %s less than out %s!",
					   type_to_string(tmpctx, struct amount_msat,
							  &cur->msat_in),
					   type_to_string(tmpctx, struct amount_msat,
							  &cur->msat_out));
				cur->fee = AMOUNT_MSAT(0);
			}
		}
		else {
			assert(cur->status == FORWARD_LOCAL_FAILED);
			cur->msat_out = AMOUNT_MSAT(0);
			/* For this case, this forward_payment doesn't have out channel,
			 * so the fee should be set as 0.*/
			cur->fee =  AMOUNT_MSAT(0);
		}

		if (!db_column_is_null(stmt, 3)) {
			cur->payment_hash = tal(ctx, struct sha256);
			db_column_sha256(stmt, 3, cur->payment_hash);
		} else {
			cur->payment_hash = NULL;
		}

		cur->channel_in.u64 = db_column_u64(stmt, 4);

		if (!db_column_is_null(stmt, 5)) {
			cur->channel_out.u64 = db_column_u64(stmt, 5);
		} else {
			assert(cur->status == FORWARD_LOCAL_FAILED);
			cur->channel_out.u64 = 0;
		}

		cur->received_time = db_column_timeabs(stmt, 6);

		if (!db_column_is_null(stmt, 7)) {
			cur->resolved_time = tal(ctx, struct timeabs);
			*cur->resolved_time = db_column_timeabs(stmt, 7);
		} else {
			cur->resolved_time = NULL;
		}

		if (!db_column_is_null(stmt, 8)) {
			assert(cur->status == FORWARD_FAILED ||
			       cur->status == FORWARD_LOCAL_FAILED);
			cur->failcode = db_column_int(stmt, 8);
		} else {
			cur->failcode = 0;
		}
	}
	tal_free(stmt);
	return results;
}

struct wallet_transaction *wallet_transactions_get(struct wallet *w, const tal_t *ctx)
{
	struct db_stmt *stmt;
	size_t count;
	struct wallet_transaction *cur = NULL, *txs = tal_arr(ctx, struct wallet_transaction, 0);
	struct bitcoin_txid last;

	/* Make sure we can check for changing txids */
	memset(&last, 0, sizeof(last));

	stmt = db_prepare_v2(
	    w->db,
	    SQL("SELECT"
		"  t.id"
		", t.rawtx"
		", t.blockheight"
		", t.txindex"
		", t.type as txtype"
		", c2.short_channel_id as txchan"
		", a.location"
		", a.idx as ann_idx"
		", a.type as annotation_type"
		", c.short_channel_id"
		" FROM"
		"  transactions t LEFT JOIN"
		"  transaction_annotations a ON (a.txid = t.id) LEFT JOIN"
		"  channels c ON (a.channel = c.id) LEFT JOIN"
		"  channels c2 ON (t.channel_id = c2.id) "
		"ORDER BY t.blockheight, t.txindex ASC"));
	db_query_prepared(stmt);

	for (count = 0; db_step(stmt); count++) {
		struct bitcoin_txid curtxid;
		db_column_txid(stmt, 0, &curtxid);

		/* If this is a new entry, allocate it in the array and set
		 * the common fields (all fields from the transactions table. */
		if (!bitcoin_txid_eq(&last, &curtxid)) {
			last = curtxid;
			tal_resize(&txs, tal_count(txs) + 1);
			cur = &txs[tal_count(txs) - 1];
			db_column_txid(stmt, 0, &cur->id);
			cur->tx = db_column_tx(txs, stmt, 1);
			cur->rawtx = tal_dup_arr(txs, u8, db_column_blob(stmt, 1),
						 db_column_bytes(stmt, 1), 0);
			/* TX may be unconfirmed. */
			if (!db_column_is_null(stmt, 2)) {
				cur->blockheight = db_column_int(stmt, 2);
				if (!db_column_is_null(stmt, 3)) {
					cur->txindex = db_column_int(stmt, 3);
				} else {
					cur->txindex = 0;
				}
			} else {
				cur->blockheight = 0;
				cur->txindex = 0;
			}
			if (!db_column_is_null(stmt, 4))
				cur->annotation.type = db_column_u64(stmt, 4);
			else
				cur->annotation.type = 0;
			if (!db_column_is_null(stmt, 5))
				db_column_short_channel_id(stmt, 5, &cur->annotation.channel);
			else
				cur->annotation.channel.u64 = 0;

			cur->output_annotations = tal_arrz(txs, struct tx_annotation, cur->tx->wtx->num_outputs);
			cur->input_annotations = tal_arrz(txs, struct tx_annotation, cur->tx->wtx->num_inputs);
		}

		/* This should always be set by the above if-statement,
		 * otherwise we have a txid of all 0x00 bytes... */
		assert(cur != NULL);

		/* Check if we have any annotations. If there are none the
		 * fields are all set to null */
		if (!db_column_is_null(stmt, 6)) {
			enum wallet_tx_annotation_type loc = db_column_int(stmt, 6);
			int idx = db_column_int(stmt, 7);
			struct tx_annotation *ann;

			/* Select annotation from array to fill in. */
			if (loc == OUTPUT_ANNOTATION)
				ann = &cur->output_annotations[idx];
			else if (loc == INPUT_ANNOTATION)
				ann = &cur->input_annotations[idx];
			else
				fatal("Transaction annotations are only available for inputs and outputs. Value %d", loc);

			/* cppcheck-suppress uninitvar - false positive on fatal() above */
			ann->type = db_column_int(stmt, 8);
			if (!db_column_is_null(stmt, 9))
				db_column_short_channel_id(stmt, 9, &ann->channel);
			else
				ann->channel.u64 = 0;
		}
	}
	tal_free(stmt);
	return txs;
}

void wallet_penalty_base_add(struct wallet *w, u64 chan_id,
			     const struct penalty_base *pb)
{
	struct db_stmt *stmt;
	stmt = db_prepare_v2(w->db,
			     SQL("INSERT INTO penalty_bases ("
				 "  channel_id"
				 ", commitnum"
				 ", txid"
				 ", outnum"
				 ", amount"
				 ") VALUES (?, ?, ?, ?, ?);"));

	db_bind_u64(stmt, 0, chan_id);
	db_bind_u64(stmt, 1, pb->commitment_num);
	db_bind_txid(stmt, 2, &pb->txid);
	db_bind_int(stmt, 3, pb->outnum);
	db_bind_amount_sat(stmt, 4, &pb->amount);

	db_exec_prepared_v2(take(stmt));
}

struct penalty_base *wallet_penalty_base_load_for_channel(const tal_t *ctx,
							  struct wallet *w,
							  u64 chan_id)
{
	struct db_stmt *stmt;
	struct penalty_base *res = tal_arr(ctx, struct penalty_base, 0);
	stmt = db_prepare_v2(
		w->db,
		SQL("SELECT commitnum, txid, outnum, amount "
		    "FROM penalty_bases "
		    "WHERE channel_id = ?"));

	db_bind_u64(stmt, 0, chan_id);
	db_query_prepared(stmt);

	while (db_step(stmt)) {
		struct penalty_base pb;
		pb.commitment_num = db_column_u64(stmt, 0);
		db_column_txid(stmt, 1, &pb.txid);
		pb.outnum = db_column_int(stmt, 2);
		db_column_amount_sat(stmt, 3, &pb.amount);
		tal_arr_expand(&res, pb);
	}
	tal_free(stmt);
	return res;
}

void wallet_penalty_base_delete(struct wallet *w, u64 chan_id, u64 commitnum)
{
	struct db_stmt *stmt;
	stmt = db_prepare_v2(
		w->db,
		SQL("DELETE FROM penalty_bases "
		    "WHERE channel_id = ? AND commitnum = ?"));
	db_bind_u64(stmt, 0, chan_id);
	db_bind_u64(stmt, 1, commitnum);
	db_exec_prepared_v2(take(stmt));
}

bool wallet_offer_create(struct wallet *w,
			 const struct sha256 *offer_id,
			 const char *bolt12,
			 const struct json_escape *label,
			 enum offer_status status)
{
	struct db_stmt *stmt;

	assert(offer_status_active(status));

	/* Test if already exists. */
	stmt = db_prepare_v2(w->db, SQL("SELECT 1"
					"  FROM offers"
					" WHERE offer_id = ?;"));
	db_bind_sha256(stmt, 0, offer_id);
	db_query_prepared(stmt);

	if (db_step(stmt)) {
		tal_free(stmt);
		return false;
	}
	tal_free(stmt);

	stmt = db_prepare_v2(w->db,
			     SQL("INSERT INTO offers ("
				 "  offer_id"
				 ", bolt12"
				 ", label"
				 ", status"
				 ") VALUES (?, ?, ?, ?);"));

	db_bind_sha256(stmt, 0, offer_id);
	db_bind_text(stmt, 1, bolt12);
	if (label)
		db_bind_json_escape(stmt, 2, label);
	else
		db_bind_null(stmt, 2);
	db_bind_int(stmt, 3, offer_status_in_db(status));
	db_exec_prepared_v2(take(stmt));
	return true;
}

char *wallet_offer_find(const tal_t *ctx,
			struct wallet *w,
			const struct sha256 *offer_id,
			const struct json_escape **label,
			enum offer_status *status)
{
	struct db_stmt *stmt;
	char *bolt12;

	stmt = db_prepare_v2(w->db, SQL("SELECT bolt12, label, status"
					"  FROM offers"
					" WHERE offer_id = ?;"));
	db_bind_sha256(stmt, 0, offer_id);
	db_query_prepared(stmt);

	if (!db_step(stmt)) {
		tal_free(stmt);
		return NULL;
	}

	bolt12 = tal_strdup(ctx, cast_signed(const char *, db_column_text(stmt, 0)));
	if (label) {
		if (db_column_is_null(stmt, 1))
			*label = NULL;
		else
			*label = db_column_json_escape(ctx, stmt, 1);
	}
	if (status)
		*status = offer_status_in_db(db_column_int(stmt, 2));
	tal_free(stmt);
	return bolt12;
}

struct db_stmt *wallet_offer_id_first(struct wallet *w, struct sha256 *offer_id)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(w->db, SQL("SELECT offer_id FROM offers;"));
	db_query_prepared(stmt);

	return wallet_offer_id_next(w, stmt, offer_id);
}

struct db_stmt *wallet_offer_id_next(struct wallet *w,
				     struct db_stmt *stmt,
				     struct sha256 *offer_id)
{
	if (!db_step(stmt))
		return tal_free(stmt);

	db_column_sha256(stmt, 0, offer_id);
	return stmt;
}

/* If we make an offer inactive, this also expires all invoices
 * which we issued for it. */
static void offer_status_update(struct db *db,
				const struct sha256 *offer_id,
				enum offer_status oldstatus,
				enum offer_status newstatus)
{
	struct db_stmt *stmt;

	stmt = db_prepare_v2(db, SQL("UPDATE offers"
				     " SET status=?"
				     " WHERE offer_id = ?;"));
	db_bind_int(stmt, 0, offer_status_in_db(newstatus));
	db_bind_sha256(stmt, 1, offer_id);
	db_exec_prepared_v2(take(stmt));

	if (!offer_status_active(oldstatus)
	    || offer_status_active(newstatus))
		return;

	stmt = db_prepare_v2(db, SQL("UPDATE invoices"
				     " SET state=?"
				     " WHERE state=? AND local_offer_id = ?;"));
	db_bind_int(stmt, 0, invoice_status_in_db(EXPIRED));
	db_bind_int(stmt, 1, invoice_status_in_db(UNPAID));
	db_bind_sha256(stmt, 2, offer_id);
	db_exec_prepared_v2(take(stmt));
}

enum offer_status wallet_offer_disable(struct wallet *w,
				       const struct sha256 *offer_id,
				       enum offer_status s)
{
	enum offer_status newstatus;

	assert(offer_status_active(s));

	newstatus = offer_status_in_db(s & ~OFFER_STATUS_ACTIVE_F);
	offer_status_update(w->db, offer_id, s, newstatus);

	return newstatus;
}

void wallet_offer_mark_used(struct db *db, const struct sha256 *offer_id)
{
	struct db_stmt *stmt;
	enum offer_status status;

	stmt = db_prepare_v2(db, SQL("SELECT status"
				     "  FROM offers"
				     " WHERE offer_id = ?;"));
	db_bind_sha256(stmt, 0, offer_id);
	db_query_prepared(stmt);
	if (!db_step(stmt))
		fatal("%s: unknown offer_id %s",
		      __func__,
		      type_to_string(tmpctx, struct sha256, offer_id));

	status = offer_status_in_db(db_column_int(stmt, 0));
	tal_free(stmt);

	if (!offer_status_active(status))
		fatal("%s: offer_id %s not active: status %i",
		      __func__,
		      type_to_string(tmpctx, struct sha256, offer_id),
		      status);

	if (!offer_status_used(status)) {
		enum offer_status newstatus;

		if (offer_status_single(status))
			newstatus = OFFER_SINGLE_USE_USED;
		else
			newstatus = OFFER_MULTIPLE_USE_USED;
		offer_status_update(db, offer_id, status, newstatus);
	}
}
