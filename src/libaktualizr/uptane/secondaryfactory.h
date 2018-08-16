#ifndef UPTANE_SECONDARYFACTORY_H_
#define UPTANE_SECONDARYFACTORY_H_

#include "ipuptanesecondary.h"
#include "isotpsecondary.h"
#include "legacysecondary.h"
#include "logging/logging.h"
#include "opcuasecondary.h"
#include "secondaryconfig.h"
#include "secondaryinterface.h"
#include "utilities/events.h"
#include "virtualsecondary.h"

namespace Uptane {

class SecondaryFactory {
 public:
  static std::shared_ptr<SecondaryInterface> makeSecondary(const SecondaryConfig& sconfig) {
    switch (sconfig.secondary_type) {
      case SecondaryType::kVirtual:
        return std::make_shared<VirtualSecondary>(sconfig);
        break;
      case SecondaryType::kLegacy:
        return std::make_shared<LegacySecondary>(sconfig);
        break;
      case SecondaryType::kIpUptane:
        return std::make_shared<IpUptaneSecondary>(sconfig);
      case SecondaryType::kIsoTpUptane:
        return std::make_shared<IsoTpSecondary>(sconfig);
      case SecondaryType::kOpcuaUptane:
#ifdef OPCUA_SECONDARY_ENABLED
        return std::make_shared<OpcuaSecondary>(sconfig);
#else
        LOG_ERROR << "Built with no OPC-UA secondary support";
        return std::shared_ptr<SecondaryInterface>();  // NULL-equivalent
#endif
      default:
        LOG_ERROR << "Unrecognized secondary type: " << static_cast<int>(sconfig.secondary_type);
        return std::shared_ptr<SecondaryInterface>();  // NULL-equivalent
    }
  }
};
}  // namespace Uptane

#endif  // UPTANE_SECONDARYFACTORY_H_
