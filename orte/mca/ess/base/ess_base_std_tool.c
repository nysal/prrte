/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2009 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2018 Intel, Inc. All rights reserved.
 * Copyright (c) 2014      Hochschule Esslingen.  All rights reserved.
 *
 * Copyright (c) 2015      Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

#include <sys/types.h>
#include <stdio.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "opal/mca/event/event.h"
#include "opal/mca/pmix/base/base.h"
#include "opal/runtime/opal.h"
#include "opal/runtime/opal_progress_threads.h"
#include "opal/util/arch.h"
#include "opal/util/opal_environ.h"
#include "opal/util/argv.h"
#include "opal/util/proc.h"

#include "orte/mca/errmgr/base/base.h"
#include "orte/mca/state/base/base.h"
#include "orte/util/proc_info.h"
#include "orte/util/session_dir.h"
#include "orte/util/show_help.h"

#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_wait.h"

#include "orte/mca/ess/base/base.h"


static void infocb(int status,
                   opal_list_t *info,
                   void *cbdata,
                   opal_pmix_release_cbfunc_t release_fn,
                   void *release_cbdata)
{
    opal_value_t *kv;
    opal_pmix_lock_t *lock = (opal_pmix_lock_t*)cbdata;

    if (OPAL_SUCCESS != status) {
        ORTE_ERROR_LOG(status);
    } else {
        kv = (opal_value_t*)opal_list_get_first(info);
        if (NULL == kv) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_SUPPORTED);
        } else {
            if (0 == strcmp(kv->key, OPAL_PMIX_SERVER_URI)) {
                orte_process_info.my_hnp_uri = strdup(kv->data.string);
            } else {
                ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
            }
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    OPAL_PMIX_WAKEUP_THREAD(lock);
}

int orte_ess_base_tool_setup(opal_list_t *flags)
{
    int ret;
    char *error = NULL;
    opal_list_t info;
    opal_value_t *kv, *knext;
    opal_pmix_query_t *q;
    opal_pmix_lock_t lock;

    /* we need an external progress thread to ensure that things run
     * async with the PMIx code */
    orte_event_base = opal_progress_thread_init("tool");

    /* setup the PMIx framework - ensure it skips all non-PMIx components,
     * but do not override anything we were given */
    opal_setenv("OMPI_MCA_pmix", "^s1,s2,cray,isolated", false, &environ);
    if (OPAL_SUCCESS != (ret = mca_base_framework_open(&opal_pmix_base_framework, 0))) {
        ORTE_ERROR_LOG(ret);
        error = "orte_pmix_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = opal_pmix_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "opal_pmix_base_select";
        goto error;
    }
    if (NULL == opal_pmix.tool_init) {
        /* we no longer support non-pmix tools */
        orte_show_help("help-ess-base.txt",
                       "legacy-tool", true);
        ret = ORTE_ERR_SILENT;
        error = "opal_pmix.tool_init";
        goto error;
    }
    /* set the event base for the pmix component code */
    opal_pmix_base_set_evbase(orte_event_base);

    /* initialize */
    OBJ_CONSTRUCT(&info, opal_list_t);
    if (NULL != flags) {
        /* pass along any directives */
        OPAL_LIST_FOREACH_SAFE(kv, knext, flags, opal_value_t) {
            opal_list_remove_item(flags, &kv->super);
            opal_list_append(&info, &kv->super);
        }
    }
    if (OPAL_SUCCESS != (ret = opal_pmix.tool_init(&info))) {
        ORTE_ERROR_LOG(ret);
        error = "opal_pmix.init";
        OPAL_LIST_DESTRUCT(&info);
        goto error;
    }
    OPAL_LIST_DESTRUCT(&info);
    /* the PMIx server set our name - record it here */
    ORTE_PROC_MY_NAME->jobid = OPAL_PROC_MY_NAME.jobid;
    ORTE_PROC_MY_NAME->vpid = OPAL_PROC_MY_NAME.vpid;
    orte_process_info.super.proc_hostname = strdup(orte_process_info.nodename);
    orte_process_info.super.proc_flags = OPAL_PROC_ALL_LOCAL;
    orte_process_info.super.proc_arch = opal_local_arch;
    opal_proc_local_set(&orte_process_info.super);

    if (NULL != opal_pmix.query) {
        /* query the server for its URI so we can get any IO forwarded to us */
        OBJ_CONSTRUCT(&info, opal_list_t);
        q = OBJ_NEW(opal_pmix_query_t);
        opal_argv_append_nosize(&q->keys, OPAL_PMIX_SERVER_URI);
        opal_list_append(&info, &q->super);
        OPAL_PMIX_CONSTRUCT_LOCK(&lock);
        opal_pmix.query(&info, infocb, &lock);
        OPAL_PMIX_WAIT_THREAD(&lock);
        OPAL_PMIX_DESTRUCT_LOCK(&lock);
        OPAL_LIST_DESTRUCT(&info);
    }

    /* open and setup the state machine */
    if (ORTE_SUCCESS != (ret = mca_base_framework_open(&orte_state_base_framework, 0))) {
        ORTE_ERROR_LOG(ret);
        error = "orte_state_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_state_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_state_base_select";
        goto error;
    }
    /* open and setup the error manager */
    if (ORTE_SUCCESS != (ret = mca_base_framework_open(&orte_errmgr_base_framework, 0))) {
        ORTE_ERROR_LOG(ret);
        error = "orte_errmgr_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_errmgr_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_errmgr_base_select";
        goto error;
    }

    /* we -may- need to know the name of the head
     * of our session directory tree, particularly the
     * tmp base where any other session directories on
     * this node might be located
     */

    ret = orte_session_setup_base(ORTE_PROC_MY_NAME);
    if (ORTE_SUCCESS != ret ) {
        ORTE_ERROR_LOG(ret);
        error = "define session dir names";
        goto error;
    }

    return ORTE_SUCCESS;

 error:
    orte_show_help("help-orte-runtime.txt",
                   "orte_init:startup:internal-failure",
                   true, error, ORTE_ERROR_NAME(ret), ret);

    return ret;
}

int orte_ess_base_tool_finalize(void)
{
    orte_wait_finalize();

    /* if I am a tool, then all I will have done is
     * a very small subset of orte_init - ensure that
     * I only back those elements out
     */
    (void) mca_base_framework_close(&orte_errmgr_base_framework);

    opal_pmix.finalize();
    (void) mca_base_framework_close(&opal_pmix_base_framework);

    return ORTE_SUCCESS;
}
