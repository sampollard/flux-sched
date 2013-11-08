/* cmbutil.c - exercise public interfaces */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <stdbool.h>
#include <json/json.h>
#include <sys/param.h>

#include "cmb.h"
#include "log.h"
#include "util.h"
#include "zmsg.h"

static int _parse_logstr (char *s, int *lp, char **fp);
static void dump_kvs_dir (cmb_t c, const char *path);

#define OPTIONS "p:s:b:B:k:SK:Ct:P:d:n:x:e:TL:W:D:r:R:qz:Zyl:Y:X:M:"
static const struct option longopts[] = {
    {"ping",       required_argument,  0, 'p'},
    {"stats",      required_argument,  0, 'x'},
    {"ping-padding", required_argument,0, 'P'},
    {"ping-delay", required_argument,  0, 'd'},
    {"subscribe",  required_argument,  0, 's'},
    {"event",      required_argument,  0, 'e'},
    {"barrier",    required_argument,  0, 'b'},
    {"barrier-torture", required_argument,  0, 'B'},
    {"nprocs",     required_argument,  0, 'n'},
    {"kvs-put",    required_argument,  0, 'k'},
    {"kvs-get",    required_argument,  0, 'K'},
    {"kvs-list",   required_argument,  0, 'l'},
    {"kvs-watch",  required_argument,  0, 'Y'},
    {"kvs-watch-dir",required_argument,  0, 'X'},
    {"kvs-commit", no_argument,        0, 'C'},
    {"kvs-dropcache", no_argument,     0, 'y'},
    {"kvs-torture",required_argument,  0, 't'},
    {"mrpc-echo",  required_argument,  0, 'M'},
    {"sync",       no_argument,        0, 'S'},
    {"snoop",      no_argument,        0, 'T'},
    {"log",        required_argument,  0, 'L'},
    {"log-watch",  required_argument,  0, 'W'},
    {"log-dump",   required_argument,  0, 'D'},
    {"route-add",  required_argument,  0, 'r'},
    {"route-del",  required_argument,  0, 'R'},
    {"route-query",no_argument,        0, 'q'},
    {"socket-path",required_argument,  0, 'z'},
    {"trace-apisock",no_argument,      0, 'Z'},
    {0, 0, 0, 0},
};

static void usage (void)
{
    fprintf (stderr, "Usage: cmbutil OPTIONS\n"
"  -p,--ping name         route a message through a plugin\n"
"  -P,--ping-padding N    pad ping packets with N bytes (adds a JSON string)\n"
"  -d,--ping-delay N      set delay between ping packets (in msec)\n"
"  -x,--stats name        get plugin statistics\n"
"  -T,--snoop             display messages to/from router socket\n"
"  -b,--barrier name      execute barrier across slurm job\n"
"  -B,--barrier-torture N execute N barriers across slurm job\n"
"  -n,--nprocs N          override nprocs (default $SLURM_NPROCS or 1)\n"
"  -k,--kvs-put key=val   set a key\n"
"  -K,--kvs-get key       get a key\n"
"  -Y,--kvs-watch key     watch a key (non-directory)\n"
"  -X,--kvs-watch-dir key watch a key (directory)\n"
"  -l,--kvs-list name     list keys in a particular \"directory\"\n"
"  -C,--kvs-commit        commit pending kvs puts\n"
"  -y,--kvs-dropcache     drop cached and unreferenced kvs data\n"
"  -t,--kvs-torture N     set N keys, then commit\n"
"  -M,--mrpc-echo NODES   exercise mrpc echo server\n"
"  -s,--subscribe sub     subscribe to events matching substring\n"
"  -e,--event name        publish event\n"
"  -S,--sync              block until event.sched.triger\n"
"  -L,--log fac:lev MSG   log MSG to facility at specified level\n"
"  -W,--log-watch fac:lev watch logs for messages matching tag\n"
"  -D,--log-dump fac:lev  dump circular log buffer\n"
"  -r,--route-add dst:gw  add local route to dst via gw\n"
"  -R,--route-del dst     delete local route to dst\n"
"  -q,--route-query       list routes in JSON format\n"
"  -z,--socket-path PATH  use non-default API socket path\n"
"  -Z,--trace-apisock     trace api socket messages\n"
);
    exit (1);
}

int main (int argc, char *argv[])
{
    int ch;
    cmb_t c;
    int nprocs;
    int padding = 0;
    int pingdelay_ms = 1000;
    static char socket_path[PATH_MAX + 1];
    char *val;
    bool Lopt = false;
    char *Lopt_facility;
    int Lopt_level;
    int flags = 0;

    log_init (basename (argv[0]));

    nprocs = env_getint ("SLURM_NPROCS", 1);

    if ((val = getenv ("CMB_API_PATH"))) {
        if (strlen (val) > PATH_MAX)
            err_exit ("What a long CMB_API_PATH you have!");
        strcpy (socket_path, val);
    }
    else {
        snprintf (socket_path, sizeof (socket_path),
                  CMB_API_PATH_TMPL, getuid ());
    }

    while ((ch = getopt_long (argc, argv, OPTIONS, longopts, NULL)) != -1) {
        switch (ch) {
            case 'P': /* --ping-padding N */
                padding = strtoul (optarg, NULL, 10);
                break;
            case 'd': /* --ping-delay N */
                pingdelay_ms = strtoul (optarg, NULL, 10);
                break;
            case 'n': /* --nprocs N */
                nprocs = strtoul (optarg, NULL, 10);
                break;
            case 'z': /* --socket-path PATH */
                snprintf (socket_path, sizeof (socket_path), "%s", optarg);
                break;
            case 'Z': /* --trace-apisock */
                flags |= CMB_FLAGS_TRACE;
                break;
        }
    }
    if (!(c = cmb_init_full (socket_path, flags)))
        err_exit ("cmb_init");
    optind = 0;
    while ((ch = getopt_long (argc, argv, OPTIONS, longopts, NULL)) != -1) {
        switch (ch) {
            case 'P':
            case 'd':
            case 'n':
            case 'z':
            case 'Z':
                break; /* handled in first getopt */
            case 'p': { /* --ping name */
                int i;
                struct timeval t, t1, t2;
                char *tag, *route;
                for (i = 0; ; i++) {
                    xgettimeofday (&t1, NULL);
                    if (cmb_ping (c, optarg, i, padding, &tag, &route) < 0)
                        err_exit ("cmb_ping");
                    xgettimeofday (&t2, NULL);
                    timersub (&t2, &t1, &t);
                    msg ("%s pad=%d seq=%d time=%0.3f ms (%s)", tag,
                         padding,i,
                         (double)t.tv_sec * 1000 + (double)t.tv_usec / 1000,
                         route);
                    usleep (pingdelay_ms * 1000);
                    free (tag);
                    free (route);
                }
                break;
            }
            case 'x': { /* --stats name */
                json_object *o;
                char *s;

                if (!(s = cmb_stats (c, optarg)))
                    err_exit ("cmb_stats");
                if (!(o = json_tokener_parse (s)))
                    err_exit ("json_tokener_parse");
                printf ("%s\n", json_object_to_json_string_ext (o,
                                    JSON_C_TO_STRING_PRETTY));
                json_object_put (o);
                free (s);
                break;
            }
            case 'b': { /* --barrier NAME */
                struct timeval t, t1, t2;
                xgettimeofday (&t1, NULL);
                if (flux_barrier (c, optarg, nprocs) < 0)
                    err_exit ("flux_barrier");
                xgettimeofday (&t2, NULL);
                timersub (&t2, &t1, &t);
                msg ("barrier time=%0.3f ms",
                     (double)t.tv_sec * 1000 + (double)t.tv_usec / 1000);
                break;
            }
            case 'B': { /* --barrier-torture N */
                int i, n = strtoul (optarg, NULL, 10);
                char name[16];
                for (i = 0; i < n; i++) {
                    snprintf (name, sizeof (name), "%d", i);
                    if (flux_barrier (c, name, nprocs) < 0)
                        err_exit ("flux_barrier %s", name);
                }
                break;
            }
            case 's': { /* --subscribe substr */
                char *event;
                if (flux_event_subscribe (c, optarg) < 0)
                    err_exit ("flux_event_subscribe");
                while (cmb_recv_message (c, &event, NULL, false) == 0) {
                    msg ("%s", event);
                    free (event);
                }
                if (flux_event_unsubscribe (c, optarg) < 0)
                    err_exit ("flux_event_unsubscribe");
                break;
            }
            case 'T': { /* --snoop */
                zmsg_t *zmsg;
                if (flux_snoop_subscribe (c, "") < 0)
                    err_exit ("flux_snoop_subscribe");
                while ((zmsg = cmb_recv_zmsg (c, false))) {
                    zmsg_dump_compact (zmsg);
                    zmsg_destroy (&zmsg);
                }
                if (flux_snoop_unsubscribe (c, "") < 0)
                    err_exit ("flux_snoop_unsubscribe");
                break;
            }
            case 'S': { /* --sync */
                char *event;
                if (flux_event_subscribe (c, "event.sched.trigger.") < 0)
                    err_exit ("flux_event_subscribe");
                if (cmb_recv_message (c, &event, NULL, false) < 0)
                    err_exit ("cmb_recv_message");
                free (event);
                break;
            }
            case 'e': { /* --event name */
                if (flux_event_send (c, NULL, "%s", optarg) < 0)
                    err_exit ("flux_event_send");
                break;
            }
            case 'k': { /* --kvs-put key=val */
                char *key = optarg;
                char *val = strchr (optarg, '=');
                json_object *vo = NULL;

                if (!val)
                    msg_exit ("malformed key=[val] argument");
                *val++ = '\0';
                if (strlen (val) > 0)
                    if (!(vo = json_tokener_parse (val)))
                        vo = json_object_new_string (val);
                if (kvs_put (c, key, vo) < 0)
                    err_exit ("kvs_put");
                if (vo)
                    json_object_put (vo);
                break;

            }
            case 'K': { /* --kvs-get key */
                json_object *o;

                if (kvs_get (c, optarg, &o) < 0)
                    err_exit ("kvs_get");
                printf ("%s = %s\n", optarg, json_object_to_json_string_ext (o,
                                             JSON_C_TO_STRING_PLAIN));
                json_object_put (o);
                break;
            }
            case 'Y': { /* --kvs-watch key */
                json_object *val = NULL;
                int rc;

                rc = kvs_get (c, optarg, &val);
                while (rc == 0 || (rc < 0 && errno == ENOENT)) {
                    if (rc < 0) {
                        printf ("%s: %s\n", optarg, strerror (errno));
                        if (val)
                            json_object_put (val);
                        val = NULL;
                    } else
                        printf ("%s=%s\n", optarg,
                                json_object_to_json_string_ext (val,
                                JSON_C_TO_STRING_PLAIN));
                    rc = kvs_watch_once (c, optarg, &val);
                }
                err_exit ("%s", optarg);
                break;
            }
            case 'X': { /* --kvs-watch-dir key */
                kvsdir_t dir = NULL;
                int rc;

                rc = kvs_get_dir (c, &dir, "%s", optarg);
                while (rc == 0 || (rc < 0 && errno == ENOENT)) {
                    if (rc < 0) {
                        printf ("%s: %s\n", optarg, strerror (errno));
                        if (dir)
                            kvsdir_destroy (dir);
                        dir = NULL;
                    } else {
                        dump_kvs_dir (c, optarg);
                        printf ("======================\n");
                    }
                    rc = kvs_watch_once_dir (c, &dir, "%s", optarg);
                } 
                err_exit ("%s", optarg);
                break;
            }
            case 'l': { /* --kvs-list name */
                dump_kvs_dir (c, optarg);
                break;
            }
            case 'C': { /* --kvs-commit */
                if (kvs_commit (c) < 0)
                    err_exit ("kvs_commit");
                break;
            }
            case 'y': { /* --kvs-dropcache */
                if (kvs_dropcache (c) < 0)
                    err_exit ("kvs_dropcache");
                break;
            }
            case 't': { /* --kvs-torture N */
                int i, n = strtoul (optarg, NULL, 10);
                char key[16], val[16];
                struct timeval t1, t2, t;
                json_object *vo = NULL;

                xgettimeofday (&t1, NULL);
                for (i = 0; i < n; i++) {
                    snprintf (key, sizeof (key), "key%d", i);
                    snprintf (val, sizeof (key), "val%d", i);
                    vo = json_object_new_string (val);
                    if (kvs_put (c, key, vo) < 0)
                        err_exit ("kvs_put");
                    if (vo)
                        json_object_put (vo);
                }
                xgettimeofday (&t2, NULL);
                timersub(&t2, &t1, &t);
                msg ("kvs_put:    time=%0.3f ms",
                     (double)t.tv_sec * 1000 + (double)t.tv_usec / 1000);

                xgettimeofday (&t1, NULL);
                if (kvs_commit (c) < 0)
                    err_exit ("kvs_commit");
                xgettimeofday (&t2, NULL);
                timersub (&t2, &t1, &t);
                msg ("kvs_commit: time=%0.3f ms",
                     (double)t.tv_sec * 1000 + (double)t.tv_usec / 1000);

                xgettimeofday (&t1, NULL);
                for (i = 0; i < n; i++) {
                    snprintf (key, sizeof (key), "key%d", i);
                    snprintf (val, sizeof (key), "val%d", i);
                    if (kvs_get (c, key, &vo) < 0)
                        err_exit ("kvs_get");
                    if (strcmp (json_object_get_string (vo), val) != 0)
                        msg_exit ("kvs_get: key '%s' wrong value '%s'",
                                  key, json_object_get_string (vo));
                    if (vo)
                        json_object_put (vo);
                }
                xgettimeofday (&t2, NULL);
                timersub(&t2, &t1, &t);
                msg ("kvs_get:    time=%0.3f ms",
                     (double)t.tv_sec * 1000 + (double)t.tv_usec / 1000);
                break;
            }
            case 'L': { /* --log */
                if (_parse_logstr (optarg, &Lopt_level, &Lopt_facility) < 0)
                    msg_exit ("bad log level string");
                Lopt = true; /* see code after getopt */
                break;
            }
            case 'W': { /* --log-watch fac:lev */
                char *src, *fac, *s;
                struct timeval tv, start = { .tv_sec = 0 }, rel;
                int count, lev;
                const char *levstr;

                if (_parse_logstr (optarg, &lev, &fac) < 0)
                    msg_exit ("bad log level string");
                if (cmb_log_subscribe (c, lev, fac) < 0)
                    err_exit ("cmb_log_subscribe");
                free (fac);
                while ((s = cmb_log_recv (c, &lev, &fac, &count, &tv, &src))) {
                    if (start.tv_sec == 0)
                        start = tv;
                    timersub (&tv, &start, &rel);
                    levstr = log_leveltostr (lev);
                    //printf ("XXX lev=%d (%s)\n", lev, levstr);
                    fprintf (stderr, "[%-.6lu.%-.6lu] %dx %s.%s[%s]: %s\n",
                             rel.tv_sec, rel.tv_usec, count,
                             fac, levstr ? levstr : "unknown", src, s);
                    free (fac);
                    free (src);
                    free (s);
                }
                if (errno != ENOENT)
                    err ("cmbv_log_recv");
                break;
            }
            case 'D': { /* --log-dump fac:lev */
                char *src, *fac, *s;
                struct timeval tv, start = { .tv_sec = 0 }, rel;
                int lev, count;
                const char *levstr;

                if (_parse_logstr (optarg, &lev, &fac) < 0)
                    msg_exit ("bad log level string");
                if (cmb_log_dump (c, lev, fac) < 0)
                    err_exit ("cmb_log_dump");
                free (fac);
                while ((s = cmb_log_recv (c, &lev, &fac, &count, &tv, &src))) {
                    if (start.tv_sec == 0)
                        start = tv;
                    timersub (&tv, &start, &rel);
                    levstr = log_leveltostr (lev);
                    fprintf (stderr, "[%-.6lu.%-.6lu] %dx %s.%s[%s]: %s\n",
                             rel.tv_sec, rel.tv_usec, count,
                             fac, levstr ? levstr : "unknown", src, s);
                    free (fac);
                    free (src);
                    free (s);
                }
                if (errno != ENOENT)
                    err ("cmbv_log_recv");
                break;
            }
            case 'r': { /* --route-add dst:gw */
                char *gw, *dst = xstrdup (optarg);
                if (!(gw = strchr (dst, ':')))
                    usage ();
                *gw++ = '\0';
                if (cmb_route_add (c, dst, gw) < 0)
                    err ("cmb_route_add %s via %s", dst, gw);
                break;
            }
            case 'R': { /* --route-del dst */
                char *gw, *dst = xstrdup (optarg);
                if (!(gw = strchr (dst, ':')))
                    usage ();
                *gw++ = '\0';
                if (cmb_route_del (c, dst, gw) < 0)
                    err ("cmb_route_del %s via %s", dst, gw);
                break;
            }
            case 'q': { /* --route-query */
                json_object *o;
                char *s;

                if (!(s = cmb_route_query (c)))
                    err_exit ("cmb_route_query");
                if (!(o = json_tokener_parse (s)))
                    err_exit ("json_tokener_parse");
                printf ("%s\n", json_object_to_json_string_ext (o,
                                    JSON_C_TO_STRING_PRETTY));
                json_object_put (o);
                free (s);
                msg ("rank=%d size=%d", flux_rank (c), flux_size (c));
                break;
            }
            case 'M': { /* --mrpc-echo NODELIST */
                flux_mrpc_t f;
                json_object *inarg, *outarg;
                int id;
                char s[1024];
 
                if (*optarg != '[')
                    snprintf (s, sizeof (s), "[%s]", optarg); 
                else
                    snprintf (s, sizeof (s), "%s", optarg);
                if (!(f = flux_mrpc_create (c, s)))
                    err_exit ("flux_mrpc_create");
                inarg = util_json_object_new_object ();
                util_json_object_add_int (inarg, "seq", 0);
                flux_mrpc_put_inarg (f, inarg);
                if (flux_mrpc (f, "mecho") < 0)
                    err_exit ("flux_mrpc");
                while ((id = flux_mrpc_next_outarg (f)) != -1) {
                    if (flux_mrpc_get_outarg (f, id, &outarg) < 0) {
                        printf ("%d: no response\n", id);
                        continue;
                    }
                    if (!util_json_match (inarg, outarg))
                        printf ("%d: mangled response\n", id);
                    else
                        printf ("%d: OK\n", id);
                    json_object_put (outarg);
                }
                json_object_put (inarg);
                flux_mrpc_destroy (f);
                break;
            }
            default:
                usage ();
        }
    }

    if (Lopt) {
        char *argstr = argv_concat (argc - optind, argv + optind);

        cmb_log_set_facility (c, Lopt_facility);
        if (cmb_log (c, Lopt_level, "%s", argstr) < 0)
            err_exit ("cmb_log");
        free (argstr);
    } else {
        if (optind < argc)
            usage ();
    }

    cmb_fini (c);
    exit (0);
}

static int _parse_logstr (char *s, int *lp, char **fp)
{
    char *p, *fac = xstrdup (s);
    int lev = LOG_INFO;

    if ((p = strchr (fac, ':'))) {
        *p++ = '\0';
        lev = log_strtolevel (p);
        if (lev < 0)
            return -1;
    }
    *lp = lev;
    *fp = fac;
    return 0;
}

static void dump_kvs_dir (cmb_t c, const char *path)
{
    kvsdir_t dir;
    kvsitr_t itr;
    const char *name;
    char *key;

    if (kvs_get_dir (c, &dir, "%s", path) < 0)
        err_exit ("kvs_get_dir %s", path);

    itr = kvsitr_create (dir);
    while ((name = kvsitr_next (itr))) {
        key = kvsdir_key_at (dir, name);
        if (kvsdir_issymlink (dir, name)) {
            char *link;

            if (kvs_get_symlink (c, key, &link) < 0)
                err_exit ("kvs_get_symlink %s", key);
            printf ("%s -> %s\n", key, link);
            free (link);

        } else if (kvsdir_isdir (dir, name)) {
            dump_kvs_dir (c, key);

        } else {
            json_object *o;

            if (kvs_get (c, key, &o) < 0)
                err_exit ("kvs_get %s", key);
            printf ("%s = %s\n", key,
                    json_object_to_json_string_ext (o, JSON_C_TO_STRING_PLAIN));
            json_object_put (o);
        }
        free (key);
    }
    kvsitr_destroy (itr);
    kvsdir_destroy (dir);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
