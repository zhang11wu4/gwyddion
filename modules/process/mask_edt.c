/*
 *  @(#) $Id$
 *  Copyright (C) 2014-2015 David Necas (Yeti).
 *  E-mail: yeti@gwyddion.net.
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
#include <stdlib.h>
#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwymath.h>
#include <libprocess/arithmetic.h>
#include <libprocess/grains.h>
#include <libgwydgets/gwyradiobuttons.h>
#include <libgwydgets/gwycombobox.h>
#include <libgwydgets/gwystock.h>
#include <libgwydgets/gwydgetutils.h>
#include <libgwymodule/gwymodule-process.h>
#include <app/gwyapp.h>

#define MASKEDT_RUN_MODES (GWY_RUN_IMMEDIATE | GWY_RUN_INTERACTIVE)
#define MASKTHIN_RUN_MODES GWY_RUN_IMMEDIATE

typedef enum {
    MASKEDT_INTERIOR = 0,
    MASKEDT_EXTERIOR = 1,
    MASKEDT_SIGNED   = 2,
    MASKEDT_NTYPES
} MaskEdtType;

typedef struct {
    gdouble dist, ndist;
    gint j, i;
} ThinCandidate;

typedef struct {
    MaskEdtType mask_type;
    GwyDistanceTransformType dist_type;
    gboolean from_border;
} MaskEdtArgs;

typedef struct {
    MaskEdtArgs *args;
    GtkWidget *dialog;
    GtkWidget *dist_type;
    GSList *mask_type;
    GtkWidget *from_border;
} MaskEdtControls;

static gboolean      module_register              (void);
static void          mask_thin                    (GwyContainer *data,
                                                   GwyRunType run);
static void          thin_mask                    (GwyDataField *mask);
static void          mask_edt                     (GwyContainer *data,
                                                   GwyRunType run);
static gboolean      maskedt_dialog               (MaskEdtArgs *args);
static void          mask_type_changed            (GtkToggleButton *toggle,
                                                   MaskEdtControls *controls);
static void          dist_type_changed            (GtkComboBox *combo,
                                                   MaskEdtControls *controls);
static void          from_border_changed          (GtkToggleButton *toggle,
                                                   MaskEdtControls *controls);
static GwyDataField* maskedt_do                   (GwyDataField *mfield,
                                                   GwyDataField *dfield,
                                                   MaskEdtArgs *args);
static void          maskedt_sanitize_args        (MaskEdtArgs *args);
static void          maskedt_load_args            (GwyContainer *settings,
                                                   MaskEdtArgs *args);
static void          maskedt_save_args            (GwyContainer *settings,
                                                   MaskEdtArgs *args);

static const MaskEdtArgs maskedt_defaults = {
    MASKEDT_INTERIOR,
    GWY_DISTANCE_TRANSFORM_EUCLIDEAN,
    TRUE,
};

static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    N_("Performs simple and true Euclidean distance transforms of masks."),
    "Yeti <yeti@gwyddion.net>",
    "2.1",
    "David Nečas (Yeti)",
    "2014",
};

GWY_MODULE_QUERY(module_info)

static gboolean
module_register(void)
{
    gwy_process_func_register("mask_edt",
                              (GwyProcessFunc)&mask_edt,
                              N_("/_Mask/Distanc_e Transform..."),
                              GWY_STOCK_DISTANCE_TRANSFORM,
                              MASKEDT_RUN_MODES,
                              GWY_MENU_FLAG_DATA_MASK | GWY_MENU_FLAG_DATA,
                              N_("Distance transform of mask"));
    gwy_process_func_register("mask_thin",
                              (GwyProcessFunc)&mask_thin,
                              N_("/_Mask/Thi_n"),
                              GWY_STOCK_MASK_THIN,
                              MASKTHIN_RUN_MODES,
                              GWY_MENU_FLAG_DATA_MASK | GWY_MENU_FLAG_DATA,
                              N_("Thin mask"));

    return TRUE;
}

static void
mask_thin(GwyContainer *data, GwyRunType run)
{
    GwyDataField *mfield;
    GQuark quark;
    gint id;

    g_return_if_fail(run & MASKTHIN_RUN_MODES);
    gwy_app_data_browser_get_current(GWY_APP_MASK_FIELD, &mfield,
                                     GWY_APP_MASK_FIELD_KEY, &quark,
                                     GWY_APP_DATA_FIELD_ID, &id,
                                     0);
    g_return_if_fail(mfield);

    gwy_app_undo_qcheckpointv(data, 1, &quark);
    thin_mask(mfield);
    gwy_data_field_data_changed(mfield);
    gwy_app_channel_log_add_proc(data, id, id);
}

static gint
compare_candidate(gconstpointer pa, gconstpointer pb)
{
    const ThinCandidate *a = (const ThinCandidate*)pa;
    const ThinCandidate *b = (const ThinCandidate*)pb;

    /* Take pixels with lowest Euclidean distances first. */
    if (a->dist < b->dist)
        return -1;
    if (a->dist > b->dist)
        return 1;

    /* If equal, take pixels with largest Euclidean distance *of their
     * neighbours* first.  This essentially mean flat edges go before corners,
     * preserving useful branches. */
    if (a->ndist > b->ndist)
        return -1;
    if (a->ndist < b->ndist)
        return 1;

    /* When desperate, sort bottom and right coordinates first so that we try
     * to remove them first.  Anyway we must impose some rule to make the
     * sort stable. */
    if (a->i > b->i)
        return -1;
    if (a->i < b->i)
        return 1;
    if (a->j > b->j)
        return -1;
    if (a->j < b->j)
        return 1;

    return 0;
}

static void
thin_mask(GwyDataField *mask)
{
    /* TRUE means removing the central pixel in a 3x3 pixel configuration does
     * not break any currently connected parts. */
    static const gboolean ok_to_remove[0x100] = {
        TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE,
        TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,
        TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,
        TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,
        TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,
        TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,
        TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,
        FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,
        TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,
        TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  TRUE,  TRUE,
    };

    GwyDataField *dfield;
    gint i, j, k, xres = mask->xres, yres = mask->yres, ncand;
    gdouble *d, *m;
    ThinCandidate *candidates;

    dfield = gwy_data_field_duplicate(mask);
    gwy_data_field_copy(mask, dfield, FALSE);
    gwy_data_field_grain_distance_transform(dfield);
    d = gwy_data_field_get_data(dfield);
    m = gwy_data_field_get_data(mask);

    ncand = 0;
    for (k = 0; k < xres*yres; k++) {
        if (d[k] > 0.0)
            ncand++;
    }

    candidates = g_new(ThinCandidate, ncand);
    k = 0;
    for (i = 0; i < yres; i++) {
        for (j = 0; j < xres; j++) {
            if (d[i*xres + j] > 0.0) {
                gdouble ndist = 0.0;
                candidates[k].i = i;
                candidates[k].j = j;
                candidates[k].dist = d[i*xres + j];

                if (i && j)
                   ndist += d[(i-1)*xres + (j-1)];
                if (i)
                   ndist += d[(i-1)*xres + j];
                if (i && j < xres-1)
                    ndist += d[(i-1)*xres + (j+1)];
                if (j < xres-1)
                    ndist += d[i*xres + (j+1)];
                if (i < yres-1 && j < xres-1)
                    ndist += d[(i+1)*xres + (j+1)];
                if (i < yres-1)
                    ndist += d[(i+1)*xres + j];
                if (i < yres-1 && j)
                    ndist += d[(i+1)*xres + (j-1)];
                if (j)
                   ndist += d[i*xres + (j-1)];

                candidates[k].ndist = ndist;
                k++;
            }
        }
    }
    g_assert(k == ncand);

    qsort(candidates, ncand, sizeof(ThinCandidate), &compare_candidate);

    for (k = 0; k < ncand; k++) {
        guint b = 0;

        i = candidates[k].i;
        j = candidates[k].j;
        if (i && j && d[(i-1)*xres + (j-1)] > 0.0)
            b |= 1;
        if (i && d[(i-1)*xres + j] > 0.0)
            b |= 2;
        if (i && j < xres-1 && d[(i-1)*xres + (j+1)] > 0.0)
            b |= 4;
        if (j < xres-1 && d[i*xres + (j+1)] > 0.0)
            b |= 8;
        if (i < yres-1 && j < xres-1 && d[(i+1)*xres + (j+1)] > 0.0)
            b |= 16;
        if (i < yres-1 && d[(i+1)*xres + j] > 0.0)
            b |= 32;
        if (i < yres-1 && j && d[(i+1)*xres + (j-1)] > 0.0)
            b |= 64;
        if (j && d[i*xres + (j-1)] > 0.0)
            b |= 128;

        if (ok_to_remove[b]) {
            d[i*xres + j] = 0.0;
            m[i*xres + j] = 0.0;
        }
    }

    g_free(candidates);
    g_object_unref(dfield);
}

static void
mask_edt(GwyContainer *data, GwyRunType run)
{
    GwyDataField *mfield, *dfield;
    MaskEdtArgs args;
    gint oldid, newid;

    g_return_if_fail(run & MASKEDT_RUN_MODES);
    gwy_app_data_browser_get_current(GWY_APP_MASK_FIELD, &mfield,
                                     GWY_APP_DATA_FIELD, &dfield,
                                     GWY_APP_DATA_FIELD_ID, &oldid,
                                     0);
    g_return_if_fail(mfield && dfield);

    maskedt_load_args(gwy_app_settings_get(), &args);
    if (run == GWY_RUN_IMMEDIATE || maskedt_dialog(&args)) {
        dfield = maskedt_do(mfield, dfield, &args);

        newid = gwy_app_data_browser_add_data_field(dfield, data, TRUE);
        g_object_unref(dfield);
        gwy_app_sync_data_items(data, data, oldid, newid, FALSE,
                                GWY_DATA_ITEM_GRADIENT,
                                GWY_DATA_ITEM_MASK_COLOR,
                                GWY_DATA_ITEM_REAL_SQUARE,
                                0);
        gwy_app_set_data_field_title(data, newid, _("Distance Transform"));
        gwy_app_channel_log_add_proc(data, oldid, newid);
    }
    maskedt_save_args(gwy_app_settings_get(), &args);
}

static gboolean
maskedt_dialog(MaskEdtArgs *args)
{
    static const GwyEnum mask_types[] = {
        { N_("Interior"),  MASKEDT_INTERIOR },
        { N_("Exterior"),  MASKEDT_EXTERIOR },
        { N_("Two-sided"), MASKEDT_SIGNED   },
    };

    MaskEdtControls controls;
    GtkWidget *dialog;
    GtkWidget *table, *label;
    gint response, row = 0;

    controls.args = args;

    dialog = gtk_dialog_new_with_buttons(_("Distance Transform"),
                                         NULL, 0,
                                         GTK_STOCK_CANCEL,
                                         GTK_RESPONSE_CANCEL,
                                         GTK_STOCK_OK,
                                         GTK_RESPONSE_OK,
                                         NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gwy_help_add_to_proc_dialog(GTK_DIALOG(dialog), GWY_HELP_DEFAULT);
    controls.dialog = dialog;

    table = gtk_table_new(6, 4, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 2);
    gtk_table_set_col_spacings(GTK_TABLE(table), 6);
    gtk_container_set_border_width(GTK_CONTAINER(table), 4);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), table, TRUE, TRUE, 0);

    controls.dist_type
        = gwy_enum_combo_box_new(gwy_distance_transform_type_get_enum(), -1,
                                 G_CALLBACK(dist_type_changed), &controls,
                                  args->dist_type, TRUE);
    gwy_table_attach_hscale(table, row++, _("_Distance type:"), NULL,
                            GTK_OBJECT(controls.dist_type),
                            GWY_HSCALE_WIDGET);

    label = gtk_label_new(_("Output type:"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label,
                     0, 1, row, row+1, GTK_FILL, 0, 0, 0);
    row++;

    controls.mask_type = gwy_radio_buttons_create(mask_types,
                                                  G_N_ELEMENTS(mask_types),
                                                  G_CALLBACK(mask_type_changed),
                                                  &controls,
                                                  args->mask_type);
    row = gwy_radio_buttons_attach_to_table(controls.mask_type,
                                            GTK_TABLE(table),
                                            3, row);
    gtk_table_set_row_spacing(GTK_TABLE(table), row-1, 8);

    controls.from_border
        = gtk_check_button_new_with_mnemonic(_("Shrink from _border"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(controls.from_border),
                                 args->from_border);
    gtk_table_attach(GTK_TABLE(table), controls.from_border,
                     0, 3, row, row+1, GTK_FILL, 0, 0, 0);
    g_signal_connect(controls.from_border, "toggled",
                     G_CALLBACK(from_border_changed), &controls);
    row++;

    gtk_widget_show_all(dialog);

    do {
        response = gtk_dialog_run(GTK_DIALOG(dialog));
        switch (response) {
            case GTK_RESPONSE_CANCEL:
            case GTK_RESPONSE_DELETE_EVENT:
            gtk_widget_destroy(dialog);
            case GTK_RESPONSE_NONE:
            return FALSE;
            break;

            case GTK_RESPONSE_OK:
            break;

            default:
            g_assert_not_reached();
            break;
        }
    } while (response != GTK_RESPONSE_OK);

    gtk_widget_destroy(dialog);

    return TRUE;
}

static void
mask_type_changed(G_GNUC_UNUSED GtkToggleButton *toggle,
                  MaskEdtControls *controls)
{
    controls->args->mask_type
        = gwy_radio_buttons_get_current(controls->mask_type);
}

static void
dist_type_changed(GtkComboBox *combo,
                  MaskEdtControls *controls)
{
    controls->args->dist_type = gwy_enum_combo_box_get_active(combo);
}

static void
from_border_changed(GtkToggleButton *toggle,
                    MaskEdtControls *controls)
{
    controls->args->from_border = gtk_toggle_button_get_active(toggle);
}

static GwyDataField*
maskedt_do(GwyDataField *mfield,
           GwyDataField *dfield,
           MaskEdtArgs *args)
{
    GwyDistanceTransformType dtype = args->dist_type;
    gboolean from_border = args->from_border;
    GwySIUnit *unitxy, *unitz;
    gdouble q;

    dfield = gwy_data_field_duplicate(dfield);
    gwy_data_field_copy(mfield, dfield, FALSE);

    if (args->mask_type == MASKEDT_INTERIOR) {
        gwy_data_field_grain_simple_dist_trans(dfield, dtype, from_border);
    }
    else if (args->mask_type == MASKEDT_EXTERIOR) {
        gwy_data_field_grains_invert(dfield);
        gwy_data_field_grain_simple_dist_trans(dfield, dtype, from_border);
    }
    else if (args->mask_type == MASKEDT_SIGNED) {
        GwyDataField *tmp = gwy_data_field_duplicate(dfield);

        gwy_data_field_grain_simple_dist_trans(dfield, dtype, from_border);
        gwy_data_field_grains_invert(tmp);
        gwy_data_field_grain_simple_dist_trans(tmp, dtype, from_border);
        gwy_data_field_subtract_fields(dfield, dfield, tmp);
        g_object_unref(tmp);
    }

    q = sqrt(gwy_data_field_get_xmeasure(dfield)
             * gwy_data_field_get_ymeasure(dfield));
    gwy_data_field_multiply(dfield, q);
    unitxy = gwy_data_field_get_si_unit_xy(dfield);
    unitz = gwy_data_field_get_si_unit_z(dfield);
    gwy_serializable_clone(G_OBJECT(unitxy), G_OBJECT(unitz));

    return dfield;
}

static const gchar dist_type_key[]   = "/module/mask_edt/dist_type";
static const gchar mask_type_key[]   = "/module/mask_edt/mask_type";
static const gchar from_border_key[] = "/module/mask_edt/from_border";

static void
maskedt_sanitize_args(MaskEdtArgs *args)
{
    args->mask_type = MIN(args->mask_type, MASKEDT_NTYPES-1);
    args->dist_type = gwy_enum_sanitize_value(args->dist_type,
                                              GWY_TYPE_DISTANCE_TRANSFORM_TYPE);
    args->from_border = !!args->from_border;
}

static void
maskedt_load_args(GwyContainer *settings,
                  MaskEdtArgs *args)
{
    *args = maskedt_defaults;
    gwy_container_gis_enum_by_name(settings, mask_type_key, &args->mask_type);
    gwy_container_gis_enum_by_name(settings, dist_type_key, &args->dist_type);
    gwy_container_gis_boolean_by_name(settings, from_border_key,
                                      &args->from_border);
    maskedt_sanitize_args(args);
}

static void
maskedt_save_args(GwyContainer *settings,
                  MaskEdtArgs *args)
{
    gwy_container_set_enum_by_name(settings, mask_type_key, args->mask_type);
    gwy_container_set_enum_by_name(settings, dist_type_key, args->dist_type);
    gwy_container_set_boolean_by_name(settings, from_border_key,
                                      args->from_border);
}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
