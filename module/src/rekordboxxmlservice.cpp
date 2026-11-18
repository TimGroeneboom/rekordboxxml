// Local Includes
#include "rekordboxxmlservice.h"

// External Includes
#include <nap/core.h>
#include <nap/resourcemanager.h>
#include <nap/logger.h>
#include <iostream>

RTTI_BEGIN_CLASS_NO_DEFAULT_CONSTRUCTOR(nap::rekordboxxmlService)
	RTTI_CONSTRUCTOR(nap::ServiceConfiguration*)
RTTI_END_CLASS

namespace nap
{
	bool rekordboxxmlService::init(nap::utility::ErrorState& errorState)
	{
		//Logger::info("Initializing rekordboxxmlService");
		return true;
	}


	void rekordboxxmlService::update(double deltaTime)
	{
	}
	

	void rekordboxxmlService::getDependentServices(std::vector<rtti::TypeInfo>& dependencies)
	{
	}
	

	void rekordboxxmlService::shutdown()
	{
	}
}
