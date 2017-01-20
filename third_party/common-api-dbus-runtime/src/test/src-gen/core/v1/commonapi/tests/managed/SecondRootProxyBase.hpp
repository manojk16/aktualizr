/*
* This file was generated by the CommonAPI Generators.
* Used org.genivi.commonapi.core 3.1.5.v201512091035.
* Used org.franca.core 0.9.1.201412191134.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file, You can obtain one at
* http://mozilla.org/MPL/2.0/.
*/
#ifndef V1_COMMONAPI_TESTS_MANAGED_Second_Root_PROXY_BASE_HPP_
#define V1_COMMONAPI_TESTS_MANAGED_Second_Root_PROXY_BASE_HPP_

#include <v1/commonapi/tests/managed/SecondRoot.hpp>


#include <v1/commonapi/tests/managed/LeafInterfaceStub.hpp>

#if !defined (COMMONAPI_INTERNAL_COMPILATION)
#define COMMONAPI_INTERNAL_COMPILATION
#endif


#include <CommonAPI/ProxyManager.hpp>
#include <CommonAPI/Proxy.hpp>

#undef COMMONAPI_INTERNAL_COMPILATION

namespace v1 {
namespace commonapi {
namespace tests {
namespace managed {

class SecondRootProxyBase
    : virtual public CommonAPI::Proxy {
public:




    virtual CommonAPI::ProxyManager& getProxyManagerLeafInterface() = 0;
};

} // namespace managed
} // namespace tests
} // namespace commonapi
} // namespace v1


// Compatibility
namespace v1_0 = v1;

#endif // V1_COMMONAPI_TESTS_MANAGED_Second_Root_PROXY_BASE_HPP_