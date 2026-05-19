#ifndef _EETCONFIG_H_
#define _EETCONFIG_H_

#include <string_view>
#include <unordered_set>

namespace forte::eet {

  inline bool isMonitoringExcluded(const char* paFBId) {
    static const std::unordered_set<std::string_view> excluded = {
      "MGR",
      "MGR_FF",
      "START",
      "EMB_RES",
    };
    return excluded.count(paFBId) > 0;
  }

} // namespace forte::eet

#endif /* _EETCONFIG_H_ */