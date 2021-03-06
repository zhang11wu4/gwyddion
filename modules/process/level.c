/*
 *  @(#) $Id$
 *  Copyright (C) 2003-2016 David Necas (Yeti), Petr Klapetek.
 *  E-mail: yeti@gwyddion.net, klapetek@gwyddion.net.
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
#include <string.h>
#include <gtk/gtk.h>
#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwymath.h>
#include <libprocess/level.h>
#include <libprocess/grains.h>
#include <libprocess/stats.h>
#include <libprocess/gwyprocesstypes.h>
#include <libgwydgets/gwystock.h>
#include <libgwydgets/gwydgetutils.h>
#include <libgwydgets/gwyradiobuttons.h>
#include <libgwymodule/gwymodule-process.h>
#include <app/gwyapp.h>
#include "preview.h"

#define LEVEL_RUN_MODES (GWY_RUN_IMMEDIATE | GWY_RUN_INTERACTIVE)

typedef struct {
    GwyMaskingType masking;
} LevelArgs;

typedef struct {
    LevelArgs *args;
    GSList *masking;
} LevelControls;

static gboolean module_register(void);
static void     level_func     (GwyContainer *data,
                                GwyRunType run,
                                const gchar *funcname);
static void     fix_zero       (GwyContainer *data,
                                GwyRunType run);
static gboolean level_dialog   (LevelArgs *args,
                                const gchar *title);
static void     masking_changed(GtkToggleButton *button,
                                LevelControls *controls);
static void     load_args      (GwyContainer *container,
                                const gchar *funcname,
                                LevelArgs *args);
static void     save_args      (GwyContainer *container,
                                const gchar *funcname,
                                LevelArgs *args);

static const LevelArgs level_defaults = {
    GWY_MASK_EXCLUDE
};

static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    N_("Levels data by simple plane subtraction or by rotation, "
       "and fixes minimal or mean value to zero."),
    "Yeti <yeti@gwyddion.net>",
    "2.0",
    "David Nečas (Yeti) & Petr Klapetek",
    "2003",
};

GWY_MODULE_QUERY(module_info)

static gboolean
module_register(void)
{
    gwy_process_func_register("level",
                              &level_func,
                              N_("/_Level/Plane _Level"),
                              GWY_STOCK_LEVEL,
                              LEVEL_RUN_MODES,
                              GWY_MENU_FLAG_DATA,
                              N_("Level data by mean plane subtraction"));
    gwy_process_func_register("level_rotate",
                              &level_func,
                              N_("/_Level/Level _Rotate"),
                              NULL,
                              LEVEL_RUN_MODES,
                              GWY_MENU_FLAG_DATA,
                              N_("Automatically level data by plane rotation"));
    gwy_process_func_register("fix_zero",
                              (GwyProcessFunc)&fix_zero,
                              N_("/_Level/Fix _Zero"),
                              GWY_STOCK_FIX_ZERO,
                              LEVEL_RUN_MODES,
                              GWY_MENU_FLAG_DATA,
                              N_("Shift minimum data value to zero"));
    gwy_process_func_register("zero_mean",
                              &level_func,
                              N_("/_Level/Zero _Mean Value"),
                              GWY_STOCK_ZERO_MEAN,
                              LEVEL_RUN_MODES,
                              GWY_MENU_FLAG_DATA,
                              N_("Shift mean data value to zero"));

    return TRUE;
}

static void
level_func(GwyContainer *data,
           GwyRunType run,
           const gchar *funcname)
{
    GwyDataField *dfield;
    GwyDataField *mfield;
    LevelArgs args;
    gdouble c, bx, by;
    gint xres, yres;
    GQuark quark;
    gint id;

    g_return_if_fail(run & LEVEL_RUN_MODES);
    gwy_app_data_browser_get_current(GWY_APP_DATA_FIELD_KEY, &quark,
                                     GWY_APP_DATA_FIELD, &dfield,
                                     GWY_APP_DATA_FIELD_ID, &id,
                                     GWY_APP_MASK_FIELD, &mfield,
                                     0);
    g_return_if_fail(dfield && quark);

    load_args(gwy_app_settings_get(), funcname, &args);
    if (run != GWY_RUN_IMMEDIATE && mfield) {
        const gchar *dialog_title = NULL;
        gboolean ok;

        if (gwy_strequal(funcname, "level"))
            dialog_title = _("Plane Level");
        else if (gwy_strequal(funcname, "level_rotate"))
            dialog_title = _("Level Rotate");
        else if (gwy_strequal(funcname, "zero_mean"))
            dialog_title = _("Zero Mean Value");
        else {
            g_assert_not_reached();
        }
        ok = level_dialog(&args, dialog_title);
        save_args(gwy_app_settings_get(), funcname, &args);
        if (!ok)
            return;
    }
    if (args.masking == GWY_MASK_IGNORE)
        mfield = NULL;

    xres = gwy_data_field_get_xres(dfield);
    yres = gwy_data_field_get_yres(dfield);

    gwy_app_undo_qcheckpoint(data, quark, NULL);

    if (gwy_stramong(funcname, "level", "level_rotate", NULL)) {
        if (mfield) {
            if (args.masking == GWY_MASK_EXCLUDE) {
                mfield = gwy_data_field_duplicate(mfield);
                gwy_data_field_grains_invert(mfield);
            }
            else
                g_object_ref(mfield);
        }

        if (mfield)
            gwy_data_field_area_fit_plane(dfield, mfield, 0, 0, xres, yres,
                                          &c, &bx, &by);
        else
            gwy_data_field_fit_plane(dfield, &c, &bx, &by);

        if (gwy_strequal(funcname, "level_rotate")) {
            bx = gwy_data_field_rtoj(dfield, bx);
            by = gwy_data_field_rtoi(dfield, by);
            gwy_data_field_plane_rotate(dfield, atan2(bx, 1), atan2(by, 1),
                                        GWY_INTERPOLATION_LINEAR);
            gwy_debug("b = %g, alpha = %g deg, c = %g, beta = %g deg",
                      bx, 180/G_PI*atan2(bx, 1), by, 180/G_PI*atan2(by, 1));
        }
        else {
            c = -0.5*(bx*gwy_data_field_get_xres(dfield)
                      + by*gwy_data_field_get_yres(dfield));
            gwy_data_field_plane_level(dfield, c, bx, by);
        }
        GWY_OBJECT_UNREF(mfield);
    }
    else if (gwy_strequal(funcname, "zero_mean")) {
        if (mfield) {
            c = gwy_data_field_area_get_avg_mask(dfield, mfield, args.masking,
                                                 0, 0, xres, yres);
        }
        else
            c = gwy_data_field_get_avg(dfield);
        gwy_data_field_add(dfield, -c);
    }
    else {
        g_assert_not_reached();
    }

    gwy_app_channel_log_add_proc(data, id, id);
    gwy_data_field_data_changed(dfield);
}

static void
fix_zero(GwyContainer *data, GwyRunType run)
{
    GwyDataField *dfield;
    GQuark quark;
    gint id;

    g_return_if_fail(run & LEVEL_RUN_MODES);
    gwy_app_data_browser_get_current(GWY_APP_DATA_FIELD_KEY, &quark,
                                     GWY_APP_DATA_FIELD, &dfield,
                                     GWY_APP_DATA_FIELD_ID, &id,
                                     0);
    g_return_if_fail(dfield && quark);
    gwy_app_undo_qcheckpoint(data, quark, NULL);
    gwy_data_field_add(dfield, -gwy_data_field_get_min(dfield));
    gwy_app_channel_log_add_proc(data, id, id);
    gwy_data_field_data_changed(dfield);
}

/* XXX: Duplicate with facet-level.c. Merge modules? */
static gboolean
level_dialog(LevelArgs *args,
             const gchar *title)
{
    GtkWidget *dialog, *label, *table;
    gint row, response;
    LevelControls controls;

    controls.args = args;

    dialog = gtk_dialog_new_with_buttons(title, NULL, 0,
                                         _("_Reset"), RESPONSE_RESET,
                                         GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                         GTK_STOCK_OK, GTK_RESPONSE_OK,
                                         NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gwy_help_add_to_proc_dialog(GTK_DIALOG(dialog), GWY_HELP_DEFAULT);

    table = gtk_table_new(4, 3, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 2);
    gtk_table_set_col_spacings(GTK_TABLE(table), 6);
    gtk_container_set_border_width(GTK_CONTAINER(table), 4);
    gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), table);
    row = 0;

    label = gwy_label_new_header(_("Masking Mode"));
    gtk_table_attach(GTK_TABLE(table), label,
                     0, 3, row, row+1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    row++;

    controls.masking = gwy_radio_buttons_create(gwy_masking_type_get_enum(), -1,
                                                G_CALLBACK(masking_changed),
                                                &controls, args->masking);
    row = gwy_radio_buttons_attach_to_table(controls.masking, GTK_TABLE(table),
                                            3, row);

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

            case RESPONSE_RESET:
            *args = level_defaults;
            gwy_radio_buttons_set_current(controls.masking, args->masking);
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
masking_changed(GtkToggleButton *button, LevelControls *controls)
{
    if (!gtk_toggle_button_get_active(button))
        return;

    controls->args->masking = gwy_radio_buttons_get_current(controls->masking);
}

static const gchar masking_key[] = "/module/%s/mode";

static void
sanitize_args(LevelArgs *args)
{
    args->masking = gwy_enum_sanitize_value(args->masking,
                                            GWY_TYPE_MASKING_TYPE);
}

static void
load_args(GwyContainer *container, const gchar *funcname, LevelArgs *args)
{
    gchar key[32];

    *args = level_defaults;

    g_snprintf(key, sizeof(key), masking_key, funcname);
    gwy_container_gis_enum_by_name(container, key, &args->masking);
    sanitize_args(args);
}

static void
save_args(GwyContainer *container, const gchar *funcname, LevelArgs *args)
{
    gchar key[32];

    g_snprintf(key, sizeof(key), masking_key, funcname);
    gwy_container_set_enum_by_name(container, key, args->masking);
}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
