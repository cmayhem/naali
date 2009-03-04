// For conditions of distribution and use, see copyright notice in license.txt

#include "StableHeaders.h"
#include "Framework.h"
#include "ComponentManager.h"
#include "EntityManager.h"
//#include "ServiceInterfaces.h"
#include "ServiceManager.h"
#include "ModuleManager.h"

namespace Foundation
{
    Framework::Framework() : mExitSignal(false)
    {
        // create managers
        mModuleManager = ModuleManagerPtr(new ModuleManager(this));
        mComponentManager = ComponentManagerPtr(new ComponentManager(this));
        mEntityManager = EntityManagerPtr(new EntityManager(this));
        mServiceManager = ServiceManagerPtr(new ServiceManager(this));
    }

    Framework::~Framework()
    {
    }


    void Framework::go()
    {
        loadModules();

        // main loop
        while (mExitSignal == false)
        {
            // do synchronized update for modules
            mModuleManager->updateModules();

            // call asynchronous update on systems / do parallel tasks

            // synchronize shared data across modules
            //mChangeManager->_propagateChanges();
        }

         unloadModules();
    }

    void Framework::loadModules()
    {
        mModuleManager->loadAvailableModules();
        mModuleManager->initializeModules();
    }

    void Framework::unloadModules()
    {
        mModuleManager->uninitializeModules();
        mModuleManager->unloadModules();
    }
}

