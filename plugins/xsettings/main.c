#include <gdk/gdk.h>

#define GDK_BACKEND "x11"
#include "gsd-main-helper.h"
#include "gsd-xsettings-manager.h"

int
main (int argc, char **argv)
{
        const gchar *setup_display = getenv ("GNOME_SETUP_DISPLAY");
        if (setup_display && *setup_display != '\0')
                g_setenv ("DISPLAY", setup_display, TRUE);

        gdk_set_allowed_backends (GDK_BACKEND);

        /* GDK would fail to initialize with e.g. GDK_BACKEND=wayland */
        g_unsetenv ("GDK_BACKEND");

        return gsd_main_helper (GSD_TYPE_XSETTINGS_MANAGER, argc, argv);
}

