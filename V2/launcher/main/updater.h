void updater_show_dialog(void);

// Runs the automatic update check; call it from the launcher's main loop. `idle`
// says the menu has been left alone for a moment. True = a dialog was shown and
// the screen needs redrawing.
bool updater_poll(bool idle);
