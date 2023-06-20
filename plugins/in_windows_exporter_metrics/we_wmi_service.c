/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2023 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_input_plugin.h>
#include <fluent-bit/flb_config.h>
#include <fluent-bit/flb_config_map.h>
#include <fluent-bit/flb_error.h>
#include <fluent-bit/flb_pack.h>

#include "we.h"
#include "we_wmi.h"
#include "we_wmi_service.h"
#include "we_util.h"
#include "we_metric.h"

static double nop_adjust(double value)
{
    return value;
}

int we_wmi_service_init(struct flb_we *ctx)
{
    struct cmt_gauge *g;

    ctx->wmi_service = flb_calloc(1, sizeof(struct we_wmi_service_counters));
    if (!ctx->wmi_service) {
        flb_errno();
        return -1;
    }
    ctx->wmi_service->operational = FLB_FALSE;

    g = cmt_gauge_create(ctx->cmt, "windows", "service", "info",
                         "A metric for Windows Service information",
                         4, (char *[]) {"name", "display_name", "process_id", "run_as"});

    if (!g) {
        return -1;
    }
    ctx->wmi_service->information = g;


    g = cmt_gauge_create(ctx->cmt, "windows", "service", "state",
                         "A state of the service",
                         2, (char *[]){"name", "state"});
    if (!g) {
        return -1;
    }
    ctx->wmi_service->state = g;

    g = cmt_gauge_create(ctx->cmt, "windows", "service", "start_mode",
                         "A start mode of the service",
                         2, (char *[]){"name", "start_mode"});
    if (!g) {
        return -1;
    }
    ctx->wmi_service->start_mode = g;

    g = cmt_gauge_create(ctx->cmt, "windows", "service", "status",
                         "A status of the service",
                         2, (char *[]){"name", "status"});
    if (!g) {
        return -1;
    }
    ctx->wmi_service->status = g;

    ctx->wmi_service->info = flb_calloc(1, sizeof(struct wmi_query_spec));
    if (!ctx->wmi_service->info) {
        flb_errno();
        return -1;
    }
    ctx->wmi_service->info->metric_instance = (void *)g;
    ctx->wmi_service->info->type = CMT_GAUGE;
    ctx->wmi_service->info->value_adjuster = nop_adjust;
    ctx->wmi_service->info->wmi_counter = "Win32_Service";
    ctx->wmi_service->info->wmi_property = "";
    ctx->wmi_service->info->label_property_count = 0;
    ctx->wmi_service->info->label_property_keys = NULL;
    ctx->wmi_service->info->where_clause = ctx->raw_where_clause;

    ctx->wmi_service->operational = FLB_TRUE;

    return 0;
}

int we_wmi_service_exit(struct flb_we *ctx)
{
    flb_free(ctx->wmi_service->info);
    flb_free(ctx->wmi_service);

    return 0;
}

int we_wmi_service_update(struct flb_we *ctx)
{
    IEnumWbemClassObject* enumerator = NULL;
    HRESULT hr;

    IWbemClassObject *class_obj = NULL;
    ULONG ret = 0;
    int i = 0;
    uint64_t timestamp = 0;
    char *service_name = NULL;
    char *display_name = NULL;
    char *pid = NULL;
    char *run_as = NULL;
    char *str_prop = NULL;
    char *state = NULL;
    char *start_mode = NULL;
    char *status = NULL;
    char **states = (char *[]){
        "stopped", "start pending", "stop pending", "running",
        "continue pending", "pause pending", "paused", "unknown", NULL
    };
    char **statuses = (char *[]){
        "ok", "error", "degraded", "unknown",
        "pred fail", "starting", "stopping", "service",
        "stressed", "nonrecover", "no contact", "lost comm", NULL
    };
    char **start_modes = (char *[]) {
        "boot", "system", "auto", "manual", "disabled", NULL
    };

    if (!ctx->wmi_service->operational) {
        flb_plg_error(ctx->ins, "windows_service collector not yet in operational state");

        return -1;
    }

    if (FAILED(we_wmi_coinitialize(ctx))) {
        return -1;
    }

    timestamp = cfl_time_now();

    if (FAILED(we_wmi_execute_query(ctx, ctx->wmi_service->info, &enumerator))) {
        return -1;
    }

    while (enumerator) {
        hr = enumerator->lpVtbl->Next(enumerator, WBEM_INFINITE, 1,
                                      &class_obj, &ret);

        if (0 == ret) {
            break;
        }

        service_name = we_wmi_get_property_str_value(ctx, "Name",        class_obj);
        display_name = we_wmi_get_property_str_value(ctx, "DisplayName", class_obj);
        pid          = we_wmi_get_property_str_value(ctx, "ProcessID",   class_obj);
        run_as       = we_wmi_get_property_str_value(ctx, "StartName",   class_obj);
        state        = we_wmi_get_property_str_value(ctx, "State",       class_obj);
        start_mode   = we_wmi_get_property_str_value(ctx, "StartMode",   class_obj);
        status       = we_wmi_get_property_str_value(ctx, "Status",      class_obj);

        /* Information */
        cmt_gauge_set(ctx->wmi_service->information, timestamp, 1.0,
                      4, (char *[]){ service_name, display_name, pid, run_as});

        /* State */
        for (i = 0; states[i] != NULL; i++) {
            if (strcasecmp(state, states[i]) == 0) {
                cmt_gauge_set(ctx->wmi_service->state, timestamp, 1.0,
                              2, (char *[]){ service_name, states[i]});
            }
            else {
                cmt_gauge_set(ctx->wmi_service->state, timestamp, 0.0,
                              2, (char *[]){ service_name, states[i]});
            }
        }
        /* Start Mode */
        for (i = 0; start_modes[i] != NULL; i++) {
            if (strcasecmp(start_mode, start_modes[i]) == 0) {
                cmt_gauge_set(ctx->wmi_service->start_mode, timestamp, 1.0,
                              2, (char *[]){ service_name, start_modes[i]});
            }
            else {
                cmt_gauge_set(ctx->wmi_service->start_mode, timestamp, 0.0,
                              2, (char *[]){ service_name, start_modes[i]});
            }
        }

        /* Status */
        for (i = 0; statuses[i] != NULL; i++) {
            if (strcasecmp(status, statuses[i]) == 0) {
                cmt_gauge_set(ctx->wmi_service->status, timestamp, 1.0,
                              2, (char *[]){ service_name, statuses[i]});
            } else {
                cmt_gauge_set(ctx->wmi_service->status, timestamp, 0.0,
                              2, (char *[]){ service_name, statuses[i]});
            }
        }

        class_obj->lpVtbl->Release(class_obj);

        flb_free(service_name);
        flb_free(display_name);
        flb_free(pid);
        flb_free(run_as);
        flb_free(state);
        flb_free(start_mode);
        flb_free(status);
    }

    enumerator->lpVtbl->Release(enumerator);
    we_wmi_cleanup(ctx);

    return 0;
}
