/* Code to be notified when various standardized events happen. */
#include <ccan/array_size/array_size.h>
#include <common/json_command.h>
#include <common/overflows.h>
#include <common/param.h>
#include <lightningd/jsonrpc.h>
#include <lightningd/lightningd.h>
#include <lightningd/wait.h>

struct waiter {
	struct list_node list;
	struct command *cmd;
	/* These are pointers because of how param_ works */
	enum wait_subsystem *subsystem;
	enum wait_index *index;
	u64 *nextval;
};


static const char *subsystem_names[] = {
	"invoices",
};

static const char *index_names[] = {
	"created_index",
	"updated_index",
	"deleted_index",
};

u64 *index_get(struct lightningd *ld,
	       enum wait_subsystem subsystem, enum wait_index index)
{
	struct indexes *indexes;

	assert(subsystem < ARRAY_SIZE(ld->indexes));
	indexes = &ld->indexes[subsystem];

	assert(index < ARRAY_SIZE(indexes->i));

	return &indexes->i[index];
}

static void json_add_index(struct json_stream *response,
			   enum wait_subsystem subsystem,
			   enum wait_index index,
			   u64 val,
			   va_list *ap)
{
	const char *name, *value;
	json_add_string(response, "subsystem",subsystem_names[subsystem]);
	json_add_u64(response, index_names[index], val);

	if (!ap)
		return;

	json_object_start(response, "details");
	while ((name = va_arg(*ap, const char *)) != NULL) {
		value = va_arg(*ap, const char *);
		if (!value)
			continue;

		/* This is a hack! */
		if (name[0] == '=')
			json_add_literal(response, name + 1, value, strlen(value));
		else
			json_add_string(response, name, value);
	}
	json_object_end(response);
}

u64 index_update(struct lightningd *ld,
		 enum wait_subsystem subsystem,
		 enum wait_index index,
		 u64 increase,
		 ...)
{
	struct waiter *i, *n;
	va_list ap;
	u64 *idxval = index_get(ld, subsystem, index);

	assert(!add_overflows_u64(*idxval, increase));
	*idxval += increase;

	list_for_each_safe(&ld->wait_commands, i, n, list) {
		struct json_stream *response;

		if (*i->subsystem != subsystem)
			continue;
		if (*i->index != index)
			continue;
		if (*idxval < *i->nextval)
			continue;

		response = json_stream_success(i->cmd);
		va_start(ap, increase);
		json_add_index(response, subsystem, index, *idxval, &ap);
		va_end(ap);
		/* Delete before freeing */
		list_del_from(&ld->wait_commands, &i->list);
		was_pending(command_success(i->cmd, response));
	}

	return *idxval;
}

static struct command_result *param_subsystem(struct command *cmd,
					      const char *name,
					      const char *buffer,
					      const jsmntok_t *tok,
					      enum wait_subsystem **subsystem)
{
	for (size_t i = 0; i < ARRAY_SIZE(subsystem_names); i++) {
		if (json_tok_streq(buffer, tok, subsystem_names[i])) {
			*subsystem = tal(cmd, enum wait_subsystem);
			**subsystem = i;
			return NULL;
		}
	}

	return command_fail_badparam(cmd, name, buffer, tok,
				     "unknown subsystem");
}

struct command_result *param_index(struct command *cmd,
				   const char *name,
				   const char *buffer,
				   const jsmntok_t *tok,
				   enum wait_index **index)
{
	for (size_t i = 0; i < ARRAY_SIZE(index_names); i++) {
		if (json_tok_streq(buffer, tok, index_names[i])) {
			*index = tal(cmd, enum wait_index);
			**index = i;
			return NULL;
		}
	}

	return command_fail_badparam(cmd, name, buffer, tok,
				     "unknown index");
}

static struct command_result *json_wait(struct command *cmd,
					const char *buffer,
					const jsmntok_t *obj UNNEEDED,
					const jsmntok_t *params)
{
	struct waiter *waiter = tal(cmd, struct waiter);
	u64 val;

	if (!param(cmd, buffer, params,
		   p_req("subsystem", param_subsystem,
			 &waiter->subsystem),
		   p_req("indexname", param_index, &waiter->index),
		   p_req("nextvalue", param_u64, &waiter->nextval),
		   NULL))
		return command_param_failed();

	/* Are we there already?  Return immediately. */
	val = *index_get(cmd->ld, *waiter->subsystem, *waiter->index);
	if (val >= *waiter->nextval) {
		struct json_stream *response;

		response = json_stream_success(cmd);
		json_add_index(response,
			       *waiter->subsystem,
			       *waiter->index,
			       val, NULL);
		return command_success(cmd, response);
	}

	waiter->cmd = cmd;
	list_add_tail(&cmd->ld->wait_commands, &waiter->list);
	return command_still_pending(cmd);
}

static const struct json_command wait_command = {
	"wait",
	"utility",
	json_wait,
	"Wait for {subsystem} {indexname} to reach or exceed {value})"
};
AUTODATA(json_command, &wait_command);
