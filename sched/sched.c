/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

/*
 * sched.c - scheduler framework service comms module
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argz.h>
#include <libgen.h>
#include <errno.h>
#include <libgen.h>
#include <czmq.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <flux/core.h>

#include "src/common/libutil/log.h"
#include "src/common/libutil/shortjansson.h"
#include "src/common/libutil/xzmalloc.h"
#include "resrc.h"
#include "resrc_tree.h"
#include "resrc_reqst.h"
#include "rs2rank.h"
#include "rsreader.h"
#include "scheduler.h"
#include "plugin.h"

#include "../simulator/simulator.h"

#define DYNAMIC_SCHEDULING 0
#define ENABLE_TIMER_EVENT 0
#define SCHED_UNIMPL -1
#define GET_ROOT_RESRC(rsapi) resrc_tree_resrc (resrc_tree_root ((rsapi)))

#if ENABLE_TIMER_EVENT
static int timer_event_cb (flux_t *h, void *arg);
#endif
static void ev_prep_cb (flux_reactor_t *r, flux_watcher_t *w,
                        int revents, void *arg);
static void ev_check_cb (flux_reactor_t *r, flux_watcher_t *w,
                         int revents, void *arg);
static void res_event_cb (flux_t *h, flux_msg_handler_t *w,
                          const flux_msg_t *msg, void *arg);
static void cancel_request_cb (flux_t *h, flux_msg_handler_t *w,
                               const flux_msg_t *msg, void *arg);
static void exclude_request_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg);
static void include_request_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg);
static void sched_params_set_request_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg);
static void sched_params_get_request_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg);
static int job_status_cb (const char *jcbstr, void *arg, int errnum);



/******************************************************************************
 *                                                                            *
 *              Scheduler Framework Service Module Context                    *
 *                                                                            *
 ******************************************************************************/

typedef struct {
    json_t  *jcb;
    void         *arg;
    int           errnum;
} jsc_event_t;

typedef struct {
    flux_t       *h;
    void         *arg;
} res_event_t;

typedef struct {
    bool          in_sim;
    sim_state_t  *sim_state;
    zlist_t      *res_queue;
    zlist_t      *jsc_queue;
    zlist_t      *timer_queue;
} simctx_t;

typedef struct {
    char         *path;
    char         *uri;
    char         *userplugin;
    char         *userplugin_opts;
    char         *prio_plugin;
    bool          reap;               /* Enable job reap support */
    bool          node_excl;          /* Node exclusive */
    bool          sim;
    bool          schedonce;          /* Use resources only once */
    bool          fail_on_error;      /* Fail immediately on error */
    int           verbosity;
    rsreader_t    r_mode;
    sched_params_t s_params;
} ssrvarg_t;

/* TODO: Implement prioritization function for p_queue */
typedef struct {
    flux_t       *h;
    zhash_t      *job_index;          /* For fast job lookup for all queues*/
    zlist_t      *p_queue;            /* Pending job priority queue */
    bool          pq_state;           /* schedulable state change in p_queue */
    zlist_t      *r_queue;            /* Running job queue */
    zlist_t      *c_queue;            /* Complete/cancelled job queue */
    machs_t      *machs;              /* Helps resolve resources to ranks */
    bool          ooo_capable;        /* sched policy schedule jobs out of order */
    ssrvarg_t     arg;                /* args passed to this module */
    simctx_t      sctx;               /* simulator context */
    resrc_api_ctx_t *rsapi;           /* resrc_api handle */
    struct sched_plugin_loader *loader; /* plugin loader */
    flux_watcher_t *before;
    flux_watcher_t *after;
    flux_watcher_t *idle;
    flux_msg_handler_t **handlers;
    flux_msg_handler_t **sim_handlers;
} ssrvctx_t;

static int schedule_jobs (ssrvctx_t *ctx);  /* Forward declaration */

/******************************************************************************
 *                                                                            *
 *                                 Utilities                                  *
 *                                                                            *
 ******************************************************************************/

static inline void sched_params_default (sched_params_t *params)
{
    params->queue_depth = SCHED_PARAM_Q_DEPTH_DEFAULT;
    params->delay_sched = SCHED_PARAM_DELAY_DEFAULT;
    /* set other scheduling parameters to their default values here */
}

static inline int sched_params_args (char *arg, sched_params_t *params)
{
    int rc = 0;
    char *argz = NULL;
    size_t argz_len = 0;
    const char *e = NULL;
    char *o_arg = NULL;
    long val = 0;

    argz_create_sep (arg, ',', &argz, &argz_len);
    while ((e = argz_next (argz, argz_len, e))) {
        if (!strncmp ("queue-depth=", e, sizeof ("queue-depth"))) {
            o_arg = strstr(e, "=") + 1;
            val = strtol(o_arg, (char **) NULL, 10);
            if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
                   || (errno != 0 && val == 0))
                rc = -1;
            else
                params->queue_depth = val;
        } else if (!strncmp ("delay-sched=", e, sizeof ("delay-sched"))) {
            if (!strncmp ((strstr(e, "=") + 1), "true", sizeof ("true")))
                params->delay_sched = true;
            else if (!strncmp ((strstr(e, "=") + 1), "false", sizeof ("false")))
                params->delay_sched = false;
            else {
                errno = EINVAL;
                rc = -1;
            }
        } else {
           errno = EINVAL;
           rc = -1;
        }

        if (rc != 0)
            break;
    }

    if (argz)
        free (argz);
    return rc;
}

static inline void ssrvarg_init (ssrvarg_t *arg)
{
    arg->path = NULL;
    arg->uri = NULL;
    arg->userplugin = NULL;
    arg->userplugin_opts = NULL;
    arg->prio_plugin = NULL;
    arg->reap = false;
    arg->node_excl = false;
    arg->sim = false;
    arg->schedonce = false;
    arg->fail_on_error = false;
    arg->verbosity = 0;
    sched_params_default (&(arg->s_params));
}

static inline void ssrvarg_free (ssrvarg_t *arg)
{
    free (arg->path);
    free (arg->uri);
    free (arg->userplugin);
    free (arg->userplugin_opts);
    free (arg->prio_plugin);
}

static inline int ssrvarg_process_args (int argc, char **argv, ssrvarg_t *a)
{
    int i = 0, rc = 0;
    char *reap = NULL;
    char *schedonce = NULL;
    char *node_excl = NULL;
    char *immediate = NULL;
    char *vlevel= NULL;
    char *sim = NULL;
    char *sprms = NULL;
    for (i = 0; i < argc; i++) {
        if (!strncmp ("rdl-conf=", argv[i], sizeof ("rdl-conf"))) {
            a->path = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("reap=", argv[i], sizeof ("reap"))) {
            a->reap = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("node-excl=", argv[i], sizeof ("node-excl"))) {
            node_excl = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("sched-once=", argv[i], sizeof ("sched-once"))) {
            schedonce = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("fail-on-error=", argv[i],
                    sizeof ("fail-on-error"))) {
            immediate = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("verbosity=", argv[i], sizeof ("verbosity"))) {
            vlevel = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("rdl-resource=", argv[i], sizeof ("rdl-resource"))) {
            a->uri = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("in-sim=", argv[i], sizeof ("in-sim"))) {
            sim = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("plugin=", argv[i], sizeof ("plugin"))) {
            a->userplugin = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("plugin-opts=", argv[i], sizeof ("plugin-opts"))) {
            a->userplugin_opts = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("priority-plugin=", argv[i],
                             sizeof ("priority-plugin"))) {
            a->prio_plugin = xstrdup (strstr (argv[i], "=") + 1);
        } else if (!strncmp ("sched-params=", argv[i], sizeof ("sched-params"))) {
            sprms = xstrdup (strstr (argv[i], "=") + 1);
        } else {
            rc = -1;
            errno = EINVAL;
            goto done;
        }
    }

    if (!(a->userplugin))
        a->userplugin = xstrdup ("sched.fcfs");

    if (reap && !strncmp (reap, "true", sizeof ("true"))) {
        a->reap = true;
        free (reap);
    }
    if (sim && !strncmp (sim, "true", sizeof ("true"))) {
        a->sim = true;
        free (sim);
    }
    if (node_excl && !strncmp (node_excl, "true", sizeof ("true"))) {
        a->node_excl = true;
        free (node_excl);
    }
    if (schedonce && !strncmp (schedonce, "true", sizeof ("true"))) {
        a->schedonce = true;
        free (schedonce);
    }
    if (immediate && !strncmp (immediate, "true", sizeof ("true"))) {
        a->fail_on_error = true;
        free (immediate);
    }
    if (vlevel) {
         a->verbosity = strtol(vlevel, (char **)NULL, 10);
         free (vlevel);
    }
    if (a->path)
        a->r_mode = (a->sim)? RSREADER_RESRC_EMUL : RSREADER_RESRC;
    else
        a->r_mode = RSREADER_HWLOC;
    if (sprms)
        rc = sched_params_args (sprms, &(a->s_params));
done:
    return rc;
}

static int adjust_for_sched_params (ssrvctx_t *ctx)
{
    flux_reactor_t *r = NULL;
    int rc = 0;
    flux_msg_t *msg = NULL;
    /* delay_sched = true.
    Watchers need to be created the first time delay_sched is set to true.
    Otherwise, start the prepare/check watchers. The idle watcher is started by the ev_prep_cb. */
    if (ctx->arg.s_params.delay_sched && !ctx->sctx.in_sim) {
        if (ctx->before && ctx->after) {                    /* Watchers exist */
            flux_watcher_start (ctx->before);
            flux_watcher_start (ctx->after);
        }

        /* If one of the watchers is not available, send error */
        if ((!ctx->before && ctx->after) || (!ctx->after && ctx->before)) {
            rc = -1;
            errno = EINVAL;
            goto done;
        }

        if (!ctx->before && !ctx->after) {      /* Create and start watchers */
            if (!(r = flux_get_reactor (ctx->h))) {
                rc = -1;
                goto done;
            }
            if (!(ctx->before = flux_prepare_watcher_create (r, ev_prep_cb, ctx))) {
                rc = -1;
                goto done;
            }
            if (!(ctx->after = flux_check_watcher_create (r, ev_check_cb, ctx))) {
                rc = -1;
                goto done;
            }
            /* idle watcher makes sure the check watcher (after) is called
            even with no external events delivered */
            if (!(ctx->idle = flux_idle_watcher_create (r, NULL, NULL))) {
                rc = -1;
                goto done;
            }
            flux_watcher_start (ctx->before);
            flux_watcher_start (ctx->after);
        }
    } // End if, delay = true

    /* delay_sched = false.
    If set at runtime, stop the watchers, call schedule_jobs */
    if (!ctx->arg.s_params.delay_sched && !ctx->sctx.in_sim) {
        if (ctx->before && ctx->after) {
            flux_watcher_stop (ctx->before);
            flux_watcher_stop (ctx->after);

            /* Create an event to schedule_jobs */
            flux_log (ctx->h, LOG_DEBUG, "Update delay_sched parameter");
            msg = flux_event_encode ("sched.res.param_update", NULL);
            if (!msg || flux_send (ctx->h, msg, 0) < 0) {
                flux_log (ctx->h, LOG_ERR, "%s: error sending event: %s",
                __FUNCTION__, strerror (errno));
            }
            flux_msg_destroy (msg);
        }
        /* If one of the watchers is not available, send error */
        if ((!ctx->before && ctx->after) || (!ctx->after && ctx->before)) {
            rc = -1;
            errno = EINVAL;
            goto done;
        }
    } // End if, delay = false
done:
    return rc;
}


static void freectx (void *arg)
{
    ssrvctx_t *ctx = arg;
    zhash_destroy (&(ctx->job_index));
    zlist_destroy (&(ctx->p_queue));
    zlist_destroy (&(ctx->r_queue));
    zlist_destroy (&(ctx->c_queue));
    rs2rank_tab_destroy (ctx->machs);
    ssrvarg_free (&(ctx->arg));
    resrc_tree_destroy (ctx->rsapi, resrc_tree_root (ctx->rsapi), true, true);
    resrc_api_fini (ctx->rsapi);
    free_simstate (ctx->sctx.sim_state);
    if (ctx->sctx.res_queue)
        zlist_destroy (&(ctx->sctx.res_queue));
    if (ctx->sctx.jsc_queue)
        zlist_destroy (&(ctx->sctx.jsc_queue));
    if (ctx->sctx.timer_queue)
        zlist_destroy (&(ctx->sctx.timer_queue));
    if (ctx->loader)
        sched_plugin_loader_destroy (ctx->loader);
    if (ctx->before)
        flux_watcher_destroy (ctx->before);
    if (ctx->after)
        flux_watcher_destroy (ctx->after);
    if (ctx->idle)
        flux_watcher_destroy (ctx->idle);
    flux_msg_handler_delvec (ctx->handlers);
    flux_msg_handler_delvec (ctx->sim_handlers);
    free (ctx);
}

static ssrvctx_t *getctx (flux_t *h)
{
    ssrvctx_t *ctx = (ssrvctx_t *)flux_aux_get (h, "sched");
    if (!ctx) {
        ctx = xzmalloc (sizeof (*ctx));
        ctx->h = h;
        if (!(ctx->job_index = zhash_new ()))
            oom ();
        if (!(ctx->p_queue = zlist_new ()))
            oom ();
        ctx->pq_state = false;
        if (!(ctx->r_queue = zlist_new ()))
            oom ();
        if (!(ctx->c_queue = zlist_new ()))
            oom ();
        if (!(ctx->machs = rs2rank_tab_new ()))
            oom ();
        ctx->ooo_capable = true;
        ssrvarg_init (&(ctx->arg));
        ctx->rsapi = resrc_api_init ();
        ctx->sctx.in_sim = false;
        ctx->sctx.sim_state = NULL;
        ctx->sctx.res_queue = NULL;
        ctx->sctx.jsc_queue = NULL;
        ctx->sctx.timer_queue = NULL;
        ctx->loader = NULL;
        ctx->before = NULL;
        ctx->after = NULL;
        ctx->idle = NULL;
        ctx->handlers = NULL;
        ctx->sim_handlers = NULL;
        flux_aux_set (h, "sched", ctx, freectx);
    }
    return ctx;
}

static inline void get_jobid (json_t *jcb, int64_t *jid)
{
    Jget_int64 (jcb, JSC_JOBID, jid);
}

static inline void get_states (json_t *jcb, int64_t *os, int64_t *ns)
{
    json_t *o = NULL;
    Jget_obj (jcb, JSC_STATE_PAIR, &o);
    Jget_int64 (o, JSC_STATE_PAIR_OSTATE, os);
    Jget_int64 (o, JSC_STATE_PAIR_NSTATE, ns);
}

static inline int fill_resource_req (flux_t *h, flux_lwj_t *j, json_t *jcb)
{
    int rc = -1;
    int64_t nn = 0;
    int64_t nc = 0;
    int64_t ngpus = 0;
    int64_t walltime = 0;
    json_t *o = NULL;
    ssrvctx_t *ctx = getctx (h);

    j->req = (flux_res_t *) xzmalloc (sizeof (flux_res_t));
    /* TODO:  add user name and charge account to info the JSC provides */
    j->user = NULL;
    j->account = NULL;

    if (!Jget_obj (jcb, JSC_RDESC, &o))
        goto done;
    if (!Jget_int64 (o, JSC_RDESC_NNODES, &nn))
        goto done;
    if (!Jget_int64 (o, JSC_RDESC_NCORES, &nc))
        goto done;
    if (!Jget_int64 (o, JSC_RDESC_NGPUS, &ngpus))
        goto done;

    j->req->nnodes = (uint64_t) nn;
    j->req->ncores = (uint64_t) nc;
    j->req->ngpus = (uint64_t) ngpus;
    if (!Jget_int64 (o, JSC_RDESC_WALLTIME, &walltime) || !walltime) {
        j->req->walltime = (uint64_t) 3600;
    } else {
        j->req->walltime = (uint64_t) walltime;
    }
    j->req->node_exclusive = ctx->arg.node_excl;
    rc = 0;
done:
    return rc;
}

static int update_state (flux_t *h, uint64_t jid, job_state_t os, job_state_t ns)
{
    char *jcbstr = NULL;
    int rc = -1;
    json_t *jcb = Jnew ();
    json_t *o = Jnew ();
    Jadd_int64 (o, JSC_STATE_PAIR_OSTATE, (int64_t) os);
    Jadd_int64 (o, JSC_STATE_PAIR_NSTATE , (int64_t) ns);
    /* don't want to use Jadd_obj because I want to transfer the ownership */
    json_object_set_new (jcb, JSC_STATE_PAIR, o);
    jcbstr = Jtostr (jcb);
    rc = jsc_update_jcb (h, jid, JSC_STATE_PAIR, jcbstr);
    Jput (jcb);
    free (jcbstr);
    return rc;
}

static inline bool is_newjob (json_t *jcb)
{
    int64_t os = J_NULL, ns = J_NULL;
    get_states (jcb, &os, &ns);
    return ((os == J_NULL) && (ns == J_SUBMITTED))? true : false;
}

static int plugin_process_args (ssrvctx_t *ctx, char *userplugin_opts)
{
    int rc = -1;
    char *argz = NULL;
    size_t argz_len = 0;
    struct behavior_plugin *plugin = behavior_plugin_get (ctx->loader);
    const sched_params_t *sp = &(ctx->arg.s_params);

    if (userplugin_opts)
        argz_create_sep (userplugin_opts, ',', &argz, &argz_len);
    if (plugin->process_args (ctx->h, argz, argz_len, sp) < 0)
        goto done;

    rc = 0;

 done:
    free (argz);

    return rc;
}


/********************************************************************************
 *                                                                              *
 *                          Simple Job Queue Methods                            *
 *                                                                              *
 *******************************************************************************/

static int q_enqueue_into_pqueue (ssrvctx_t *ctx, json_t *jcb)
{
    int rc = -1;
    int64_t jid = -1;
    char *key = NULL;
    flux_lwj_t *job = NULL;

    get_jobid (jcb, &jid);
    if ( !(job = (flux_lwj_t *) xzmalloc (sizeof (*job))))
        oom ();

    job->lwj_id = jid;
    job->state = J_NULL;
    job->submittime = time (NULL);
    if (zlist_append (ctx->p_queue, job) != 0) {
        flux_log (ctx->h, LOG_ERR, "failed to append to pending job queue.");
        goto done;
    }
    job->enqueue_pos = (int64_t)zlist_size (ctx->p_queue);
    key = xasprintf ("%"PRId64"", jid);
    if (zhash_insert(ctx->job_index, key, job) != 0) {
        flux_log (ctx->h, LOG_ERR, "failed to index a job.");
        goto done;
    }
    /* please don't free the job using job_index; this is just a lookup table */
    rc = 0;
done:
    free (key);
    return rc;
}

static flux_lwj_t *q_find_job (ssrvctx_t *ctx, int64_t id)
{
    flux_lwj_t *j = NULL;
    char *key = NULL;
    key = xasprintf ("%"PRId64"", id);
    j = zhash_lookup (ctx->job_index, key);
    free (key);
    return j;
}

static int q_mark_schedulability (ssrvctx_t *ctx, flux_lwj_t *job)
{
    if (ctx->pq_state == false
        && job->enqueue_pos <= ctx->arg.s_params.queue_depth) {
        ctx->pq_state = true;
        return 0;
    }
    return -1;
}

static void q_rm_from_pqueue (ssrvctx_t *ctx, flux_lwj_t *j)
{
    zlist_remove (ctx->p_queue, j);
    /* dequeue operation should always be a schedulable queue operation */
    if (ctx->pq_state == false)
        ctx->pq_state = true;
}

static void q_rm_from_rqueue (ssrvctx_t *ctx, flux_lwj_t *j)
{
    zlist_remove (ctx->r_queue, j);
    /* dequeue operation should always be a schedulable queue operation */
    if (ctx->pq_state == false)
        ctx->pq_state = true;
}

static int q_move_to_rqueue (ssrvctx_t *ctx, flux_lwj_t *j)
{
    zlist_remove (ctx->p_queue, j);
    /* dequeue operation should always be a schedulable queue operation */
    if (ctx->pq_state == false)
        ctx->pq_state = true;
    return zlist_append (ctx->r_queue, j);
}

static int q_move_to_cqueue (ssrvctx_t *ctx, flux_lwj_t *j)
{
    zlist_remove (ctx->r_queue, j);
    /* dequeue operation should always be a schedulable queue operation */
    if (ctx->pq_state == false)
        ctx->pq_state = true;
    return zlist_append (ctx->c_queue, j);
}

static flux_lwj_t *fetch_job_and_event (ssrvctx_t *ctx, json_t *jcb,
                                        job_state_t *ns)
{
    int64_t jid = -1, os64 = 0, ns64 = 0;
    get_jobid (jcb, &jid);
    get_states (jcb, &os64, &ns64);
    *ns = (job_state_t) ns64;
    return q_find_job (ctx, jid);
}

/******************************************************************************
 *                                                                            *
 *                   Setting Up RDL (RFC 4)                                   *
 *                                                                            *
 ******************************************************************************/

static void setup_rdl_lua (flux_t *h)
{
    flux_log (h, LOG_DEBUG, "LUA_PATH %s", getenv ("LUA_PATH"));
    flux_log (h, LOG_DEBUG, "LUA_CPATH %s", getenv ("LUA_CPATH"));
}

/* Block until value of 'key' becomes non-NULL.
 * It is an EPROTO error if value is type other than json_type_string.
 * On success returns value, otherwise NULL with errno set.
 */
static json_t *get_string_blocking (flux_t *h, const char *key)
{
    char *json_str = NULL; /* initial value for watch */
    json_t *o = NULL;
    int saved_errno;

    if (flux_kvs_watch_once (h, key, &json_str) < 0) {
        saved_errno = errno;
        goto error;
    }

    if (!json_str || !(o = Jfromstr (json_str))
                  || !json_is_string (o)) {
        saved_errno = EPROTO;
        goto error;
    }
    free (json_str);
    return o;
error:
    free (json_str);
    Jput (o);
    errno = saved_errno;
    return NULL;
}

static int build_hwloc_rs2rank (ssrvctx_t *ctx, rsreader_t r_mode)
{
    int rc = -1;
    uint32_t rank = 0, size = 0;

    if (flux_get_size (ctx->h, &size) == -1) {
        flux_log_error (ctx->h, "flux_get_size");
        goto done;
    }
    for (rank=0; rank < size; rank++) {
        json_t *o;
        char k[64];
        int n = snprintf (k, sizeof (k), "resource.hwloc.xml.%"PRIu32"", rank);
        assert (n < sizeof (k));
        if (!(o = get_string_blocking (ctx->h, k))) {
            flux_log_error (ctx->h, "kvs_get %s", k);
            goto done;
        }
        const char *s = json_string_value (o);
        char *err_str = NULL;
        size_t len = strlen (s);
        if (rsreader_hwloc_load (ctx->rsapi, s, len, rank, r_mode, ctx->machs,
                                 &err_str)) {
            Jput (o);
            flux_log (ctx->h, LOG_ERR, "can't load hwloc data: %s", err_str);
            free (err_str);
            goto done;
        }
        Jput (o);
    }
    rc = 0;

done:
    return rc;
}

static void dump_resrc_state (flux_t *h, resrc_tree_t *rt)
{
    char *str;
    if (!rt)
        return;
    str = resrc_to_string (resrc_tree_resrc (rt));
    flux_log (h, LOG_INFO, "%s", str);
    free (str);
    if (resrc_tree_num_children (rt)) {
        resrc_tree_t *child = resrc_tree_list_first (resrc_tree_children (rt));
        while (child) {
            dump_resrc_state (h, child);
            child = resrc_tree_list_next (resrc_tree_children (rt));
        }
    }
    return;
}

static int load_resources (ssrvctx_t *ctx)
{
    int rc = -1;
    char *e_str = NULL;
    char *path = ctx->arg.path;
    char *uri = ctx->arg.uri;
    rsreader_t r_mode = ctx->arg.r_mode;

    setup_rdl_lua (ctx->h);

    flux_log (ctx->h, LOG_INFO, "start to read resources");

    switch (r_mode) {
    case RSREADER_RESRC_EMUL:
        if (rsreader_resrc_bulkload (ctx->rsapi, path, uri) != 0) {
            flux_log (ctx->h, LOG_ERR, "failed to load resrc");
            errno = EINVAL;
            goto done;
        } else if (build_hwloc_rs2rank (ctx, r_mode) != 0) {
            flux_log (ctx->h, LOG_ERR, "failed to build rs2rank");
            errno = EINVAL;
            goto done;
        } else if (rsreader_force_link2rank (ctx->rsapi, ctx->machs) != 0) {
            flux_log (ctx->h, LOG_ERR, "failed to force a link to a rank");
            errno = EINVAL;
            goto done;
        }
        flux_log (ctx->h, LOG_INFO, "loaded resrc");
        rc = 0;
        break;

    case RSREADER_RESRC:
        if (rsreader_resrc_bulkload (ctx->rsapi, path, uri) != 0) {
            flux_log (ctx->h, LOG_ERR, "failed to load resrc");
            errno = EINVAL;
            goto done;
        } else if (build_hwloc_rs2rank (ctx, r_mode) != 0) {
            flux_log (ctx->h, LOG_ERR, "failed to build rs2rank");
            errno = EINVAL;
            goto done;
        }
        flux_log (ctx->h, LOG_INFO, "resrc constructed using RDL ");
        if (ctx->arg.verbosity > 0) {
            flux_log (ctx->h, LOG_INFO, "resrc state after resrc read");
            dump_resrc_state (ctx->h, resrc_tree_root (ctx->rsapi));
        }
        if (rsreader_link2rank (ctx->rsapi, ctx->machs, &e_str) != 0) {
            flux_log (ctx->h, LOG_INFO, "RDL(%s) inconsistent w/ hwloc!", path);
            if (e_str) {
                flux_log (ctx->h, LOG_INFO, "%s", e_str);
                free (e_str);
            }
            if (ctx->arg.fail_on_error)
                goto done; /* don't set errno: only needed for testing */

            flux_log (ctx->h, LOG_INFO, "rebuild resrc using hwloc");
            resrc_tree_t *mismatch_root = resrc_tree_root (ctx->rsapi);
            if (mismatch_root)
                resrc_tree_destroy (ctx->rsapi, mismatch_root, true, true);
            r_mode = RSREADER_HWLOC;
            /* deliberate fall-through to RSREADER_HWLOC! */
        } else {
            flux_log (ctx->h, LOG_INFO, "loaded resrc");
            rc = 0;
            break;
        }

    case RSREADER_HWLOC:
        if (build_hwloc_rs2rank (ctx, r_mode) != 0) {
            flux_log (ctx->h, LOG_ERR, "failed to load resrc using hwloc");
            errno = EINVAL;
            goto done;
        }
        flux_log (ctx->h, LOG_INFO, "resrc constructed using hwloc");
        /* linking has already been done by build_hwloc_rs2rank above */
        if (ctx->arg.verbosity > 0) {
            flux_log (ctx->h, LOG_INFO, "resrc state after hwloc read");
            dump_resrc_state (ctx->h, resrc_tree_root (ctx->rsapi));
        }
        flux_log (ctx->h, LOG_INFO, "loaded resrc");
        rc = 0;
        break;

    default:
        flux_log (ctx->h, LOG_ERR, "unkwown resource reader type");
        break;
    }

done:
    return rc;
}


/******************************************************************************
 *                                                                            *
 *                         Emulator Specific Code                             *
 *                                                                            *
 ******************************************************************************/

/*
 * Simulator Helper Functions
 */
static void queue_timer_change (ssrvctx_t *ctx, const char *module)
{
    zlist_append (ctx->sctx.timer_queue, (void *)module);
}

// Set the timer for "module" to happen relatively soon
// If the mod is sim_exec, it shouldn't happen immediately
// because the scheduler still needs to transition through
// 3->4 states before the sim_exec module can actually "exec" a job
static void set_next_event (const char *module, sim_state_t *sim_state)
{
    double next_event;
    double *timer = zhash_lookup (sim_state->timers, module);
    next_event =
        sim_state->sim_time + ((!strcmp (module, "sim_exec")) ? .0001 : .00001);
    if (*timer > next_event || *timer < 0) {
        *timer = next_event;
    }
}

static void handle_timer_queue (ssrvctx_t *ctx, sim_state_t *sim_state)
{
    while (zlist_size (ctx->sctx.timer_queue) > 0)
        set_next_event (zlist_pop (ctx->sctx.timer_queue), sim_state);

#if ENABLE_TIMER_EVENT
    // Set scheduler loop to run in next occuring scheduler block
    double *this_timer = zhash_lookup (sim_state->timers, "sched");
    double next_schedule_block =
        sim_state->sim_time
        + (SCHED_INTERVAL - ((int)sim_state->sim_time % SCHED_INTERVAL));
    if (ctx->run_schedule_loop &&
        ((next_schedule_block < *this_timer || *this_timer < 0))) {
        *this_timer = next_schedule_block;
    }
    flux_log (ctx->h,
              LOG_DEBUG,
              "run_sched_loop: %d, next_schedule_block: %f, this_timer: %f",
              ctx->run_schedule_loop,
              next_schedule_block,
              *this_timer);
#endif
}

static void handle_jsc_queue (ssrvctx_t *ctx)
{
    jsc_event_t *jsc_event = NULL;

    while (zlist_size (ctx->sctx.jsc_queue) > 0) {
        char *jcbstr;
        jsc_event = (jsc_event_t *)zlist_pop (ctx->sctx.jsc_queue);
        jcbstr = Jtostr (jsc_event->jcb);
        flux_log (ctx->h,
                  LOG_DEBUG,
                  "JscEvent being handled - JSON: %s, errnum: %d",
                  jcbstr, jsc_event->errnum);
        job_status_cb (jcbstr, jsc_event->arg, jsc_event->errnum);
        Jput (jsc_event->jcb);
        free (jsc_event);
        free (jcbstr);
    }
}

static void handle_res_queue (ssrvctx_t *ctx)
{
    res_event_t *res_event = NULL;

    while (zlist_size (ctx->sctx.res_queue) > 0) {
        res_event = (res_event_t *)zlist_pop (ctx->sctx.res_queue);
        flux_log (ctx->h,
                  LOG_DEBUG,
                  "ResEvent being handled");
        res_event_cb (res_event->h, NULL, NULL, res_event->arg);
        free (res_event);
    }
}

/*
 * Simulator Callbacks
 */
static void start_cb (flux_t *h,
                      flux_msg_handler_t *w,
                      const flux_msg_t *msg,
                      void *arg)
{
    flux_log (h, LOG_DEBUG, "received a start event");
    if (send_join_request (h, "sched", -1) < 0) {
        flux_log (h,
                  LOG_ERR,
                  "submit module failed to register with sim module");
        return;
    }
    flux_log (h, LOG_DEBUG, "sent a join request");

    if (flux_event_unsubscribe (h, "sim.start") < 0) {
        flux_log (h, LOG_ERR, "failed to unsubscribe from \"sim.start\"");
        return;
    } else {
        flux_log (h, LOG_DEBUG, "unsubscribed from \"sim.start\"");
    }

    return;
}

static int sim_job_status_cb (const char *jcbstr, void *arg, int errnum)
{
    char *s;
    json_t *jcb = NULL;
    ssrvctx_t *ctx = getctx ((flux_t *)arg);
    jsc_event_t *event = (jsc_event_t*) xzmalloc (sizeof (jsc_event_t));

    if (errnum > 0) {
        flux_log (ctx->h, LOG_ERR, "%s: errnum passed in", __FUNCTION__);
        return -1;
    }

    if (!(jcb = Jfromstr (jcbstr))) {
        flux_log (ctx->h, LOG_ERR, "%s: error parsing JSON string",
                  __FUNCTION__);
        return -1;
    }

    event->jcb = Jget (jcb);
    event->arg = arg;
    event->errnum = errnum;

    s = Jtostr (event->jcb);
    flux_log (ctx->h,
              LOG_DEBUG,
              "JscEvent being queued - JSON: %s, errnum: %d",
              s, event->errnum);
    free (s);
    zlist_append (ctx->sctx.jsc_queue, event);
    return 0;
}

static void sim_res_event_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg) {
    ssrvctx_t *ctx = getctx ((flux_t *)arg);
    res_event_t *event = (res_event_t*) xzmalloc (sizeof (res_event_t));
    const char *topic = NULL;

    event->h = h;
    event->arg = arg;

    flux_msg_get_topic (msg, &topic);
    flux_log (ctx->h,
              LOG_DEBUG,
              "ResEvent being queued - topic: %s",
              topic);
    zlist_append (ctx->sctx.res_queue, event);
}

static void trigger_cb (flux_t *h,
                        flux_msg_handler_t *w,
                        const flux_msg_t *msg,
                        void *arg)
{
    clock_t start, diff;
    double seconds;
    bool sched_loop;
    const char *json_str = NULL;
    json_t *o = NULL;
    ssrvctx_t *ctx = getctx (h);

    if (flux_request_decode (msg, NULL, &json_str) < 0 || json_str == NULL
        || !(o = Jfromstr (json_str))) {
        flux_log (h, LOG_ERR, "%s: bad message", __FUNCTION__);
        Jput (o);
        return;
    }

    flux_log (h, LOG_DEBUG, "Setting sim_state to new values");
    ctx->sctx.sim_state = json_to_sim_state (o);
    ev_prep_cb (NULL, NULL, 0, ctx);

    start = clock ();

    handle_jsc_queue (ctx);
    handle_res_queue (ctx);

    sched_loop = true;
    diff = clock () - start;
    seconds = ((double)diff) / CLOCKS_PER_SEC;
    ctx->sctx.sim_state->sim_time += seconds;
    if (sched_loop) {
        flux_log (h,
                  LOG_DEBUG,
                  "scheduler timer: events + loop took %f seconds",
                  seconds);
    } else {
        flux_log (h,
                  LOG_DEBUG,
                  "scheduler timer: events took %f seconds",
                  seconds);
    }

    ev_check_cb (NULL, NULL, 0, ctx);
    handle_timer_queue (ctx, ctx->sctx.sim_state);

    send_reply_request (h, "sched", ctx->sctx.sim_state);

    free_simstate (ctx->sctx.sim_state);
    ctx->sctx.sim_state = NULL;
    Jput (o);
}


/******************************************************************************
 *                                                                            *
 *                     Scheduler Eventing For Emulation Mode                  *
 *                                                                            *
 ******************************************************************************/

/*
 * Simulator Initialization Functions
 */
static const struct flux_msg_handler_spec sim_htab[] = {
    {FLUX_MSGTYPE_EVENT, "sim.start", start_cb, 0},
    {FLUX_MSGTYPE_REQUEST, "sched.trigger", trigger_cb, 0},
    {FLUX_MSGTYPE_EVENT, "sched.res.*", sim_res_event_cb, 0},
    FLUX_MSGHANDLER_TABLE_END,
};

static int reg_sim_events (ssrvctx_t *ctx)
{
    int rc = -1;
    flux_t *h = ctx->h;

    if (flux_event_subscribe (ctx->h, "sim.start") < 0) {
        flux_log (ctx->h, LOG_ERR, "subscribing to event: %s", strerror (errno));
        goto done;
    }
    if (flux_event_subscribe (ctx->h, "sched.res.") < 0) {
        flux_log (ctx->h, LOG_ERR, "subscribing to event: %s", strerror (errno));
        goto done;
    }
    if (flux_msg_handler_addvec (ctx->h, sim_htab, (void *)h,
                                 &ctx->sim_handlers) < 0) {
        flux_log (ctx->h, LOG_ERR, "flux_msg_handler_addvec: %s", strerror (errno));
        goto done;
    }
    if (jsc_notify_status (h, sim_job_status_cb, (void *)h) != 0) {
        flux_log (h, LOG_ERR, "error registering a job status change CB");
        goto done;
    }

    send_alive_request (ctx->h, "sched");

    rc = 0;
done:
    return rc;
}

static int setup_sim (ssrvctx_t *ctx, bool sim)
{
    int rc = 0;

    if (sim) {
        flux_log (ctx->h, LOG_INFO, "setting up sim in scheduler");
        ctx->sctx.in_sim = true;
        ctx->sctx.sim_state = NULL;
        ctx->sctx.res_queue = zlist_new ();
        ctx->sctx.jsc_queue = zlist_new ();
        ctx->sctx.timer_queue = zlist_new ();
    }
    else
        rc = -1;

    return rc;
}


/******************************************************************************
 *                                                                            *
 *                     Scheduler Eventing For Normal Mode                     *
 *                                                                            *
 ******************************************************************************/

static const struct flux_msg_handler_spec htab[] = {
    { FLUX_MSGTYPE_REQUEST,   "sched.cancel", cancel_request_cb, 0},
    { FLUX_MSGTYPE_REQUEST,   "sched.exclude",  exclude_request_cb, 0},
    { FLUX_MSGTYPE_REQUEST,   "sched.include",  include_request_cb, 0},
    { FLUX_MSGTYPE_REQUEST,   "sched.params.set", sched_params_set_request_cb, 0},
    { FLUX_MSGTYPE_REQUEST,   "sched.params.get", sched_params_get_request_cb, 0},
    { FLUX_MSGTYPE_EVENT,     "sched.res.*",  res_event_cb, 0},
    FLUX_MSGHANDLER_TABLE_END
};

/*
 * Register events, some of which CAN triger a scheduling loop iteration.
 * Currently,
 *    -  Resource event: invoke the schedule loop;
 *    -  Timer event: invoke the schedule loop;
 *    -  Job event (JSC notification): triggers actions based on FSM
 *          and some state changes trigger the schedule loop.
 */
static int inline reg_events (ssrvctx_t *ctx)
{
    int rc = 0;
    flux_t *h = ctx->h;

    if (flux_event_subscribe (h, "sched.res.") < 0) {
        flux_log (h, LOG_ERR, "subscribing to event: %s", strerror (errno));
        rc = -1;
        goto done;
    }
    if (flux_msg_handler_addvec (h, htab, (void *)h, &ctx->handlers) < 0) {
        flux_log (h, LOG_ERR,
                  "error registering resource event handler: %s",
                  strerror (errno));
        rc = -1;
        goto done;
    }
    /* TODO: we need a way to manage environment variables or
       configrations
    */
#if ENABLE_TIMER_EVENT
    if (flux_tmouthandler_add (h, 30000, false, timer_event_cb, (void *)h) < 0) {
        flux_log (h, LOG_ERR,
                  "error registering timer event CB: %s",
                  strerror (errno));
        rc = -1;
        goto done;
    }
#endif
    if (jsc_notify_status (h, job_status_cb, (void *)h) != 0) {
        flux_log (h, LOG_ERR, "error registering a job status change CB");
        rc = -1;
        goto done;
    }

done:
    return rc;
}


/******************************************************************************
 *                                                                            *
 *            Mode Bridging Layer to Hide Emulation vs. Normal Mode           *
 *                                                                            *
 ******************************************************************************/

static inline int bridge_set_execmode (ssrvctx_t *ctx)
{
    int rc = 0;
    if (ctx->arg.sim && setup_sim (ctx, ctx->arg.sim) != 0) {
        flux_log (ctx->h, LOG_ERR, "failed to setup sim mode");
        rc = -1;
        goto done;
    }
done:
    return rc;
}

static inline int bridge_set_events (ssrvctx_t *ctx)
{
    int rc = -1;
    if (ctx->sctx.in_sim) {
        if (reg_sim_events (ctx) != 0) {
            flux_log (ctx->h, LOG_ERR, "failed to reg sim events");
            goto done;
        }
        flux_log (ctx->h, LOG_INFO, "sim events registered");
    } else {
        if (reg_events (ctx) != 0) {
            flux_log (ctx->h, LOG_ERR, "failed to reg events");
            goto done;
        }
        flux_log (ctx->h, LOG_INFO, "events registered");
    }
    rc = 0;

done:
    return rc;
}

static inline int bridge_send_runrequest (ssrvctx_t *ctx, flux_lwj_t *job)
{
    int rc = -1;
    flux_t *h = ctx->h;
    char *topic = NULL;
    flux_msg_t *msg = NULL;

    if (ctx->sctx.in_sim) {
        /* Emulation mode */
        if (asprintf (&topic, "sim_exec.run.%"PRId64"", job->lwj_id) < 0) {
            flux_log (h, LOG_ERR, "%s: topic create failed: %s",
                      __FUNCTION__, strerror (errno));
        } else if (!(msg = flux_request_encode (topic, NULL))
                   || flux_send (h, msg, 0) < 0) {
            flux_log (h, LOG_ERR, "%s: request create failed: %s",
                      __FUNCTION__, strerror (errno));
        } else {
            queue_timer_change (ctx, "sim_exec");
            flux_log (h, LOG_DEBUG, "job %"PRId64" runrequest", job->lwj_id);
            rc = 0;
        }
    } else {
        /* Normal mode */
        if (asprintf (&topic, "wrexec.run.%"PRId64"", job->lwj_id) < 0) {
            flux_log (h, LOG_ERR, "%s: topic create failed: %s",
                      __FUNCTION__, strerror (errno));
        } else if (!(msg = flux_event_encode (topic, NULL))
                   || flux_send (h, msg, 0) < 0) {
            flux_log (h, LOG_ERR, "%s: event create failed: %s",
                      __FUNCTION__, strerror (errno));
        } else {
            flux_log (h, LOG_DEBUG, "job %"PRId64" runrequest", job->lwj_id);
            rc = 0;
        }
    }
    flux_msg_destroy (msg);
    free (topic);
    return rc;
}

static inline void bridge_update_timer (ssrvctx_t *ctx)
{
    if (ctx->sctx.in_sim)
        queue_timer_change (ctx, "sched");
}

static inline int bridge_rs2rank_tab_query (ssrvctx_t *ctx, const char *name,
                                            const char *digest, uint32_t *rank)
{
    int rc = -1;
    if (ctx->sctx.in_sim) {
        rc = rs2rank_tab_query_by_none (ctx->machs, digest, false, rank);
    } else {
        flux_log (ctx->h, LOG_INFO, "hostname: %s, digest: %s", name, digest);
        rc = rs2rank_tab_query_by_sign (ctx->machs, name, digest, false, rank);
    }
    if (rc == 0)
        flux_log (ctx->h, LOG_INFO, "broker found, rank: %"PRIu32, *rank);
    else
        flux_log (ctx->h, LOG_ERR, "controlling broker not found!");

    return rc;
}

/********************************************************************************
 *                                                                              *
 *            Task Program Execution Service Request (RFC 8)                    *
 *                                                                              *
 *******************************************************************************/

static int resolve_rank (ssrvctx_t *ctx, json_t *o)
{
    int rc = -1;
    size_t index = 0;
    json_t *value = NULL;

    json_array_foreach (o, index, value) {
        uint32_t rank = 0;
        char *hn = NULL;
        char *digest = NULL;
        if (json_unpack (value, "{s:s s:s}", "node", &hn, "digest", &digest))
            goto done;
        if (bridge_rs2rank_tab_query (ctx, hn, digest, &rank))
            goto done;

        json_t *j_rank = json_integer ((json_int_t)rank);
        if (json_object_del (value, "digest"))
            goto done;
        if (json_object_set_new (value, "rank", j_rank))
            goto done;
    }
    rc = 0;

done:
    return rc;
}


/*
 * Once the job gets allocated to its own copy of rdl, this
 *    1) serializes the rdl and sends it to TP exec service
 *    2) builds JSC_RDL_ALLOC JCB and sends it to TP exec service
 *    3) sends JCB state update with J_ALLOCATE
 */
static int req_tpexec_allocate (ssrvctx_t *ctx, flux_lwj_t *job)
{
    char *jcbstr = NULL;
    int rc = -1;
    flux_t *h = ctx->h;
    json_t *jcb = Jnew ();
    json_t *gat = Jnew_ar ();
    json_t *red = Jnew ();
    resrc_api_map_t *gmap = resrc_api_map_new ();
    resrc_api_map_t *rmap = resrc_api_map_new ();

    if (ctx->arg.verbosity > 0) {
        flux_log (h, LOG_DEBUG, "job(%"PRId64"): selected resource tree",
                  job->lwj_id);
        dump_resrc_state (ctx->h, job->resrc_tree);
    }

    resrc_api_map_put (gmap, "node", (void *)(intptr_t)REDUCE_UNDER_ME);
    resrc_api_map_put (rmap, "core", (void *)(intptr_t)NONE_UNDER_ME);
    resrc_api_map_put (rmap, "gpu", (void *)(intptr_t)NONE_UNDER_ME);
    if (resrc_tree_serialize_lite (gat, red, job->resrc_tree, gmap, rmap)) {
        flux_log (h, LOG_ERR, "job (%"PRId64") resource serialization failed",
                  job->lwj_id);
        goto done;
    } else if (resolve_rank (ctx, gat)) {
        flux_log (ctx->h, LOG_ERR, "resolving a hostname to rank failed");
        goto done;
    }

    json_object_set_new (jcb, JSC_R_LITE, gat);
    jcbstr = Jtostr (jcb);
    if (jsc_update_jcb (h, job->lwj_id, JSC_R_LITE, jcbstr) != 0) {
        flux_log (h, LOG_ERR, "error jsc udpate: %"PRId64" (%s)", job->lwj_id,
                  strerror (errno));
        free (jcbstr);
        goto done;
    }
    if(jcbstr)
        free((void*)jcbstr);
    if ((update_state (h, job->lwj_id, job->state, J_ALLOCATED)) != 0) {
        flux_log (h, LOG_ERR, "failed to update the state of job %"PRId64"",
                  job->lwj_id);
        goto done;
    }
    bridge_update_timer (ctx);
    rc = 0;
done:
    Jput (jcb);
    Jput (red);
    resrc_api_map_destroy (&gmap);
    resrc_api_map_destroy (&rmap);
    return rc;
}

#if DYNAMIC_SCHEDULING
static int req_tpexec_grow (flux_t *h, flux_lwj_t *job)
{
    /* TODO: NOT IMPLEMENTED */
    /* This runtime grow service will grow the resource set of the job.
       The special non-elastic case will be to grow the resource from
       zero to the selected RDL
    */
    return SCHED_UNIMPL;
}

static int req_tpexec_shrink (flux_t *h, flux_lwj_t *job)
{
    /* TODO: NOT IMPLEMENTED */
    return SCHED_UNIMPL;
}

static int req_tpexec_map (flux_t *h, flux_lwj_t *job)
{
    /* TODO: NOT IMPLEMENTED */
    /* This runtime grow service will grow the resource set of the job.
       The special non-elastic case will be to grow the resource from
       zero to RDL
    */
    return SCHED_UNIMPL;
}
#endif

static int req_tpexec_exec (flux_t *h, flux_lwj_t *job)
{
    ssrvctx_t *ctx = getctx (h);
    int rc = -1;

    if ((update_state (h, job->lwj_id, job->state, J_RUNREQUEST)) != 0) {
        flux_log (h, LOG_ERR, "failed to update the state of job %"PRId64"",
                  job->lwj_id);
        goto done;
    } else if (bridge_send_runrequest (ctx, job) != 0) {
        flux_log (h, LOG_ERR, "failed to send runrequest for job %"PRId64"",
                  job->lwj_id);
        goto done;
    }
    rc = 0;
done:
    return rc;
}

static int req_tpexec_run (flux_t *h, flux_lwj_t *job)
{
    /* TODO: wreckrun does not provide grow and map yet
     *   we will switch to the following sequence under the TP exec service
     *   that provides RFC 8.
     *
     *   req_tpexec_grow
     *   req_tpexec_map
     *   req_tpexec_exec
     */
    return req_tpexec_exec (h, job);
}


/********************************************************************************
 *                                                                              *
 *           Actions on Job/Res/Timer event including Scheduling Loop           *
 *                                                                              *
 *******************************************************************************/

static resrc_reqst_t *get_resrc_reqst (ssrvctx_t *ctx, flux_lwj_t *job,
                          int64_t starttime, int64_t *nreqrd)
{
    json_t *req_res = NULL;
    resrc_reqst_t *resrc_reqst = NULL;

    /*
     * Require at least one task per node, and
     * Assume (for now) one task per core.
     *
     * At this point, our flux_lwj_t structure supplies a simple count
     * of nodes and cores.  This is a short term solution that
     * supports the typical request.  Until a more complex model is
     * available, we will have to interpret the request along these
     * most likely scenarios:
     *
     * - If only cores are requested, the number of nodes we find to
     *   supply the requested cores does not matter to the user.
     *
     * - If only nodes are requested, we will return only nodes whose
     *   cores are all idle.
     *
     * - If nodes and cores are requested, we will return the
     *   requested number of nodes with at least the requested number
     *   of cores on each node.  We will not attempt to provide a
     *   balanced number of cores per node.
     */
    req_res = Jnew ();
    if (job->req->nnodes > 0) {
        Jadd_str (req_res, "type", "node");
        Jadd_int64 (req_res, "req_qty", job->req->nnodes);
        *nreqrd = job->req->nnodes;

        /* Since nodes are requested, make sure we look for at
         * least one core on each node */
        if (job->req->ncores < job->req->nnodes)
            job->req->ncores = job->req->nnodes;
        job->req->corespernode = (job->req->ncores + job->req->nnodes - 1) /
            job->req->nnodes;
        job->req->gpuspernode = 0;
        if (job->req->node_exclusive) {
            Jadd_int64 (req_res, "req_size", 1);
            Jadd_bool (req_res, "exclusive", true);
        } else {
            Jadd_int64 (req_res, "req_size", 0);
            Jadd_bool (req_res, "exclusive", false);
        }

        json_t *child_core = Jnew ();
        Jadd_str (child_core, "type", "core");
        Jadd_int64 (child_core, "req_qty", job->req->corespernode);
        /* setting size == 1 devotes (all of) the core to the job */
        Jadd_int64 (child_core, "req_size", 1);
        /* setting exclusive to true prevents multiple jobs per core */
        Jadd_bool (child_core, "exclusive", true);
        Jadd_int64 (child_core, "starttime", starttime);
        Jadd_int64 (child_core, "endtime", starttime + job->req->walltime);

        json_t *children = Jnew_ar();
        json_array_append_new (children, child_core);
        if (job->req->ngpus) {
            job->req->gpuspernode = (job->req->ngpus + job->req->nnodes - 1) /
            job->req->nnodes;
            json_t *child_gpu = Jnew ();
            Jadd_str (child_gpu, "type", "gpu");
            Jadd_int64 (child_gpu, "req_qty", job->req->gpuspernode);
            /* setting size == 1 devotes (all of) the gpu to the job */
            Jadd_int64 (child_gpu, "req_size", 1);
            /* setting exclusive to true prevents multiple jobs per core */
            Jadd_bool (child_gpu, "exclusive", true);
            Jadd_int64 (child_gpu, "starttime", starttime);
            Jadd_int64 (child_gpu, "endtime", starttime + job->req->walltime);
            json_array_append_new (children, child_gpu);
        }
        json_object_set_new (req_res, "req_children", children);
    } else if (job->req->ncores > 0) {
        Jadd_str (req_res, "type", "core");
        Jadd_int (req_res, "req_qty", job->req->ncores);
        *nreqrd = job->req->ncores;

        Jadd_int64 (req_res, "req_size", 1);
        /* setting exclusive to true prevents multiple jobs per core */
        Jadd_bool (req_res, "exclusive", true);
    } else
        goto done;

    Jadd_int64 (req_res, "starttime", starttime);
    Jadd_int64 (req_res, "endtime", starttime + job->req->walltime);
    resrc_reqst = resrc_reqst_from_json (ctx->rsapi, req_res, NULL);

done:
    Jput (req_res);
    return resrc_reqst;
}

/* Sort jobs by decreasing priority */
static int compare_priority (void *item1, void *item2)
{
    int ret = 0;
    flux_lwj_t *job1 = (flux_lwj_t*)item1;
    flux_lwj_t *job2 = (flux_lwj_t*)item2;

    if (job1->priority < job2->priority)
        ret = 1;
    else if (job1->priority > job2->priority)
        ret = -1;
    return ret;
}

/*
 * schedule_job() searches through all of the idle resources to
 * satisfy a job's requirements.  If enough resources are found, it
 * proceeds to allocate those resources and update the kvs's lwj entry
 * in preparation for job execution.  If less resources
 * are found than the job requires, and if the job asks to reserve
 * resources, then those resources will be reserved.
 */
int schedule_job (ssrvctx_t *ctx, flux_lwj_t *job, int64_t starttime)
{
    flux_t *h = ctx->h;
    int rc = -1;
    int64_t nfound = 0;
    int64_t nreqrd = 0;
    resrc_reqst_t *resrc_reqst = NULL;
    resrc_tree_t *found_tree = NULL;
    resrc_tree_t *selected_tree = NULL;
    struct behavior_plugin *plugin = behavior_plugin_get (ctx->loader);

    if (!plugin) {
        flux_log (h, LOG_ERR, "No scheduler policy plugin has been loaded!");
        return rc;
    }

    if (!(resrc_reqst = get_resrc_reqst (ctx, job, starttime, &nreqrd))) {
        flux_log (h, LOG_ERR, "Null resource request object!");
        goto done;
    }

    if ((nfound = plugin->find_resources (h, ctx->rsapi,
                                          GET_ROOT_RESRC(ctx->rsapi),
                                          resrc_reqst, &found_tree))) {
        flux_log (h, LOG_DEBUG, "Found %"PRId64" %s(s) for job %"PRId64", "
                  "required: %"PRId64"", nfound,
                  resrc_type (resrc_reqst_resrc (resrc_reqst)), job->lwj_id,
                  nreqrd);

        resrc_tree_unstage_resources (found_tree);
        resrc_reqst_clear_found (resrc_reqst);
        if ((selected_tree = plugin->select_resources (h, ctx->rsapi, found_tree,
                                                       resrc_reqst, NULL))) {
            if (resrc_reqst_all_found (resrc_reqst)) {
                plugin->allocate_resources (h, ctx->rsapi,
                                            selected_tree, job->lwj_id,
                                            starttime, starttime +
                                            job->req->walltime);
                /* Scheduler specific job transition */
                // TODO: handle this some other way (JSC?)
                job->starttime = starttime;
                job->state = J_SELECTED;
                if (job->resrc_tree != NULL) {
                    resrc_tree_destroy (ctx->rsapi, job->resrc_tree, false, false);
                    job->resrc_tree = NULL;
                }
                job->resrc_tree = selected_tree;
                if (req_tpexec_allocate (ctx, job) != 0) {
                    flux_log (h, LOG_ERR,
                              "failed to request allocate for job %"PRId64"",
                              job->lwj_id);
                    resrc_tree_destroy (ctx->rsapi, job->resrc_tree, false,false);
                    job->resrc_tree = NULL;
                    goto done;
                }
                flux_log (h, LOG_DEBUG, "Allocated %"PRId64" %s(s) for job "
                          "%"PRId64"", nreqrd,
                          resrc_type (resrc_reqst_resrc (resrc_reqst)),
                          job->lwj_id);
            } else {
                rc = plugin->reserve_resources (h, ctx->rsapi,
                                                &selected_tree, job->lwj_id,
                                                starttime, job->req->walltime,
                                                GET_ROOT_RESRC(ctx->rsapi),
                                                resrc_reqst);
                if (rc) {
                    resrc_tree_destroy (ctx->rsapi, selected_tree, false, false);
                    job->resrc_tree = NULL;
                } else {
                    if (job->resrc_tree != NULL) {
                        resrc_tree_destroy (ctx->rsapi, job->resrc_tree, false, false);
                        job->resrc_tree = NULL;
                    }
                    job->resrc_tree = selected_tree;
                }
            }
        }
    }
    rc = 0;
done:
    if (resrc_reqst)
        resrc_reqst_destroy (ctx->rsapi, resrc_reqst);
    if (found_tree)
        resrc_tree_destroy (ctx->rsapi, found_tree, false, false);

    return rc;
}

static int schedule_jobs (ssrvctx_t *ctx)
{
    int rc = 0;
    int qdepth = 0;
    flux_lwj_t *job = NULL;
    struct behavior_plugin *behavior_plugin = behavior_plugin_get (ctx->loader);
    struct priority_plugin *priority_plugin = priority_plugin_get (ctx->loader);
    /*
     * TODO: when dynamic scheduling is supported, the loop should
     * traverse through running job queue as well.
     */
    zlist_t *jobs = ctx->p_queue;
    int64_t starttime = (ctx->sctx.in_sim) ?
        (int64_t) ctx->sctx.sim_state->sim_time : epochtime();

    if (priority_plugin)
        priority_plugin->prioritize_jobs (ctx->h, jobs);
    if (!behavior_plugin)
        return -1;

    /* Sort by decreasing priority */
    zlist_sort (jobs, compare_priority);
    if (ctx->ooo_capable)
        resrc_tree_release_all_reservations (resrc_tree_root (ctx->rsapi));
    rc = behavior_plugin->sched_loop_setup (ctx->h);
    job = zlist_first (jobs);
    while (!rc && job && (qdepth < ctx->arg.s_params.queue_depth)) {
        if (job->state == J_SCHEDREQ) {
            rc = schedule_job (ctx, job, starttime);
        }
        job = (flux_lwj_t*)zlist_next (jobs);
        qdepth++;
    }

    return rc;
}


/********************************************************************************
 *                                                                              *
 *                        Scheduler Event Handling                              *
 *                                                                              *
 ********************************************************************************/

#define VERIFY(rc) if (!(rc)) {goto bad_transition;}
static inline bool trans (job_state_t ex, job_state_t n, job_state_t *o)
{
    if (ex == n) {
        *o = n;
        return true;
    }
    return false;
}

/*
 * Following is a state machine. action is invoked when an external job
 * state event is delivered. But in action, certain events are also generated,
 * some events are realized by falling through some of the case statements.
 */
static int action (ssrvctx_t *ctx, flux_lwj_t *job, job_state_t newstate,
                   json_t *jcb)
{
    flux_t *h = ctx->h;
    char *key = NULL;
    job_state_t oldstate = job->state;
    struct priority_plugin *priority_plugin = priority_plugin_get (ctx->loader);

    flux_log (h, LOG_DEBUG, "attempting job %"PRId64" state change from "
              "%s to %s", job->lwj_id, jsc_job_num2state (oldstate),
              jsc_job_num2state (newstate));

    switch (oldstate) {
    case J_NULL:
        VERIFY (trans (J_SUBMITTED, newstate, &(job->state)));
        fill_resource_req (h, job, jcb);
        /* fall through for implicit event generation */
    case J_SUBMITTED:
        VERIFY (trans (J_PENDING, J_PENDING, &(job->state)));
        /* fall through for implicit event generation */
    case J_PENDING:
        VERIFY (trans (J_SCHEDREQ, J_SCHEDREQ, &(job->state)));
        if (!ctx->arg.s_params.delay_sched)
            schedule_jobs (ctx);
        else
            q_mark_schedulability (ctx, job);
        break;
    case J_SCHEDREQ:
        /* A schedule requested should not get an event. */
        /* SCHEDREQ -> SELECTED happens implicitly within schedule_jobs */
        VERIFY (trans (J_CANCELLED, newstate, &(job->state)));
        if (!ctx->arg.reap) {
            if (job->req)
                free (job->req);
            key = xasprintf ("%"PRId64"", job->lwj_id);
            zhash_delete (ctx->job_index, key);
            free (key);
            free (job);
        }
        break;
    case J_SELECTED:
        VERIFY (trans (J_ALLOCATED, newstate, &(job->state)));
        req_tpexec_run (h, job);
        break;
    case J_ALLOCATED:
        VERIFY (trans (J_RUNREQUEST, newstate, &(job->state)));
        break;
    case J_RUNREQUEST:
        VERIFY (trans (J_STARTING, newstate, &(job->state))
                || trans (J_FAILED, newstate, &(job->state)));
        if (newstate == J_FAILED) {
            if (!ctx->arg.schedonce) {
                /* support testing by actually not releasing the resrc */
                if (resrc_tree_release (job->resrc_tree, job->lwj_id)) {
                    flux_log (h, LOG_ERR, "%s: failed to release resources for job "
                          "%"PRId64"", __FUNCTION__, job->lwj_id);
                }
            }
            if (!ctx->arg.s_params.delay_sched) {
                flux_msg_t *msg = flux_event_encode ("sched.res.freed", NULL);
                if (!msg || flux_send (h, msg, 0) < 0) {
                    flux_log (h, LOG_ERR, "%s: error sending event: %s",
                              __FUNCTION__, strerror (errno));
                } else {
                    flux_msg_destroy (msg);
                    flux_log (h, LOG_DEBUG, "Released resources for job %"PRId64"",
                              job->lwj_id);
                }
            }
            if (ctx->arg.reap) {
                q_move_to_cqueue (ctx, job);
            } else {
                q_rm_from_pqueue (ctx, job);
                free (job->req);
                resrc_tree_destroy (ctx->rsapi, job->resrc_tree, false, false);
                key = xasprintf ("%"PRId64"", job->lwj_id);
                zhash_delete (ctx->job_index, key);
                free (key);
                free (job);
            }
         }
        break;
    case J_STARTING:
        VERIFY (trans (J_RUNNING, newstate, &(job->state))
                || trans (J_FAILED, newstate, &(job->state)));
        if (newstate == J_RUNNING) {
            q_move_to_rqueue (ctx, job);
        } else if (newstate == J_FAILED) {
            if (!ctx->arg.schedonce) {
                /* support testing by actually not releasing the resrc */
                if (resrc_tree_release (job->resrc_tree, job->lwj_id)) {
                    flux_log (h, LOG_ERR, "%s: failed to release resources for job "
                          "%"PRId64"", __FUNCTION__, job->lwj_id);
                }
            }
            if (!ctx->arg.s_params.delay_sched) {
                flux_msg_t *msg = flux_event_encode ("sched.res.freed", NULL);
                if (!msg || flux_send (h, msg, 0) < 0) {
                    flux_log (h, LOG_ERR, "%s: error sending event: %s",
                              __FUNCTION__, strerror (errno));
                } else {
                    flux_msg_destroy (msg);
                    flux_log (h, LOG_DEBUG, "Released resources for job %"PRId64"",
                              job->lwj_id);
                }
            }
            if (ctx->arg.reap) {
                q_move_to_cqueue (ctx, job);
            } else {
                q_rm_from_pqueue (ctx, job);
                free (job->req);
                resrc_tree_destroy (ctx->rsapi, job->resrc_tree, false, false);
                key = xasprintf ("%"PRId64"", job->lwj_id);
                zhash_delete (ctx->job_index, key);
                free (key);
                free (job);
            }
        }
        break;
    case J_RUNNING:
        VERIFY (trans (J_COMPLETING, newstate, &(job->state)));
        break;
    case J_COMPLETING:
        VERIFY (trans (J_COMPLETE, newstate, &(job->state)));
        if (!ctx->arg.schedonce) {
            /* support testing by actually not releasing the resrc */
            if (resrc_tree_release (job->resrc_tree, job->lwj_id)) {
                flux_log (h, LOG_ERR, "%s: failed to release resources for job "
                      "%"PRId64"", __FUNCTION__, job->lwj_id);
            }
        }
        if (!ctx->arg.s_params.delay_sched) {
            flux_msg_t *msg = flux_event_encode ("sched.res.freed", NULL);
            if (!msg || flux_send (h, msg, 0) < 0) {
                flux_log (h, LOG_ERR, "%s: error sending event: %s",
                          __FUNCTION__, strerror (errno));
            } else {
                flux_msg_destroy (msg);
                flux_log (h, LOG_DEBUG, "Released resources for job %"PRId64"",
                          job->lwj_id);
            }
        }
        if (ctx->arg.reap) {
            /* move the job to complete queue when reap should be supported */
            q_move_to_cqueue (ctx, job);
        } else {
            /* free resource here if reap is not enabled */
            q_rm_from_rqueue (ctx, job);
            free (job->req);
            resrc_tree_destroy (ctx->rsapi, job->resrc_tree, false, false);
            key = xasprintf ("%"PRId64"", job->lwj_id);
            zhash_delete (ctx->job_index, key);
            free (key);
            free (job);
        }
        break;
    case J_CANCELLED:
        VERIFY (trans (J_REAPED, newstate, &(job->state)));
        if (ctx->arg.reap) {
             free (job->req);
             key = xasprintf ("%"PRId64"", job->lwj_id);
             zhash_delete (ctx->job_index, key);
             free (key);
             free (job);
        } else {
            flux_log (h, LOG_ERR, "Reap support is not enabled (Use reap=true");
        }
        break;
    case J_COMPLETE:
        VERIFY (trans (J_REAPED, newstate, &(job->state)));
        if (ctx->arg.reap) {
            if (priority_plugin)
                priority_plugin->record_job_usage (ctx->h, job);
            zlist_remove (ctx->c_queue, job);
            free (job->req);
            resrc_tree_destroy (ctx->rsapi, job->resrc_tree, false, false);
            key = xasprintf ("%"PRId64"", job->lwj_id);
            zhash_delete (ctx->job_index, key);
            free (key);
            free (job);
        } else {
            flux_log (h, LOG_ERR, "Reap support is not enabled (Use reap=true");
        }
        break;
    case J_REAPED:
    default:
        VERIFY (false);
        break;
    }
    return 0;

bad_transition:
    flux_log (h, LOG_ERR, "job %"PRId64" bad state transition from %s to %s",
              job->lwj_id, jsc_job_num2state (oldstate),
              jsc_job_num2state (newstate));
    return -1;
}

/* TODO: we probably need to abstract out resource status & control  API
 * For now, the only resource event is raised when a job releases its
 * RDL allocation.
 */
static void res_event_cb (flux_t *h, flux_msg_handler_t *w,
                          const flux_msg_t *msg, void *arg)
{
    schedule_jobs (getctx ((flux_t *)arg));
    return;
}

static void send_cancelled_event (flux_t *h, int64_t jobid)
{
    flux_msg_t *msg;
    msg = flux_event_pack ("wreck.state.cancelled", "{s:I}",
                            "jobid", jobid);
    if (!msg || (flux_send (h, msg, 0) < 0))
        flux_log (h, LOG_DEBUG, "%s: error sending event", __FUNCTION__);
    flux_msg_destroy (msg);
}

static void cancel_request_cb (flux_t *h, flux_msg_handler_t *w,
                               const flux_msg_t *msg, void *arg)
{
    ssrvctx_t *ctx = getctx ((flux_t *)arg);
    int64_t jobid = -1;
    uint32_t userid = 0;
    flux_lwj_t *job = NULL;

    if (flux_msg_get_userid (msg, &userid) < 0)
        goto error;

    flux_log (h, LOG_INFO, "cancel requested by user (%u).", userid);

    if (flux_request_unpack (msg, NULL, "{s:I}", "jobid", &jobid) < 0) {
        goto error;
    } else if (!(job = q_find_job (ctx, jobid))) {
        errno = ENOENT;
        flux_log (h, LOG_DEBUG,
                  "attempt to cancel nonexistent job (%"PRId64").", jobid);
        goto error;
    } else if (job->state != J_SCHEDREQ) {
        errno = EINVAL;
        flux_log (h, LOG_DEBUG, "attempt to cancel state=%s job (%"PRId64").",
                  jsc_job_num2state (job->state), jobid);
        goto error;
    }

    q_rm_from_pqueue (ctx, job);

    if ((update_state (h, jobid, job->state, J_CANCELLED)) != 0) {
        flux_log (h, LOG_ERR,
                  "error updating the job (%"PRId64") state to cancelled.",
                  jobid);
        goto error;
    }
    flux_log (ctx->h, LOG_INFO, "pending job (%"PRId64") removed.", jobid);
    if (flux_respond_pack (h, msg, "{s:I}", "jobid", jobid) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
    send_cancelled_event (h, jobid);
    return;
error:
    if (flux_respond (h, msg, errno, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
}

static int kill_jobs_on_node (flux_t *h, resrc_t *node)
{
   int64_t jobid = -1;
   char *topic = NULL;
   flux_msg_t *msg = NULL;

   for (jobid = resrc_alloc_job_first (node); jobid > 0;
        jobid = resrc_alloc_job_next (node)) {
       if (asprintf (&topic, "wreck.%"PRId64".kill", jobid) < 0) {
           flux_log (h, LOG_DEBUG, "%s: topic creation failed", __FUNCTION__);
           goto error;
       } else if (!(msg = flux_event_encode (topic, NULL))
                  || flux_send (h, msg, 0) < 0) {
           flux_log (h, LOG_DEBUG, "%s: event failed", __FUNCTION__);
           goto error;
       }
       free (topic);
       topic = NULL;
       flux_msg_destroy (msg);
       msg = NULL;
   }
   return 0;

error:
    free (topic);
    flux_msg_destroy (msg);
    return -1;
}

static void exclude_request_cb (flux_t *h, flux_msg_handler_t *w,
                                const flux_msg_t *msg, void *arg)
{
    ssrvctx_t *ctx = getctx ((flux_t *)arg);
    const char *hostname = NULL;
    int kill = false;
    uint32_t userid = 0;
    char *topic = NULL;
    resrc_t *node = NULL;
    flux_msg_t *msg2 = NULL;

    if (flux_msg_get_userid (msg, &userid) < 0)
        goto error;

    flux_log (h, LOG_INFO, "node exclusion requested by user (%u).", userid);
    if (flux_request_unpack (msg, NULL, "{s:s s:b}",
                            "node", &hostname, "kill", &kill) < 0)
        goto error;

    node = resrc_lookup_first (ctx->rsapi, hostname);
    do {
        if (!node) {
            errno = ENOENT;
            flux_log (h, LOG_DEBUG,
                      "attempt to exclude nonexistent node (%s).", hostname);
            goto error;
        }
        resrc_set_state (node, RESOURCE_EXCLUDED);
        if (kill) {
            if (kill_jobs_on_node (h, node) != 0) {
                goto error;
            }
        }
    } while ((node = resrc_lookup_next (ctx->rsapi, hostname)));

    flux_log (ctx->h, LOG_INFO, "%s excluded from scheduling.", hostname);
    msg2 = flux_event_encode ("sched.res.excluded", NULL);
    if (!msg2 || flux_send (h, msg2, 0) < 0) {
        flux_log (h, LOG_DEBUG, "%s: error sending event", __FUNCTION__);
        goto error;
    }
    flux_msg_destroy (msg2);

    if (flux_respond_pack (h, msg, "{}") < 0)
        flux_log_error (h, "%s", __FUNCTION__);
    return;

error:
    free (topic);
    flux_msg_destroy (msg2);
    if (flux_respond (h, msg, errno, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
}

static void include_request_cb (flux_t *h, flux_msg_handler_t *w,
                                const flux_msg_t *msg, void *arg)
{
    ssrvctx_t *ctx = getctx ((flux_t *)arg);
    const char *hostname = NULL;
    flux_msg_t *msg2 = NULL;
    uint32_t userid = 0;
    resrc_t *node = NULL;

    if (flux_msg_get_userid (msg, &userid) < 0)
        goto error;

    flux_log (h, LOG_INFO, "node inclusion requested by user (%u).", userid);
    if (flux_request_unpack (msg, NULL, "{s:s}", "node", &hostname) < 0)
        goto error;

    node = resrc_lookup_first (ctx->rsapi, hostname);
    do {
        if (!node) {
            errno = ENOENT;
            flux_log (h, LOG_DEBUG,
                      "attempt to include nonexistent node (%s).", hostname);
            goto error;
        } else if (resrc_state (node) != RESOURCE_EXCLUDED
                   && resrc_state (node) != RESOURCE_IDLE
                   && resrc_state (node) != RESOURCE_INVALID) {
            errno = EINVAL;
            flux_log (h, LOG_DEBUG,
                      "cannot include node (%s) due to state (%s).",
                      hostname, resrc_state_string (node));
            continue;
        }
        resrc_set_state (node, RESOURCE_IDLE);
    } while ((node = resrc_lookup_next (ctx->rsapi, hostname)));

    flux_log (h, LOG_DEBUG, "include node resource (%s), ", hostname);
    msg2 = flux_event_encode ("sched.res.included", NULL);
    if (!msg2 || flux_send (h, msg2, 0) < 0) {
        flux_log (h, LOG_ERR, "%s: error sending event: %s",
                  __FUNCTION__, strerror (errno));
    }
    flux_msg_destroy (msg2);
    if (flux_respond_pack (h, msg, "{}") < 0)
        flux_log_error (h, "%s", __FUNCTION__);
    return;

error:
    if (flux_respond (h, msg, errno, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
}

#if ENABLE_TIMER_EVENT
static int timer_event_cb (flux_t *h, void *arg)
{
    //flux_log (h, LOG_ERR, "TIMER CALLED");
    schedule_jobs (getctx ((flux_t *)arg));
    return 0;
}
#endif

static int job_status_cb (const char *jcbstr, void *arg, int errnum)
{
    int rc = -1;
    json_t *jcb = NULL;
    ssrvctx_t *ctx = getctx ((flux_t *)arg);
    flux_lwj_t *j = NULL;
    job_state_t ns = J_FOR_RENT;

    if (errnum > 0) {
        flux_log (ctx->h, LOG_ERR, "%s: errnum passed in", __FUNCTION__);
        goto out;
    }
    if (!(jcb = Jfromstr (jcbstr))) {
        flux_log (ctx->h, LOG_ERR, "%s: error parsing JSON string",
                  __FUNCTION__);
        goto out;
    }
    if (is_newjob (jcb))
        q_enqueue_into_pqueue (ctx, jcb);
    if ((j = fetch_job_and_event (ctx, jcb, &ns)) == NULL) {
        flux_log (ctx->h, LOG_INFO, "%s: nonexistent job", __FUNCTION__);
        flux_log (ctx->h, LOG_INFO, "%s: directly launched job?", __FUNCTION__);
        goto out;
    }
    rc = action (ctx, j, ns, jcb);
out:
    Jput (jcb);
    return rc;
}


static void sched_params_set_request_cb (flux_t *h, flux_msg_handler_t *w,
                                const flux_msg_t *msg, void *arg)
{
    ssrvctx_t *ctx = getctx ((flux_t *)arg);
    uint32_t userid = 0;
    char *sprms = NULL;
    bool prev_delay_sched = ctx->arg.s_params.delay_sched;

    if (flux_msg_get_userid (msg, &userid) < 0)
        goto error;

    flux_log (h, LOG_INFO,
            "sched_params change requested by user (%u).", userid);

    if (flux_request_unpack (msg, NULL, "{s:s}", "param", &sprms) < 0)
        goto error;

    if (sched_params_args (sprms, &(ctx->arg.s_params)) != 0)
        goto error;

    /* Only call adjust_for_sched_params if there's a state change in delay_sched,
    otherwise, flux_watcher_start gets called more than once */
    if (prev_delay_sched != ctx->arg.s_params.delay_sched) {
        if (adjust_for_sched_params (ctx) != 0)
            goto error;
    }

    if (flux_respond_pack (h, msg, "{}") < 0)
        flux_log_error (h, "%s", __FUNCTION__);
    return;

error:
    if (flux_respond (h, msg, errno, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
}


static void sched_params_get_request_cb (flux_t *h, flux_msg_handler_t *w,
                                const flux_msg_t *msg, void *arg)
{
    ssrvctx_t *ctx = getctx ((flux_t *)arg);
    uint32_t userid = 0;

    if (flux_msg_get_userid (msg, &userid) < 0)
        goto error;

    flux_log (h, LOG_INFO,
            "sched_param values requested by user (%u).", userid);

     if (flux_request_unpack (msg, NULL, "{}") < 0)
        goto error;

    if (flux_respond_pack (h, msg, "{s:i s:i}",
            "queue-depth", (int)ctx->arg.s_params.queue_depth,
            "delay-sched", (int)ctx->arg.s_params.delay_sched) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
    return;

error:
    if (flux_respond (h, msg, errno, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
}

static void ev_prep_cb (flux_reactor_t *r, flux_watcher_t *w, int ev, void *a)
{
    ssrvctx_t *ctx = (ssrvctx_t *)a;
    if (ctx->pq_state && ctx->idle)
        flux_watcher_start (ctx->idle);
}

static void ev_check_cb (flux_reactor_t *r, flux_watcher_t *w, int ev, void *a)
{
    ssrvctx_t *ctx = (ssrvctx_t *)a;
    if (ctx->idle)
        flux_watcher_stop (ctx->idle);
    if (ctx->pq_state) {
        flux_log (ctx->h, LOG_DEBUG, "check callback about to schedule jobs.");
        ctx->pq_state = false;
        schedule_jobs (ctx);
    }
}


/******************************************************************************
 *                                                                            *
 *                     Scheduler Service Module Main                          *
 *                                                                            *
 ******************************************************************************/

const sched_params_t *sched_params_get (flux_t *h)
{
    const sched_params_t *rp = NULL;
    ssrvctx_t *ctx = NULL;
    if (!(ctx = getctx (h))) {
        flux_log (h, LOG_ERR, "can't find or allocate the context");
        goto done;
    }
    rp = (const sched_params_t *) &(ctx->arg.s_params);
done:
    return rp;
}

int mod_main (flux_t *h, int argc, char **argv)
{
    int rc = -1;
    ssrvctx_t *ctx = NULL;
    uint32_t rank = 1;

    if (!(ctx = getctx (h))) {
        flux_log (h, LOG_ERR, "can't find or allocate the context");
        goto done;
    }
    if (flux_get_rank (h, &rank)) {
        flux_log (h, LOG_ERR, "failed to determine rank");
        goto done;
    } else if (rank) {
        flux_log (h, LOG_ERR, "sched module must only run on rank 0");
        goto done;
    } else if (ssrvarg_process_args (argc, argv, &(ctx->arg)) != 0) {
        flux_log (h, LOG_ERR, "can't process module args");
        goto done;
    }
    if (bridge_set_execmode (ctx) != 0) {
        flux_log (h, LOG_ERR, "failed to setup execution mode");
        goto done;
    }
    if (adjust_for_sched_params (ctx) != 0) {
        flux_log (h, LOG_ERR, "can't adjust for schedule parameters");
        goto done;
    }
    flux_log (h, LOG_INFO, "sched comms module starting");
    if (!(ctx->loader = sched_plugin_loader_create (h))) {
        flux_log_error (h, "failed to initialize plugin loader");
        goto done;
    }
    if (ctx->arg.userplugin) {
        if (sched_plugin_load (ctx->loader, ctx->arg.userplugin) < 0) {
            flux_log_error (h, "failed to load %s", ctx->arg.userplugin);
            goto done;
        }
        flux_log (h, LOG_INFO, "%s plugin loaded", ctx->arg.userplugin);
        if (plugin_process_args (ctx, ctx->arg.userplugin_opts) < 0) {
            flux_log_error (h, "failed to process args for %s",
                            ctx->arg.userplugin);
            goto done;
        }
        struct sched_prop prop;
        struct behavior_plugin *behavior_plugin = behavior_plugin_get (ctx->loader);
        if (behavior_plugin->get_sched_properties (h, &prop) < 0) {
            flux_log_error (h, "failed to fetch sched plugin properties for %s",
                            ctx->arg.userplugin);
            errno = EINVAL;
            goto done;
        }
        ctx->ooo_capable = prop.out_of_order_capable;
    }
    if (ctx->arg.prio_plugin) {
        if (sched_plugin_load (ctx->loader, ctx->arg.prio_plugin) < 0) {
            flux_log_error (h, "failed to load %s", ctx->arg.prio_plugin);
            goto done;
        }
        flux_log (h, LOG_INFO, "%s plugin loaded", ctx->arg.prio_plugin);
        struct priority_plugin *priority_plugin = priority_plugin_get
            (ctx->loader);
        if (priority_plugin) {
            if (priority_plugin->priority_setup (h))
                flux_log (h, LOG_ERR, "failed to setup priority plugin");
            else
                flux_log (h, LOG_INFO, "successfully setup priority plugin");
        } else {
            flux_log (h, LOG_ERR, "failed to get priority plugin");
        }
    }
    if (load_resources (ctx) != 0) {
        flux_log (h, LOG_ERR, "failed to load resources");
        goto done;
    }
    flux_log (h, LOG_INFO, "resources loaded");
    if (bridge_set_events (ctx) != 0) {
        flux_log (h, LOG_ERR, "failed to set events");
        goto done;
    }
    if (flux_reactor_run (flux_get_reactor (h), 0) < 0) {
        flux_log (h, LOG_ERR, "flux_reactor_run: %s", strerror (errno));
        goto done;
    }
    rc = 0;
done:
    return rc;
}

MOD_NAME ("sched");

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
