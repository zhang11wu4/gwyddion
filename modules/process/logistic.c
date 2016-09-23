/*
 *  @(#) $Id$
 *  Copyright (C) 2016 David Necas (Yeti), Petr Klapetek,
 *  Daniil Bratashov.
 *  E-mail: yeti@gwyddion.net, klapetek@gwyddion.net,
 *  dn2010@gwyddion.net
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include <gtk/gtk.h>
#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwymath.h>
#include <libprocess/stats.h>
#include <libprocess/filters.h>
#include <libgwydgets/gwystock.h>
#include <libgwydgets/gwydgetutils.h>
#include <libgwydgets/gwydgets.h>
#include <libgwymodule/gwymodule-process.h>
#include <app/gwyapp.h>
#include <string.h>

#define LOGISTIC_RUN_MODES GWY_RUN_INTERACTIVE

#define NFEATURES (1 + 4 * 5)
#define FWHM2SIGMA (1.0/(2.0*sqrt(2*G_LN2)))

enum {
    RESPONSE_RESET   = 1,
};

typedef enum {
    LOGISTIC_MODE_TRAIN,
    LOGISTIC_MODE_USE
} LogisticMode;

typedef struct {
    LogisticMode mode;
    gint nfeatures;
    GwyDataLine *thetas;
} LogisticArgs;

typedef struct {
    LogisticArgs *args;
    GSList *mode;
    GtkWidget *dialog;
} LogisticControls;

static gboolean module_register         (void);
static void      logistic_run           (GwyContainer *data,
                                         GwyRunType run);
static void      logistic_dialog        (GwyContainer *data,
                                         LogisticArgs *args);
static GwyBrick* create_feature_vector  (GwyDataField *dfield);
static gdouble   cost_function          (GwyBrick *brick,
                                         GwyDataField *mask,
                                         gdouble *thetas,
                                         gdouble *grad,
                                         gdouble lambda);
static void      train_logistic         (GwyContainer *container,
                                         GwyBrick *features,
                                         GwyDataField *mfield,
                                         gdouble *thetas,
                                         gdouble lambda);
static void      predict                (GwyBrick *brick,
                                         gdouble *thetas,
                                         GwyDataField *dfield);
static void      predict_mask           (GwyBrick *brick,
                                         gdouble *thetas,
                                         GwyDataField *mask);
static void      logistic_mode_changed  (GtkWidget *button,
                                         LogisticControls *controls);
static void      logistic_values_update (LogisticControls *controls,
                                         LogisticArgs *args);
static void      logistic_dialog_update (LogisticControls *controls,
                                         LogisticArgs *args);
static void      logistic_load_args     (GwyContainer *settings,
                                         LogisticArgs *args);
static void      logistic_save_args     (GwyContainer *settings,
                                         LogisticArgs *args);
static void      logistic_reset_args    (LogisticArgs *args);

static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    N_("Trains logistic regression to mark grains."),
    "Daniil Bratashov <dn2010@gwyddion.net>",
    "0.1",
    "David Nečas (Yeti) & Petr Klapetek & Daniil Bratashov",
    "2016",
};

GWY_MODULE_QUERY(module_info)

static gboolean
module_register(void)
{
    gwy_process_func_register("logistic_regression",
                              (GwyProcessFunc)&logistic_run,
                              N_("/_Grains/Logistic _Regression..."),
                              GWY_STOCK_GRAINS,
                              LOGISTIC_RUN_MODES,
                              GWY_MENU_FLAG_DATA,
                              N_("Mark grains with logistic regression"));

    return TRUE;
}

static void
logistic_run(GwyContainer *data, GwyRunType run)
{
    GwyDataField *dfield, *mfield;
    GwyBrick *features;
    gint id;
    LogisticArgs args;
    gdouble *thetas;
    GQuark quark;

    g_return_if_fail(run & LOGISTIC_RUN_MODES);
    logistic_load_args(gwy_app_settings_get(), &args);
    gwy_app_data_browser_get_current(GWY_APP_DATA_FIELD, &dfield,
                                     GWY_APP_DATA_FIELD_ID, &id,
                                     GWY_APP_MASK_FIELD, &mfield,
                                     GWY_APP_MASK_FIELD_KEY, &quark,
                                     0);
    logistic_dialog(data, &args);
    features = create_feature_vector(dfield);
    thetas = gwy_data_line_get_data(args.thetas);
    if (args.mode == LOGISTIC_MODE_TRAIN) {
        if (mfield) {
            train_logistic(data, features, mfield, thetas, 1.0);
        }
    }
    else {
        if (!mfield) {
            mfield = gwy_data_field_new_alike(dfield, TRUE);
            predict_mask(features, thetas, mfield);
            gwy_container_set_object(data, quark, mfield);
            g_object_unref(mfield);
        }
        else {
            gwy_app_undo_qcheckpointv(data, 1, &quark);
            predict_mask(features, thetas, mfield);
            gwy_data_field_data_changed(mfield);
        }
    }
    gwy_app_channel_log_add_proc(data, id, id);
    g_object_unref(features);
}

static void
logistic_dialog(GwyContainer *data, LogisticArgs *args)
{
    GtkWidget *dialog, *table, *button;
    gint response, row;
    LogisticControls controls;

    controls.args = args;

    dialog = gtk_dialog_new_with_buttons(_("Logistic Regression"),
                                         NULL, 0, NULL);
    gtk_dialog_add_button(GTK_DIALOG(dialog),
                          _("_Reset"), RESPONSE_RESET);
    gtk_dialog_add_button(GTK_DIALOG(dialog),
                          GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog),
                          GTK_STOCK_OK, GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog),
                                    GTK_RESPONSE_OK);
    gwy_help_add_to_proc_dialog(GTK_DIALOG(dialog), GWY_HELP_DEFAULT);
    controls.dialog = dialog;

    table = gtk_table_new(2, 4, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 2);
    gtk_table_set_col_spacings(GTK_TABLE(table), 6);
    gtk_container_set_border_width(GTK_CONTAINER(table), 4);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), table,
                       TRUE, TRUE, 4);
    row = 0;

    controls.mode = gwy_radio_buttons_createl(G_CALLBACK(logistic_mode_changed),
                                              &controls, args->mode,
                                              _("_Train logistic regression"),
                                              LOGISTIC_MODE_TRAIN,
                                              _("_Use trained regression"),
                                              LOGISTIC_MODE_USE,
                                              NULL);
    button = gwy_radio_buttons_find(controls.mode, LOGISTIC_MODE_TRAIN);
    gtk_table_attach(GTK_TABLE(table), button, 0, 3, row, row+1,
                     GTK_FILL, 0, 0, 0);
    row++;

    button = gwy_radio_buttons_find(controls.mode, LOGISTIC_MODE_USE);
    gtk_table_attach(GTK_TABLE(table), button, 0, 3, row, row+1,
                     GTK_FILL, 0, 0, 0);
    row++;
    logistic_dialog_update(&controls, args);
    gtk_widget_show_all(dialog);

    do {
        response = gtk_dialog_run(GTK_DIALOG(dialog));
        switch (response) {
            case GTK_RESPONSE_CANCEL:
            case GTK_RESPONSE_DELETE_EVENT:
            logistic_values_update(&controls, args);
            gtk_widget_destroy(dialog);
            case GTK_RESPONSE_NONE:
            return;
            break;

            case GTK_RESPONSE_OK:
            break;

            case RESPONSE_RESET:
            logistic_reset_args(args);
            logistic_dialog_update(&controls, args);
            break;

            default:
            g_assert_not_reached();
            break;
        }
    } while (response != GTK_RESPONSE_OK);

    logistic_values_update(&controls, args);
    logistic_save_args(gwy_app_settings_get(), args);
    gtk_widget_destroy(dialog);
}

static GwyBrick *
create_feature_vector(GwyDataField *dfield)
{
    GwyBrick *features = NULL;
    GwyDataField *feature0 = NULL, *feature = NULL;
    gdouble max, min, avg, xreal, yreal, size;
    gdouble *fdata, *bdata;
    gint xres, yres, z, zres, i, ngauss;

    feature0 = gwy_data_field_duplicate(dfield);
    xres = gwy_data_field_get_xres(feature0);
    yres = gwy_data_field_get_yres(feature0);
    xreal = gwy_data_field_get_xreal(feature0);
    yreal = gwy_data_field_get_yreal(feature0);
    ngauss = 5;
    zres = NFEATURES;
    z = 0;
    max = gwy_data_field_get_max(feature0);
    min = gwy_data_field_get_min(feature0);
    g_return_val_if_fail(max - min > 0.0, NULL);
    gwy_data_field_multiply(feature0, 1.0/(max - min));
    avg = gwy_data_field_get_avg(feature0);
    gwy_data_field_add(feature0, -avg);

    features = gwy_brick_new(xres, yres, zres,
                              xreal, yreal, zres, TRUE);
    bdata = gwy_brick_get_data(features);
    fdata = gwy_data_field_get_data(feature0);
    memmove(bdata, fdata, xres * yres * sizeof(gdouble));
    z++;

    for (i = 0, size = 1.0; i < ngauss; i++, size *= 2.0) {
        feature = gwy_data_field_duplicate(feature0);
        gwy_data_field_filter_gaussian(feature, size * FWHM2SIGMA);
        max = gwy_data_field_get_max(feature);
        min = gwy_data_field_get_min(feature);
        g_return_val_if_fail(max - min > 0.0, NULL);
        gwy_data_field_multiply(feature, 1.0/(max - min));
        avg = gwy_data_field_get_avg(feature);
        gwy_data_field_add(feature, -avg);
        fdata = gwy_data_field_get_data(feature);
        memmove(bdata + z * xres * yres, fdata,
                xres * yres * sizeof(gdouble));
        z++;
        gwy_data_field_filter_laplacian(feature);
        max = gwy_data_field_get_max(feature);
        min = gwy_data_field_get_min(feature);
        g_return_val_if_fail(max - min > 0.0, NULL);
        gwy_data_field_multiply(feature, 1.0/(max - min));
        avg = gwy_data_field_get_avg(feature);
        gwy_data_field_add(feature, -avg);
        memmove(bdata + z * xres * yres, fdata,
                xres * yres * sizeof(gdouble));
        memmove(fdata, bdata + (z-1) * xres * yres,
                xres * yres * sizeof(gdouble));
        z++;
        gwy_data_field_filter_sobel(feature, GWY_ORIENTATION_HORIZONTAL);
        max = gwy_data_field_get_max(feature);
        min = gwy_data_field_get_min(feature);
        g_return_val_if_fail(max - min > 0.0, NULL);
        gwy_data_field_multiply(feature, 1.0/(max - min));
        avg = gwy_data_field_get_avg(feature);
        gwy_data_field_add(feature, -avg);
        memmove(bdata + z * xres * yres, fdata,
                xres * yres * sizeof(gdouble));
        memmove(fdata, bdata + (z-2) * xres * yres,
                xres * yres * sizeof(gdouble));
        z++;
        gwy_data_field_filter_sobel(feature, GWY_ORIENTATION_VERTICAL);
        max = gwy_data_field_get_max(feature);
        min = gwy_data_field_get_min(feature);
        g_return_val_if_fail(max - min > 0.0, NULL);
        gwy_data_field_multiply(feature, 1.0/(max - min));
        avg = gwy_data_field_get_avg(feature);
        gwy_data_field_add(feature, -avg);
        memmove(bdata + z * xres * yres, fdata,
                xres * yres * sizeof(gdouble));
        gwy_data_field_filter_sobel(feature, GWY_ORIENTATION_HORIZONTAL);
        max = gwy_data_field_get_max(feature);
        min = gwy_data_field_get_min(feature);
        g_return_val_if_fail(max - min > 0.0, NULL);
        gwy_data_field_multiply(feature, 1.0/(max - min));
        avg = gwy_data_field_get_avg(feature);
        gwy_data_field_add(feature, -avg);
        memmove(bdata + z * xres * yres, fdata,
                xres * yres * sizeof(gdouble));
        memmove(fdata, bdata + (z-3) * xres * yres,
                xres * yres * sizeof(gdouble));
        z++;
        g_object_unref(feature);
    }
    g_object_unref(feature0);

    return features;
}

static inline gdouble
sigmoid(gdouble z)
{
    return 1.0/(1.0 + exp(-z));
}

static void
train_logistic(GwyContainer *container, GwyBrick *features,
GwyDataField *mfield, gdouble *thetas, gdouble lambda)
{
    gdouble *grad, *oldgrad;
    gdouble epsilon, alpha, cost, sum;
    gint i, iter, maxiter, zres, id;
    gboolean converged = FALSE, cancelled = FALSE;

    gwy_app_data_browser_get_current(GWY_APP_DATA_FIELD_ID, &id,
                                     0);
    zres = gwy_brick_get_zres(features);
    thetas = g_malloc(zres * sizeof(gdouble));
    grad = g_malloc(zres * sizeof(gdouble));
    oldgrad = g_malloc(zres * sizeof(gdouble));
    for (i = 0; i < zres; i++) {
        thetas[i] = 0.0;
        oldgrad[i] = 0.0;
    }
    epsilon = 1E-5;
    alpha = 10.0;
    iter = 0;
    maxiter = 2000;
    gwy_app_wait_start(gwy_app_find_window_for_channel(container, id),
                       _("Training..."));
    while(!converged && !cancelled) {
        if (!gwy_app_wait_set_fraction((gdouble)iter/maxiter)) {
            cancelled = TRUE;
            break;
        }
        cost = cost_function(features, mfield, thetas, grad, lambda);

        sum = 0;
        for (i = 0; i < zres; i++) {
            sum += grad[i]*oldgrad[i];
        }

        if (sum > 0) {
            alpha *= 1.05;
        }
        else if (sum < 0) {
            alpha /= 2.0;
            for (i = 0; i < zres; i++) {
                grad[i] += oldgrad[i];
            }
        }

        converged = TRUE;
        for (i = 0;  i < zres; i++) {
            thetas[i] -= alpha * grad[i];
            if (fabs(grad[i]) > epsilon) {
                converged = FALSE;
            }
            oldgrad[i] = grad[i];
        }

        if (iter >= maxiter) {
            converged = TRUE;
        }
        fprintf(stderr, "iter=%d cost=%g grad[0] = %g grad[20] = %g alpha=%g\n",
                iter, cost, grad[0], grad[20], alpha);
        iter++;
    }

    for (i = 0; i < zres; i++) {
        fprintf(stderr,"thetas[%d] = %g\n", i, thetas[i]);
    }

    gwy_app_wait_finish();
    g_free(grad);
    g_free(oldgrad);
}

static gdouble
cost_function(GwyBrick *brick, GwyDataField *mask,
              gdouble *thetas, gdouble *grad, gdouble lambda)
{
    gint i, j, k, m, xres, yres, zres;
    GwyDataField *hfield;
    gdouble sum, jsum, h, x, y, theta;
    gdouble *bp, *mp, *hp;

    xres = gwy_brick_get_xres(brick);
    yres = gwy_brick_get_yres(brick);
    zres = gwy_brick_get_zres(brick);
    bp = gwy_brick_get_data(brick);
    m = xres * yres;

    hfield = gwy_data_field_new_alike(mask, TRUE);
    hp = gwy_data_field_get_data(hfield);
    mp = gwy_data_field_get_data(mask);
    jsum = 0;

    grad[0] = 0;
    for (k = 1; k < zres; k++) {
        grad[k] = thetas[k] * lambda / m;
    }

    for (i = 0; i < yres; i++)
        for (j = 0; j < xres; j++) {
            sum = 0;
            for (k = 0; k < zres; k++) {
                x = *(bp + k *(xres * yres) + i * xres + j);
                sum += x * thetas[k];
            }
            h = sigmoid(sum);
            *(hp + i * xres + j) = h;
            y = *(mp + i * xres + j);
            jsum += -log(h)*y - log(1-h)*(1-y);
            for (k = 0; k < zres; k++) {
                x = *(bp + k *(xres * yres) + i * xres + j);
                grad[k] += 1.0 / m * x * (h - y);
            }
        }
    jsum /= m;

    sum = 0;
    for (k = 1; k < zres; k++) {
        theta = *(thetas + k);
        sum += theta * theta;
    }
    jsum += sum * lambda/2.0/m;

    g_object_unref(hfield);

    return jsum;
}

static void
predict(GwyBrick *brick, gdouble *thetas, GwyDataField *dfield)
{
    gint i, j, k, xres, yres, zres;
    gdouble sum, x;
    gdouble *bp, *dp;

    xres = gwy_brick_get_xres(brick);
    yres = gwy_brick_get_yres(brick);
    zres = gwy_brick_get_zres(brick);
    bp = gwy_brick_get_data(brick);
    dp = gwy_data_field_get_data(dfield);

    for (i = 0; i < yres; i++)
        for (j = 0; j < yres; j++) {
            sum = 0;
            for (k = 0; k < zres; k++) {
                x = *(bp + k *(xres * yres) + i * xres + j);
                sum += x * thetas[k];
            }
            *(dp + i * xres + j) = sigmoid(sum);
        }
}

static void
predict_mask(GwyBrick *brick, gdouble *thetas, GwyDataField *mask)
{
    gint i, j, k, xres, yres, zres;
    gdouble sum, x;
    gdouble *bp, *mp;

    xres = gwy_brick_get_xres(brick);
    yres = gwy_brick_get_yres(brick);
    zres = gwy_brick_get_zres(brick);
    bp = gwy_brick_get_data(brick);
    mp = gwy_data_field_get_data(mask);

    for (i = 0; i < yres; i++)
        for (j = 0; j < xres; j++) {
            sum = 0;
            for (k = 0; k < zres; k++) {
                x = *(bp + k *(xres * yres) + i * xres + j);
                sum += x * thetas[k];
            }
            *(mp + i * xres + j) = (sigmoid(sum) > 0.5) ? 1.0 : 0.0;
        }
}

static void
logistic_mode_changed(G_GNUC_UNUSED GtkWidget *button,
                      LogisticControls *controls)
{
    controls->args->mode = gwy_radio_buttons_get_current(controls->mode);
}

static void
logistic_values_update(LogisticControls *controls,
                       LogisticArgs *args)
{
    args = controls->args;
}

static void
logistic_dialog_update(LogisticControls *controls,
                       LogisticArgs *args)
{
    controls->args = args;
}
static const gchar thetas_key[]  = "/module/logistic/thetas";

static void
logistic_load_args(GwyContainer *settings,
                   LogisticArgs *args)
{
    if (!gwy_container_gis_object_by_name(settings,
                                          thetas_key, &args->thetas)) {
        args->thetas = gwy_data_line_new(NFEATURES, NFEATURES, TRUE);
        logistic_reset_args(args);
    }
}

static void
logistic_save_args(GwyContainer *settings,
                   LogisticArgs *args)
{
    /*
    gwy_container_set_object_by_name(settings,
                                     thetas_key, args->thetas);
                                     */
}

static void
logistic_reset_args(LogisticArgs *args)
{
    gdouble thetas[NFEATURES];
    gdouble *p;
    gint i;

    thetas[0] = 1.25099;
    thetas[1] = 1.41348;
    thetas[2] = 1.41405;
    thetas[3] = -0.283954;
    thetas[4] = -0.0738085;
    thetas[5] = 2.95126;
    thetas[6] = -0.157917;
    thetas[7] = 0.0817234;
    thetas[8] = 0.0388422;
    thetas[9] = 3.60457;
    thetas[10] = 3.18731;
    thetas[11] = 3.92896;
    thetas[12] = 0.434874;
    thetas[13] = 2.02171;
    thetas[14] = -24.9334;
    thetas[15] = 0.116424;
    thetas[16] = 0.0659015;
    thetas[17] = -8.54693;
    thetas[18] = -38.0947;
    thetas[19] = -6.55105;
    thetas[20] = 1.31064;

    p = gwy_data_line_get_data(args->thetas);
    for (i = 0; i < NFEATURES; i++) {
        *(p++) = thetas[i];
    }
}

static void
logistic_filter_dx2(GwyDataField *dfield)
{
    gint xres, yres;

    static const gdouble dx2_kernel[] = {
        0.125, -0.25, 0.125,
        0.25,  -0.5,  0.25,
        0.125, -0.25, 0.125,
    };

    g_return_if_fail(GWY_IS_DATA_FIELD(dfield));
    xres = gwy_data_field_get_xres(dfield);
    yres = gwy_data_field_get_yres(dfield);

    gwy_data_field_convolve(dfield, dx2_kernel);
}

static void
logistic_filter_dy2(GwyDataField *dfield)
{
    gint xres, yres;

    static const gdouble dy2_kernel[] = {
        0.125,  0.25, 0.125,
        -0.25,  -0.5, -0.25,
        0.125,  0.25, 0.125,
    };

    g_return_if_fail(GWY_IS_DATA_FIELD(dfield));
    xres = gwy_data_field_get_xres(dfield);
    yres = gwy_data_field_get_yres(dfield);

    gwy_data_field_convolve(dfield, dy2_kernel);
}

static void
logistic_filter_dxdy(GwyDataField *dfield)
{
    gint xres, yres;

    static const gdouble dxdy_kernel[] = {
        0.5,  0, -0.5,
        0,    0, 0,
        -0.5, 0, 0.5,
    };

    g_return_if_fail(GWY_IS_DATA_FIELD(dfield));
    xres = gwy_data_field_get_xres(dfield);
    yres = gwy_data_field_get_yres(dfield);

    gwy_data_field_convolve(dfield, dxdy_kernel);
}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
