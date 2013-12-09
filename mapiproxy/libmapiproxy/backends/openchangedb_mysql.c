#include "openchangedb_mysql.h"

#include "mapiproxy/dcesrv_mapiproxy.h"
#include "mapiproxy/libmapistore/mapistore.h"
#include "../libmapiproxy.h"
#include "libmapi/libmapi.h"
#include "libmapi/libmapi_private.h"
#include <mysql/mysql.h>
#include <samba_util.h>
#include <inttypes.h>
#include <time.h>

#define SCHEMA_FILE "openchangedb_schema.sql"
#define PUBLIC_FOLDER "public"
#define SYSTEM_FOLDER "system"

#define THRESHOLD_SLOW_QUERIES 0.25

#define TRANSPORT_FOLDER_NAME "Outbox"
#define TRANSPORT_FOLDER_LOCALE "en_US"

// We only used one connection
// FIXME assumption that every user will use the same mysql server. Change
// into list of connections indexed by username, like indexing backend.
static MYSQL *conn = NULL;

static enum MAPISTATUS _not_implemented(const char *caller) {
	DEBUG(0, ("Called not implemented function `%s` from mysql backend",
		  caller));
	return MAPI_E_NOT_IMPLEMENTED;
}
#define not_implemented() _not_implemented(__PRETTY_FUNCTION__)


float timespec_diff_in_seconds(struct timespec *end, struct timespec *start)
{
	return ((float)((end->tv_sec * 1000000000 + end->tv_nsec) -
			(start->tv_sec * 1000000000 + start->tv_nsec)))
		/ 1000000000;
}

static enum MAPISTATUS execute_query(MYSQL *conn, const char *sql)
{
	struct timespec start, end;
	float seconds_spent;

	clock_gettime(CLOCK_MONOTONIC, &start);
	if (mysql_query(conn, sql) != 0) {
		printf("Error on query `%s`: %s\n", sql, mysql_error(conn));
		DEBUG(5, ("Error on query `%s`: %s", sql, mysql_error(conn)));
		return MAPI_E_CALL_FAILED;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	seconds_spent = timespec_diff_in_seconds(&end, &start);
	if (seconds_spent > THRESHOLD_SLOW_QUERIES) {
		printf("Openchangedb mysql backend slow query!\n"
		       "\tQuery: `%s`\n\tTime: %.3f\n", sql, seconds_spent);
		DEBUG(5, ("Openchangedb mysql backend slow query!\n"
			  "\tQuery: `%s`\n\tTime: %.3f\n", sql, seconds_spent));
	}
	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS select_without_fetch(MYSQL *conn, const char *sql,
					    MYSQL_RES **res)
{
	enum MAPISTATUS ret;

	ret = execute_query(conn, sql);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, NULL);

	*res = mysql_store_result(conn);
	if (*res == NULL) {
		DEBUG(0, ("Error getting results of `%s`: %s", sql,
			  mysql_error(conn)));
		return MAPI_E_CALL_FAILED;
	}

	if (mysql_num_rows(*res) == 0) {
		mysql_free_result(*res);
		return MAPI_E_NOT_FOUND;
	}

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS select_all_strings(TALLOC_CTX *mem_ctx, MYSQL *conn,
					  const char *sql,
					  struct StringArrayW_r **_results)
{
	MYSQL_RES *res;
	struct StringArrayW_r *results;
	uint32_t i;
	enum MAPISTATUS ret;

	ret = select_without_fetch(conn, sql, &res);
	if (ret == MAPI_E_NOT_FOUND) {
		results = talloc_zero(mem_ctx, struct StringArrayW_r);
		results->cValues = 0;
	} else if (ret == MAPI_E_SUCCESS) {
		results = talloc_zero(mem_ctx, struct StringArrayW_r);
		results->cValues = mysql_num_rows(res);
	} else {
		// Unexpected error on sql query
		return ret;
	}

	results->lppszW = talloc_zero_array(results, const char *,
					    results->cValues);

	for (i = 0; i < results->cValues; i++) {
		MYSQL_ROW row = mysql_fetch_row(res);
		if (row == NULL) {
			DEBUG(0, ("Error getting row %d of `%s`: %s", i, sql,
				  mysql_error(conn)));
			mysql_free_result(res);
			return MAPI_E_CALL_FAILED;
		}
		results->lppszW[i] = talloc_strdup(results, row[0]);
	}

	mysql_free_result(res);
	*_results = results;

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS select_first_string(TALLOC_CTX *mem_ctx, MYSQL *conn,
					   const char *sql, const char **s)
{
	MYSQL_RES *res;
	enum MAPISTATUS ret;

	ret = select_without_fetch(conn, sql, &res);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, NULL);

	MYSQL_ROW row = mysql_fetch_row(res);
	if (row == NULL) {
		DEBUG(0, ("Error getting row of `%s`: %s", sql,
			  mysql_error(conn)));
		return MAPI_E_CALL_FAILED;
	}

	*s = talloc_strdup(mem_ctx, row[0]);
	mysql_free_result(res);

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS select_first_uint(MYSQL *conn, const char *sql,
					 uint64_t *n)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "select_first_uint");
	const char *result;
	enum MAPISTATUS ret;

	ret = select_first_string(mem_ctx, conn, sql, &result);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	*n = strtoul(result, NULL, 10);
	talloc_free(mem_ctx);

	return MAPI_E_SUCCESS;
}

// v openchangedb -------------------------------------------------------------

static enum MAPISTATUS get_SystemFolderID(struct openchangedb_context *self,
					  const char *recipient,
					  uint32_t SystemIdx,
					  uint64_t *FolderId)
{
	char *sql;
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_SystemFolderId");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;

	if (SystemIdx == 0x1) {
		// FIXME ou_id
		sql = talloc_asprintf(mem_ctx,
			"SELECT folder_id FROM mailboxes WHERE name = '%s'",
			recipient);
	} else {
		// FIXME ou_id
		sql = talloc_asprintf(mem_ctx,
			"SELECT f.folder_id FROM folders f JOIN mailboxes m ON "
			"f.mailbox_id = m.id AND m.name = '%s' "
			"WHERE f.SystemIdx = %"PRIu32" AND f.folder_class = '%s'",
			recipient, SystemIdx, SYSTEM_FOLDER);
	}

	ret = select_first_uint(conn, sql, FolderId);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	talloc_free(mem_ctx);

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS get_PublicFolderID(struct openchangedb_context *self,
					  uint32_t SystemIdx,
					  uint64_t *FolderId)
{
	char *sql;
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_PublicFolderID");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;

	// FIXME ou_id
	sql = talloc_asprintf(mem_ctx,
		"SELECT f.folder_id FROM folders f WHERE f.SystemIdx = %"PRIu32
		" AND f.folder_class = '%s'", SystemIdx, PUBLIC_FOLDER);

	ret = select_first_uint(conn, sql, FolderId);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	talloc_free(mem_ctx);

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS get_distinguishedName(TALLOC_CTX *parent_ctx,
					     struct openchangedb_context *self,
					     uint64_t fid,
					     char **distinguishedName)
{
	return not_implemented();
}

static enum MAPISTATUS get_MailboxGuid(struct openchangedb_context *self,
				       const char *recipient,
				       struct GUID *MailboxGUID)
{
	char *sql;
	const char *guid;
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_PublicFolderID");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;

	// FIXME ou_id
	sql = talloc_asprintf(mem_ctx,
		"SELECT MailboxGUID FROM mailboxes WHERE name = '%s'",
		recipient);

	ret = select_first_string(mem_ctx, conn, sql, &guid);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	GUID_from_string(guid, MailboxGUID);

	talloc_free(mem_ctx);

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS get_MailboxReplica(struct openchangedb_context *self,
					  const char *recipient,
					  uint16_t *ReplID,
					  struct GUID *ReplGUID)
{
	char *sql;
	const char *guid;
	uint64_t n;
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_PublicFolderID");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;

	if (ReplID) {
		// FIXME ou_id
		sql = talloc_asprintf(mem_ctx,
			"SELECT ReplicaID FROM mailboxes WHERE name = '%s'",
			recipient);

		ret = select_first_uint(conn, sql, &n);
		OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

		*ReplID = (uint16_t) n;
	}

	if (ReplGUID) {
		// FIXME ou_id
		sql = talloc_asprintf(mem_ctx,
			"SELECT ReplicaGUID FROM mailboxes WHERE name = '%s'",
			recipient);

		ret = select_first_string(mem_ctx, conn, sql, &guid);
		OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

		GUID_from_string(guid, ReplGUID);
	}

	talloc_free(mem_ctx);

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS get_PublicFolderReplica(struct openchangedb_context *self,
					       uint16_t *ReplID,
					       struct GUID *ReplGUID)
{
	char *sql;
	const char *guid;
	uint64_t n;
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_PublicFolderReplica");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;

	if (ReplID) {
		// FIXME ou_id
		sql = talloc_asprintf(mem_ctx,
			"SELECT ReplicaID FROM public_folders");

		ret = select_first_uint(conn, sql, &n);
		OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

		*ReplID = (uint16_t) n;
	}

	if (ReplGUID) {
		// FIXME ou_id
		sql = talloc_asprintf(mem_ctx,
			"SELECT StoreGUID FROM public_folders");

		ret = select_first_string(mem_ctx, conn, sql, &guid);
		OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

		GUID_from_string(guid, ReplGUID);
	}

	talloc_free(mem_ctx);

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS get_mapistoreURI(TALLOC_CTX *parent_ctx,
				        struct openchangedb_context *self,
				        const char *username, uint64_t fid,
				        char **mapistoreURL, bool mailboxstore)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_mapistoreURI");
	enum MAPISTATUS ret;
	char *sql;
	MYSQL *conn = self->data;

	if (!mailboxstore) { // FIXME is it possible?
		return _not_implemented("get_mapistoreURI with mailboxstore=false");
	}

	sql = talloc_asprintf(mem_ctx,
		"SELECT MAPIStoreURI FROM folders f "
		"JOIN mailboxes m ON m.id = f.mailbox_id AND m.name = '%s' "
		"WHERE f.folder_id = %"PRIu64, username, fid);

	ret = select_first_string(parent_ctx, conn, sql,
				  (const char **)mapistoreURL);
	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS set_mapistoreURI(struct openchangedb_context *self,
					const char *username, uint64_t fid,
					const char *mapistoreURL)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "set_mapistoreURI");
	enum MAPISTATUS ret;
	char *sql;
	MYSQL *conn = self->data;
	// FIXME ou_id
	sql = talloc_asprintf(mem_ctx,
		"UPDATE folders f "
		"JOIN mailboxes m ON m.id = f.mailbox_id AND m.name = '%s' "
		"SET f.MAPIStoreURI = '%s' "
		"WHERE f.folder_id = %"PRIu64, username, mapistoreURL, fid);
	ret = execute_query(conn, sql);
	if (mysql_affected_rows(conn) == 0) {
		ret = MAPI_E_NOT_FOUND;
	}
	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS get_parent_fid(struct openchangedb_context *self,
				      const char *username, uint64_t fid,
				      uint64_t *parent_fidp, bool mailboxstore)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_parent_fid");
	enum MAPISTATUS ret;
	char *sql;
	MYSQL *conn = self->data;

	if (mailboxstore) {
		sql = talloc_asprintf(mem_ctx, // FIXME ou_id
			"SELECT f1.folder_id FROM folders f1 "
			"JOIN folders f2 ON f1.id = f2.parent_folder_id"
			"  AND f2.folder_id = %"PRIu64" "
			"JOIN mailboxes m ON m.id = f2.mailbox_id"
			"  AND m.name = '%s'",
			fid, username);
	} else {
		sql = talloc_asprintf(mem_ctx, //FIXME ou_id
			"SELECT f1.folder_id FROM folders f1 "
			"JOIN folders f2 ON f1.id = f2.parent_folder_id"
			"  AND f2.folder_id = %"PRIu64" "
			"WHERE f1.folder_class = '%s'", fid, PUBLIC_FOLDER);
	}

	ret = select_first_uint(conn, sql, parent_fidp);
	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS get_fid(struct openchangedb_context *self,
			       const char *mapistore_uri, uint64_t *fidp)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_fid");
	enum MAPISTATUS ret;
	MYSQL *conn = self->data;
	char *sql, *mapistore_uri_2;

	mapistore_uri_2 = talloc_strdup(mem_ctx, mapistore_uri);
	if (mapistore_uri_2[strlen(mapistore_uri_2)-1] == '/') {
		mapistore_uri_2[strlen(mapistore_uri_2)-1] = '\0';
	} else {
		mapistore_uri_2 = talloc_asprintf(mem_ctx, "%s/",
						  mapistore_uri_2);
	}

	sql = talloc_asprintf(mem_ctx,
		"SELECT folder_id FROM folders "
		"WHERE MAPIStoreURI = '%s' OR MAPIStoreURI = '%s'",
		mapistore_uri, mapistore_uri_2);

	ret = select_first_uint(conn, sql, fidp);
	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS get_MAPIStoreURIs(struct openchangedb_context *self,
					 const char *username,
					 TALLOC_CTX *mem_ctx,
					 struct StringArrayW_r **urisP)
{
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;
	char *sql;

	// TODO ou_id
	sql = talloc_asprintf(mem_ctx,
		"SELECT MAPIStoreURI FROM folders f JOIN mailboxes m "
		"ON f.mailbox_id = m.id AND m.name = '%s' "
		"WHERE MAPIStoreURI IS NOT NULL", username);

	ret = select_all_strings(mem_ctx, conn, sql, urisP);

	talloc_free(sql);
	return ret;
}

static enum MAPISTATUS get_ReceiveFolder(TALLOC_CTX *mem_ctx,
					 struct openchangedb_context *self,
					 const char *recipient,
					 const char *MessageClass,
					 uint64_t *fid,
					 const char **ExplicitMessageClass)
{
	TALLOC_CTX *local_mem_ctx = talloc_named(NULL, 0, "get_ReceiveFolder");
	MYSQL *conn = self->data;
	char *sql, *explicit = NULL;
	enum MAPISTATUS ret;
	MYSQL_RES *res;
	my_ulonglong nrows, i;
	size_t length;

	// Check PidTagMessageClass from mailbox and folders
	// TODO ou_id
	sql = talloc_asprintf(local_mem_ctx,
		"SELECT fp.value, f.folder_id FROM folders f "
		"JOIN mailboxes m ON m.id = f.mailbox_id AND m.name = '%s' "
		"JOIN folders_properties fp ON fp.folder_id = f.id AND"
		"     fp.name = 'PidTagMessageClass' "
		"UNION "
		"SELECT mp.value, m2.folder_id FROM mailboxes m2 "
		"JOIN mailboxes_properties mp ON mp.mailbox_id = m2.id AND"
		"     mp.name = 'PidTagMessageClass' "
		"WHERE m2.name = '%s'", recipient, recipient);

	ret = select_without_fetch(conn, sql, &res);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, local_mem_ctx);

	nrows = mysql_num_rows(res);
	for (i = 0, length = 0; i < nrows; i++) {
		MYSQL_ROW row = mysql_fetch_row(res);
		const char *message_class;
		size_t message_class_length;
		if (row == NULL) {
			DEBUG(0, ("Error getting row %d of `%s`: %s", (int) i,
				  sql, mysql_error(conn)));
			mysql_free_result(res);
			return MAPI_E_CALL_FAILED;
		}
		message_class = row[0];
		message_class_length = strlen(message_class);
		if (!strncasecmp(MessageClass, message_class, message_class_length) &&
		    message_class_length > length) {
			if (explicit && strcmp(explicit, "")) {
				talloc_free(explicit);
			}
			if (MessageClass && !strcmp(MessageClass, "All")) {
				explicit = "";
			} else {
				explicit = talloc_strdup(mem_ctx, message_class);
			}
			*ExplicitMessageClass = explicit;
			*fid = strtoul(row[1], NULL, 10);
			length = message_class_length;
		}
	}

	mysql_free_result(res);

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS get_TransportFolder(struct openchangedb_context *self,
					   const char *recipient,
					   uint64_t *FolderId)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_TransportFolder");
	MYSQL *conn = self->data;
	char *sql;
	enum MAPISTATUS ret;

	// FIXME ou_id
	sql = talloc_asprintf(mem_ctx,
		"SELECT f.folder_id FROM folders f "
		"JOIN mailboxes m ON f.mailbox_id = m.id AND m.name = '%s' "
		"JOIN folders_names n ON n.folder_id = f.id"
		" AND n.locale = '%s' AND n.display_name = '%s'",
		recipient, TRANSPORT_FOLDER_LOCALE, TRANSPORT_FOLDER_NAME);

	ret = select_first_uint(conn, sql, FolderId);
	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS get_mailbox_ids_by_name(MYSQL *conn,
					       const char *username,
					       uint64_t *mailbox_id,
					       uint64_t *mailbox_folder_id)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_mailbox_ids_by_name");
	enum MAPISTATUS ret;
	char * sql;
	MYSQL_RES *res;
	MYSQL_ROW row;

	sql = talloc_asprintf(mem_ctx, // FIXME ou_id
		"SELECT m.id, m.folder_id FROM mailboxes m WHERE m.name = '%s'",
		username);
	ret = select_without_fetch(conn, sql, &res);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);
	row = mysql_fetch_row(res);
	if (row == NULL) {
		DEBUG(0, ("Error getting user's mailbox `%s`: %s", sql,
			  mysql_error(conn)));
		ret = MAPI_E_NOT_FOUND;
		goto end;
	}

	*mailbox_id = strtoul(row[0], NULL, 10);
	*mailbox_folder_id = strtoul(row[1], NULL, 10);
end:
	mysql_free_result(res);
	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS get_folder_count(struct openchangedb_context *self,
					const char *username, uint64_t fid,
					uint32_t *RowCount)
{
	// FIXME always a folder from a mailbox?
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_folder_count");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;
	char *sql;
	uint64_t count = 0, mailbox_id = 0, mailbox_folder_id = 0;

	// The fid could be from either the mailbox or a folder from user's
	// mailbox
	ret = get_mailbox_ids_by_name(conn, username,
				      &mailbox_id, &mailbox_folder_id);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	if (mailbox_folder_id == fid) {
		sql = talloc_asprintf(mem_ctx,
			"SELECT count(f.id) FROM folders f "
			"WHERE f.mailbox_id = %"PRIu64" "
			"  AND f.parent_folder_id IS NULL",
			mailbox_id);
	} else {
		sql = talloc_asprintf(mem_ctx,
			"SELECT count(f1.id) FROM folders f1 "
			"JOIN folders f2 ON f2.id = f1.parent_folder_id"
			"  AND f2.folder_id = %"PRIu64" "
			"WHERE f1.mailbox_id = %"PRIu64,
			fid, mailbox_id);
	}

	ret = select_first_uint(conn, sql, &count);
	*RowCount = count;

	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS lookup_folder_property(struct openchangedb_context *self,
					      uint32_t proptag, uint64_t fid)
{
	return not_implemented();
}

/**
 * Get the current change number field
 * TODO ou_id
 */
static enum MAPISTATUS get_server_change_number(MYSQL *conn, uint64_t *change_number)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_server_change_number");
	char *sql;
	enum MAPISTATUS ret;

	sql = talloc_asprintf(mem_ctx,
		"SELECT change_number FROM servers s "
		"JOIN company c ON c.id = s.company_id "
		"JOIN organizational_units ou ON ou.company_id = c.id "
		//FIXME restrictions
		);
	ret = select_first_uint(conn, sql, change_number);
	talloc_free(mem_ctx);
	return ret;
}

/**
 * Set the current change number field
 * TODO ou_id
 */
static enum MAPISTATUS set_server_change_number(MYSQL *conn, uint64_t change_number)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "set_server_change_number");
	char *sql;
	enum MAPISTATUS ret;

	sql = talloc_asprintf(mem_ctx,
		"UPDATE servers s "
		"JOIN company c ON c.id = s.company_id "
		"JOIN organizational_units ou ON ou.company_id = c.id "
		"SET s.change_number=%"PRIu64""
		//FIXME ou_id
		, change_number);
	ret = execute_query(conn, sql);
	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS get_new_changeNumber(struct openchangedb_context *self,
					    uint64_t *cn)
{
	enum MAPISTATUS ret;
	MYSQL *conn = self->data;

	ret = get_server_change_number(conn, cn); // TODO ou_id
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, NULL);

	ret = set_server_change_number(conn, (*cn) + 1); // TODO ou_id
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, NULL);

	// Transform the number the way exchange protocol likes it
	*cn = (exchange_globcnt(*cn) << 16) | 0x0001;

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS get_new_changeNumbers(struct openchangedb_context *self,
					     TALLOC_CTX *mem_ctx, uint64_t max,
					     struct UI8Array_r **cns_p)
{
	return not_implemented();
}

static enum MAPISTATUS get_next_changeNumber(struct openchangedb_context *self,
					     uint64_t *cn)
{
	enum MAPISTATUS ret;
	MYSQL *conn = self->data;

	ret = get_server_change_number(conn, cn);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, NULL);

	// Transform the number the way exchange protocol likes it
	*cn = (exchange_globcnt(*cn) << 16) | 0x0001;

	return MAPI_E_SUCCESS;
}

static char *_unknown_property(TALLOC_CTX *mem_ctx, uint32_t proptag)
{
	return talloc_asprintf(mem_ctx, "Unknown%.8x", proptag);
}

static struct BinaryArray_r *decode_mv_binary(TALLOC_CTX *mem_ctx, const char *str)
{
	const char *start;
	char *tmp;
	size_t i, current, len;
	uint32_t j;
	struct BinaryArray_r *bin_array;

	bin_array = talloc_zero(mem_ctx, struct BinaryArray_r);

	start = str;
	len = strlen(str);
	i = 0;
	while (i < len && start[i] != ';') {
		i++;
	}
	if (i < len) {
		tmp = talloc_memdup(NULL, start, i + 1);
		tmp[i] = 0;
		bin_array->cValues = strtol(tmp, NULL, 16);
		bin_array->lpbin = talloc_array(bin_array, struct Binary_r, bin_array->cValues);
		talloc_free(tmp);

		i++;
		for (j = 0; j < bin_array->cValues; j++) {
			current = i;
			while (i < len && start[i] != ';') {
				i++;
			}

			tmp = talloc_memdup(bin_array, start + current, i - current + 1);
			tmp[i - current] = 0;
			i++;

			if (tmp[0] != 0 && strcmp(tmp, nil_string) != 0) {
				bin_array->lpbin[j].lpb = (uint8_t *) tmp;
				bin_array->lpbin[j].cb = ldb_base64_decode((char *) bin_array->lpbin[j].lpb);
			}
			else {
				bin_array->lpbin[j].lpb = talloc_zero(bin_array, uint8_t);
				bin_array->lpbin[j].cb = 0;
			}
		}
	}

	return bin_array;
}

static struct LongArray_r *decode_mv_long(TALLOC_CTX *mem_ctx, const char *str)
{
	const char *start;
	char *tmp;
	size_t i, current, len;
	uint32_t j;
	struct LongArray_r *long_array;

	long_array = talloc_zero(mem_ctx, struct LongArray_r);

	start = str;
	len = strlen(str);
	i = 0;
	while (i < len && start[i] != ';') {
		i++;
	}
	if (i < len) {
		tmp = talloc_memdup(NULL, start, i + 1);
		tmp[i] = 0;
		long_array->cValues = strtol(tmp, NULL, 16);
		long_array->lpl = talloc_array(long_array, uint32_t, long_array->cValues);
		talloc_free(tmp);

		i++;
		for (j = 0; j < long_array->cValues; j++) {
			current = i;
			while (i < len && start[i] != ';') {
				i++;
			}

			tmp = talloc_memdup(long_array, start + current, i - current + 1);
			tmp[i - current] = 0;
			i++;

			long_array->lpl[j] = strtol(tmp, NULL, 16);
			talloc_free(tmp);
		}
	}

	return long_array;
}

static void *_get_special_property(TALLOC_CTX *mem_ctx, uint32_t proptag)
{
	uint32_t *l;

	switch (proptag) {
	case PR_DEPTH:
		l = talloc_zero(mem_ctx, uint32_t);
		*l = 0;
		return (void *)l;
	}

	return NULL;
}

/**
   \details Retrieve a MAPI property from an OpenChange LDB message

   \param mem_ctx pointer to the memory context
   \param msg pointer to the LDB message
   \param proptag the MAPI property tag to lookup
   \param PidTagAttr the mapped MAPI property name

   \return valid data pointer on success, otherwise NULL
 */
static void *get_property_data(TALLOC_CTX *mem_ctx, uint32_t proptag, const char *value)
{
	void			*data;
	uint64_t		*ll;
	uint32_t		*l;
	int			*b;
	struct FILETIME		*ft;
	struct Binary_r		*bin;
	struct BinaryArray_r	*bin_array;
	struct LongArray_r	*long_array;

	switch (proptag & 0xFFFF) {
	case PT_BOOLEAN:
		b = talloc_zero(mem_ctx, int);
		if (strlen(value) == 4 && strncasecmp(value, "TRUE", 4) == 0) {
			*b = 1;
		}
		data = (void *)b;
		break;
	case PT_LONG:
		l = talloc_zero(mem_ctx, uint32_t);
		*l = strtoul(value, NULL, 10);
		data = (void *)l;
		break;
	case PT_I8:
		ll = talloc_zero(mem_ctx, uint64_t);
		*ll = strtoul(value, NULL, 10);
		data = (void *)ll;
		break;
	case PT_STRING8:
	case PT_UNICODE:
		data = (void *)talloc_strdup(mem_ctx, value);
		break;
	case PT_SYSTIME:
		ft = talloc_zero(mem_ctx, struct FILETIME);
		ll = talloc_zero(mem_ctx, uint64_t);
		*ll = strtoul(value, NULL, 10);
		ft->dwLowDateTime = (*ll & 0xffffffff);
		ft->dwHighDateTime = *ll >> 32;
		data = (void *)ft;
		talloc_free(ll);
		break;
	case PT_BINARY:
		bin = talloc_zero(mem_ctx, struct Binary_r);
		if (strcmp(value, nil_string) == 0) {
			bin->lpb = (uint8_t *) talloc_zero(mem_ctx, uint8_t);
			bin->cb = 0;
		} else {
			bin->lpb = (uint8_t *) talloc_strdup(mem_ctx, value);
			bin->cb = ldb_base64_decode((char *) bin->lpb);
		}
		data = (void *)bin;
		break;
	case PT_MV_BINARY:
		bin_array = decode_mv_binary(mem_ctx, value);
		data = (void *)bin_array;
		break;
	case PT_MV_LONG:
		long_array = decode_mv_long(mem_ctx, value);
		data = (void *)long_array;
		break;
	default:
		DEBUG(0, ("[%s:%d] Property Type 0x%.4x not supported\n", __FUNCTION__, __LINE__, (proptag & 0xFFFF)));
		abort();
		return NULL;
	}

	return data;
}

static enum MAPISTATUS get_folder_property(TALLOC_CTX *parent_ctx,
					   struct openchangedb_context *self,
					   const char *username,
					   uint32_t proptag, uint64_t fid,
					   void **data)
{
	// FIXME always a folder from a mailbox?
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_folder_property");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;
	char *sql;
	uint64_t mailbox_id, mailbox_folder_id;
	uint64_t *n;
	const char *attr, *value;

	ret = get_mailbox_ids_by_name(conn, username,
				      &mailbox_id, &mailbox_folder_id);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	attr = openchangedb_property_get_attribute(proptag);
	if (!attr) {
		attr = _unknown_property(parent_ctx, proptag);
	}

	*data = _get_special_property(parent_ctx, proptag);
	if (*data != NULL) goto end;

	if (proptag == PidTagFolderId) {
		n = talloc_zero(parent_ctx, uint64_t);
		*n = fid;
		*data = (void *) n;
		goto end;
	}

	if (mailbox_folder_id == fid) {
		if (proptag == PidTagDisplayName) {
			// FIXME i18n
			*data = talloc_asprintf(parent_ctx,
						"OpenChange Mailbox: %s",
						username);
			goto end;
		}

		sql = talloc_asprintf(mem_ctx,
			"SELECT mp.value FROM mailboxes_properties mp "
			"WHERE mp.mailbox_id = %"PRIu64" AND mp.name = '%s'",
			mailbox_id, attr);
	} else {
		if (proptag == PidTagParentFolderId) {
			n = talloc_zero(parent_ctx, uint64_t);
			ret = get_parent_fid(self, username, fid, n, true);
			OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);
			*data = (void *) n;
			goto end;
		}

		sql = talloc_asprintf(mem_ctx,
			"SELECT fp.value FROM folders_properties fp "
			"JOIN folders f ON f.id = fp.folder_id "
			"  AND f.mailbox_id = %"PRIu64" "
			"  AND f.folder_id = %"PRIu64" "
			"WHERE fp.name = '%s'",
			mailbox_id, fid, attr);
	}

	ret = select_first_string(mem_ctx, conn, sql, &value);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);
	// Transform string into the expected data type
	*data = get_property_data(parent_ctx, proptag, value);
end:
	talloc_free(mem_ctx);
	return MAPI_E_SUCCESS;
}

static char *str_list_join_for_sql(TALLOC_CTX *mem_ctx, const char **list)
{
	char *ret = NULL;
	int i;

	if (list[0] == NULL) {
		return talloc_strdup(mem_ctx, "");
	}

	ret = talloc_asprintf(mem_ctx, "'%s'", list[0]);

	for (i = 1; list[i]; i++) {
		ret = talloc_asprintf_append_buffer(ret, ",'%s'", list[i]);
	}

	return ret;
}

static enum MAPISTATUS set_folder_properties(struct openchangedb_context *self,
					     const char *username, uint64_t fid,
					     struct SRow *row)
{
	// FIXME always a folder from a mailbox?
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_folder_property");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;
	char *sql;
	uint64_t mailbox_id, mailbox_folder_id, id;
	uint32_t i;
	const char *attr;
	struct SPropValue *value;
	enum MAPITAGS tag;
	const char **names, **values;
	char *table, *str_value, *column_id, *names_for_sql, *values_for_sql;
	time_t unix_time = time(NULL);
	NTTIME nt_time;

	ret = get_mailbox_ids_by_name(conn, username,
				      &mailbox_id, &mailbox_folder_id);
	if (mailbox_folder_id == fid) {
		// Updating mailbox properties
		table = talloc_strdup(mem_ctx, "mailboxes_properties");
		column_id = talloc_strdup(mem_ctx, "mailbox_id");
		id = fid;
	} else {
		// Updating folder
		table = talloc_strdup(mem_ctx, "folders_properties");
		column_id = talloc_strdup(mem_ctx, "folder_id");
		sql = talloc_asprintf(mem_ctx,
			"SELECT f.id FROM folders f "
			"WHERE f.folder_id = %"PRIu64" "
			"  AND f.mailbox_id = %"PRIu64, fid, mailbox_id);
		ret = select_first_uint(conn, sql, &id);
		OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);
	}

	names = (const char **)str_list_make_empty(mem_ctx);
	values = (const char **)str_list_make_empty(mem_ctx);
	for (i = 0; i < row->cValues; i++) {
		value = row->lpProps + i;
		tag = value->ulPropTag;
		if (tag == PR_DEPTH || tag == PR_SOURCE_KEY ||
		    tag == PR_PARENT_SOURCE_KEY || tag == PR_CREATION_TIME ||
		    tag == PR_LAST_MODIFICATION_TIME) {
			DEBUG(5, ("Ignored attempt to set handled property %.8x\n",
				  tag));
			continue;
		}
		attr = openchangedb_property_get_attribute(tag);
		if (!attr) {
			attr = _unknown_property(mem_ctx, tag);
		}

		str_value = openchangedb_set_folder_property_data(mem_ctx, value);
		if (!str_value) {
			DEBUG(5, ("Ignored property of unhandled type %.4x\n",
				  (tag & 0xffff)));
			continue;
		}

		names = str_list_add(names, attr);
		values = str_list_add(values, str_value);
	}

	// Add last modification
	value = talloc_zero(mem_ctx, struct SPropValue);
	value->ulPropTag = PR_LAST_MODIFICATION_TIME;
	unix_to_nt_time(&nt_time, unix_time);
	value->value.ft.dwLowDateTime = nt_time & 0xffffffff;
	value->value.ft.dwHighDateTime = nt_time >> 32;
	attr = openchangedb_property_get_attribute(value->ulPropTag);
	str_value = openchangedb_set_folder_property_data(mem_ctx, value);
	names = str_list_add(names, attr);
	values = str_list_add(values, str_value);

	// Add change number
	value->ulPropTag = PidTagChangeNumber;
	get_new_changeNumber(self, (uint64_t *) &value->value.d);
	attr = openchangedb_property_get_attribute(value->ulPropTag);
	str_value = openchangedb_set_folder_property_data(mem_ctx, value);
	names = str_list_add(names, attr);
	values = str_list_add(values, str_value);

	// Delete previous values
	names_for_sql = str_list_join_for_sql(mem_ctx, names);
	sql = talloc_asprintf(mem_ctx,
		"DELETE FROM %s WHERE %s = %"PRIu64" AND name IN (%s)",
		table, column_id, id, names_for_sql);
	ret = execute_query(conn, sql);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	// Insert new values
	values_for_sql = talloc_asprintf(mem_ctx, "(%"PRIu64", '%s', '%s')",
					 id, names[0], values[0]);
	for (i = 1; names[i]; i++) {
		values_for_sql = talloc_asprintf_append_buffer(values_for_sql,
			",(%"PRIu64", '%s', '%s')", id, names[i], values[i]);
	}
	sql = talloc_asprintf(mem_ctx, "INSERT INTO %s VALUES %s",
			      table, values_for_sql);
	ret = execute_query(conn, sql);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	talloc_free(mem_ctx);
	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS get_table_property(TALLOC_CTX *parent_ctx,
					  struct openchangedb_context *self,
					  const char *ldb_filter,
					  uint32_t proptag, uint32_t pos,
					  void **data)
{
	return not_implemented();
}

static enum MAPISTATUS get_fid_by_name(struct openchangedb_context *self,
				       const char *username,
				       uint64_t parent_fid,
				       const char *foldername, uint64_t *fid)
{
	// FIXME public folders
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_fid_by_name");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;
	char *sql;
	uint64_t mailbox_id, mailbox_folder_id;

	ret = get_mailbox_ids_by_name(conn, username,
				      &mailbox_id, &mailbox_folder_id);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	// FIXME i18n
	if (mailbox_folder_id == parent_fid) {
		// The parent folder is the mailbox itself
		sql = talloc_asprintf(mem_ctx,
			"SELECT f.folder_id FROM folders f "
			"JOIN folders_names fn ON fn.folder_id = f.id"
			"  AND fn.locale = 'en_US'"
			"  AND fn.display_name = '%s' "
			"WHERE f.mailbox_id = %"PRIu64,
			foldername, mailbox_id);
	} else {
		sql = talloc_asprintf(mem_ctx,
			"SELECT f1.folder_id FROM folders f1 "
			"JOIN folders_names fn ON fn.folder_id = f1.id"
			"  AND fn.locale = 'en_US'"
			"  AND fn.display_name = '%s' "
			"JOIN folders f2 ON f2.id = f1.parent_folder_id"
			" AND f2.folder_id = %"PRIu64,
			foldername, parent_fid);
	}

	ret = select_first_uint(conn, sql, fid);

	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS get_mid_by_subject_from_public_folder(struct openchangedb_context *self,
							     const char *username,
					  	  	     uint64_t parent_fid,
					  	  	     const char *subject,
					  	  	     uint64_t *mid)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0,
		"get_mid_by_subject_from_public_folder");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;
	char *sql;

	// Parent folder is a public folder
	sql = talloc_asprintf(mem_ctx, //FIXME ou_id
		"SELECT m.message_id FROM messages m "
		"JOIN folders f1 ON f1.id = m.folder_id"
		"  AND f1.folder_class = '%s'"
		"  AND f1.folder_id = %"PRIu64" "
		"WHERE m.normalized_subject = '%s'",
		PUBLIC_FOLDER, parent_fid, subject);

	ret = select_first_uint(conn, sql, mid);

	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS get_mid_by_subject_from_system_folder(struct openchangedb_context *self,
							     const char *username,
					  	  	     uint64_t parent_fid,
					  	  	     const char *subject,
					  	  	     uint64_t *mid)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0,
		"get_mid_by_subject_from_system_folder");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;
	char *sql;
	uint64_t mailbox_id, mailbox_folder_id;

	ret = get_mailbox_ids_by_name(conn, username,
				      &mailbox_id, &mailbox_folder_id);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	if (mailbox_folder_id == parent_fid) {
		// The parent folder is the mailbox itself
		sql = talloc_asprintf(mem_ctx,
			"SELECT m.message_id FROM messages m "
			"WHERE m.mailbox_id = %"PRIu64
			"  AND m.normalized_subject = '%s'",
			mailbox_id, subject);
	} else {
		// Parent folder is a system folder
		sql = talloc_asprintf(mem_ctx,
			"SELECT m.message_id FROM messages m "
			"JOIN folders f1 ON f1.id = m.folder_id "
			"  AND f1.folder_class = '%s'"
			"  AND f1.folder_id = %"PRIu64" "
			"  AND f1.mailbox_id = %"PRIu64" "
			"WHERE m.normalized_subject = '%s'",
			SYSTEM_FOLDER, parent_fid, mailbox_id, subject);
	}

	ret = select_first_uint(conn, sql, mid);

	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS get_mid_by_subject(struct openchangedb_context *self,
					  const char *username,
					  uint64_t parent_fid,
					  const char *subject,
					  bool mailboxstore, uint64_t *mid)
{
	if (mailboxstore) {
		return get_mid_by_subject_from_system_folder(self, username,
							     parent_fid,
							     subject, mid);
	} else {
		return get_mid_by_subject_from_public_folder(self, username,
							     parent_fid,
							     subject, mid);
	}
}

static enum MAPISTATUS delete_folder(struct openchangedb_context *self,
				     const char *username,
				     uint64_t fid)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "delete_folder");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;
	char *sql;

	// Parent folder is a public folder
	sql = talloc_asprintf(mem_ctx, //FIXME ou_id
		"DELETE f FROM folders f "
		"JOIN mailboxes m ON m.id = f.mailbox_id"
		"  AND m.name = '%s' "
		"WHERE f.folder_id = %"PRIu64,
		username, fid);

	ret = execute_query(conn, sql);

	talloc_free(mem_ctx);
	return ret;
}

static enum MAPISTATUS set_ReceiveFolder(struct openchangedb_context *self,
					 const char *recipient,
					 const char *MessageClass, uint64_t fid)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "set_ReceiveFolder");
	char *sql;
	enum MAPISTATUS ret;

	// Delete current receive folder for that message class if exists
	sql = talloc_asprintf(mem_ctx,
		"DELETE fp FROM folders_properties fp "
		"JOIN folders f ON f.id = fp.folder_id "
		"JOIN mailboxes m ON m.id = f.mailbox_id AND m.name = '%s' "
		"WHERE fp.name = 'PidTagMessageClass' AND fp.value = '%s'",
		recipient, MessageClass); //TODO ou_id
	ret = execute_query(conn, sql);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	// Create PidTagMessageClass folder property for the fid specified
	sql = talloc_asprintf(mem_ctx,
		"INSERT INTO folders_properties VALUES ("
		" ("
		"  SELECT f.id FROM folders f "
		"  JOIN mailboxes m ON m.id = f.mailbox_id AND m.name = '%s' "
		"  WHERE f.folder_id = %"PRIu64
		" ), 'PidTagMessageClass', '%s')",
		recipient, fid, MessageClass); //TODO ou_id
	ret = execute_query(conn, sql);
	OPENCHANGE_RETVAL_IF(ret != MAPI_E_SUCCESS, ret, mem_ctx);

	talloc_free(mem_ctx);

	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS get_fid_from_partial_uri(struct openchangedb_context *self,
						const char *partialURI,
						uint64_t *fid)
{
	return not_implemented();
}

static enum MAPISTATUS get_users_from_partial_uri(TALLOC_CTX *parent_ctx,
						  struct openchangedb_context *self,
						  const char *partialURI,
						  uint32_t *count,
						  char ***MAPIStoreURI,
						  char ***users)
{
	return not_implemented();
}

static enum MAPISTATUS create_mailbox(struct openchangedb_context *self,
				      const char *username, int systemIdx,
				      uint64_t fid)
{
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS create_folder(struct openchangedb_context *self,
				     uint64_t parentFolderID, uint64_t fid,
				     uint64_t changeNumber,
				     const char *MAPIStoreURI, int systemIdx)
{//TODO NEEDS USER
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS get_message_count(struct openchangedb_context *self,
					 uint64_t fid, uint32_t *RowCount,
					 bool fai)
{//TODO NEEDS USER
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS get_system_idx(struct openchangedb_context *self,
				      const char *username, uint64_t fid,
				      int *system_idx_p)
{
	TALLOC_CTX *mem_ctx = talloc_named(NULL, 0, "get_system_idx");
	MYSQL *conn = self->data;
	enum MAPISTATUS ret;
	char *sql;
	uint64_t system_idx = 0;

	sql = talloc_asprintf(mem_ctx, //FIXME ou_id
		"SELECT f.SystemIdx FROM folders f "
		"JOIN mailboxes m ON m.id = f.mailbox_id AND m.name = '%s' "
		"WHERE f.folder_id = %"PRIu64,
		username, fid);

	ret = select_first_uint(conn, sql, &system_idx);
	*system_idx_p = (int)system_idx;

	return ret;
}

static enum MAPISTATUS transaction_start(struct openchangedb_context *self)
{
	MYSQL *conn = self->data;
	int res = mysql_query(conn, "START TRANSACTION");
	OPENCHANGE_RETVAL_IF(res, MAPI_E_CALL_FAILED, NULL);
	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS transaction_rollback(struct openchangedb_context *self)
{
	MYSQL *conn = self->data;
	int res = mysql_query(conn, "ROLLBACK");
	OPENCHANGE_RETVAL_IF(res, MAPI_E_CALL_FAILED, NULL);
	return MAPI_E_SUCCESS;
}

static enum MAPISTATUS transaction_commit(struct openchangedb_context *self)
{
	MYSQL *conn = self->data;
	int res = mysql_query(conn, "COMMIT");
	OPENCHANGE_RETVAL_IF(res, MAPI_E_CALL_FAILED, NULL);
	return MAPI_E_SUCCESS;
}

// ^ openchangedb -------------------------------------------------------------

// v openchangedb table -------------------------------------------------------
// TODO types
struct openchangedb_table {
	uint64_t			folderID;
	uint8_t				table_type;
	struct SSortOrderSet		*lpSortCriteria;
	struct mapi_SRestriction	*restrictions;
	struct ldb_result		*res;
};


static enum MAPISTATUS table_init(TALLOC_CTX *mem_ctx,
				  struct openchangedb_context *self,
				  uint8_t table_type, uint64_t folderID,
				  void **table_object)
{//TODO NEEDS USER
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS table_set_sort_order(struct openchangedb_context *self,
					    void *table_object,
					    struct SSortOrderSet *lpSortCriteria)
{
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS table_set_restrictions(struct openchangedb_context *self,
					      void *table_object,
					      struct mapi_SRestriction *res)
{
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS table_get_property(TALLOC_CTX *mem_ctx,
					  struct openchangedb_context *self,
					  void *table_object,
					  enum MAPITAGS proptag, uint32_t pos,
					  bool live_filtered, void **data)
{
	return MAPI_E_NOT_IMPLEMENTED;
}

// ^ openchangedb table -------------------------------------------------------

// v openchangedb message -----------------------------------------------------

// TODO types
enum openchangedb_message_type {
	OPENCHANGEDB_MESSAGE_SYSTEM	= 0x1,
	OPENCHANGEDB_MESSAGE_FAI	= 0x2
};

struct openchangedb_message_properties {
	char		**names;
	char 		**values;
	uint64_t	size;
};

struct openchangedb_message {
	uint64_t				id;
	uint64_t				message_id;
	enum openchangedb_message_type		message_type;
	uint64_t				folder_id;
	uint64_t				mailbox_id;
	char					*normalized_subject;
	struct openchangedb_message_properties	properties;
};

static enum MAPISTATUS message_create(TALLOC_CTX *mem_ctx,
				      struct openchangedb_context *self,
				      uint64_t message_id, uint64_t folder_id,
				      bool fai, void **message_object)
{ // TODO
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS message_save(struct openchangedb_context *self,
				    void *_msg, uint8_t save_flags)
{ // TODO
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS message_open(TALLOC_CTX *mem_ctx,
				    struct openchangedb_context *self,
				    uint64_t messageID, uint64_t folderID,
				    void **message_object, void **msgp)
{
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS message_get_property(TALLOC_CTX *mem_ctx,
					    struct openchangedb_context *self,
					    void *message_object,
					    uint32_t proptag, void **data)
{
	// PidTagParentFolderId -> folder_id
	// PidTagMessageId -> message_id
	// PidTagNormalizedSubject -> normalized_subject
	return MAPI_E_NOT_IMPLEMENTED;
}

static enum MAPISTATUS message_set_properties(TALLOC_CTX *mem_ctx,
					      struct openchangedb_context *self,
					      void *message_object,
					      struct SRow *row)
{
	return MAPI_E_NOT_IMPLEMENTED;
}

// ^ openchangedb message -----------------------------------------------------

static const char *openchangedb_data_dir(void)
{
	return OPENCHANGEDB_DATA_DIR; // defined on compilation time
}

static bool parse_connection_string(TALLOC_CTX *mem_ctx,
				   const char *connection_string,
				   char **host, char **user, char **passwd,
				   char **db)
{
	// connection_string has format mysql://user[:pass]@host/database
	int prefix_size = strlen("mysql://");
	const char *s = connection_string + prefix_size;
	if (!connection_string || strlen(connection_string) < prefix_size ||
	    !strstr(connection_string, "mysql://") || !strchr(s, '@') ||
	    !strchr(s, '/')) {
		// Invalid format
		return false;
	}
	if (strchr(s, ':') == NULL || strchr(s, ':') > strchr(s, '@')) {
		// No password
		int user_size = strchr(s, '@') - s;
		*user = talloc_zero_array(mem_ctx, char, user_size + 1);
		strncpy(*user, s, user_size);
		(*user)[user_size] = '\0';
		*passwd = talloc_zero_array(mem_ctx, char, 1);
		(*passwd)[0] = '\0';
	} else {
		// User
		int user_size = strchr(s, ':') - s;
		*user = talloc_zero_array(mem_ctx, char, user_size);
		strncpy(*user, s, user_size);
		(*user)[user_size] = '\0';
		// Password
		int passwd_size = strchr(s, '@') - strchr(s, ':') - 1;
		*passwd = talloc_zero_array(mem_ctx, char, passwd_size + 1);
		strncpy(*passwd, strchr(s, ':') + 1, passwd_size);
		(*passwd)[passwd_size] = '\0';
	}
	// Host
	int host_size = strchr(s, '/') - strchr(s, '@') - 1;
	*host = talloc_zero_array(mem_ctx, char, host_size + 1);
	strncpy(*host, strchr(s, '@') + 1, host_size);
	(*host)[host_size] = '\0';
	// Database name
	int db_size = strlen(strchr(s, '/') + 1);
	*db = talloc_zero_array(mem_ctx, char, db_size + 1);
	strncpy(*db, strchr(s, '/') + 1, db_size);
	(*db)[db_size] = '\0';

	return true;
}

static bool is_schema_created(MYSQL *conn)
{
	MYSQL_RES *res;
	bool created;

	res = mysql_list_tables(conn, "folders");
	if (res == NULL) return false;
	created = mysql_num_rows(res) == 1;
	mysql_free_result(res);

	return created;
}

static bool create_schema(MYSQL *conn)
{
	TALLOC_CTX *mem_ctx;
	FILE *f;
	int sql_size, bytes_read;
	char *schema, *schema_file, *query;
	bool ret, queries_to_execute;

	mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	schema_file = talloc_asprintf(mem_ctx, "%s/"SCHEMA_FILE,
				      openchangedb_data_dir());
	f = fopen(schema_file, "r");
	if (!f) {
		DEBUG(0, ("schema file %s not found", schema_file));
		ret = false;
		goto end;
	}
	fseek(f, 0, SEEK_END);
	sql_size = ftell(f);
	rewind(f);
	schema = talloc_zero_array(mem_ctx, char, sql_size + 1);
	bytes_read = fread(schema, sizeof(char), sql_size, f);
	if (bytes_read != sql_size) {
		DEBUG(0, ("error reading schema file %s", schema_file));
		ret = false;
		goto end;
	}
	// schema is a series of create table/index queries separated by ';'
	query = strtok (schema, ";");
	queries_to_execute = query != NULL;
	while (queries_to_execute) {
		ret = mysql_query(conn, query) ? false : true;
		if (!ret) break;
		query = strtok(NULL, ";");
		queries_to_execute = ret && query && strlen(query) > 10;
	}
end:
	talloc_free(mem_ctx);
	if (f) fclose(f);

	return ret;
}

static MYSQL *create_connection(const char *connection_string)
{
	TALLOC_CTX *mem_ctx;
	my_bool reconnect;
	char *host, *user, *passwd, *db, *sql;
	bool parsed;

	if (conn != NULL) return conn;

	mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	conn = mysql_init(NULL);
	reconnect = true;
	mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);
	parsed = parse_connection_string(mem_ctx, connection_string,
					 &host, &user, &passwd, &db);
	if (!parsed) {
		DEBUG(0, ("Wrong connection string to mysql %s", connection_string));
		conn = NULL;
		goto end;
	}
	// First try to connect to the database, if it fails try to create it
	if (mysql_real_connect(conn, host, user, passwd, db, 0, NULL, 0)) {
		goto end;
	}

	// Try to create database
	if (!mysql_real_connect(conn, host, user, passwd, NULL, 0, NULL, 0)) {
		// Nop
		DEBUG(0, ("Can't connect to mysql using %s", connection_string));
		conn = NULL;
	} else {
		// Connect it!, let's try to create database
		sql = talloc_asprintf(mem_ctx, "CREATE DATABASE %s", db);
		if (mysql_query(conn, sql) != 0 || mysql_select_db(conn, db) != 0) {
			DEBUG(0, ("Can't connect to mysql using %s",
				  connection_string));
			conn = NULL;
		}
	}
end:
	talloc_free(mem_ctx);
	return conn;

}

_PUBLIC_ enum MAPISTATUS openchangedb_mysql_initialize(TALLOC_CTX *mem_ctx,
					 	       const char *connection_string,
						       struct openchangedb_context **ctx)
{
	struct openchangedb_context *oc_ctx;

	oc_ctx = talloc_zero(mem_ctx, struct openchangedb_context);
	// Initialize context with function pointers
	oc_ctx->backend_type = talloc_strdup(oc_ctx, "mysql");

	oc_ctx->get_new_changeNumber = get_new_changeNumber;
	oc_ctx->get_new_changeNumbers = get_new_changeNumbers;
	oc_ctx->get_next_changeNumber = get_next_changeNumber;
	oc_ctx->get_SystemFolderID = get_SystemFolderID;
	oc_ctx->get_PublicFolderID = get_PublicFolderID;
	oc_ctx->get_distinguishedName = get_distinguishedName;
	oc_ctx->get_MailboxGuid = get_MailboxGuid;
	oc_ctx->get_MailboxReplica = get_MailboxReplica;
	oc_ctx->get_PublicFolderReplica = get_PublicFolderReplica;
	oc_ctx->get_parent_fid = get_parent_fid;
	oc_ctx->get_MAPIStoreURIs = get_MAPIStoreURIs;
	oc_ctx->get_mapistoreURI = get_mapistoreURI;
	oc_ctx->set_mapistoreURI = set_mapistoreURI;
	oc_ctx->get_fid = get_fid;
	oc_ctx->get_ReceiveFolder = get_ReceiveFolder;
	oc_ctx->get_TransportFolder = get_TransportFolder;
	oc_ctx->lookup_folder_property = lookup_folder_property;
	oc_ctx->set_folder_properties = set_folder_properties;
	oc_ctx->get_folder_property = get_folder_property;
	oc_ctx->get_folder_count = get_folder_count;
	oc_ctx->get_message_count = get_message_count;
	oc_ctx->get_system_idx = get_system_idx;
	oc_ctx->get_table_property = get_table_property;
	oc_ctx->get_fid_by_name = get_fid_by_name;
	oc_ctx->get_mid_by_subject = get_mid_by_subject;
	oc_ctx->set_ReceiveFolder = set_ReceiveFolder;
	oc_ctx->create_mailbox = create_mailbox;
	oc_ctx->create_folder = create_folder;
	oc_ctx->delete_folder = delete_folder;
	oc_ctx->get_fid_from_partial_uri = get_fid_from_partial_uri;
	oc_ctx->get_users_from_partial_uri = get_users_from_partial_uri;

	oc_ctx->table_init = table_init;
	oc_ctx->table_set_sort_order = table_set_sort_order;
	oc_ctx->table_set_restrictions = table_set_restrictions;
	oc_ctx->table_get_property = table_get_property;

	oc_ctx->message_create = message_create;
	oc_ctx->message_save = message_save;
	oc_ctx->message_open = message_open;
	oc_ctx->message_get_property = message_get_property;
	oc_ctx->message_set_properties = message_set_properties;

	oc_ctx->transaction_start = transaction_start;
	oc_ctx->transaction_commit = transaction_commit;

	// Connect to mysql
	oc_ctx->data = create_connection(connection_string);
	OPENCHANGE_RETVAL_IF(!oc_ctx->data, MAPI_E_NOT_INITIALIZED, oc_ctx);
	if (!is_schema_created(oc_ctx->data)) {
		DEBUG(0, ("Creating schema for openchangedb on mysql %s",
			  connection_string));
		bool schema_created = create_schema(oc_ctx->data);
		OPENCHANGE_RETVAL_IF(!schema_created, MAPI_E_NOT_INITIALIZED, oc_ctx);
	}

	*ctx = oc_ctx;

	return MAPI_E_SUCCESS;
}
