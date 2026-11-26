//
//  VoodooGPIO.cpp
//  VoodooGPIO
//
//  Created by Visual on 6/3/23.
//  Copyright © 2017 CoolStar. All rights reserved.
//  Copyright © 2023 Visual/Noot Inc/ChefKiss Inc. All rights reserved.
//

#include "VoodooGPIO.hpp"

OSDefineMetaClassAndAbstractStructors(VoodooGPIO, IOService);

IOWorkLoop *VoodooGPIO::getWorkLoop() const {
    // Never spin here: if start() failed before creating the work loop this would hang forever
    return workLoop ? workLoop : IOService::getWorkLoop();
}
