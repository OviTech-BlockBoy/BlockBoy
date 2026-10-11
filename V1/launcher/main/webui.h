#include <stdbool.h>

// Starts the HTTP file server. Returns false when it could not be started
// (port already in use, transfer buffer allocation failed). Callers must not
// report success to the user without checking.
bool webui_start(void);
void webui_stop(void);
// True while the HTTP server is actually listening.
bool webui_running(void);

// Shortest password the server will accept. There is no "no password" option:
// the server hands out the whole SD card, and a check the user can switch off
// is not a check. It also costs the presumption of conformity under the RED,
// which is granted to EN 18031-1 only on the condition that the equipment
// cannot be operated without a password.
#define RG_WEBUI_PASSWORD_MIN 8

// This device's file server password. Generated and stored on first use, so it
// differs per unit and publishing the source gives nothing away. Never returns
// an empty string. Shown in the file server dialog.
const char *webui_password_current(void);

// Re-reads the server password from settings and clears the lockout. Call after
// changing it; it takes effect on the next request, no restart needed.
void webui_reload_password(void);
