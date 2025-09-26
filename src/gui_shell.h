#ifndef GUI_SHELL_H
#define GUI_SHELL_H

#include "uv_viewer.h"

int uv_gui_run(UvViewer *viewer, const UvViewerConfig *cfg, const char *program_name);

#endif // GUI_SHELL_H
