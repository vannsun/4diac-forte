#ifndef _FETCONFIG_H_
#define _FETCONFIG_H_

#include <string>

/*! \brief Loads FET deadline configuration from a JSON file and registers
 *  all listed FBs with CFETMonitor.
 *
 *  Expected format:
 *  {
 *    "fet_deadlines": [
 *      { "fb": "Device.Resource.FBInstanceName", "deadline_us": 500 },
 *      ...
 *    ]
 *  }
 *
 *  - "fb" must match the full qualified instance name FORTE uses internally.
 *    Use the same name you see in FORTE's log output for that FB.
 *  - "deadline_us" is in microseconds (integer).
 *  - FBs not listed in the file are silently ignored by CFETMonitor.
 *  - If the file does not exist or cannot be parsed, a warning is logged
 *    and FET monitoring is simply not activated — FORTE runs normally.
 *
 *  \param paConfigFile  Path to the JSON config file.
 *                       Default: "fet_config.json" in the working directory.
 */
void loadFETConfig(const std::string& paConfigFile = "fet_config.json");
#endif // _FETCONFIG_H_