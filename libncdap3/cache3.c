/*********************************************************************
 *   Copyright 1993, UCAR/Unidata
 *   See netcdf/COPYRIGHT file for copying and redistribution conditions.
 *   $Header$
 *********************************************************************/
#include "ncdap3.h"
#include "dapodom.h"
#include "dapdump.h"

/* Return 1 if we can reuse cached data to address
   the current get_vara request; return 0 otherwise.
   Target is in the constrained tree space.
   Currently, if the target matches a cache not that is not
   a whole variable, then match is false.
*/
int
iscached(NCDRNO* drno, CDFnode* target, NCcachenode** cachenodep)
{
    int i,j,found,index;
    NCcache* cache;
    NCcachenode* cachenode;

    found = 0;
    if(target == NULL) goto done;

    if(!FLAGSET(drno,NCF_CACHE)) goto done;

    /* match the target variable against elements in the cache */

    index = 0;
    cache = drno->cdf.cache;
    cachenode = cache->prefetch;

    /* always check prefetch (if it exists) */
    if(cachenode!= NULL) {
        for(found=0,i=0;i<nclistlength(cachenode->vars);i++) {
            CDFnode* var = (CDFnode*)nclistget(cachenode->vars,i);
	    if(var == target) {found=1; break;}
	}
    }
    if(!found) {/*search other cache nodes starting at latest first */
        for(i=nclistlength(cache->nodes)-1;i>=0;i--) {
            cachenode = (NCcachenode*)nclistget(cache->nodes,i);
            for(found=0,j=0;j<nclistlength(cachenode->vars);j++) {
                CDFnode* var = (CDFnode*)nclistget(cachenode->vars,j);
	        if(var == target) {found=1;index=i;break;}
	    }
	    if(found) break;
	}	
    }

    if(found) {
        ASSERT((cachenode != NULL));
        if(cachenode != cache->prefetch && nclistlength(cache->nodes) > 1) {
	    /* Manage the cache nodes as LRU */
	    nclistremove(cache->nodes,index);
	    nclistpush(cache->nodes,(ncelem)cachenode);
	}
        if(cachenodep) *cachenodep = cachenode;
    }
done:
#ifdef DBG
fprintf(stderr,"iscached: search: %s\n",makesimplepathstring3(target));
if(found)
   fprintf(stderr,"iscached: found: %s\n",dumpcachenode(cachenode));
else
   fprintf(stderr,"iscached: notfound\n");
#endif
    return found;
}

/* Compute the set of prefetched data */
NCerror
prefetchdata3(NCDRNO* drno)
{
    int i,j;
    NCerror ncstat = NC_NOERR;
    NClist* allvars = drno->cdf.varnodes;
    NCconstraint* constraint = drno->dap.dapconstraint;
    NClist* vars = nclistnew();
    NCcachenode* cache = NULL;
    NCconstraint* newconstraint = NULL;

    /* If caching is off, and we can do constraints, then
       don't even do prefetch
    */
    if(!FLAGSET(drno,NCF_CACHE) && !FLAGSET(drno,NCF_UNCONSTRAINABLE)) {
	drno->cdf.cache->prefetch = NULL;
	goto done;
    }

    for(i=0;i<nclistlength(allvars);i++) {
	CDFnode* var = (CDFnode*)nclistget(allvars,i);
	size_t nelems = 1;
	/* Compute the # of elements in the variable */
	for(j=0;j<nclistlength(var->array.dimensions);j++) {
	    CDFnode* dim = (CDFnode*)nclistget(var->array.dimensions,j);
	    nelems *= dim->dim.declsize;
	}
	/* If we cannot constrain, then pull in everything */
	if(FLAGSET(drno,NCF_UNCONSTRAINABLE)
           ||nelems <= drno->cdf.smallsizelimit)
	    nclistpush(vars,(ncelem)var);
    }
    /* If we cannot constrain, then pull in everything */
    newconstraint = createncconstraint();
    if(FLAGSET(drno,NCF_UNCONSTRAINABLE) || nclistlength(vars) == 0) {
	newconstraint->projections = NULL;
	newconstraint->selections= NULL;
    } else {/* Construct the projections for this set of vars */
        /* Initially, the constraints are same as the merged constraints */
        newconstraint->projections = clonencprojections(constraint->projections);
        restrictprojection3(drno,vars,newconstraint->projections);
        /* similar for selections */
        newconstraint->selections = clonencselections(constraint->selections);
    }

if(FLAGSET(drno,NCF_SHOWFETCH)) {
oc_log(OCLOGNOTE,"prefetch.");
}

    if(nclistlength(vars) == 0)
        cache = NULL;
    else {
        ncstat = buildcachenode3(drno,newconstraint,vars,&cache,1);
        if(ncstat) goto done;
    }
    /* Make cache node be the prefetch node */
    drno->cdf.cache->prefetch = cache;

#ifdef DEBUG
/* Log the set of prefetch variables */
NCbytes* buf = ncbytesnew();
ncbytescat(buf,"prefetch.vars: ");
for(i=0;i<nclistlength(vars);i++) {
CDFnode* var = (CDFnode*)nclistget(vars,i);
ncbytescat(buf," ");
ncbytescat(buf,makesimplepathstring3(var));
}
ncbytescat(buf,"\n");
oc_log(OCLOGNOTE,"%s",ncbytescontents(buf));
ncbytesfree(buf);
#endif

done:
    nclistfree(vars);
    freencconstraint(newconstraint);    
    if(ncstat) freenccachenode(drno,cache);
    return THROW(ncstat);
}

NCerror
buildcachenode3(NCDRNO* drno,
	        NCconstraint* constraint,
		NClist* varlist,
		NCcachenode** cachep,
		int isprefetch)
{
    NCerror ncstat = NC_NOERR;
    OCerror ocstat = OC_NOERR;
    OCconnection conn = drno->dap.conn;
    OCobject ocroot = OCNULL;
    CDFnode* dxdroot = NULL;
    NCcachenode* cachenode = NULL;
    char* ce = NULL;

    if(FLAGSET(drno,NCF_UNCONSTRAINABLE))
        ce = NULL;
    else
        ce = buildconstraintstring3(constraint);

    ocstat = dap_oc_fetch(drno,conn,ce,OCDATADDS,&ocroot);
    efree(ce);
    if(ocstat) {THROWCHK(ocerrtoncerr(ocstat)); goto done;}

    ncstat = buildcdftree34(drno,ocroot,OCDATA,&dxdroot);
    if(ncstat) {THROWCHK(ncstat); goto done;}

    /* regrid */
    if(!FLAGSET(drno,NCF_UNCONSTRAINABLE)) {
        ncstat = regrid3(dxdroot,drno->cdf.ddsroot,constraint->projections);
        if(ncstat) {THROWCHK(ncstat); goto done;}
    }

    /* create the cache node */
    cachenode = createnccachenode();
    cachenode->prefetch = isprefetch;
    cachenode->vars = nclistclone(varlist);
    cachenode->datadds = dxdroot;
    *cachenode->constraint = *constraint;
    constraint->projections = NULL;
    constraint->selections = NULL;

    /* save the root content*/
    cachenode->ocroot = ocroot;
    cachenode->content = oc_data_new(conn);
    ocstat = oc_data_root(conn,ocroot,cachenode->content);
    if(ocstat) {THROWCHK(ocerrtoncerr(ocstat)); goto done;}

    /* capture the packet size */
    ocstat = oc_raw_xdrsize(conn,ocroot,&cachenode->xdrsize);
    if(ocstat) {THROWCHK(ocerrtoncerr(ocstat)); goto done;}

    /* Insert into the cache */

    if(!FLAGSET(drno,NCF_CACHE)) goto done;

    if(isprefetch) {
        cachenode->prefetch = 1;
	drno->cdf.cache->prefetch = cachenode;
    } else {
	NCcache* cache = drno->cdf.cache;
	if(cache->nodes == NULL) cache->nodes = nclistnew();
	/* remove cache nodes to get below the max cache size */
	while(cache->cachesize + cachenode->xdrsize > cache->cachelimit) {
	    NCcachenode* node = (NCcachenode*)nclistremove(cache->nodes,0);
#ifdef DBG
fprintf(stderr,"buildcachenode: purge cache node: %s\n",
	dumpcachenode(cachenode));
#endif
	    cache->cachesize -= node->xdrsize;
	    freenccachenode(drno,node);
	}
	/* remove cache nodes to get below the max cache count */
	while(nclistlength(cache->nodes) >= cache->cachecount) {
	    NCcachenode* node = (NCcachenode*)nclistremove(cache->nodes,0);
#ifdef DBG
fprintf(stderr,"buildcachenode: count purge cache node: %s\n",
	dumpcachenode(cachenode));
#endif
	    cache->cachesize -= node->xdrsize;
	    freenccachenode(drno,node);
        }
        nclistpush(drno->cdf.cache->nodes,(ncelem)cachenode);
        cache->cachesize += cachenode->xdrsize;
    }

#ifdef DBG
fprintf(stderr,"buildcachenode: %s\n",dumpcachenode(cachenode));
#endif

done:
    if(cachep) *cachep = cachenode;
    if(ocstat != OC_NOERR) ncstat = ocerrtoncerr(ocstat);
    if(ncstat) {
	freecdfroot34(dxdroot);
	freenccachenode(drno,cachenode);
    }
    return THROW(ncstat);
}

NCcachenode*
createnccachenode(void)
{
    NCcachenode* mem = (NCcachenode*)emalloc(sizeof(NCcachenode));
    memset((void*)mem,0,sizeof(NCcachenode));
    return mem;
}

void
freenccachenode(NCDRNO* drno, NCcachenode* node)
{
    if(node == NULL) return;
    oc_data_free(drno->dap.conn,node->content);
    oc_data_free(drno->dap.conn,node->content);
    freencconstraint(node->constraint);
    freecdfroot34(node->datadds);
    nclistfree(node->vars);
    efree(node);
}


NCcache*
createnccache()
{
    NCcache* c = (NCcache*)emalloc(sizeof(NCcache));
    memset((void*)c,0,sizeof(NCcache));
    c->cachelimit = DFALTCACHELIMIT;
    c->cachesize = 0;
    c->nodes = nclistnew();
    c->cachecount = DFALTCACHECOUNT;
    return c;
}

void
freenccache(NCDRNO* drno, NCcache* cache)
{
    int i;
    if(cache == NULL) return;
    freenccachenode(drno,cache->prefetch);
    for(i=0;i<nclistlength(cache->nodes);i++) {
	freenccachenode(drno,(NCcachenode*)nclistget(cache->nodes,i));
    }
    efree(cache);
}