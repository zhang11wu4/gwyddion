/*
 *  @(#) $Id$
 *  Copyright (C) 2003 David Necas (Yeti), Petr Klapetek.
 *  E-mail: yeti@physics.muni.cz, klapetek@physics.muni.cz.
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111 USA
 */

#include <math.h>
#include <libgwyddion/gwymacros.h>
#include <libgwymodule/gwymodule.h>
#include <libprocess/datafield.h>
#include <app/app.h>

#define LEVEL_RUN_MODES \
    (GWY_RUN_NONINTERACTIVE | GWY_RUN_WITH_DEFAULTS)

static gboolean    module_register            (const gchar *name);
static gboolean    level                      (GwyContainer *data,
                                               GwyRunType run);
static gboolean    level_rotate               (GwyContainer *data,
                                               GwyRunType run);

/* The module info. */
static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    "level",
    "Simple automatic levelling.",
    "Yeti",
    "1.0",
    "Yeti & PK",
    "2003",
};

/* This is the ONLY exported symbol.  The argument is the module info.
 * NO semicolon after. */
GWY_MODULE_QUERY(module_info)

static gboolean
module_register(const gchar *name)
{
    static GwyProcessFuncInfo level_func_info = {
        "level",
        "/_Level/Level",
        (GwyProcessFunc)&level,
        LEVEL_RUN_MODES,
    };
    static GwyProcessFuncInfo level_rotate_func_info = {
        "level_rotate",
        "/_Level/Level Rotate (BROKEN)",
        (GwyProcessFunc)&level_rotate,
        LEVEL_RUN_MODES,
    };

    gwy_process_func_register(name, &level_func_info);
    gwy_process_func_register(name, &level_rotate_func_info);

    return TRUE;
}

static gboolean
level(GwyContainer *data, GwyRunType run)
{
    GwyDataField *dfield;
    gdouble a, b, c;

    g_return_val_if_fail(run & LEVEL_RUN_MODES, FALSE);
    dfield = GWY_DATA_FIELD(gwy_container_get_object_by_name(data, "/0/data"));
    gwy_app_undo_checkpoint(data, "/0/data");
    gwy_data_field_plane_coeffs(dfield, &a, &b, &c);
    gwy_data_field_plane_level(dfield, a, b, c);

    return TRUE;
}

static gboolean
level_rotate(GwyContainer *data, GwyRunType run)
{
    GwyDataField *dfield;
    gdouble a, b, c;

    g_return_val_if_fail(run & LEVEL_RUN_MODES, FALSE);
    g_warning("level_rotate is borken!");
    dfield = GWY_DATA_FIELD(gwy_container_get_object_by_name(data, "/0/data"));
    gwy_app_undo_checkpoint(data, "/0/data");
    gwy_data_field_plane_coeffs(dfield, &a, &b, &c);
    /* FIXME: what funny scale the b and c have? */
    gwy_data_field_plane_rotate(dfield,
                                180/G_PI*atan2(b, 1),
                                180/G_PI*atan2(c, 1),
                                GWY_INTERPOLATION_BILINEAR);
    gwy_debug("%s: b = %g, alpha = %g deg, c = %g, beta = %g deg",
              __FUNCTION__, b, 180/G_PI*atan2(b, 1), c, 180/G_PI*atan2(c, 1));

    return TRUE;
}

/* vim: set cin et ts=4 sw=4 cino=>1s,e0,n0,f0,{0,}0,^0,\:1s,=0,g1s,h0,t0,+1s,c3,(0,u0 : */
