/**
 * @file savedstatus.c Saved Status API
 * @ingroup core
 *
 * gaim
 *
 * Gaim is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "internal.h"

#include "debug.h"
#include "notify.h"
#include "savedstatuses.h"
#include "status.h"
#include "util.h"
#include "xmlnode.h"

/*
 * TODO: Need to allow transient statuses to have empty titles.
 */

/**
 * The information of a snap-shot of the statuses of all
 * your accounts.  Basically these are your saved away messages.
 * There is an overall status and message that applies to
 * all your accounts, and then each individual account can
 * optionally have a different custom status and message.
 *
 * The changes to status.xml caused by the new status API
 * are fully backward compatible.  The new status API just
 * adds the optional sub-statuses to the XML file.
 */
struct _GaimSavedStatus
{
	/**
	 * A "transient" status is one that was used recently by
	 * a Gaim user, but was not explicitly created using the
	 * saved status UI.  For example, Gaim's previous status
	 * is saved in the status.xml file, but should not show
	 * up in the UI.
	 */
	gboolean transient;

	char *title;
	GaimStatusPrimitive type;
	char *message;

	GList *substatuses;      /**< A list of GaimSavedStatusSub's. */
};

/*
 * TODO: If a GaimStatusType is deleted, need to also delete any
 *       associated GaimSavedStatusSub's?
 */
struct _GaimSavedStatusSub
{
	GaimAccount *account;
	const GaimStatusType *type;
	char *message;
};

static GList   *saved_statuses = NULL;
static guint    save_timer = 0;
static gboolean statuses_loaded = FALSE;


/*********************************************************************
 * Private utility functions                                         *
 *********************************************************************/

static void
free_statussavedsub(GaimSavedStatusSub *substatus)
{
	g_return_if_fail(substatus != NULL);

	g_free(substatus->message);
	g_free(substatus);
}

static void
free_statussaved(GaimSavedStatus *status)
{
	g_return_if_fail(status != NULL);

	g_free(status->title);
	g_free(status->message);

	while (status->substatuses != NULL)
	{
		GaimSavedStatusSub *substatus = status->substatuses->data;
		status->substatuses = g_list_remove(status->substatuses, substatus);
		free_statussavedsub(substatus);
	}

	g_free(status);
}

/*********************************************************************
 * Writing to disk                                                   *
 *********************************************************************/

static xmlnode *
substatus_to_xmlnode(GaimSavedStatusSub *substatus)
{
	xmlnode *node, *child;

	node = xmlnode_new("substatus");

	child = xmlnode_new_child(node, "account");
	xmlnode_set_attrib(child, "protocol", gaim_account_get_protocol_id(substatus->account));
	xmlnode_insert_data(child, gaim_account_get_username(substatus->account), -1);

	child = xmlnode_new_child(node, "state");
	xmlnode_insert_data(child, gaim_status_type_get_id(substatus->type), -1);

	if (substatus->message != NULL)
	{
		child = xmlnode_new_child(node, "message");
		xmlnode_insert_data(child, substatus->message, -1);
	}

	return node;
}

static xmlnode *
status_to_xmlnode(GaimSavedStatus *status)
{
	xmlnode *node, *child;
	char transient[2];
	GList *cur;

	snprintf(transient, sizeof(transient), "%d", status->transient);

	node = xmlnode_new("status");
	xmlnode_set_attrib(node, "transient", transient);
	xmlnode_set_attrib(node, "name", status->title);

	child = xmlnode_new_child(node, "state");
	xmlnode_insert_data(child, gaim_primitive_get_id_from_type(status->type), -1);

	if (status->message != NULL)
	{
		child = xmlnode_new_child(node, "message");
		xmlnode_insert_data(child, status->message, -1);
	}

	for (cur = status->substatuses; cur != NULL; cur = cur->next)
	{
		child = substatus_to_xmlnode(cur->data);
		xmlnode_insert_child(node, child);
	}

	return node;
}

static xmlnode *
statuses_to_xmlnode(void)
{
	xmlnode *node, *child;
	GList *cur;

	node = xmlnode_new("statuses");
	xmlnode_set_attrib(node, "version", "1.0");

	for (cur = saved_statuses; cur != NULL; cur = cur->next)
	{
		child = status_to_xmlnode(cur->data);
		xmlnode_insert_child(node, child);
	}

	return node;
}

static void
sync_statuses(void)
{
	xmlnode *node;
	char *data;

	if (!statuses_loaded)
	{
		gaim_debug_error("status", "Attempted to save statuses before they "
						 "were read!\n");
		return;
	}

	node = statuses_to_xmlnode();
	data = xmlnode_to_formatted_str(node, NULL);
	gaim_util_write_data_to_file("status.xml", data, -1);
	g_free(data);
	xmlnode_free(node);
}

static gboolean
save_cb(gpointer data)
{
	sync_statuses();
	save_timer = 0;
	return FALSE;
}

static void
schedule_save(void)
{
	if (save_timer == 0)
		save_timer = gaim_timeout_add(5000, save_cb, NULL);
}


/*********************************************************************
 * Reading from disk                                                 *
 *********************************************************************/

static GaimSavedStatusSub *
parse_substatus(xmlnode *substatus)
{
	GaimSavedStatusSub *ret;
	xmlnode *node;
	char *data;

	ret = g_new0(GaimSavedStatusSub, 1);

	/* Read the account */
	node = xmlnode_get_child(substatus, "account");
	if (node != NULL)
	{
		char *acct_name;
		const char *protocol;
		acct_name = xmlnode_get_data(node);
		protocol = xmlnode_get_attrib(node, "protocol");
		if ((acct_name != NULL) && (protocol != NULL))
			ret->account = gaim_accounts_find(acct_name, protocol);
		g_free(acct_name);
	}

	if (ret->account == NULL)
	{
		g_free(ret);
		return NULL;
	}

	/* Read the state */
	node = xmlnode_get_child(substatus, "state");
	if ((node != NULL) && ((data = xmlnode_get_data(node)) != NULL))
	{
		ret->type = gaim_status_type_find_with_id(
							ret->account->status_types, data);
		g_free(data);
	}

	/* Read the message */
	node = xmlnode_get_child(substatus, "message");
	if ((node != NULL) && ((data = xmlnode_get_data(node)) != NULL))
	{
		ret->message = data;
	}

	return ret;
}

/**
 * Parse a saved status and add it to the saved_statuses linked list.
 *
 * Here's an example of the XML for a saved status:
 *   <status name="Girls">
 *       <state>away</state>
 *       <message>I like the way that they walk
 *   And it's chill to hear them talk
 *   And I can always make them smile
 *   From White Castle to the Nile</message>
 *       <substatus>
 *           <account protocol='prpl-oscar'>markdoliner</account>
 *           <state>available</state>
 *           <message>The ladies man is here to answer your queries.</message>
 *       </substatus>
 *       <substatus>
 *           <account protocol='prpl-oscar'>giantgraypanda</account>
 *           <state>away</state>
 *           <message>A.C. ain't in charge no more.</message>
 *       </substatus>
 *   </status>
 *
 * I know.  Moving, huh?
 */
static GaimSavedStatus *
parse_status(xmlnode *status)
{
	GaimSavedStatus *ret;
	xmlnode *node;
	const char *attrib;
	char *data;
	int i;

	ret = g_new0(GaimSavedStatus, 1);

	/* Read the transient property */
	attrib = xmlnode_get_attrib(status, "transient");
	if ((attrib != NULL) && (attrib[0] == '1'))
		ret->transient = TRUE;

	/* Read the title */
	attrib = xmlnode_get_attrib(status, "name");
	if (attrib == NULL)
		attrib = "No Title";
	/* Ensure the title is unique */
	ret->title = g_strdup(attrib);
	i = 2;
	while (gaim_savedstatus_find(ret->title) != NULL)
	{
		g_free(ret->title);
		ret->title = g_strdup_printf("%s %d", attrib, i);
		i++;
	}

	/* Read the primitive status type */
	node = xmlnode_get_child(status, "state");
	if ((node != NULL) && ((data = xmlnode_get_data(node)) != NULL))
	{
		ret->type = gaim_primitive_get_type_from_id(data);
		g_free(data);
	}

	/* Read the message */
	node = xmlnode_get_child(status, "message");
	if ((node != NULL) && ((data = xmlnode_get_data(node)) != NULL))
	{
		ret->message = data;
	}

	/* Read substatuses */
	for (node = xmlnode_get_child(status, "substatus"); node != NULL;
			node = xmlnode_get_next_twin(node))
	{
		GaimSavedStatusSub *new;
		new = parse_substatus(node);
		if (new != NULL)
			ret->substatuses = g_list_prepend(ret->substatuses, new);
	}

	return ret;
}

/**
 * Read the saved statuses from a file in the Gaim user dir.
 *
 * @return TRUE on success, FALSE on failure (if the file can not
 *         be opened, or if it contains invalid XML).
 */
static void
load_statuses(void)
{
	xmlnode *statuses, *status;

	statuses_loaded = TRUE;

	statuses = gaim_util_read_xml_from_file("status.xml", _("saved statuses"));

	if (statuses == NULL)
		return;

	for (status = xmlnode_get_child(statuses, "status"); status != NULL;
			status = xmlnode_get_next_twin(status))
	{
		GaimSavedStatus *new;
		new = parse_status(status);
		saved_statuses = g_list_prepend(saved_statuses, new);
	}

	xmlnode_free(statuses);
}


/**************************************************************************
* Saved status API
**************************************************************************/
GaimSavedStatus *
gaim_savedstatus_new(const char *title, GaimStatusPrimitive type)
{
	GaimSavedStatus *status;

	/* Make sure we don't already have a saved status with this title. */
	g_return_val_if_fail(gaim_savedstatus_find(title) == NULL, NULL);

	status = g_new0(GaimSavedStatus, 1);
	status->title = g_strdup(title);
	status->type = type;

	saved_statuses = g_list_prepend(saved_statuses, status);

	schedule_save();

	return status;
}

void
gaim_savedstatus_set_title(GaimSavedStatus *status, const char *title)
{
	g_return_if_fail(status != NULL);

	/* Make sure we don't already have a saved status with this title. */
	g_return_if_fail(gaim_savedstatus_find(title) == NULL);

	g_free(status->title);
	status->title = g_strdup(title);

	schedule_save();
}

void
gaim_savedstatus_set_type(GaimSavedStatus *status, GaimStatusPrimitive type)
{
	g_return_if_fail(status != NULL);

	status->type = type;

	schedule_save();
}

void
gaim_savedstatus_set_message(GaimSavedStatus *status, const char *message)
{
	g_return_if_fail(status != NULL);

	g_free(status->message);
	status->message = g_strdup(message);

	schedule_save();
}

void
gaim_savedstatus_set_substatus(GaimSavedStatus *saved_status,
							   const GaimAccount *account,
							   const GaimStatusType *type,
							   const char *message)
{
	GaimSavedStatusSub *substatus;

	g_return_if_fail(saved_status != NULL);
	g_return_if_fail(account      != NULL);
	g_return_if_fail(type         != NULL);

	/* Find an existing substatus or create a new one */
	substatus = gaim_savedstatus_get_substatus(saved_status, account);
	if (substatus == NULL)
	{
		substatus = g_new0(GaimSavedStatusSub, 1);
		substatus->account = (GaimAccount *)account;
		saved_status->substatuses = g_list_prepend(saved_status->substatuses, substatus);
	}

	substatus->type = type;
	g_free(substatus->message);
	substatus->message = g_strdup(message);

	schedule_save();
}

void
gaim_savedstatus_unset_substatus(GaimSavedStatus *saved_status,
								 const GaimAccount *account)
{
	GList *iter;
	GaimSavedStatusSub *substatus;

	g_return_if_fail(saved_status != NULL);
	g_return_if_fail(account      != NULL);

	for (iter = saved_status->substatuses; iter != NULL; iter = iter->next)
	{
		substatus = iter->data;
		if (substatus->account == account)
		{
			saved_status->substatuses = g_list_delete_link(saved_status->substatuses, iter);
			g_free(substatus->message);
			g_free(substatus);
			return;
		}
	}
}

gboolean
gaim_savedstatus_delete(const char *title)
{
	GaimSavedStatus *status;

	status = gaim_savedstatus_find(title);

	if (status == NULL)
		return FALSE;

	saved_statuses = g_list_remove(saved_statuses, status);
	free_statussaved(status);

	schedule_save();

	return TRUE;
}

const GList *
gaim_savedstatuses_get_all(void)
{
	return saved_statuses;
}

GaimSavedStatus *
gaim_savedstatus_find(const char *title)
{
	GList *iter;
	GaimSavedStatus *status;

	g_return_val_if_fail(title != NULL, NULL);

	for (iter = saved_statuses; iter != NULL; iter = iter->next)
	{
		status = (GaimSavedStatus *)iter->data;
		if (!strcmp(status->title, title))
			return status;
	}

	return NULL;
}

gboolean
gaim_savedstatus_is_transient(const GaimSavedStatus *saved_status)
{
	return saved_status->transient;
}

const char *
gaim_savedstatus_get_title(const GaimSavedStatus *saved_status)
{
	return saved_status->title;
}

GaimStatusPrimitive
gaim_savedstatus_get_type(const GaimSavedStatus *saved_status)
{
	return saved_status->type;
}

const char *
gaim_savedstatus_get_message(const GaimSavedStatus *saved_status)
{
	return saved_status->message;
}

gboolean
gaim_savedstatus_has_substatuses(const GaimSavedStatus *saved_status)
{
	return (saved_status->substatuses != NULL);
}

GaimSavedStatusSub *
gaim_savedstatus_get_substatus(const GaimSavedStatus *saved_status,
							   const GaimAccount *account)
{
	GList *iter;
	GaimSavedStatusSub *substatus;

	g_return_val_if_fail(saved_status != NULL, NULL);
	g_return_val_if_fail(account      != NULL, NULL);

	for (iter = saved_status->substatuses; iter != NULL; iter = iter->next)
	{
		substatus = iter->data;
		if (substatus->account == account)
			return substatus;
	}

	return NULL;
}

const GaimStatusType *
gaim_savedstatus_substatus_get_type(const GaimSavedStatusSub *substatus)
{
	g_return_val_if_fail(substatus != NULL, NULL);

	return substatus->type;
}

const char *
gaim_savedstatus_substatus_get_message(const GaimSavedStatusSub *substatus)
{
	g_return_val_if_fail(substatus != NULL, NULL);

	return substatus->message;
}

void
gaim_savedstatus_activate(const GaimSavedStatus *saved_status)
{
	GList *accounts, *node;

	g_return_if_fail(saved_status != NULL);

	accounts = gaim_accounts_get_all_active();

	for (node = accounts; node != NULL; node = node->next)
	{
		GaimAccount *account;

		account = node->data;
		gaim_savedstatus_activate_for_account(saved_status, account);
	}

	g_list_free(accounts);

	gaim_prefs_set_string("/core/status/current",
						  gaim_savedstatus_get_title(saved_status));
}

void
gaim_savedstatus_activate_for_account(const GaimSavedStatus *saved_status,
									  GaimAccount *account)
{
	const GaimStatusType *status_type;
	const GaimSavedStatusSub *substatus;
	const char *message = NULL;

	g_return_if_fail(saved_status != NULL);
	g_return_if_fail(account != NULL);

	substatus = gaim_savedstatus_get_substatus(saved_status, account);
	if (substatus != NULL)
	{
		status_type = substatus->type;
		message = substatus->message;
	}
	else
	{
		status_type = gaim_account_get_status_type_with_primitive(account, saved_status->type);
		if (status_type == NULL)
			return;
		message = saved_status->message;
	}

	if ((message != NULL) &&
		(gaim_status_type_get_attr(status_type, "message")))
	{
		gaim_account_set_status(account, gaim_status_type_get_id(status_type),
								TRUE, "message", message, NULL);
	}
	else
	{
		gaim_account_set_status(account, gaim_status_type_get_id(status_type),
								TRUE, NULL);
	}
}

void *
gaim_savedstatuses_get_handle(void)
{
	static int handle;

	return &handle;
}

void
gaim_savedstatuses_init(void)
{
	load_statuses();

	if (saved_statuses == NULL)
	{
		/*
		 * We don't have any saved statuses!  This is probably a new account,
		 * so we add the "Default" status and the "Default when idle" status.
		 */
		GaimSavedStatus *saved_status;

		saved_status = gaim_savedstatus_new(_("Default"), GAIM_STATUS_AVAILABLE);
		gaim_savedstatus_set_message(saved_status, _("Hello!"));

		saved_status = gaim_savedstatus_new(_("Default when idle"), GAIM_STATUS_AWAY);
		gaim_savedstatus_set_message(saved_status, _("I'm not here right now"));
	}
}

void
gaim_savedstatuses_uninit(void)
{
	if (save_timer != 0)
	{
		gaim_timeout_remove(save_timer);
		save_timer = 0;
		sync_statuses();
	}

	while (saved_statuses != NULL) {
		GaimSavedStatus *saved_status = saved_statuses->data;
		saved_statuses = g_list_remove(saved_statuses, saved_status);
		free_statussaved(saved_status);
	}
}

