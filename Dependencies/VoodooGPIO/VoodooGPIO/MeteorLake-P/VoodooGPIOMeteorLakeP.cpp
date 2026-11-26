//
//  VoodooGPIOMeteorLakeP.cpp
//  VoodooGPIO
//
//  Meteor Lake-P / Arrow Lake-H pin controller (INTC1083, INTC105E).
//

#include "VoodooGPIOMeteorLakeP.hpp"

OSDefineMetaClassAndStructors(VoodooGPIOMeteorLakeP, VoodooGPIOIntel);

bool VoodooGPIOMeteorLakeP::start(IOService *provider) {
    this->pins = mtlp_pins;
    this->npins = ARRAY_SIZE(mtlp_pins);
    this->communities = mtlp_communities;
    this->ncommunities = ARRAY_SIZE(mtlp_communities);

    IOLog("%s::Loading GPIO Data for MeteorLake-P\n", getName());

    return VoodooGPIOIntel::start(provider);
}
