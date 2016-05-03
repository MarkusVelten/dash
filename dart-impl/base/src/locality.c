/**
 * \file dash/dart/base/locality.c
 */

/*
 * Include config and utmpx.h first to prevent previous include of utmpx.h
 * without _GNU_SOURCE in included headers:
 */
#include <dash/dart/base/config.h>
#ifdef DART__PLATFORM__LINUX
#  define _GNU_SOURCE
#  include <utmpx.h>
#endif
#include <dash/dart/base/macro.h>
#include <dash/dart/base/logging.h>
#include <dash/dart/base/locality.h>
#include <dash/dart/base/logging.h>
#include <dash/dart/base/assert.h>
#include <dash/dart/base/hwinfo.h>

#include <dash/dart/base/internal/papi.h>
#include <dash/dart/base/internal/unit_locality.h>
#include <dash/dart/base/internal/host_topology.h>

#include <dash/dart/if/dart_types.h>
#include <dash/dart/if/dart_locality.h>

#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sched.h>
#include <limits.h>

#ifdef DART_ENABLE_LIKWID
#  include <likwid.h>
#endif

#ifdef DART_ENABLE_HWLOC
#  include <hwloc.h>
#  include <hwloc/helper.h>
#endif

#ifdef DART_ENABLE_PAPI
#  include <papi.h>
#endif

#ifdef DART_ENABLE_NUMA
#  include <utmpx.h>
#  include <numa.h>
#endif

/* ======================================================================== *
 * Private Data                                                             *
 * ======================================================================== */

dart_domain_locality_t dart__base__locality__domain_root_;
dart_host_topology_t * dart__base__locality__host_topology_;

/* ======================================================================== *
 * Private Functions                                                        *
 * ======================================================================== */

dart_ret_t dart__base__locality__create_subdomains(
  dart_domain_locality_t * loc,
  dart_host_topology_t   * host_topology);

dart_ret_t dart__base__locality__create_global_subdomain(
  dart_host_topology_t   * host_topology,
  dart_domain_locality_t * global_domain,
  dart_domain_locality_t * subdomain,
  int                      rel_idx);

dart_ret_t dart__base__locality__create_node_subdomain(
  dart_host_topology_t   * host_topology,
  dart_domain_locality_t * node_domain,
  dart_domain_locality_t * subdomain,
  int                      rel_idx);

dart_ret_t dart__base__locality__create_module_subdomain(
  dart_host_topology_t   * host_topology,
  dart_domain_locality_t * module_domain,
  dart_domain_locality_t * subdomain,
  int                      rel_idx);

dart_ret_t dart__base__locality__create_numa_subdomain(
  dart_host_topology_t   * host_topology,
  dart_domain_locality_t * numa_domain,
  dart_domain_locality_t * subdomain,
  int                      rel_idx);

/* ======================================================================== *
 * Init / Finalize                                                          *
 * ======================================================================== */

dart_ret_t dart__base__locality__init()
{
  DART_LOG_DEBUG("dart__base__locality__init()");

  dart_hwinfo_t * hwinfo;
  DART_ASSERT_RETURNS(dart_hwinfo(&hwinfo), DART_OK);

  /* Initialize the global domain as the root entry in the locality
   * hierarchy: */
  dart__base__locality__domain_root_.scope  = DART_LOCALITY_SCOPE_GLOBAL;
  dart__base__locality__domain_root_.hwinfo = *hwinfo;

  char   hostname[DART_LOCALITY_HOST_MAX_SIZE];
  gethostname(hostname, DART_LOCALITY_HOST_MAX_SIZE);
  strncpy(dart__base__locality__domain_root_.host, hostname,
          DART_LOCALITY_HOST_MAX_SIZE);

  strncpy(dart__base__locality__domain_root_.domain_tag, ".", 1);

  size_t num_units = 0;
  DART_ASSERT_RETURNS(dart_size(&num_units), DART_OK);
  dart__base__locality__domain_root_.num_units = num_units;

  dart__base__locality__domain_root_.unit_ids =
    malloc(num_units * sizeof(dart_unit_t));
  for (int u = 0; u < num_units; ++u) {
    dart__base__locality__domain_root_.unit_ids[u] = u;
  }

  /* Exchange unit locality information between all units: */
  DART_ASSERT_RETURNS(
    dart__base__unit_locality__init(),
    DART_OK);

  /* Filter unique host names from locality information of all units.
   * Could be further optimized but only runs once during startup. */
  const int max_host_len = DART_LOCALITY_HOST_MAX_SIZE;
  DART_LOG_TRACE("dart__base__locality__init: copying host names");
  /* Copy host names of all units into array: */
  char ** hosts = malloc(sizeof(char *) * num_units);
  for (size_t u = 0; u < num_units; ++u) {
    hosts[u] = malloc(sizeof(char) * max_host_len);
    dart_unit_locality_t * ul;
    DART_ASSERT_RETURNS(dart__base__unit_locality__at(u, &ul), DART_OK);
    strncpy(hosts[u], ul->host, max_host_len);
  }

  dart_host_topology_t * topo = malloc(sizeof(dart_host_topology_t));
  DART_ASSERT_RETURNS(
    dart__base__host_topology__create(hosts, DART_TEAM_ALL, topo),
    DART_OK);
  dart__base__locality__host_topology_ = topo;
  size_t num_nodes   = dart__base__locality__host_topology_->num_nodes;
  size_t num_modules = dart__base__locality__host_topology_->num_modules;
  DART_LOG_TRACE("dart__base__locality__init: nodes:   %d", num_nodes);
  DART_LOG_TRACE("dart__base__locality__init: modules: %d", num_modules);

  dart__base__locality__domain_root_.num_nodes          = num_nodes;
  dart__base__locality__domain_root_.hwinfo.num_modules = num_modules;

#ifdef DART_ENABLE_LOGGING
  for (size_t h = 0; h < topo->num_hosts; ++h) {
    dart_node_units_t * node_units = &topo->node_units[h];
    char * hostname = topo->host_names[h];
    DART_LOG_TRACE("dart__base__locality__init: "
                   "host %s: units:%d level:%d parent:%s", hostname,
                   node_units->num_units, node_units->level,
                   node_units->parent);
    for (size_t u = 0; u < node_units->num_units; ++u) {
      DART_LOG_TRACE("dart__base__locality__init: %s unit[%d]: %d",
                     hostname, u, node_units->units[u]);
    }
  }
#endif

  /* recursively create locality information of the global domain's
   * sub-domains: */
  DART_ASSERT_RETURNS(
    dart__base__locality__create_subdomains(
      &dart__base__locality__domain_root_,
      dart__base__locality__host_topology_),
    DART_OK);

  DART_LOG_DEBUG("dart__base__locality__init >");
  return DART_OK;
}

dart_ret_t dart__base__locality__finalize()
{
  DART_LOG_DEBUG("dart__base__locality__finalize()");

  dart_ret_t ret;

  ret = dart__base__locality__domain_delete(
          &dart__base__locality__domain_root_);
  if (ret != DART_OK) {
    DART_LOG_ERROR("dart__base__locality__finalize ! "
                   "dart__base__locality__domain_delete failed: %d", ret);
    return ret;
  }

  ret = dart__base__host_topology__delete(
          dart__base__locality__host_topology_);
  if (ret != DART_OK) {
    DART_LOG_ERROR("dart__base__locality__finalize ! "
                   "dart__base__host_topology__delete failed: %d", ret);
    return ret;
  }

  DART_LOG_DEBUG("dart__base__locality__finalize >");
  return DART_OK;
}

/* ======================================================================== *
 * Domain Locality                                                          *
 * ======================================================================== */

dart_ret_t dart__base__locality__domain_locality_init(
  dart_domain_locality_t * loc)
{
  DART_LOG_TRACE("dart__base__locality__domain_locality_init() loc: %p", loc);
  if (loc == NULL) {
    DART_LOG_ERROR("dart__base__locality__domain_locality_init ! null");
    return DART_ERR_INVAL;
  }
  loc->domain_tag[0] = '\0';
  loc->host[0]       = '\0';
  loc->scope         = DART_LOCALITY_SCOPE_UNDEFINED;
  loc->level         =  0;
  loc->parent        = NULL;
  loc->num_domains   =  0;
  loc->domains       = NULL;
  loc->num_nodes     = -1;
  loc->num_units     = -1;
  DART_LOG_TRACE("dart__base__locality__domain_locality_init >");
  return DART_OK;
}

dart_ret_t dart__base__locality__set_subdomains(
  const char             * domain_tag,
  dart_domain_locality_t * subdomains,
  int                      num_subdomains)
{
  dart_domain_locality_t * domain;
  /* find node in domain tree for specified domain tag: */
  if (dart__base__locality__domain(domain_tag, &domain) != DART_OK) {
    return DART_ERR_INVAL;
  }

  /* allocate child nodes: */
  domain->num_domains = num_subdomains;
  domain->domains     = (dart_domain_locality_t *)(
                            malloc(
                              num_subdomains *
                              sizeof(dart_domain_locality_t)));
  /* initialize child nodes: */
  int sub_level = domain->level + 1;
  for (int subdom_idx = 0; subdom_idx < num_subdomains; ++subdom_idx) {
    domain->domains[subdom_idx]             = subdomains[subdom_idx];
    domain->domains[subdom_idx].level       = sub_level;
    domain->domains[subdom_idx].num_domains = 0;
    domain->domains[subdom_idx].domains     = NULL;
  }

  return DART_OK;
}

dart_ret_t dart__base__locality__domain_delete(
  dart_domain_locality_t * domain)
{
  if (domain == NULL) {
    return DART_OK;
  }
  /* deallocate child nodes in depth-first recursion: */
  int num_child_nodes = domain->num_domains;
  for (int subdom_idx = 0; subdom_idx < num_child_nodes; ++subdom_idx) {
    if (domain->num_domains <= subdom_idx) {
      /* child index out of range: */
      return DART_ERR_INVAL;
    }
    if (domain->domains == NULL) {
      /* child nodes field is unspecified: */
      return DART_ERR_INVAL;
    }
    dart__base__locality__domain_delete(
      domain->domains + subdom_idx);
  }
  /* deallocate node itself: */
  free(domain->domains);
  domain->domains     = NULL;
  domain->num_domains = 0;

  return DART_OK;
}

dart_ret_t dart__base__locality__create_subdomains(
  dart_domain_locality_t * domain,
  dart_host_topology_t   * host_topology)
{
  DART_LOG_DEBUG("dart__base__locality__create_subdomains() "
                 "parent: %p scope: %d level: %d",
                 domain, domain->scope, domain->level);
  /*
   * First step: determine the number of sub-domains and their scope.
   */
  dart_locality_scope_t sub_scope;
  sub_scope           = DART_LOCALITY_SCOPE_UNDEFINED;
  char * module_hostname;
  switch (domain->scope) {
    case DART_LOCALITY_SCOPE_UNDEFINED:
      DART_LOG_ERROR("dart__base__locality__create_subdomains ! "
                     "locality scope undefined");
      return DART_ERR_INVAL;
    case DART_LOCALITY_SCOPE_GLOBAL:
      domain->num_domains = host_topology->num_nodes;
      sub_scope           = DART_LOCALITY_SCOPE_NODE;
      break;
    case DART_LOCALITY_SCOPE_NODE:
      domain->num_domains = domain->hwinfo.num_modules;
      sub_scope           = DART_LOCALITY_SCOPE_MODULE;
      break;
    case DART_LOCALITY_SCOPE_MODULE:
      domain->num_domains = domain->hwinfo.num_numa;
      sub_scope           = DART_LOCALITY_SCOPE_NUMA;
      module_hostname     = host_topology->host_names[domain->relative_index];
      /* Requires to resolve number of units in this module domain.
       * Cannot use local hwinfo, number of cores could refer to non-local
       * module. Use host topology instead. */
      size_t        num_module_units;
      dart_unit_t * module_units;
      DART_ASSERT_RETURNS(
        dart__base__host_topology__module_units(
          host_topology,
          module_hostname,
          &module_units,
          &num_module_units),
        DART_OK);
      domain->num_units   = num_module_units;
      break;
    case DART_LOCALITY_SCOPE_NUMA:
      domain->num_domains = domain->num_units;
      sub_scope           = DART_LOCALITY_SCOPE_CORE;
      break;
    default:
      domain->num_domains = 0;
      break;
  }
  DART_LOG_TRACE("dart__base__locality__create_subdomains: subdomains: %d",
                 domain->num_domains);
  if (domain->num_domains == 0) {
    domain->domains = NULL;
    DART_LOG_DEBUG("dart__base__locality__create_subdomains > "
                   "domain: %p - scope: %d level: %d "
                   "subdomains: %d domain(%s) - final",
                   domain, domain->scope, domain->level,
                   domain->num_domains, domain->domain_tag);
    return DART_OK;
  }
  /* Allocate subdomains: */
  domain->domains = (dart_domain_locality_t *)(
                       malloc(domain->num_domains *
                              sizeof(dart_domain_locality_t)));
  /*
   * Second step: determine the subdomain capacities and distribute
   * domain elements like units and cores.
   */
  for (int rel_idx = 0; rel_idx < domain->num_domains; ++rel_idx) {
    DART_LOG_TRACE("dart__base__locality__create_subdomains: "
                   "initialize, level: %d, subdomain %d of %d",
                   domain->level + 1, rel_idx, domain->num_domains);

    dart_domain_locality_t * subdomain = domain->domains + rel_idx;
    dart__base__locality__domain_locality_init(subdomain);

    /* Initialize hwinfo from parent as most properties are identical: */
    subdomain->hwinfo         = domain->hwinfo;
    subdomain->parent         = domain;
    subdomain->scope          = sub_scope;
    subdomain->relative_index = rel_idx;
    subdomain->level          = domain->level + 1;
    subdomain->node_id        = domain->node_id;
    /* set domain tag of subdomain: */
    strncpy(subdomain->host, domain->host, DART_LOCALITY_HOST_MAX_SIZE);
    int base_tag_len = 0;
    if (domain->level > 0) {
      base_tag_len = sprintf(subdomain->domain_tag, "%s",
                             domain->domain_tag);
    }
    /* append the subdomain tag part to subdomain tag, e.g. ".0.1": */
    sprintf(subdomain->domain_tag + base_tag_len, ".%d", rel_idx);

    switch (domain->scope) {
      case DART_LOCALITY_SCOPE_GLOBAL:
        dart__base__locality__create_global_subdomain(
          host_topology, domain, subdomain, rel_idx);
        break;
      case DART_LOCALITY_SCOPE_NODE:
        dart__base__locality__create_node_subdomain(
          host_topology, domain, subdomain, rel_idx);
        break;
      case DART_LOCALITY_SCOPE_MODULE:
        dart__base__locality__create_module_subdomain(
          host_topology, domain, subdomain, rel_idx);
        break;
      case DART_LOCALITY_SCOPE_NUMA:
        dart__base__locality__create_numa_subdomain(
          host_topology, domain, subdomain, rel_idx);
        break;
      case DART_LOCALITY_SCOPE_CORE:
        subdomain->hwinfo.num_modules = 1;
        subdomain->hwinfo.num_numa    = 1;
        subdomain->hwinfo.num_cores   = 1;
        subdomain->num_nodes          = 1;
        subdomain->num_units          = 1;
        subdomain->unit_ids           = malloc(sizeof(dart_unit_t));
        subdomain->unit_ids[0]        = domain->unit_ids[rel_idx];
        break;
    }
    /* recursively initialize subdomains: */
    DART_ASSERT_RETURNS(
        dart__base__locality__create_subdomains(subdomain, host_topology),
        DART_OK);
  }
  DART_LOG_DEBUG("dart__base__locality__create_subdomains >");
  return DART_OK;
}

dart_ret_t dart__base__locality__create_global_subdomain(
  dart_host_topology_t   * host_topology,
  dart_domain_locality_t * global_domain,
  dart_domain_locality_t * subdomain,
  int                      rel_idx)
{
  /* Loop iterates on nodes. Partitioning is trivial, split into one
   * node per sub-domain. */

  DART_LOG_TRACE("dart__base__locality__create_subdomains: "
                 "== SPLIT GLOBAL ==");
  DART_LOG_TRACE("dart__base__locality__create_subdomains: == %d of %d",
                 rel_idx, global_domain->num_domains);
  /* units in node at relative index: */
  dart_unit_t * node_unit_ids;
  size_t        num_node_units;
  char        * node_hostname = host_topology->host_names[rel_idx];
  DART_LOG_TRACE("dart__base__locality__create_subdomains: host: %s",
                 node_hostname);
  strncpy(subdomain->host, node_hostname, DART_LOCALITY_HOST_MAX_SIZE);
  dart__base__host_topology__node_units(
      host_topology,
      node_hostname,
      &node_unit_ids,
      &num_node_units);
  /* relative sub-domain index at global scope is node id: */
  subdomain->node_id    = rel_idx;
  subdomain->num_nodes  = 1;
  subdomain->num_units  = num_node_units;
  subdomain->unit_ids   = malloc(num_node_units * sizeof(dart_unit_t));
  for (int nu = 0; nu < num_node_units; ++nu) {
    subdomain->unit_ids[nu] = node_unit_ids[nu];
  }
  return DART_OK;
}

dart_ret_t dart__base__locality__create_node_subdomain(
  dart_host_topology_t   * host_topology,
  dart_domain_locality_t * node_domain,
  dart_domain_locality_t * subdomain,
  int                      rel_idx)
{
  /* Loop splits into processing modules.
   * Usually there is only one module (the host system), otherwise
   * partitioning is heterogenous. */
  DART_LOG_TRACE("dart__base__locality__create_subdomains: "
                 "== SPLIT NODE ==");
  DART_LOG_TRACE("dart__base__locality__create_subdomains: == %d of %d",
                 rel_idx, node_domain->num_domains);
  char *        module_hostname = host_topology->host_names[rel_idx];
  dart_unit_t * module_unit_ids;
  size_t        num_module_units;
  DART_LOG_TRACE("dart__base__locality__create_subdomains: host: %s",
                 module_hostname);
  /* set subdomain hostname to module's hostname: */
  strncpy(subdomain->host, module_hostname,
          DART_LOCALITY_HOST_MAX_SIZE);
  strncpy(subdomain->host, module_hostname, DART_LOCALITY_HOST_MAX_SIZE);
  dart__base__host_topology__module_units(
      host_topology,
      module_hostname,
      &module_unit_ids,
      &num_module_units);
  subdomain->num_nodes = 1;
  subdomain->num_units = num_module_units;
  subdomain->unit_ids  = malloc(num_module_units * sizeof(dart_unit_t));
  for (int nu = 0; nu < num_module_units; ++nu) {
    subdomain->unit_ids[nu] = module_unit_ids[nu];
  }
  return DART_OK;
}

dart_ret_t dart__base__locality__create_module_subdomain(
  dart_host_topology_t   * host_topology,
  dart_domain_locality_t * module_domain,
  dart_domain_locality_t * subdomain,
  int                      rel_idx)
{
  /* Loop splits into NUMA nodes. */
  DART_LOG_TRACE("dart__base__locality__create_subdomains: "
                 "== SPLIT MODULE ==");
  DART_LOG_TRACE("dart__base__locality__create_subdomains: == %d of %d",
                 rel_idx, module_domain->num_domains);

  size_t        num_numa_units  = 0;
  int           node_id         = module_domain->node_id;
  char *        module_hostname = module_domain->host;
  /* set subdomain hostname to module's hostname: */
  strncpy(subdomain->host, module_hostname,
          DART_LOCALITY_HOST_MAX_SIZE);
  size_t        num_module_units;
  dart_unit_t * module_unit_ids;
  dart__base__host_topology__module_units(
      host_topology,
      module_hostname,
      &module_unit_ids,
      &num_module_units);
  /* Assign units in the node that have the NUMA domain's NUMA id: */
  /* First pass: Count number of units in NUMA domain: */
  for (size_t mu = 0; mu < num_module_units; ++mu) {
    dart_unit_t module_unit_id = module_unit_ids[mu];
    /* query the unit's hw- and locality information: */
    dart_unit_locality_t * module_unit_loc;
    dart__base__unit_locality__at(module_unit_id, &module_unit_loc);
    int module_unit_numa_id = module_unit_loc->hwinfo.numa_id;
    /* TODO: assuming that rel_idx corresponds to NUMA id: */
    DART_LOG_TRACE("dart__base__locality__create_subdomains: "
                   "unit %d numa id: %d", mu, module_unit_numa_id);
    if (module_unit_numa_id == rel_idx) {
      ++num_numa_units;
    }
  }
  subdomain->hwinfo.num_modules = 1;
  subdomain->hwinfo.num_numa    = 1;
  subdomain->hwinfo.num_cores   = num_numa_units;
  subdomain->num_nodes          = 1;
  subdomain->num_units          = num_numa_units;
  subdomain->unit_ids           = malloc(subdomain->num_units *
                                         sizeof(dart_unit_t));
  /* Second pass: Assign unit ids in NUMA domain: */
  dart_unit_t numa_unit_idx = 0;
  for (size_t mu = 0; mu < num_module_units; ++mu) {
    dart_unit_t module_unit_id = module_unit_ids[mu];
    /* query the unit's hw- and locality information: */
    dart_unit_locality_t * module_unit_loc;
    dart__base__unit_locality__at(module_unit_id, &module_unit_loc);
    int module_unit_numa_id = module_unit_loc->hwinfo.numa_id;
    if (module_unit_numa_id == rel_idx) {
      dart_unit_t numa_unit_id = module_unit_id;
      DART_LOG_TRACE("dart__base__locality__create_subdomains: "
                     "NUMA unit %d of %d: unit id %d",
                     numa_unit_idx, subdomain->num_units, numa_unit_id);
      subdomain->unit_ids[numa_unit_idx] = numa_unit_id;
      ++numa_unit_idx;
    }
  }
  return DART_OK;
}

dart_ret_t dart__base__locality__create_numa_subdomain(
  dart_host_topology_t   * host_topology,
  dart_domain_locality_t * numa_domain,
  dart_domain_locality_t * subdomain,
  int                      rel_idx)
{
  /* Loop splits into UMA segments within a NUMA domain or module.
   * Using balanced split, segments are assumed to be homogenous at
   * this level. */
  DART_LOG_TRACE("dart__base__locality__create_subdomains: "
                 "== SPLIT NUMA ==");
  DART_LOG_TRACE("dart__base__locality__create_subdomains: == %d of %d",
                 rel_idx, numa_domain->num_domains);
  size_t num_uma_units          = numa_domain->num_units /
                                  numa_domain->num_domains;
  subdomain->hwinfo.num_modules = 1;
  subdomain->num_nodes          = 1;
  subdomain->hwinfo.num_numa    = 1;
  subdomain->hwinfo.num_cores   = numa_domain->hwinfo.num_cores /
                                  numa_domain->num_domains;
  subdomain->num_units          = num_uma_units;
  subdomain->unit_ids           = malloc(subdomain->num_units *
                                         sizeof(dart_unit_t));
  for (int u = 0; u < subdomain->num_units; ++u) {
    int numa_unit_idx      = (rel_idx * num_uma_units) + u;
    dart_unit_t unit_id    = numa_domain->unit_ids[numa_unit_idx];
    subdomain->unit_ids[u] = unit_id;
    DART_LOG_TRACE("dart__base__locality__create_subdomains: "
                   "UMA unit %d of %d (NUMA unit %d): unit id %d",
                   u, subdomain->num_units, numa_unit_idx, unit_id);
    /* set host and domain tag of unit in unit locality map: */
    dart_unit_locality_t * unit_loc;
    DART_ASSERT_RETURNS(
      dart__base__unit_locality__at(unit_id, &unit_loc),
      DART_OK);
    DART_LOG_TRACE("dart__base__locality__create_subdomains: "
                   "setting unit %d domain_tag: %s host: %s",
                   unit_id, subdomain->domain_tag, subdomain->host);
    strncpy(unit_loc->domain_tag, subdomain->domain_tag,
            DART_LOCALITY_DOMAIN_TAG_MAX_SIZE);
  }
  return DART_OK;
}

dart_ret_t dart__base__locality__domain(
  const char              * domain_tag,
  dart_domain_locality_t ** locality)
{
  DART_LOG_DEBUG("dart__base__locality__domain() domain(%s)", domain_tag);

  dart_domain_locality_t * domain = &dart__base__locality__domain_root_;

  /* pointer to dot separator in front of tag part:  */
  char * dot_begin  = strchr(domain_tag, '.');
  /* pointer to begin of tag part: */
  char * part_begin = dot_begin + 1;
  /* Iterate tag (.1.2.3) by parts (1, 2, 3):        */
  int    level     = 0;
  while (dot_begin != NULL && *dot_begin != '\0' && *part_begin != '\0') {
    char * dot_end;
    /* domain tag part converted to int is relative index of child: */
    long   tag_part      = strtol(part_begin, &dot_end, 10);
    int    subdomain_idx = (int)(tag_part);
    if (domain == NULL) {
      /* tree leaf node reached before last tag part: */
      DART_LOG_ERROR("dart__base__locality__domain ! domain(%s): "
                     "subdomain at index %d in level %d is undefined",
                     domain_tag, subdomain_idx, level);
      return DART_ERR_INVAL;
    }
    if (domain->num_domains <= subdomain_idx) {
      /* child index out of range: */
      DART_LOG_ERROR("dart__base__locality__domain ! domain(%s): "
                     "subdomain index %d in level %d is out ouf bounds "
                     "(number of subdomains: %d)",
                     domain_tag, subdomain_idx, level, domain->num_domains);
      return DART_ERR_INVAL;
    }
    /* descend to child at relative index: */
    domain     = domain->domains + subdomain_idx;
    /* continue scanning of domain tag at next part: */
    dot_begin  = dot_end;
    part_begin = dot_end+1;
    level++;
  }
  *locality = domain;
  return DART_OK;
}

/* ======================================================================== *
 * Unit Locality                                                            *
 * ======================================================================== */

dart_ret_t dart__base__locality__unit_locality_init(
  dart_unit_locality_t * loc)
{
  DART_LOG_TRACE("dart__base__locality__unit_locality_init() loc: %p", loc);
  if (loc == NULL) {
    DART_LOG_ERROR("dart__base__locality__unit_locality_init ! null");
    return DART_ERR_INVAL;
  }
  loc->unit               = DART_UNDEFINED_UNIT_ID;
  loc->domain_tag[0]      = '\0';
  loc->host[0]            = '\0';
  loc->hwinfo.numa_id     = -1;
  loc->hwinfo.cpu_id      = -1;
  loc->hwinfo.num_cores   = -1;
  loc->hwinfo.min_threads = -1;
  loc->hwinfo.max_threads = -1;
  loc->hwinfo.max_cpu_mhz = -1;
  loc->hwinfo.min_cpu_mhz = -1;
  DART_LOG_TRACE("dart__base__locality__unit_locality_init >");
  return DART_OK;
}

dart_ret_t dart__base__locality__local_unit_new(
  dart_unit_locality_t * loc)
{
  DART_LOG_DEBUG("dart__base__locality__local_unit_new() loc(%p)", loc);
  if (loc == NULL) {
    DART_LOG_ERROR("dart__base__locality__local_unit_new ! null");
    return DART_ERR_INVAL;
  }
  dart_unit_t myid = DART_UNDEFINED_UNIT_ID;

  DART_ASSERT_RETURNS(dart__base__locality__unit_locality_init(loc), DART_OK);
  DART_ASSERT_RETURNS(dart_myid(&myid), DART_OK);

  dart_hwinfo_t * hwinfo;
  DART_ASSERT_RETURNS(dart_hwinfo(&hwinfo), DART_OK);

  /* assign global domain to unit locality descriptor: */
  strncpy(loc->domain_tag, ".", 1);
  loc->domain_tag[1] = '\0';

  dart_domain_locality_t * dloc;
  DART_ASSERT_RETURNS(dart_domain_locality(".", &dloc), DART_OK);

  loc->unit               = myid;
  loc->hwinfo             = *hwinfo;
  loc->hwinfo.num_cores   = 1;

  strncpy(loc->host, dloc->host, DART_LOCALITY_HOST_MAX_SIZE);

#ifdef DART_ENABLE_HWLOC
  hwloc_topology_t topology;
  hwloc_topology_init(&topology);
  hwloc_topology_load(topology);
  // Resolve number of threads per core:
  int n_cpus = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
  if (n_cpus > 0) {
    loc->hwinfo.min_threads = 1;
    loc->hwinfo.max_threads = n_cpus / dloc->hwinfo.num_cores;
  }
  hwloc_topology_destroy(topology);
#endif

#ifdef DART__ARCH__IS_MIC
  DART_LOG_TRACE("dart__base__locality__local_unit_new: "
                 "MIC architecture");

  if (loc->hwinfo.numa_id     <  0) { loc->hwinfo.numa_id      = 0; }
  if (loc->hwinfo.num_cores   <= 0) { loc->hwinfo.num_cores    = 1; }
  if (loc->hwinfo.min_cpu_mhz <= 0 || loc->hwinfo.max_cpu_mhz <= 0) {
    loc->hwinfo.min_cpu_mhz = 1100;
    loc->hwinfo.max_cpu_mhz = 1100;
  }
  loc->hwinfo.min_threads = loc->hwinfo.num_cores * 4;
  loc->hwinfo.max_threads = loc->hwinfo.num_cores * 4;
#endif
  if (loc->hwinfo.min_threads <= 0) {
    loc->hwinfo.min_threads = 1;
  }
  if (loc->hwinfo.max_threads <= 0) {
    loc->hwinfo.max_threads = 1;
  }
  if (loc->hwinfo.numa_id     <  0) {
    loc->hwinfo.numa_id     = 0;
  }

  DART_LOG_DEBUG("dart__base__locality__local_unit_new > loc(%p)", loc);
  return DART_OK;
}
