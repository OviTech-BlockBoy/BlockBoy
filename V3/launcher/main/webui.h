#include <stdbool.h>

// Starts the HTTP file server. Returns false when it could not be started
// (port already in use, transfer buffer allocation failed). Callers must not
// report success to the user without checking.
bool webui_start(void);
void webui_stop(void);
// True while the HTTP server is actually listening.
bool webui_running(void);

// This device's default password, derived from its MAC. Unique per unit, so
// publishing the source does not hand out a password that works everywhere.
// It is shown in the file server dialog.
const char *webui_default_password(void);

// Re-reads the server password from settings. Call after changing it; it takes
// effect on the next request, no restart needed. An empty password = no check.
void webui_reload_password(void);
