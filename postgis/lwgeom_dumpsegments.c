/**********************************************************************
 *
 * PostGIS - Spatial Types for PostgreSQL
 * http://postgis.net
 *
 * PostGIS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * PostGIS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PostGIS.  If not, see <http://www.gnu.org/licenses/>.
 *
 **********************************************************************
 *
 * ^copyright^
 *
 **********************************************************************/

#include "postgres.h"
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/array.h"
#include "utils/geo_decls.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
#include "funcapi.h"

#include "../postgis_config.h"
#include "lwgeom_pg.h"

#include "access/htup_details.h"

#include "liblwgeom.h"

Datum LWGEOM_dumpsegments(PG_FUNCTION_ARGS);

struct dumpnode {
	LWGEOM *geom;
	uint32_t idx; /* which member geom we're working on */
};

/* 32 is the max depth for st_dump, so it seems reasonable
 * to use the same here
 */
#define MAXDEPTH 32
struct dumpstate {
	LWGEOM *root;
	int stacklen; /* collections/geoms on stack */
	int pathlen;  /* polygon rings and such need extra path info */
	struct dumpnode stack[MAXDEPTH];
	Datum path[34]; /* two more than max depth, for ring and point */

	/* used to cache the type attributes for integer arrays */
	int16 typlen;
	bool byval;
	char align;

	uint32_t ring; /* ring of top polygon */
	uint32_t pt;   /* point of top geom or current ring */
};

PG_FUNCTION_INFO_V1(LWGEOM_dumpsegments);
Datum LWGEOM_dumpsegments(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	MemoryContext oldcontext, newcontext;

	GSERIALIZED *pglwgeom;
	LWCOLLECTION *lwcoll;
	LWGEOM *lwgeom;
	struct dumpstate *state;
	struct dumpnode *node;

	HeapTuple tuple;
	Datum pathpt[2];         /* used to construct the composite return value */
	bool isnull[2] = {0, 0}; /* needed to say neither value is null */
	Datum result;            /* the actual composite return value */

	if (SRF_IS_FIRSTCALL())
	{
		funcctx = SRF_FIRSTCALL_INIT();

		newcontext = funcctx->multi_call_memory_ctx;
		oldcontext = MemoryContextSwitchTo(newcontext);

		pglwgeom = PG_GETARG_GSERIALIZED_P_COPY(0);
		lwgeom = lwgeom_from_gserialized(pglwgeom);

		/* return early if nothing to do */
		if (!lwgeom || lwgeom_is_empty(lwgeom))
		{
			MemoryContextSwitchTo(oldcontext);
			funcctx = SRF_PERCALL_SETUP();
			SRF_RETURN_DONE(funcctx);
		}

		/* Create function state */
		state = lwalloc(sizeof *state);
		state->root = lwgeom;
		state->stacklen = 0;
		state->pathlen = 0;
		state->pt = 0;
		state->ring = 0;

		funcctx->user_fctx = state;

		/*
		 * Push a struct dumpnode on the state stack
		 */

		state->stack[0].idx = 0;
		state->stack[0].geom = lwgeom;
		state->stacklen++;

		/*
		 * get tuple description for return type
		 */
		if (get_call_result_type(fcinfo, 0, &funcctx->tuple_desc) != TYPEFUNC_COMPOSITE)
		{
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
		}

		BlessTupleDesc(funcctx->tuple_desc);

		/* get and cache data for constructing int4 arrays */
		get_typlenbyvalalign(INT4OID, &state->typlen, &state->byval, &state->align);

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	newcontext = funcctx->multi_call_memory_ctx;

	/* get state */
	state = funcctx->user_fctx;

	while (1)
	{
		node = &state->stack[state->stacklen - 1];
		lwgeom = node->geom;

		if (lwgeom->type == LINETYPE)
		{
			LWLINE *line = lwgeom_as_lwline(lwgeom);

			if (state->pt < line->points->npoints - 1)
			{
				POINT4D pt_start, pt_end;
				getPoint4d_p(line->points, state->pt, &pt_start);
				getPoint4d_p(line->points, state->pt + 1, &pt_end);

				POINTARRAY *segment_pa = ptarray_construct(lwgeom_has_z(line), lwgeom_has_m(line), 2);
				ptarray_set_point4d(segment_pa, 0, &pt_start);
				ptarray_set_point4d(segment_pa, 1, &pt_end);

				LWLINE *segment = lwline_construct(line->srid, NULL, segment_pa);

				state->pt++;

				state->path[state->pathlen] = Int32GetDatum(state->pt);
				pathpt[0] = PointerGetDatum(construct_array(state->path,
									    state->pathlen + 1,
									    INT4OID,
									    state->typlen,
									    state->byval,
									    state->align));
				pathpt[1] = PointerGetDatum(geometry_serialize((LWGEOM *)segment));

				tuple = heap_form_tuple(funcctx->tuple_desc, pathpt, isnull);
				result = HeapTupleGetDatum(tuple);
				SRF_RETURN_NEXT(funcctx, result);
			}
			else
			{
				if (--state->stacklen == 0)
					SRF_RETURN_DONE(funcctx);
				state->pathlen--;
				continue;
			}
		}

		if (lwgeom->type == COLLECTIONTYPE || lwgeom->type == MULTILINETYPE)
		{
			lwcoll = (LWCOLLECTION *)node->geom;

			/* if a collection and we have more geoms */
			if (node->idx < lwcoll->ngeoms)
			{
				/* push the next geom on the path and the stack */
				lwgeom = lwcoll->geoms[node->idx++];
				state->path[state->pathlen++] = Int32GetDatum(node->idx);

				node = &state->stack[state->stacklen++];
				node->idx = 0;
				node->geom = lwgeom;

				state->pt = 0;
				state->ring = 0;

				/* loop back to beginning, which will then check whatever node we just pushed */
				continue;
			}
		}

		/* no more geometries in the current collection */
		if (--state->stacklen == 0)
			SRF_RETURN_DONE(funcctx);
		state->pathlen--;
		state->stack[state->stacklen - 1].idx++;
	}
}
