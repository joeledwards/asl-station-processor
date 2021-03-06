#pragma ident "$Id: datlen.c,v 1.5 2008/01/15 19:41:17 dechavez Exp $"
/*======================================================================
 *
 * Lookup packet length from sint table.
 *
 *====================================================================*/
#include "isi/db.h"

static BOOL FlatFileLookup(DBIO *db, ISI_STREAM_NAME *name, int *datlen)
{
IDAFF_SINTMAP smap;

    strlcpy(smap.sta, name->sta, ISI_STALEN+1);
    strlcpy(smap.chn, name->chnloc, ISI_CHNLOCLEN+1);
    if (!idaffLookupSintmap(db->ff, &smap)) {
        *datlen = 0;
        errno = ENOENT;
        return FALSE;
    }

    *datlen = smap.datlen;

    return TRUE;
}

static BOOL MysqlLookup(DBIO *db, ISI_STREAM_NAME *name, int *datlen)
{
#ifdef HAVE_MYSQL
char query[1024];
MYSQL_RES *result;
MYSQL_ROW row;
static char *fid = "isidbLookupDatlen:MysqlLookup";

/* Look first for a station specific entry */

    query[0] = 0;
    strcat(query, "SELECT datlen FROM sint ");
    sprintf(query + strlen(query), "WHERE sta=\"%s\" ", name->sta);
    sprintf(query + strlen(query), "AND chan=\"%s\" ", name->chn);
    sprintf(query + strlen(query), "AND loc=\"%s\" ", name->loc);

    if (!dbioExecuteQuery(db, query)) return FALSE;
    result = mysql_store_result(&db->mysql);

/* There should be at most 1 row.  We don't check for duplicates, leaving that
 * to some exernal database integrity checker and just take the first entry.
 */

    if ((row = mysql_fetch_row(result)) != NULL) {
        *datlen = atoi(row[0]);
        mysql_free_result(result);
        return TRUE;
    }

/* If we didn't find a match for specific station name, try again with wild card */

    query[0] = 0;
    strcat(query, "SELECT datlen FROM sint ");
    sprintf(query + strlen(query), "WHERE sta IS NULL ");
    sprintf(query + strlen(query), "AND chan=\"%s\" ", name->chn);
    sprintf(query + strlen(query), "AND loc=\"%s\" ", name->loc);

    if (!dbioExecuteQuery(db, query)) return FALSE;
    result = mysql_store_result(&db->mysql);

/* Again, take the first row, if any */

    if ((row = mysql_fetch_row(result)) != NULL) {
        *datlen = atoi(row[0]);
        mysql_free_result(result);
        return TRUE;
    }

    mysql_free_result(result);
#endif

    return FALSE;
}

BOOL isidbLookupDatlen(DBIO *db, ISI_STREAM_NAME *name, int *datlen)
{
LOGIO *lp;
static char *fid = "isidbLookupDatlen";

    if (db == NULL || name == NULL || datlen == NULL) {
        lp = (db == NULL) ? NULL : db->lp;
        logioMsg(lp, LOG_DEBUG, "%s: NULL input(s)", fid);
        errno = EINVAL;
        return FALSE;
    }

    if (db->engine != DBIO_MYSQL) {
        return FlatFileLookup(db, name, datlen);
    } else {
        return MysqlLookup(db, name, datlen);
    }
}

/* Revision History
 *
 * $Log: datlen.c,v $
 * Revision 1.5  2008/01/15 19:41:17  dechavez
 * fixed error in building channel name for comparison
 *
 * Revision 1.4  2007/01/11 21:59:25  dechavez
 * use new isidl and/or isidb function prefixes
 *
 * Revision 1.3  2007/01/08 16:00:51  dechavez
 * switch to size-bounded string operations
 *
 * Revision 1.2  2006/04/20 18:55:11  dechavez
 * support builds witout MYSQL support
 *
 * Revision 1.1  2006/02/09 00:21:15  dechavez
 * initial release
 *
 */
