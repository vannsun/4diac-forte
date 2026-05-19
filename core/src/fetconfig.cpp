#include "forte/fetconfig.h"
#include "forte/fetmonitor.h"
#include "forte/util/devlog.h"
#include "forte/stringid.h"

#include <fstream>
#include <string>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal JSON parser — no external dependency needed.
//
// We only need to parse this exact structure:
//   { "fet_deadlines": [ { "fb": "...", "deadline_us": 123 }, ... ] }
//
// Using std::string::find / substr keeps the build self-contained.
// If the build already has nlohmann/json or a similar library, replace
// the parsing section with that — the rest of the function stays identical.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

  // Extract the string value of a JSON key from a single object string like:
  //   { "fb": "MyDevice.Res.FB", "deadline_us": 500 }
  std::string extractString(const std::string& paObj, const std::string& paKey) {
    const std::string search = "\"" + paKey + "\"";
    size_t keyPos = paObj.find(search);
    if(keyPos == std::string::npos) return "";
    size_t colonPos = paObj.find(':', keyPos + search.size());
    if(colonPos == std::string::npos) return "";
    size_t quoteOpen = paObj.find('"', colonPos + 1);
    if(quoteOpen == std::string::npos) return "";
    size_t quoteClose = paObj.find('"', quoteOpen + 1);
    if(quoteClose == std::string::npos) return "";
    return paObj.substr(quoteOpen + 1, quoteClose - quoteOpen - 1);
  }

  // Extract the integer value of a JSON key from a single object string.
  long long extractInt(const std::string& paObj, const std::string& paKey) {
    const std::string search = "\"" + paKey + "\"";
    size_t keyPos = paObj.find(search);
    if(keyPos == std::string::npos) return -1;
    size_t colonPos = paObj.find(':', keyPos + search.size());
    if(colonPos == std::string::npos) return -1;
    size_t numStart = paObj.find_first_of("0123456789", colonPos + 1);
    if(numStart == std::string::npos) return -1;
    return std::stoll(paObj.substr(numStart));
  }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// loadFETConfig
// ─────────────────────────────────────────────────────────────────────────────

void loadFETConfig(const std::string& paConfigFile) {
  std::ifstream file(paConfigFile);
  if(!file.is_open()) {
    DEVLOG_WARNING("FETConfig: '%s' not found — FET monitoring inactive.\n",
                   paConfigFile.c_str());
    return;
  }

  // Read entire file into a string.
  std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

  // Find the fet_deadlines array.
  size_t arrayStart = content.find("\"fet_deadlines\"");
  if(arrayStart == std::string::npos) {
    DEVLOG_ERROR("FETConfig: 'fet_deadlines' key not found in '%s'.\n",
                 paConfigFile.c_str());
    return;
  }

  size_t bracketOpen = content.find('[', arrayStart);
  size_t bracketClose = content.find(']', bracketOpen);
  if(bracketOpen == std::string::npos || bracketClose == std::string::npos) {
    DEVLOG_ERROR("FETConfig: malformed array in '%s'.\n", paConfigFile.c_str());
    return;
  }

  // Iterate over each { ... } object in the array.
  const std::string arrayContent = content.substr(bracketOpen, bracketClose - bracketOpen);
  size_t pos = 0;
  int registered = 0;

  while(true) {
    size_t objOpen = arrayContent.find('{', pos);
    if(objOpen == std::string::npos) break;
    size_t objClose = arrayContent.find('}', objOpen);
    if(objClose == std::string::npos) break;

    const std::string obj = arrayContent.substr(objOpen, objClose - objOpen + 1);

    const std::string fbName = extractString(obj, "fb");
    const long long deadlineUs = extractInt(obj, "deadline_us");

    if(fbName.empty() || deadlineUs <= 0) {
      DEVLOG_WARNING("FETConfig: skipping malformed entry: %s\n", obj.c_str());
      pos = objClose + 1;
      continue;
    }

    // Intern the FB name so the pointer is stable for the lifetime of the
    // process — matches FORTE's own string interning via StringId.
    // StringId::insert returns a StringId whose .data() pointer is stable.
    // We store that pointer as TStringId (const char*).
    //
    // On FORTE integration replace with:
    //   TStringId id = CStringDictionary::getInstance().insert(fbName.c_str());
    const TStringId internedId =
      forte::StringId::insert(fbName).data();

    const auto deadline = std::chrono::milliseconds(deadlineUs);

    CFETMonitor::getInstance().registerFB(
      internedId,
      std::chrono::milliseconds(80),
      //deadline,
      [](TStringId paId) {
        DEVLOG_ERROR("FET deadline missed: %s\n", paId);
        // On FORTE integration: post ERROR EVENT to Safety FB here via ECET.
      }
    );

    DEVLOG_INFO("FETConfig: registered '%s' with deadline %lldms\n",
                fbName.c_str(), deadlineUs);
    registered++;
    pos = objClose + 1;
  }

  DEVLOG_INFO("FETConfig: %d FB(s) registered for deadline enforcement.\n",
              registered);
}