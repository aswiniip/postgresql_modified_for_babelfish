/*-------------------------------------------------------------------------
 *
 * fmgr.c
 *	  The Postgres function manager.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/fmgr/fmgr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/proclang.h"
#include "executor/functions.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "parser/parser.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrtab.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * Hooks for function calls
 */
PGDLLIMPORT needs_fmgr_hook_type needs_fmgr_hook = NULL;
PGDLLIMPORT fmgr_hook_type fmgr_hook = NULL;
PGDLLIMPORT non_tsql_proc_entry_hook_type non_tsql_proc_entry_hook = NULL;
PGDLLIMPORT get_func_language_oids_hook_type get_func_language_oids_hook = NULL;
PGDLLIMPORT pgstat_function_wrapper_hook_type pgstat_function_wrapper_hook = NULL;
set_local_schema_for_func_hook_type set_local_schema_for_func_hook = NULL;
bool pltsql_check_search_path = true;

/*
 * Hashtable for fast lookup of external C functions
 */
typedef struct
{
	/* fn_oid is the hash key and so must be first! */
	Oid			fn_oid;			/* OID of an external C function */
	TransactionId fn_xmin;		/* for checking up-to-dateness */
	ItemPointerData fn_tid;
	PGFunction	user_fn;		/* the function's address */
	const Pg_finfo_record *inforec; /* address of its info record */
} CFuncHashTabEntry;

static HTAB *CFuncHash = NULL;

static void fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
								   bool ignore_security);
static void fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static void fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static CFuncHashTabEntry *lookup_C_func(HeapTuple procedureTuple);
static void record_C_func(HeapTuple procedureTuple,
			  PGFunction user_fn, const Pg_finfo_record *inforec);

/* extern so it's callable via JIT */
extern Datum fmgr_security_definer(PG_FUNCTION_ARGS);


/*
 * Lookup routines for builtin-function table.  We can search by either Oid
 * or name, but search by Oid is much faster.
 */

static const FmgrBuiltin *
fmgr_isbuiltin(Oid id)
{
	uint16		index;

	/* fast lookup only possible if original oid still assigned */
	if (id > fmgr_last_builtin_oid)
		return NULL;

	/*
	 * Lookup function data. If there's a miss in that range it's likely a
	 * nonexistent function, returning NULL here will trigger an ERROR later.
	 */
	index = fmgr_builtin_oid_index[id];
	if (index == InvalidOidBuiltinMapping)
		return NULL;

	return &fmgr_builtins[index];
}

/*
 * Lookup a builtin by name.  Note there can be more than one entry in
 * the array with the same name, but they should all point to the same
 * routine.
 */
static const FmgrBuiltin *
fmgr_lookupByName(const char *name)
{
	int			i;

	for (i = 0; i < fmgr_nbuiltins; i++)
	{
		if (strcmp(name, fmgr_builtins[i].funcName) == 0)
			return fmgr_builtins + i;
	}
	return NULL;
}

/*
 * This routine fills a FmgrInfo struct, given the OID
 * of the function to be called.
 *
 * The caller's CurrentMemoryContext is used as the fn_mcxt of the info
 * struct; this means that any subsidiary data attached to the info struct
 * (either by fmgr_info itself, or later on by a function call handler)
 * will be allocated in that context.  The caller must ensure that this
 * context is at least as long-lived as the info struct itself.  This is
 * not a problem in typical cases where the info struct is on the stack or
 * in freshly-palloc'd space.  However, if one intends to store an info
 * struct in a long-lived table, it's better to use fmgr_info_cxt.
 */
void
fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	fmgr_info_cxt_security(functionId, finfo, CurrentMemoryContext, false);
}

/*
 * Fill a FmgrInfo struct, specifying a memory context in which its
 * subsidiary data should go.
 */
void
fmgr_info_cxt(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt)
{
	fmgr_info_cxt_security(functionId, finfo, mcxt, false);
}

/*
 * This one does the actual work.  ignore_security is ordinarily false
 * but is set to true when we need to avoid recursion.
 */
static void
fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
					   bool ignore_security)
{
	const FmgrBuiltin *fbp;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	Datum		prosrcdatum;
	char	   *prosrc;

	/*
	 * fn_oid *must* be filled in last.  Some code assumes that if fn_oid is
	 * valid, the whole struct is valid.  Some FmgrInfo struct's do survive
	 * elogs.
	 */
	finfo->fn_oid = InvalidOid;
	finfo->fn_extra = NULL;
	finfo->fn_mcxt = mcxt;
	finfo->fn_expr = NULL;		/* caller may set this later */

	if ((fbp = fmgr_isbuiltin(functionId)) != NULL)
	{
		/*
		 * Fast path for builtin functions: don't bother consulting pg_proc
		 */
		finfo->fn_nargs = fbp->nargs;
		finfo->fn_strict = fbp->strict;
		finfo->fn_retset = fbp->retset;
		finfo->fn_stats = TRACK_FUNC_ALL;	/* ie, never track */
		finfo->fn_addr = fbp->func;
		finfo->fn_oid = functionId;
		return;
	}

	/* Otherwise we need the pg_proc entry */
	procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionId));
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", functionId);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	finfo->fn_nargs = procedureStruct->pronargs;
	finfo->fn_strict = procedureStruct->proisstrict;
	finfo->fn_retset = procedureStruct->proretset;

	/*
	 * If it has prosecdef set, non-null proconfig, or if a plugin wants to
	 * hook function entry/exit, use fmgr_security_definer call handler ---
	 * unless we are being called again by fmgr_security_definer or
	 * fmgr_info_other_lang.
	 *
	 * When using fmgr_security_definer, function stats tracking is always
	 * disabled at the outer level, and instead we set the flag properly in
	 * fmgr_security_definer's private flinfo and implement the tracking
	 * inside fmgr_security_definer.  This loses the ability to charge the
	 * overhead of fmgr_security_definer to the function, but gains the
	 * ability to set the track_functions GUC as a local GUC parameter of an
	 * interesting function and have the right things happen.
	 */
	if (!ignore_security &&
		(procedureStruct->prosecdef ||
		 !heap_attisnull(procedureTuple, Anum_pg_proc_proconfig, NULL) ||
		 /*
		  * If babelfishpg_tsql extension is installed, set proconfig of functions
		  * to have sql_dialect be either postgres or tsql according to
		  * their language.
		  */
		 (get_func_language_oids_hook != NULL) ||
		 FmgrHookIsNeeded(functionId)))
	{
		finfo->fn_addr = fmgr_security_definer;
		finfo->fn_stats = TRACK_FUNC_ALL;	/* ie, never track */
		finfo->fn_oid = functionId;
		ReleaseSysCache(procedureTuple);
		return;
	}

	switch (procedureStruct->prolang)
	{
		case INTERNALlanguageId:

			/*
			 * For an ordinary builtin function, we should never get here
			 * because the fmgr_isbuiltin() search above will have succeeded.
			 * However, if the user has done a CREATE FUNCTION to create an
			 * alias for a builtin function, we can end up here.  In that case
			 * we have to look up the function by name.  The name of the
			 * internal function is stored in prosrc (it doesn't have to be
			 * the same as the name of the alias!)
			 */
			prosrcdatum = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
												 Anum_pg_proc_prosrc);
			prosrc = TextDatumGetCString(prosrcdatum);
			fbp = fmgr_lookupByName(prosrc);
			if (fbp == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("internal function \"%s\" is not in internal lookup table",
								prosrc)));
			pfree(prosrc);
			/* Should we check that nargs, strict, retset match the table? */
			finfo->fn_addr = fbp->func;
			/* note this policy is also assumed in fast path above */
			finfo->fn_stats = TRACK_FUNC_ALL;	/* ie, never track */
			break;

		case ClanguageId:
			fmgr_info_C_lang(functionId, finfo, procedureTuple);
			finfo->fn_stats = TRACK_FUNC_PL;	/* ie, track if ALL */
			break;

		case SQLlanguageId:
			finfo->fn_addr = fmgr_sql;
			finfo->fn_stats = TRACK_FUNC_PL;	/* ie, track if ALL */
			break;

		default:
			fmgr_info_other_lang(functionId, finfo, procedureTuple);
			finfo->fn_stats = TRACK_FUNC_OFF;	/* ie, track if not OFF */
			break;
	}

	finfo->fn_oid = functionId;
	ReleaseSysCache(procedureTuple);
}

/*
 * Return module and C function name providing implementation of functionId.
 *
 * If *mod == NULL and *fn == NULL, no C symbol is known to implement
 * function.
 *
 * If *mod == NULL and *fn != NULL, the function is implemented by a symbol in
 * the main binary.
 *
 * If *mod != NULL and *fn != NULL the function is implemented in an extension
 * shared object.
 *
 * The returned module and function names are pstrdup'ed into the current
 * memory context.
 */
void
fmgr_symbol(Oid functionId, char **mod, char **fn)
{
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	Datum		prosrcattr;
	Datum		probinattr;

	procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionId));
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", functionId);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	if (procedureStruct->prosecdef ||
		!heap_attisnull(procedureTuple, Anum_pg_proc_proconfig, NULL) ||
		FmgrHookIsNeeded(functionId))
	{
		*mod = NULL;			/* core binary */
		*fn = pstrdup("fmgr_security_definer");
		ReleaseSysCache(procedureTuple);
		return;
	}

	/* see fmgr_info_cxt_security for the individual cases */
	switch (procedureStruct->prolang)
	{
		case INTERNALlanguageId:
			prosrcattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
												Anum_pg_proc_prosrc);

			*mod = NULL;		/* core binary */
			*fn = TextDatumGetCString(prosrcattr);
			break;

		case ClanguageId:
			prosrcattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
												Anum_pg_proc_prosrc);

			probinattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
												Anum_pg_proc_probin);

			/*
			 * No need to check symbol presence / API version here, already
			 * checked in fmgr_info_cxt_security.
			 */
			*mod = TextDatumGetCString(probinattr);
			*fn = TextDatumGetCString(prosrcattr);
			break;

		case SQLlanguageId:
			*mod = NULL;		/* core binary */
			*fn = pstrdup("fmgr_sql");
			break;

		default:
			*mod = NULL;
			*fn = NULL;			/* unknown, pass pointer */
			break;
	}

	ReleaseSysCache(procedureTuple);
}


/*
 * Special fmgr_info processing for C-language functions.  Note that
 * finfo->fn_oid is not valid yet.
 */
static void
fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
	CFuncHashTabEntry *hashentry;
	PGFunction	user_fn;
	const Pg_finfo_record *inforec;

	/*
	 * See if we have the function address cached already
	 */
	hashentry = lookup_C_func(procedureTuple);
	if (hashentry)
	{
		user_fn = hashentry->user_fn;
		inforec = hashentry->inforec;
	}
	else
	{
		Datum		prosrcattr,
					probinattr;
		char	   *prosrcstring,
				   *probinstring;
		void	   *libraryhandle;

		/*
		 * Get prosrc and probin strings (link symbol and library filename).
		 * While in general these columns might be null, that's not allowed
		 * for C-language functions.
		 */
		prosrcattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
											Anum_pg_proc_prosrc);
		prosrcstring = TextDatumGetCString(prosrcattr);

		probinattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
											Anum_pg_proc_probin);
		probinstring = TextDatumGetCString(probinattr);

		/* Look up the function itself */
		user_fn = load_external_function(probinstring, prosrcstring, true,
										 &libraryhandle);

		/* Get the function information record (real or default) */
		inforec = fetch_finfo_record(libraryhandle, prosrcstring);

		/* Cache the addresses for later calls */
		record_C_func(procedureTuple, user_fn, inforec);

		pfree(prosrcstring);
		pfree(probinstring);
	}

	switch (inforec->api_version)
	{
		case 1:
			/* New style: call directly */
			finfo->fn_addr = user_fn;
			break;
		default:
			/* Shouldn't get here if fetch_finfo_record did its job */
			elog(ERROR, "unrecognized function API version: %d",
				 inforec->api_version);
			break;
	}
}

/*
 * Special fmgr_info processing for other-language functions.  Note
 * that finfo->fn_oid is not valid yet.
 */
static void
fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
	Form_pg_proc procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);
	Oid			language = procedureStruct->prolang;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	FmgrInfo	plfinfo;

	languageTuple = SearchSysCache1(LANGOID, ObjectIdGetDatum(language));
	if (!HeapTupleIsValid(languageTuple))
		elog(ERROR, "cache lookup failed for language %u", language);
	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);

	/*
	 * Look up the language's call handler function, ignoring any attributes
	 * that would normally cause insertion of fmgr_security_definer.  We need
	 * to get back a bare pointer to the actual C-language function.
	 */
	fmgr_info_cxt_security(languageStruct->lanplcallfoid, &plfinfo,
						   CurrentMemoryContext, true);
	finfo->fn_addr = plfinfo.fn_addr;

	ReleaseSysCache(languageTuple);
}

/*
 * Fetch and validate the information record for the given external function.
 * The function is specified by a handle for the containing library
 * (obtained from load_external_function) as well as the function name.
 *
 * If no info function exists for the given name an error is raised.
 *
 * This function is broken out of fmgr_info_C_lang so that fmgr_c_validator
 * can validate the information record for a function not yet entered into
 * pg_proc.
 */
const Pg_finfo_record *
fetch_finfo_record(void *filehandle, const char *funcname)
{
	char	   *infofuncname;
	PGFInfoFunction infofunc;
	const Pg_finfo_record *inforec;

	infofuncname = psprintf("pg_finfo_%s", funcname);

	/* Try to look up the info function */
	infofunc = (PGFInfoFunction) lookup_external_function(filehandle,
														  infofuncname);
	if (infofunc == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not find function information for function \"%s\"",
						funcname),
				 errhint("SQL-callable functions need an accompanying PG_FUNCTION_INFO_V1(funcname).")));
		return NULL;			/* silence compiler */
	}

	/* Found, so call it */
	inforec = (*infofunc) ();

	/* Validate result as best we can */
	if (inforec == NULL)
		elog(ERROR, "null result from info function \"%s\"", infofuncname);
	switch (inforec->api_version)
	{
		case 1:
			/* OK, no additional fields to validate */
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized API version %d reported by info function \"%s\"",
							inforec->api_version, infofuncname)));
			break;
	}

	pfree(infofuncname);
	return inforec;
}


/*-------------------------------------------------------------------------
 *		Routines for caching lookup information for external C functions.
 *
 * The routines in dfmgr.c are relatively slow, so we try to avoid running
 * them more than once per external function per session.  We use a hash table
 * with the function OID as the lookup key.
 *-------------------------------------------------------------------------
 */

/*
 * lookup_C_func: try to find a C function in the hash table
 *
 * If an entry exists and is up to date, return it; else return NULL
 */
static CFuncHashTabEntry *
lookup_C_func(HeapTuple procedureTuple)
{
	Oid			fn_oid = ((Form_pg_proc) GETSTRUCT(procedureTuple))->oid;
	CFuncHashTabEntry *entry;

	if (CFuncHash == NULL)
		return NULL;			/* no table yet */
	entry = (CFuncHashTabEntry *)
		hash_search(CFuncHash,
					&fn_oid,
					HASH_FIND,
					NULL);
	if (entry == NULL)
		return NULL;			/* no such entry */
	if (entry->fn_xmin == HeapTupleHeaderGetRawXmin(procedureTuple->t_data) &&
		ItemPointerEquals(&entry->fn_tid, &procedureTuple->t_self))
		return entry;			/* OK */
	return NULL;				/* entry is out of date */
}

PGFunction
lookup_C_func_by_oid(Oid fn_oid, char* probinstring, char* prosrcstring)
{
	PGFunction user_fn = NULL;

	/* Lookup hash table CFuncHash If Functions are already cached */
	if (CFuncHash != NULL)
	{
		CFuncHashTabEntry *entry;

		entry = (CFuncHashTabEntry *)
			hash_search(CFuncHash,
						&fn_oid,
						HASH_FIND,
						NULL);
		if (entry == NULL)
			user_fn = NULL;			/* no such entry */
		else
			user_fn = entry->user_fn;			/* OK */
	}
	/*
	 * If haven't found using CFuncHash hash table,
	 * load the dynamically linked library to get the function
	 */
	if (user_fn == NULL)
		user_fn = load_external_function(probinstring, prosrcstring, true, NULL);

	return user_fn;
}

/*
 * record_C_func: enter (or update) info about a C function in the hash table
 */
static void
record_C_func(HeapTuple procedureTuple,
			  PGFunction user_fn, const Pg_finfo_record *inforec)
{
	Oid			fn_oid = ((Form_pg_proc) GETSTRUCT(procedureTuple))->oid;
	CFuncHashTabEntry *entry;
	bool		found;

	/* Create the hash table if it doesn't exist yet */
	if (CFuncHash == NULL)
	{
		HASHCTL		hash_ctl;

		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(CFuncHashTabEntry);
		CFuncHash = hash_create("CFuncHash",
								100,
								&hash_ctl,
								HASH_ELEM | HASH_BLOBS);
	}

	entry = (CFuncHashTabEntry *)
		hash_search(CFuncHash,
					&fn_oid,
					HASH_ENTER,
					&found);
	/* OID is already filled in */
	entry->fn_xmin = HeapTupleHeaderGetRawXmin(procedureTuple->t_data);
	entry->fn_tid = procedureTuple->t_self;
	entry->user_fn = user_fn;
	entry->inforec = inforec;
}


/*
 * Copy an FmgrInfo struct
 *
 * This is inherently somewhat bogus since we can't reliably duplicate
 * language-dependent subsidiary info.  We cheat by zeroing fn_extra,
 * instead, meaning that subsidiary info will have to be recomputed.
 */
void
fmgr_info_copy(FmgrInfo *dstinfo, FmgrInfo *srcinfo,
			   MemoryContext destcxt)
{
	memcpy(dstinfo, srcinfo, sizeof(FmgrInfo));
	dstinfo->fn_mcxt = destcxt;
	dstinfo->fn_extra = NULL;
}


/*
 * Specialized lookup routine for fmgr_internal_validator: given the alleged
 * name of an internal function, return the OID of the function.
 * If the name is not recognized, return InvalidOid.
 */
Oid
fmgr_internal_function(const char *proname)
{
	const FmgrBuiltin *fbp = fmgr_lookupByName(proname);

	if (fbp == NULL)
		return InvalidOid;
	return fbp->foid;
}


/*
 * Support for security-definer and proconfig-using functions.  We support
 * both of these features using the same call handler, because they are
 * often used together and it would be inefficient (as well as notationally
 * messy) to have two levels of call handler involved.
 */
struct fmgr_security_definer_cache
{
	FmgrInfo	flinfo;			/* lookup info for target function */
	Oid			userid;			/* userid to set, or InvalidOid */
	ArrayType  *proconfig;		/* GUC values to set, or NULL */
	Datum		arg;			/* passthrough argument for plugin modules */
	Oid         pronamespace;
	char 		prokind;
	char 		*prosearchpath;
	Oid			prolang;
	Oid         sys_nspoid;
};

/*
 * Function handler for security-definer/proconfig/plugin-hooked functions.
 * We extract the OID of the actual function and do a fmgr lookup again.
 * Then we fetch the pg_proc row and copy the owner ID and proconfig fields.
 * (All this info is cached for the duration of the current query.)
 * To execute a call, we temporarily replace the flinfo with the cached
 * and looked-up one, while keeping the outer fcinfo (which contains all
 * the actual arguments, etc.) intact.  This is not re-entrant, but then
 * the fcinfo itself can't be used reentrantly anyway.
 */
extern Datum
fmgr_security_definer(PG_FUNCTION_ARGS)
{
	Datum		result;
	struct fmgr_security_definer_cache *volatile fcache;
	FmgrInfo   *save_flinfo;
	Oid			save_userid;
	int			save_sec_context;
	volatile int save_nestlevel;
	PgStat_FunctionCallUsage fcusage;
	Oid			pltsql_lang_oid, pltsql_validator_oid;
	bool		set_sql_dialect;
	int			sql_dialect_value;
	int			sql_dialect_value_old;
	int			pg_dialect = SQL_DIALECT_PG;
	int			tsql_dialect = SQL_DIALECT_TSQL;
	int			sys_func_count = 0;
	int			non_tsql_proc_count = 0;
	void	   *newextra = NULL;
	char 	   *cacheTupleProcname = NULL;
	int 	    pltsql_save_nestlevel;

	if (get_func_language_oids_hook)
		get_func_language_oids_hook(&pltsql_lang_oid, &pltsql_validator_oid);
	else
	{
		pltsql_lang_oid = InvalidOid;
		pltsql_validator_oid = InvalidOid;
	}

	set_sql_dialect = pltsql_lang_oid != InvalidOid;

	if (!fcinfo->flinfo->fn_extra)
	{
		HeapTuple	tuple;
		Form_pg_proc procedureStruct;
		Datum		datum;
		bool		isnull;
		MemoryContext oldcxt;

		fcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
										sizeof(*fcache));

		fmgr_info_cxt_security(fcinfo->flinfo->fn_oid, &fcache->flinfo,
							   fcinfo->flinfo->fn_mcxt, true);
		fcache->flinfo.fn_expr = fcinfo->flinfo->fn_expr;

		tuple = SearchSysCache1(PROCOID,
								ObjectIdGetDatum(fcinfo->flinfo->fn_oid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u",
				 fcinfo->flinfo->fn_oid);
		procedureStruct = (Form_pg_proc) GETSTRUCT(tuple);

		cacheTupleProcname = pstrdup(procedureStruct->proname.data);

		if (procedureStruct->prosecdef)
			fcache->userid = procedureStruct->proowner;
		
		fcache->prokind = procedureStruct->prokind;
		fcache->prolang = procedureStruct->prolang;
		fcache->pronamespace = procedureStruct->pronamespace;
		fcache->sys_nspoid = InvalidOid;
		if((set_sql_dialect && (fcache->prolang == pltsql_lang_oid || fcache->prolang == pltsql_validator_oid))
			&& strcmp(format_type_be(procedureStruct->prorettype), "trigger") != 0
			&& fcache->prokind == PROKIND_FUNCTION)
		{
			char *new_search_path = NULL;
			new_search_path = (*set_local_schema_for_func_hook)(fcache->pronamespace);
			if(new_search_path)
			{
				oldcxt = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
				fcache->prosearchpath = pstrdup(new_search_path);
				MemoryContextSwitchTo(oldcxt);
			}
		}

		datum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proconfig,
								&isnull);
		if (!isnull)
		{
			oldcxt = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
			fcache->proconfig = DatumGetArrayTypePCopy(datum);
			MemoryContextSwitchTo(oldcxt);
		}

		ReleaseSysCache(tuple);

		fcinfo->flinfo->fn_extra = fcache;
	}
	else
		fcache = fcinfo->flinfo->fn_extra;

	/* GetUserIdAndSecContext is cheap enough that no harm in a wasted call */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	if (fcache->proconfig)		/* Need a new GUC nesting level */
		save_nestlevel = NewGUCNestLevel();
	else
		save_nestlevel = 0;		/* keep compiler quiet */

	if (OidIsValid(fcache->userid))
		SetUserIdAndSecContext(fcache->userid,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);

	if (fcache->proconfig)
	{
		ProcessGUCArray(fcache->proconfig,
						(superuser() ? PGC_SUSET : PGC_USERSET),
						PGC_S_SESSION,
						GUC_ACTION_SAVE);
	}

	if (set_sql_dialect && IsTransactionState())
	{
		if ((fcache->prolang == pltsql_lang_oid) || (fcache->prolang == pltsql_validator_oid))
			sql_dialect_value = tsql_dialect;
		else
		{
			/* Record PG procedure entry */
			if (fcache->sys_nspoid == InvalidOid)
				fcache->sys_nspoid = get_namespace_oid("sys", false);

			/*
			 * For functions and C procs inside sys/pg_catalog,
			 * handle errors TSQL way. Otherwise, error in simple
			 * functions like cast/coerce will change TSQL behavior
			 */
			if ((fcache->pronamespace == fcache->sys_nspoid ||
				 fcache->pronamespace == PG_CATALOG_NAMESPACE) &&
				(fcache->prokind == PROKIND_FUNCTION ||
				 fcache->prolang == ClanguageId ||
				 fcache->prolang == INTERNALlanguageId))
				sys_func_count = 1;
			else
				non_tsql_proc_count = 1;

			non_tsql_proc_entry_hook(non_tsql_proc_count, sys_func_count);
			sql_dialect_value = pg_dialect;
		}

		sql_dialect_value_old = sql_dialect;
		sql_dialect = sql_dialect_value;
		assign_sql_dialect(sql_dialect_value, newextra);
	}

	/* function manager hook */
	if (fmgr_hook)
		(*fmgr_hook) (FHET_START, &fcache->flinfo, &fcache->arg);

	/*
	 * We don't need to restore GUC or userid settings on error, because the
	 * ensuing xact or subxact abort will do that.  The PG_TRY block is only
	 * needed to clean up the flinfo link.
	 */
	save_flinfo = fcinfo->flinfo;

	PG_TRY();
	{
		fcinfo->flinfo = &fcache->flinfo;

		/* See notes in fmgr_info_cxt_security */

		/* 
		 * The wrapper function hook is called if hook is not null 
		 * and the dialect is TSQL then we call this func using hook 
		 * otherwise we will fall back to pgstat_init_function_usage
		*/

		if(pgstat_function_wrapper_hook && sql_dialect == SQL_DIALECT_TSQL)
		{
			(*pgstat_function_wrapper_hook)(fcinfo, &fcusage, cacheTupleProcname);
		}
		
		pgstat_init_function_usage(fcinfo, &fcusage);

		if(cacheTupleProcname)
		{
			pfree(cacheTupleProcname);
		}

		if (fcache->prosearchpath)
		{
			pltsql_save_nestlevel = NewGUCNestLevel();
			pltsql_check_search_path = false;
			(void) set_config_option("search_path", fcache->prosearchpath,
									PGC_USERSET, PGC_S_SESSION,
									GUC_ACTION_SAVE, true, 0, false);
			pltsql_check_search_path = true;
		}

		result = FunctionCallInvoke(fcinfo);

		/*
		 * We could be calling either a regular or a set-returning function,
		 * so we have to test to see what finalize flag to use.
		 */
		
		if (pltsql_pgstat_end_function_usage_hook)
		{
			(*pltsql_pgstat_end_function_usage_hook)(fcinfo, &fcusage, fcache->prokind, 
													 (fcinfo->resultinfo == NULL ||
													  !IsA(fcinfo->resultinfo, ReturnSetInfo) ||
													  ((ReturnSetInfo *) fcinfo->resultinfo)->isDone != ExprMultipleResult));
		}
		else
		{
			pgstat_end_function_usage(&fcusage,
									  (fcinfo->resultinfo == NULL ||
									   !IsA(fcinfo->resultinfo, ReturnSetInfo) ||
									   ((ReturnSetInfo *) fcinfo->resultinfo)->isDone != ExprMultipleResult));
		}
	}
	PG_CATCH();
	{
		fcinfo->flinfo = save_flinfo;
		if (fmgr_hook)
			(*fmgr_hook) (FHET_ABORT, &fcache->flinfo, &fcache->arg);

		/*
		 * TODO PG set commands cannot work with transaction commands
		 * inside procedures. Fix it as part of larger interoperability
		 * design for PG vs TSQL procedures.
		 */
		if (set_sql_dialect)
		{
			sql_dialect = sql_dialect_value_old;
			assign_sql_dialect(sql_dialect_value_old, newextra);
		}

		if (fcache->prosearchpath)
			pltsql_check_search_path = true;
		
		PG_RE_THROW();
	}
	PG_END_TRY();

	fcinfo->flinfo = save_flinfo;

	if (fcache->prosearchpath)
	{
		AtEOXact_GUC(true, pltsql_save_nestlevel);
	}
	if (fcache->proconfig)
		AtEOXact_GUC(true, save_nestlevel);
	if (set_sql_dialect)
	{
		sql_dialect = sql_dialect_value_old;
		assign_sql_dialect(sql_dialect_value_old, newextra);

		if (sql_dialect_value == pg_dialect)
			non_tsql_proc_entry_hook(non_tsql_proc_count * -1, sys_func_count * -1);
	}
	if (OidIsValid(fcache->userid))
		SetUserIdAndSecContext(save_userid, save_sec_context);
	if (fmgr_hook && !set_sql_dialect)
		(*fmgr_hook) (FHET_END, &fcache->flinfo, &fcache->arg);

	return result;
}


/*-------------------------------------------------------------------------
 *		Support routines for callers of fmgr-compatible functions
 *-------------------------------------------------------------------------
 */

/*
 * These are for invocation of a specifically named function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.  Also, the function cannot be one that needs to
 * look at FmgrInfo, since there won't be any.
 */
Datum
DirectFunctionCall1Coll(PGFunction func, Oid collation, Datum arg1)
{
	LOCAL_FCINFO(fcinfo, 1);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 1, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall2Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2)
{
	LOCAL_FCINFO(fcinfo, 2);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 2, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall3Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3)
{
	LOCAL_FCINFO(fcinfo, 3);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 3, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall4Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4)
{
	LOCAL_FCINFO(fcinfo, 4);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 4, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall5Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5)
{
	LOCAL_FCINFO(fcinfo, 5);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 5, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall6Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5,
						Datum arg6)
{
	LOCAL_FCINFO(fcinfo, 6);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 6, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall7Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5,
						Datum arg6, Datum arg7)
{
	LOCAL_FCINFO(fcinfo, 7);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 7, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall8Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5,
						Datum arg6, Datum arg7, Datum arg8)
{
	LOCAL_FCINFO(fcinfo, 8);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 8, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;
	fcinfo->args[7].value = arg8;
	fcinfo->args[7].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall9Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5,
						Datum arg6, Datum arg7, Datum arg8,
						Datum arg9)
{
	LOCAL_FCINFO(fcinfo, 9);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 9, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;
	fcinfo->args[7].value = arg8;
	fcinfo->args[7].isnull = false;
	fcinfo->args[8].value = arg9;
	fcinfo->args[8].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

/*
 * These functions work like the DirectFunctionCall functions except that
 * they use the flinfo parameter to initialise the fcinfo for the call.
 * It's recommended that the callee only use the fn_extra and fn_mcxt
 * fields, as other fields will typically describe the calling function
 * not the callee.  Conversely, the calling function should not have
 * used fn_extra, unless its use is known to be compatible with the callee's.
 */

Datum
CallerFInfoFunctionCall1(PGFunction func, FmgrInfo *flinfo, Oid collation, Datum arg1)
{
	LOCAL_FCINFO(fcinfo, 1);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 1, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
CallerFInfoFunctionCall2(PGFunction func, FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
	LOCAL_FCINFO(fcinfo, 2);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 2, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;

	result = (*func) (fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

/*
 * These are for invocation of a previously-looked-up function with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.
 */
Datum
FunctionCall0Coll(FmgrInfo *flinfo, Oid collation)
{
	LOCAL_FCINFO(fcinfo, 0);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 0, collation, NULL, NULL);

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall1Coll(FmgrInfo *flinfo, Oid collation, Datum arg1)
{
	LOCAL_FCINFO(fcinfo, 1);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 1, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall2Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
	LOCAL_FCINFO(fcinfo, 2);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 2, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall3Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3)
{
	LOCAL_FCINFO(fcinfo, 3);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 3, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall4Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4)
{
	LOCAL_FCINFO(fcinfo, 4);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 4, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall5Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5)
{
	LOCAL_FCINFO(fcinfo, 5);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 5, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall6Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5,
				  Datum arg6)
{
	LOCAL_FCINFO(fcinfo, 6);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 6, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall7Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5,
				  Datum arg6, Datum arg7)
{
	LOCAL_FCINFO(fcinfo, 7);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 7, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall8Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5,
				  Datum arg6, Datum arg7, Datum arg8)
{
	LOCAL_FCINFO(fcinfo, 8);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 8, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;
	fcinfo->args[7].value = arg8;
	fcinfo->args[7].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall9Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5,
				  Datum arg6, Datum arg7, Datum arg8,
				  Datum arg9)
{
	LOCAL_FCINFO(fcinfo, 9);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 9, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;
	fcinfo->args[7].value = arg8;
	fcinfo->args[7].isnull = false;
	fcinfo->args[8].value = arg9;
	fcinfo->args[8].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}


/*
 * These are for invocation of a function identified by OID with a
 * directly-computed parameter list.  Note that neither arguments nor result
 * are allowed to be NULL.  These are essentially fmgr_info() followed
 * by FunctionCallN().  If the same function is to be invoked repeatedly,
 * do the fmgr_info() once and then use FunctionCallN().
 */
Datum
OidFunctionCall0Coll(Oid functionId, Oid collation)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall0Coll(&flinfo, collation);
}

Datum
OidFunctionCall1Coll(Oid functionId, Oid collation, Datum arg1)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall1Coll(&flinfo, collation, arg1);
}

Datum
OidFunctionCall2Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall2Coll(&flinfo, collation, arg1, arg2);
}

Datum
OidFunctionCall3Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall3Coll(&flinfo, collation, arg1, arg2, arg3);
}

Datum
OidFunctionCall4Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall4Coll(&flinfo, collation, arg1, arg2, arg3, arg4);
}

Datum
OidFunctionCall5Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall5Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5);
}

Datum
OidFunctionCall6Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5,
					 Datum arg6)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall6Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5,
							 arg6);
}

Datum
OidFunctionCall7Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5,
					 Datum arg6, Datum arg7)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall7Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5,
							 arg6, arg7);
}

Datum
OidFunctionCall8Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5,
					 Datum arg6, Datum arg7, Datum arg8)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall8Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5,
							 arg6, arg7, arg8);
}

Datum
OidFunctionCall9Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5,
					 Datum arg6, Datum arg7, Datum arg8,
					 Datum arg9)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall9Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5,
							 arg6, arg7, arg8, arg9);
}


/*
 * Special cases for convenient invocation of datatype I/O functions.
 */

/*
 * Call a previously-looked-up datatype input function.
 *
 * "str" may be NULL to indicate we are reading a NULL.  In this case
 * the caller should assume the result is NULL, but we'll call the input
 * function anyway if it's not strict.  So this is almost but not quite
 * the same as FunctionCall3.
 */
Datum
InputFunctionCall(FmgrInfo *flinfo, char *str, Oid typioparam, int32 typmod)
{
	LOCAL_FCINFO(fcinfo, 3);
	Datum		result;

	if (str == NULL && flinfo->fn_strict)
		return (Datum) 0;		/* just return null result */

	InitFunctionCallInfoData(*fcinfo, flinfo, 3, InvalidOid, NULL, NULL);

	fcinfo->args[0].value = CStringGetDatum(str);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Should get null result if and only if str is NULL */
	if (str == NULL)
	{
		if (!fcinfo->isnull)
			elog(ERROR, "input function %u returned non-NULL",
				 flinfo->fn_oid);
	}
	else
	{
		if (fcinfo->isnull)
			elog(ERROR, "input function %u returned NULL",
				 flinfo->fn_oid);
	}

	return result;
}

/*
 * Call a previously-looked-up datatype input function, with non-exception
 * handling of "soft" errors.
 *
 * This is basically like InputFunctionCall, but the converted Datum is
 * returned into *result while the function result is true for success or
 * false for failure.  Also, the caller may pass an ErrorSaveContext node.
 * (We declare that as "fmNodePtr" to avoid including nodes.h in fmgr.h.)
 *
 * If escontext points to an ErrorSaveContext, any "soft" errors detected by
 * the input function will be reported by filling the escontext struct and
 * returning false.  (The caller can choose to test SOFT_ERROR_OCCURRED(),
 * but checking the function result instead is usually cheaper.)
 *
 * If escontext does not point to an ErrorSaveContext, errors are reported
 * via ereport(ERROR), so that there is no functional difference from
 * InputFunctionCall; the result will always be true if control returns.
 */
bool
InputFunctionCallSafe(FmgrInfo *flinfo, char *str,
					  Oid typioparam, int32 typmod,
					  fmNodePtr escontext,
					  Datum *result)
{
	LOCAL_FCINFO(fcinfo, 3);

	if (str == NULL && flinfo->fn_strict)
	{
		*result = (Datum) 0;	/* just return null result */
		return true;
	}

	InitFunctionCallInfoData(*fcinfo, flinfo, 3, InvalidOid, escontext, NULL);

	fcinfo->args[0].value = CStringGetDatum(str);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	*result = FunctionCallInvoke(fcinfo);

	/* Result value is garbage, and could be null, if an error was reported */
	if (SOFT_ERROR_OCCURRED(escontext))
		return false;

	/* Otherwise, should get null result if and only if str is NULL */
	if (str == NULL)
	{
		if (!fcinfo->isnull)
			elog(ERROR, "input function %u returned non-NULL",
				 flinfo->fn_oid);
	}
	else
	{
		if (fcinfo->isnull)
			elog(ERROR, "input function %u returned NULL",
				 flinfo->fn_oid);
	}

	return true;
}

/*
 * Call a directly-named datatype input function, with non-exception
 * handling of "soft" errors.
 *
 * This is like InputFunctionCallSafe, except that it is given a direct
 * pointer to the C function to call.  We assume that that function is
 * strict.  Also, the function cannot be one that needs to
 * look at FmgrInfo, since there won't be any.
 */
bool
DirectInputFunctionCallSafe(PGFunction func, char *str,
							Oid typioparam, int32 typmod,
							fmNodePtr escontext,
							Datum *result)
{
	LOCAL_FCINFO(fcinfo, 3);

	if (str == NULL)
	{
		*result = (Datum) 0;	/* just return null result */
		return true;
	}

	InitFunctionCallInfoData(*fcinfo, NULL, 3, InvalidOid, escontext, NULL);

	fcinfo->args[0].value = CStringGetDatum(str);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	*result = (*func) (fcinfo);

	/* Result value is garbage, and could be null, if an error was reported */
	if (SOFT_ERROR_OCCURRED(escontext))
		return false;

	/* Otherwise, shouldn't get null result */
	if (fcinfo->isnull)
		elog(ERROR, "input function %p returned NULL", (void *) func);

	return true;
}

/*
 * Call a previously-looked-up datatype output function.
 *
 * Do not call this on NULL datums.
 *
 * This is currently little more than window dressing for FunctionCall1.
 */
char *
OutputFunctionCall(FmgrInfo *flinfo, Datum val)
{
	return DatumGetCString(FunctionCall1(flinfo, val));
}

/*
 * Call a previously-looked-up datatype binary-input function.
 *
 * "buf" may be NULL to indicate we are reading a NULL.  In this case
 * the caller should assume the result is NULL, but we'll call the receive
 * function anyway if it's not strict.  So this is almost but not quite
 * the same as FunctionCall3.
 */
Datum
ReceiveFunctionCall(FmgrInfo *flinfo, StringInfo buf,
					Oid typioparam, int32 typmod)
{
	LOCAL_FCINFO(fcinfo, 3);
	Datum		result;

	if (buf == NULL && flinfo->fn_strict)
		return (Datum) 0;		/* just return null result */

	InitFunctionCallInfoData(*fcinfo, flinfo, 3, InvalidOid, NULL, NULL);

	fcinfo->args[0].value = PointerGetDatum(buf);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* Should get null result if and only if buf is NULL */
	if (buf == NULL)
	{
		if (!fcinfo->isnull)
			elog(ERROR, "receive function %u returned non-NULL",
				 flinfo->fn_oid);
	}
	else
	{
		if (fcinfo->isnull)
			elog(ERROR, "receive function %u returned NULL",
				 flinfo->fn_oid);
	}

	return result;
}

/*
 * Call a previously-looked-up datatype binary-output function.
 *
 * Do not call this on NULL datums.
 *
 * This is little more than window dressing for FunctionCall1, but it does
 * guarantee a non-toasted result, which strictly speaking the underlying
 * function doesn't.
 */
bytea *
SendFunctionCall(FmgrInfo *flinfo, Datum val)
{
	return DatumGetByteaP(FunctionCall1(flinfo, val));
}

/*
 * As above, for I/O functions identified by OID.  These are only to be used
 * in seldom-executed code paths.  They are not only slow but leak memory.
 */
Datum
OidInputFunctionCall(Oid functionId, char *str, Oid typioparam, int32 typmod)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return InputFunctionCall(&flinfo, str, typioparam, typmod);
}

char *
OidOutputFunctionCall(Oid functionId, Datum val)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return OutputFunctionCall(&flinfo, val);
}

Datum
OidReceiveFunctionCall(Oid functionId, StringInfo buf,
					   Oid typioparam, int32 typmod)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return ReceiveFunctionCall(&flinfo, buf, typioparam, typmod);
}

bytea *
OidSendFunctionCall(Oid functionId, Datum val)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return SendFunctionCall(&flinfo, val);
}


/*-------------------------------------------------------------------------
 *		Support routines for standard maybe-pass-by-reference datatypes
 *
 * int8 and float8 can be passed by value if Datum is wide enough.
 * (For backwards-compatibility reasons, we allow pass-by-ref to be chosen
 * at compile time even if pass-by-val is possible.)
 *
 * Note: there is only one switch controlling the pass-by-value option for
 * both int8 and float8; this is to avoid making things unduly complicated
 * for the timestamp types, which might have either representation.
 *-------------------------------------------------------------------------
 */

#ifndef USE_FLOAT8_BYVAL		/* controls int8 too */

Datum
Int64GetDatum(int64 X)
{
	int64	   *retval = (int64 *) palloc(sizeof(int64));

	*retval = X;
	return PointerGetDatum(retval);
}

Datum
Float8GetDatum(float8 X)
{
	float8	   *retval = (float8 *) palloc(sizeof(float8));

	*retval = X;
	return PointerGetDatum(retval);
}
#endif							/* USE_FLOAT8_BYVAL */


/*-------------------------------------------------------------------------
 *		Support routines for toastable datatypes
 *-------------------------------------------------------------------------
 */

struct varlena *
pg_detoast_datum(struct varlena *datum)
{
	if (VARATT_IS_EXTENDED(datum))
		return detoast_attr(datum);
	else
		return datum;
}

struct varlena *
pg_detoast_datum_copy(struct varlena *datum)
{
	if (VARATT_IS_EXTENDED(datum))
		return detoast_attr(datum);
	else
	{
		/* Make a modifiable copy of the varlena object */
		Size		len = VARSIZE(datum);
		struct varlena *result = (struct varlena *) palloc(len);

		memcpy(result, datum, len);
		return result;
	}
}

struct varlena *
pg_detoast_datum_slice(struct varlena *datum, int32 first, int32 count)
{
	/* Only get the specified portion from the toast rel */
	return detoast_attr_slice(datum, first, count);
}

struct varlena *
pg_detoast_datum_packed(struct varlena *datum)
{
	if (VARATT_IS_COMPRESSED(datum) || VARATT_IS_EXTERNAL(datum))
		return detoast_attr(datum);
	else
		return datum;
}

/*-------------------------------------------------------------------------
 *		Support routines for extracting info from fn_expr parse tree
 *
 * These are needed by polymorphic functions, which accept multiple possible
 * input types and need help from the parser to know what they've got.
 * Also, some functions might be interested in whether a parameter is constant.
 * Functions taking VARIADIC ANY also need to know about the VARIADIC keyword.
 *-------------------------------------------------------------------------
 */

/*
 * Get the actual type OID of the function return type
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_fn_expr_rettype(FmgrInfo *flinfo)
{
	Node	   *expr;

	/*
	 * can't return anything useful if we have no FmgrInfo or if its fn_expr
	 * node has not been initialized
	 */
	if (!flinfo || !flinfo->fn_expr)
		return InvalidOid;

	expr = flinfo->fn_expr;

	return exprType(expr);
}

/*
 * Get the actual type OID of a specific function argument (counting from 0)
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_fn_expr_argtype(FmgrInfo *flinfo, int argnum)
{
	/*
	 * can't return anything useful if we have no FmgrInfo or if its fn_expr
	 * node has not been initialized
	 */
	if (!flinfo || !flinfo->fn_expr)
		return InvalidOid;

	return get_call_expr_argtype(flinfo->fn_expr, argnum);
}

/*
 * Get the actual type OID of a specific function argument (counting from 0),
 * but working from the calling expression tree instead of FmgrInfo
 *
 * Returns InvalidOid if information is not available
 */
Oid
get_call_expr_argtype(Node *expr, int argnum)
{
	List	   *args;
	Oid			argtype;

	if (expr == NULL)
		return InvalidOid;

	if (IsA(expr, FuncExpr))
		args = ((FuncExpr *) expr)->args;
	else if (IsA(expr, OpExpr))
		args = ((OpExpr *) expr)->args;
	else if (IsA(expr, DistinctExpr))
		args = ((DistinctExpr *) expr)->args;
	else if (IsA(expr, ScalarArrayOpExpr))
		args = ((ScalarArrayOpExpr *) expr)->args;
	else if (IsA(expr, NullIfExpr))
		args = ((NullIfExpr *) expr)->args;
	else if (IsA(expr, WindowFunc))
		args = ((WindowFunc *) expr)->args;
	else
		return InvalidOid;

	if (argnum < 0 || argnum >= list_length(args))
		return InvalidOid;

	argtype = exprType((Node *) list_nth(args, argnum));

	/*
	 * special hack for ScalarArrayOpExpr: what the underlying function will
	 * actually get passed is the element type of the array.
	 */
	if (IsA(expr, ScalarArrayOpExpr) &&
		argnum == 1)
		argtype = get_base_element_type(argtype);

	return argtype;
}

/*
 * Find out whether a specific function argument is constant for the
 * duration of a query
 *
 * Returns false if information is not available
 */
bool
get_fn_expr_arg_stable(FmgrInfo *flinfo, int argnum)
{
	/*
	 * can't return anything useful if we have no FmgrInfo or if its fn_expr
	 * node has not been initialized
	 */
	if (!flinfo || !flinfo->fn_expr)
		return false;

	return get_call_expr_arg_stable(flinfo->fn_expr, argnum);
}

/*
 * Find out whether a specific function argument is constant for the
 * duration of a query, but working from the calling expression tree
 *
 * Returns false if information is not available
 */
bool
get_call_expr_arg_stable(Node *expr, int argnum)
{
	List	   *args;
	Node	   *arg;

	if (expr == NULL)
		return false;

	if (IsA(expr, FuncExpr))
		args = ((FuncExpr *) expr)->args;
	else if (IsA(expr, OpExpr))
		args = ((OpExpr *) expr)->args;
	else if (IsA(expr, DistinctExpr))
		args = ((DistinctExpr *) expr)->args;
	else if (IsA(expr, ScalarArrayOpExpr))
		args = ((ScalarArrayOpExpr *) expr)->args;
	else if (IsA(expr, NullIfExpr))
		args = ((NullIfExpr *) expr)->args;
	else if (IsA(expr, WindowFunc))
		args = ((WindowFunc *) expr)->args;
	else
		return false;

	if (argnum < 0 || argnum >= list_length(args))
		return false;

	arg = (Node *) list_nth(args, argnum);

	/*
	 * Either a true Const or an external Param will have a value that doesn't
	 * change during the execution of the query.  In future we might want to
	 * consider other cases too, e.g. now().
	 */
	if (IsA(arg, Const))
		return true;
	if (IsA(arg, Param) &&
		((Param *) arg)->paramkind == PARAM_EXTERN)
		return true;

	return false;
}

/*
 * Get the VARIADIC flag from the function invocation
 *
 * Returns false (the default assumption) if information is not available
 *
 * Note this is generally only of interest to VARIADIC ANY functions
 */
bool
get_fn_expr_variadic(FmgrInfo *flinfo)
{
	Node	   *expr;

	/*
	 * can't return anything useful if we have no FmgrInfo or if its fn_expr
	 * node has not been initialized
	 */
	if (!flinfo || !flinfo->fn_expr)
		return false;

	expr = flinfo->fn_expr;

	if (IsA(expr, FuncExpr))
		return ((FuncExpr *) expr)->funcvariadic;
	else
		return false;
}

/*
 * Set options to FmgrInfo of opclass support function.
 *
 * Opclass support functions are called outside of expressions.  Thanks to that
 * we can use fn_expr to store opclass options as bytea constant.
 */
void
set_fn_opclass_options(FmgrInfo *flinfo, bytea *options)
{
	flinfo->fn_expr = (Node *) makeConst(BYTEAOID, -1, InvalidOid, -1,
										 PointerGetDatum(options),
										 options == NULL, false);
}

/*
 * Check if options are defined for opclass support function.
 */
bool
has_fn_opclass_options(FmgrInfo *flinfo)
{
	if (flinfo && flinfo->fn_expr && IsA(flinfo->fn_expr, Const))
	{
		Const	   *expr = (Const *) flinfo->fn_expr;

		if (expr->consttype == BYTEAOID)
			return !expr->constisnull;
	}
	return false;
}

/*
 * Get options for opclass support function.
 */
bytea *
get_fn_opclass_options(FmgrInfo *flinfo)
{
	if (flinfo && flinfo->fn_expr && IsA(flinfo->fn_expr, Const))
	{
		Const	   *expr = (Const *) flinfo->fn_expr;

		if (expr->consttype == BYTEAOID)
			return expr->constisnull ? NULL : DatumGetByteaP(expr->constvalue);
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("operator class options info is absent in function call context")));

	return NULL;
}

/*-------------------------------------------------------------------------
 *		Support routines for procedural language implementations
 *-------------------------------------------------------------------------
 */

/*
 * Verify that a validator is actually associated with the language of a
 * particular function and that the user has access to both the language and
 * the function.  All validators should call this before doing anything
 * substantial.  Doing so ensures a user cannot achieve anything with explicit
 * calls to validators that he could not achieve with CREATE FUNCTION or by
 * simply calling an existing function.
 *
 * When this function returns false, callers should skip all validation work
 * and call PG_RETURN_VOID().  This never happens at present; it is reserved
 * for future expansion.
 *
 * In particular, checking that the validator corresponds to the function's
 * language allows untrusted language validators to assume they process only
 * superuser-chosen source code.  (Untrusted language call handlers, by
 * definition, do assume that.)  A user lacking the USAGE language privilege
 * would be unable to reach the validator through CREATE FUNCTION, so we check
 * that to block explicit calls as well.  Checking the EXECUTE privilege on
 * the function is often superfluous, because most users can clone the
 * function to get an executable copy.  It is meaningful against users with no
 * database TEMP right and no permanent schema CREATE right, thereby unable to
 * create any function.  Also, if the function tracks persistent state by
 * function OID or name, validating the original function might permit more
 * mischief than creating and validating a clone thereof.
 */
bool
CheckFunctionValidatorAccess(Oid validatorOid, Oid functionOid)
{
	HeapTuple	procTup;
	HeapTuple	langTup;
	Form_pg_proc procStruct;
	Form_pg_language langStruct;
	AclResult	aclresult;

	/*
	 * Get the function's pg_proc entry.  Throw a user-facing error for bad
	 * OID, because validators can be called with user-specified OIDs.
	 */
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionOid));
	if (!HeapTupleIsValid(procTup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", functionOid)));
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * Fetch pg_language entry to know if this is the correct validation
	 * function for that pg_proc entry.
	 */
	langTup = SearchSysCache1(LANGOID, ObjectIdGetDatum(procStruct->prolang));
	if (!HeapTupleIsValid(langTup))
		elog(ERROR, "cache lookup failed for language %u", procStruct->prolang);
	langStruct = (Form_pg_language) GETSTRUCT(langTup);

	if (langStruct->lanvalidator != validatorOid)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("language validation function %u called for language %u instead of %u",
						validatorOid, procStruct->prolang,
						langStruct->lanvalidator)));

	/* first validate that we have permissions to use the language */
	aclresult = object_aclcheck(LanguageRelationId, procStruct->prolang, GetUserId(),
								ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_LANGUAGE,
					   NameStr(langStruct->lanname));

	/*
	 * Check whether we are allowed to execute the function itself. If we can
	 * execute it, there should be no possible side-effect of
	 * compiling/validation that execution can't have.
	 */
	aclresult = object_aclcheck(ProcedureRelationId, functionOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, NameStr(procStruct->proname));

	ReleaseSysCache(procTup);
	ReleaseSysCache(langTup);

	return true;
}

