#include "Airplay2Head.h"
#include "FgAirplayServer.h"

void* fgServerStart(const char serverName[AIRPLAY_NAME_LEN], 
	unsigned int raopPort, unsigned int airplayPort,
	IAirServerCallback* callback) 
{
	FgAirplayServer* pServer = new FgAirplayServer();
	if (pServer->start(serverName, raopPort, airplayPort, callback, false) != 0) {
		delete pServer;
		return NULL;
	}
	return pServer;
}

void* fgServerStartHeadless(const char serverName[AIRPLAY_NAME_LEN],
	unsigned int raopPort, unsigned int airplayPort,
	IAirServerCallback* callback)
{
	FgAirplayServer* pServer = new FgAirplayServer();
	if (pServer->start(serverName, raopPort, airplayPort, callback, true) != 0) {
		delete pServer;
		return NULL;
	}
	return pServer;
}

int fgServerGetPorts(void* handle, unsigned short* raopPort, unsigned short* airplayPort)
{
	if (!handle || !raopPort || !airplayPort) return 0;
	FgAirplayServer* server = static_cast<FgAirplayServer*>(handle);
	*raopPort = server->raopPort();
	*airplayPort = server->airplayPort();
	return *raopPort != 0 && *airplayPort != 0;
}

void fgServerStop(void* handle) 
{
	if (handle != NULL) {
		FgAirplayServer* pServer = (FgAirplayServer*)handle;
		pServer->stop();

		delete pServer;
		pServer = NULL;
	}
}

float fgServerScale(void* handle, float fRatio)
{
	if (handle != NULL) {
		FgAirplayServer* pServer = (FgAirplayServer*)handle;
		return pServer->setScale(fRatio);
	}

	return 1.0f;
}
