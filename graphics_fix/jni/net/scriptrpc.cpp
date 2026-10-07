#include "../main.h"
#include "../game/game.h"
#include "netgame.h"
#include "../gui/gui.h"  
#include "game/Models/ModelInfo.h"
#include "../game/Entity/Ped/Ped.h"
#include "game/CarEnterExit.h"
#include "java_systems/Speedometr.h"
#include "java_systems/RadialMenu.h"
#include "game/Camera.h"
#include "game/RemoveBuilding.h"
#include "../util/util.h"
#include "../vicecity/ViceCity.h"
     
#include <memory>   // FIX: unique_ptr untuk buffer RPC material text
#include <cmath>

// Some project revisions do not expose this RPC id through the transitive
// SAMPRPC.h include. Keep the declaration local as well; the storage is
// provided by vendor/raknet/SAMP/SAMPRPC.cpp.
extern int RPC_ScrSetObjectMaterial;

extern CGUI *pGUI;

void ScrDisplayGameText(RPCParameters *rpcParams)
{
    LOGRPC("ScrDisplayGameText");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	char szMessage[512];
	int iType;
	int iTime;
	int iLength;

	bsData.Read(iType);
	bsData.Read(iTime);
	bsData.Read(iLength);

	if(iLength > 512) return;

	bsData.Read(szMessage,iLength);
	szMessage[iLength] = '\0';

	CGame::DisplayGameText(szMessage, iTime, iType);
}

void ScrSetGravity(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetGravity");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	float fGravity;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bsData.Read(fGravity);

	CGame::SetGravity(fGravity);
}

void ScrSetPlayerPos(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetPlayerPos");
	auto Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	CVector pos;
	bsData.Read(pos.x);
	bsData.Read(pos.y);
	bsData.Read(pos.z);

	auto pPed = CGame::FindPlayerPed();

	if(pPed->m_pPed->IsInVehicle())
		pPed->m_pPed->RemoveFromVehicleAndPutAt(pos);
	else
		pPed->m_pPed->Teleport(pos, false);
}

void ScrSetCameraPos(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetCameraPos");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	CVector vecPos;
	bsData.Read(vecPos.x);
	bsData.Read(vecPos.y);
	bsData.Read(vecPos.z);
	CCamera::SetPosition(vecPos.x, vecPos.y, vecPos.z, 0.0f, 0.0f, 0.0f);
}

void ScrSetCameraLookAt(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetCameraLookAt");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	CVector vecPos;
	bsData.Read(vecPos.x);
	bsData.Read(vecPos.y);
	bsData.Read(vecPos.z);
	CCamera::LookAtPoint(vecPos.x, vecPos.y, vecPos.z, 2);
}

void ScrSetPlayerFacingAngle(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetPlayerFacingAngle");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	float fAngle;
	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bsData.Read(fAngle);
	CGame::FindPlayerPed()->ForceTargetRotation(fAngle);
}

void ScrSetFightingStyle(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetFightingStyle");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	PLAYERID playerId;
	uint8_t byteFightingStyle = 0;
	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bsData.Read(playerId);
	bsData.Read(byteFightingStyle);

	CPedSamp *pPlayerPed = nullptr;

    if(playerId == CPlayerPool::GetLocalPlayerID())
        pPlayerPed = CLocalPlayer::GetPlayerPed();
    else if(CPlayerPool::GetSpawnedPlayer(playerId))
        pPlayerPed = CPlayerPool::GetSpawnedPlayer(playerId)->GetPlayerPed();

	if(pPlayerPed)
		pPlayerPed->SetFightingStyle(byteFightingStyle);
}

void ScrSetPlayerSkin(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetPlayerSkin");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	int32 iPlayerID;
	uint32 uiSkin;
	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bsData.Read(iPlayerID);
	bsData.Read(uiSkin);

	if(iPlayerID == CPlayerPool::GetLocalPlayerID())
		CLocalPlayer::GetPlayerPed()->SetModelIndex(uiSkin);
	else
	{
		if(CPlayerPool::GetSpawnedPlayer(iPlayerID) && CPlayerPool::GetAt(iPlayerID)->GetPlayerPed())
			CPlayerPool::GetAt(iPlayerID)->GetPlayerPed()->SetModelIndex(uiSkin);
	}
}

void ScrApplyPlayerAnimation(RPCParameters *rpcParams)
{
    LOGRPC("ScrApplyPlayerAnimation");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	PLAYERID playerId;
	uint8_t byteAnimLibLen;
	uint8_t byteAnimNameLen;
	char szAnimLib[256];
	char szAnimName[256];
	float fDelta;
	bool loop, lockx, locky, freeze;
	uint32_t dTime;
	CPedSamp *pPlayerPed = nullptr;

	memset(szAnimLib, 0, 256);
	memset(szAnimName, 0, 256);

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bsData.Read(playerId);
	bsData.Read(byteAnimLibLen);
	bsData.Read(szAnimLib, byteAnimLibLen);
	bsData.Read(byteAnimNameLen);
	bsData.Read(szAnimName, byteAnimNameLen);
	bsData.Read(fDelta);
	bsData.Read(loop);
	bsData.Read(lockx);
	bsData.Read(locky);
	bsData.Read(freeze);
	bsData.Read(dTime);

	szAnimLib[byteAnimLibLen] = '\0';
	szAnimName[byteAnimNameLen] = '\0';

	if(CPlayerPool::GetLocalPlayerID() == playerId) {
			pPlayerPed = CLocalPlayer::GetPlayerPed();
	}
	else if(CPlayerPool::GetSpawnedPlayer(playerId))
		pPlayerPed = CPlayerPool::GetSpawnedPlayer(playerId)->GetPlayerPed();

	if(pPlayerPed)
		pPlayerPed->ApplyAnimation(szAnimName, szAnimLib, fDelta, (bool)loop, (bool)lockx, (bool)locky, (bool)freeze, (int)dTime);
}

void ScrClearPlayerAnimations(RPCParameters *rpcParams)
{
	LOGRPC("RPC: ScrClearPlayerAnimations");

	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;
	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	PLAYERID playerId;
	bsData.Read(playerId);

	CPedSamp *pPlayerPed=NULL;

	if(playerId == CPlayerPool::GetLocalPlayerID())
    {
        pPlayerPed = CLocalPlayer::GetPlayerPed();
    }
    else
    {
        if(CPlayerPool::GetSpawnedPlayer(playerId))
            pPlayerPed = CPlayerPool::GetAt(playerId)->GetPlayerPed();
    }
		
    if(pPlayerPed)
    {
        pPlayerPed->ClearAnimations();
    }
}


void ScrSetPlayerSpecialAction(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetPlayerSpecialAction");

	auto Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	BYTE byteSpecialAction;
	bsData.Read(byteSpecialAction);

	CPedSamp *pPed = CLocalPlayer::GetPlayerPed();

	pPed->m_iCurrentSpecialAction = byteSpecialAction;
	if (byteSpecialAction == SPECIAL_ACTION_NONE) {
		CGame::isBanJump = false;
		//	pPed->ClearAnimations();
	}
	if (byteSpecialAction == SPECIAL_ACTION_CARRY) {
		CGame::isBanJump = true;
	}
	pPed->ProcessSpecialAction(byteSpecialAction);

}

void ScrCreateExplosion(RPCParameters *rpcParams)
{
    LOGRPC("ScrCreateExplosion");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	float X, Y, Z, Radius;
	int   iType;

	bsData.Read(X);
	bsData.Read(Y);
	bsData.Read(Z);
	bsData.Read(iType);
	bsData.Read(Radius);

	ScriptCommand(&create_explosion_with_radius, X, Y, Z, iType, Radius);
}

void ScrSetPlayerHealth(RPCParameters *rpcParams)
{
    LOGRPC("ScrSetPlayerHealth");
	auto Data = reinterpret_cast<unsigned char *>(rpcParams->input);

	int iBitLength = rpcParams->numberOfBitsOfData;

	float fHealth;
	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bsData.Read(fHealth);
    CLocalPlayer::GetPlayerPed()->SetHealth(fHealth);
}

void ScrSetPlayerArmour(RPCParameters *rpcParams)
{
    LOGRPC("ScrSetPlayerArmour");
	auto Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	float fHealth;
	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bsData.Read(fHealth);

    CLocalPlayer::GetPlayerPed()->SetArmour(fHealth);
}

void ScrSetPlayerColor(RPCParameters *rpcParams)
{
    LOGRPC("ScrSetPlayerColor");
	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	PLAYERID playerId;
	uint32_t dwColor;

	bsData.Read(playerId);
	bsData.Read(dwColor);

	if(playerId == CPlayerPool::GetLocalPlayerID())
	{
        CLocalPlayer::SetPlayerColor(dwColor);
	} 
	else 
	{
		CRemotePlayer *pPlayer = CPlayerPool::GetAt(playerId);
		if(pPlayer)	pPlayer->SetPlayerColor(dwColor);
	}
}

void ScrSetPlayerName(RPCParameters *rpcParams)
{
	LOGRPC("RPC: ScrSetPlayerName");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	PLAYERID playerId;
	uint8_t byteNickLen;
	char szNewName[MAX_PLAYER_NAME+1];
	uint8_t byteSuccess;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	bsData.Read(playerId);
	bsData.Read(byteNickLen);

	if(byteNickLen > MAX_PLAYER_NAME) return;

	bsData.Read(szNewName, byteNickLen);
	bsData.Read(byteSuccess);

	szNewName[byteNickLen] = '\0';

	Log("byteSuccess = %d", byteSuccess);
	if (byteSuccess == 1) CPlayerPool::SetPlayerName(playerId, szNewName);
	
	// Extra addition which we need to do if this is the local player;
	if( CPlayerPool::GetLocalPlayerID() == playerId )
		CPlayerPool::SetLocalPlayerName( szNewName );
}

void ScrSetPlayerPosFindZ(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetPlayerPosFindZ");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	CVector vecPos;

	bsData.Read(vecPos.x);
	bsData.Read(vecPos.y);
	bsData.Read(vecPos.z);

	vecPos.z = CGame::FindGroundZForCoord(vecPos.x, vecPos.y, vecPos.z);
	vecPos.z += 1.5f;

	CLocalPlayer::GetPlayerPed()->m_pPed->Teleport(vecPos, false);
}

void ScrSetPlayerInterior(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetPlayerInterior");

	auto Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;


	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	uint8_t byteInterior;
	bsData.Read(byteInterior);

	CGame::FindPlayerPed()->m_pPed->SetInterior(byteInterior, true);
}

void ScrSetMapIcon(RPCParameters *rpcParams)
{
    LOGRPC("ScrSetMapIcon");
	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	uint8_t byteIndex;
	uint8_t byteIcon;
	uint32_t dwColor;
	float fPos[3];
	uint8_t byteStyle;

	bsData.Read(byteIndex);
	bsData.Read(fPos[0]);
	bsData.Read(fPos[1]);
	bsData.Read(fPos[2]);
	bsData.Read(byteIcon);
	bsData.Read(dwColor);
	bsData.Read(byteStyle);

	pNetGame->SetMapIcon(byteIndex, fPos[0], fPos[1], fPos[2], byteIcon, dwColor, byteStyle);
}

void ScrDisableMapIcon(RPCParameters *rpcParams)
{
    LOGRPC("ScrDisableMapIcon");
	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	uint8_t byteIndex;

	bsData.Read(byteIndex);

	pNetGame->DisableMapIcon(byteIndex);
}

void ScrSetCameraBehindPlayer(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetCameraBehindPlayer");

	CCamera::SetBehindPlayer();
}

void ScrTogglePlayerSpectating(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrTogglePlayerSpectating");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	uint32_t bToggle;
	bsData.Read(bToggle);
	CLocalPlayer::ToggleSpectating(bToggle);
	Log("toggle: %d", bToggle);
}

void ScrSetPlayerSpectating(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetPlayerSpectating");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	PLAYERID playerId;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bsData.Read(playerId);

	if (CPlayerPool::GetAt(playerId))
		CPlayerPool::GetAt(playerId)->SetState(PLAYER_STATE_SPECTATING);
}

#define SPECTATE_TYPE_NORMAL	1
#define SPECTATE_TYPE_FIXED		2
#define SPECTATE_TYPE_SIDE		3

void ScrPlayerSpectatePlayer(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrPlayerSpectatePlayer");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	
	PLAYERID playerId;
    uint8_t byteMode;
	
	bsData.Read(playerId);
	bsData.Read(byteMode);

	switch (byteMode) 
	{
		case SPECTATE_TYPE_FIXED:
			byteMode = 15;
			break;
		case SPECTATE_TYPE_SIDE:
			byteMode = 14;
			break;
		default:
			byteMode = 4;
	}

	CLocalPlayer::m_byteSpectateMode = byteMode;
    CLocalPlayer::SpectatePlayer(playerId);
}

void ScrPlayerSpectateVehicle(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrPlayerSpectateVehicle");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	VEHICLEID VehicleID;
	uint8_t byteMode;

	bsData.Read(VehicleID);
	bsData.Read(byteMode);

	switch (byteMode) 
	{
		case SPECTATE_TYPE_FIXED:
			byteMode = 15;
			break;
		case SPECTATE_TYPE_SIDE:
			byteMode = 14;
			break;
		default:
			byteMode = 3;
	}

    CLocalPlayer::m_byteSpectateMode = byteMode;
    CLocalPlayer::SpectateVehicle(VehicleID);
}

void ScrPutPlayerInVehicle(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrPutPlayerInVehicle");

	auto Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	// a truncated packet must not act on whatever was on the stack
	VEHICLEID vehicleid = INVALID_VEHICLE_ID;
	uint8_t seatid = 0;
	bsData.Read(vehicleid);
	bsData.Read(seatid);

	CPedSamp *pPed = CGame::FindPlayerPed();
	if(!pPed || !pPed->m_pPed)return;

	//if(vehicleid == pPed->GetCurrentSampVehicleID()) return;

	// Look the vehicle up first: the player used to be thrown out of his
	// current vehicle even when the target was not streamed in.
	CVehicleMP *pVehicle = CVehiclePool::GetAt(vehicleid);
	if(!pVehicle || !pVehicle->m_pVehicle)return;

	if(pPed->m_pPed->IsInVehicle()) {
		pPed->m_pPed->RemoveFromVehicle();
	}

	// seat check + "driver seat still taken" check, ported from alyn
	pPed->PutDirectlyInVehicle(pVehicle->m_dwGTAId, seatid);

	if(pPed->m_pPed->IsInVehicle())
		CCamera::SetBehindPlayer();
}

void ScrVehicleParams(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetVehicleParams");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	VEHICLEID VehicleID;
	uint8_t byteObjectiveVehicle;
	uint8_t byteDoorsLocked;

	bsData.Read(VehicleID);
	bsData.Read(byteObjectiveVehicle);
	bsData.Read(byteDoorsLocked);

	CVehiclePool::AssignSpecialParamsToVehicle(VehicleID,byteObjectiveVehicle,byteDoorsLocked);
}

void ScrVehicleParamsEx(RPCParameters* rpcParams)
{
    LOGRPC("ScrVehicleParamsEx");
	auto Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data, (iBitLength / 8) + 1, false);

	// SetVehicleParamsEx sends 0 = off, 1 = on and 0xFF (-1) = "not set".  A
	// value that is not set leaves the vehicle alone (like alyn / the PC client);
	// it used to lock the doors, switch lights and alarm on and open bonnet and
	// boot.  A field missing from a truncated packet counts as not set.
	constexpr uint8_t PARAM_UNSET = 0xFF;

	VEHICLEID VehicleId = INVALID_VEHICLE_ID;
	uint8_t engine = PARAM_UNSET, lights = PARAM_UNSET, alarm = PARAM_UNSET, doors = PARAM_UNSET,
			bonnet = PARAM_UNSET, boot = PARAM_UNSET, objective = PARAM_UNSET;
	bsData.Read(VehicleId);
	bsData.Read(engine);
	bsData.Read(lights);
	bsData.Read(alarm);
	bsData.Read(doors);
	bsData.Read(bonnet);
	bsData.Read(boot);
	bsData.Read(objective);

	auto pVehicle = CVehiclePool::GetAt(VehicleId);

	if(!pVehicle || !pVehicle->m_pVehicle)
		return;

	if (doors != PARAM_UNSET)
		pVehicle->SetDoorState(doors);

	// engine
	if (engine != PARAM_UNSET)
		pVehicle->SetEngineState((engine == 1));

	// lights (0 = off, 1 = on, 2 = high beam)
	if (lights != PARAM_UNSET)
		pVehicle->m_bIsLightOn = static_cast<eLightsState>(lights);

	// alarm: 0xFFFF only arms it, which never makes a sound in SA-MP.  A running
	// alarm is a countdown in milliseconds.
	if (alarm != PARAM_UNSET)
		pVehicle->m_pVehicle->m_nAlarmState = alarm ? 20000 : 0;

	//		pNetGame->GetVehiclePool()->AssignSpecialParamsToVehicle(VehicleId, objective,doors);

	// the boot was moved with the bonnet's door id
	if (boot != PARAM_UNSET)
		pVehicle->OpenDoor(eCarNodes::CAR_BOOT, eDoors::DOOR_BOOT, boot != 0);
	if (bonnet != PARAM_UNSET)
		pVehicle->OpenDoor(eCarNodes::CAR_BONNET, eDoors::DOOR_BONNET, bonnet != 0);

	CSpeedometr::UpdateInfo();
	CRadialMenu::Update();
}

void ScrHaveSomeMoney(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrHaveSomeMoney");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	int iAmmount;
	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bsData.Read(iAmmount);


	CGame::AddToLocalMoney(iAmmount);
}

void ScrResetMoney(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrResetMoney");

    // TODO: ScrResetMoney
	//CGame::ResetLocalMoney();
}

void ScrLinkVehicle(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrLinkVehicle");

	auto Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	VEHICLEID VehicleID;
	uint8_t byteInterior;

	bsData.Read(VehicleID);
	bsData.Read(byteInterior);

	auto pVehicle = CVehiclePool::GetAt(VehicleID);

	if(!pVehicle || !pVehicle->m_pVehicle)return;

	pVehicle->m_pVehicle->SetInterior(byteInterior);
}

void ScrRemovePlayerFromVehicle(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrRemovePlayerFromVehicle");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

    if (CPedSamp* pPlayerPed = CLocalPlayer::GetPlayerPed())
        pPlayerPed->ExitCurrentVehicle();
}

void ScrSetVehicleHealth(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetVehicleHealth");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	float fHealth;
	VEHICLEID VehicleID;

	bsData.Read(VehicleID);
	bsData.Read(fHealth);

	if(CVehiclePool::GetAt(VehicleID))
		CVehiclePool::GetAt(VehicleID)->SetHealth(fHealth);
}

void ScrSetVehiclePos(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetVehiclePos");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	VEHICLEID VehicleId;

	CVector pos;
	bsData.Read(VehicleId);
	bsData.Read(pos.x);
	bsData.Read(pos.y);
	bsData.Read(pos.z);

	if(CVehiclePool::GetAt(VehicleId))
		CVehiclePool::GetAt(VehicleId)->m_pVehicle->Teleport(pos, false);
}

void ScrSetVehicleVelocity(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetVehicleVelocity");

	auto Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	uint8_t turn = false;
	CVector vecMoveSpeed;
	bsData.Read(turn);
	bsData.Read(vecMoveSpeed.x);
	bsData.Read(vecMoveSpeed.y);
	bsData.Read(vecMoveSpeed.z);

	CPedSamp *pPlayerPed = CGame::FindPlayerPed();

	if(pPlayerPed && pPlayerPed->m_pPed)
	{
		CVehicleMP *pVehicle = CVehiclePool::GetAt( CVehiclePool::FindIDFromGtaPtr(pPlayerPed->GetGtaVehicle()) );
		if(pVehicle && pVehicle->m_pVehicle)
		{
			// The same RPC carries SetVehicleAngularVelocity (turn = 1); it was
			// applied as a linear velocity and launched the vehicle.
			if(turn)
				pVehicle->m_pVehicle->SetTurnSpeed(vecMoveSpeed);
			else
				pVehicle->m_pVehicle->SetVelocity(vecMoveSpeed);
		}
	}
}

void ScrNumberPlate(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrNumberPlate");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	VEHICLEID VehicleID;
	char len;
	char szNumberPlate[32+1];

	/*bsData.Read(VehicleID);
	bsData.Read(len);
	bsData.Read(szNumberPlate, len);
	szNumberPlate[len] = '\0';*/
}

void ScrInterpolateCamera(RPCParameters *rpcParams)
{
    LOGRPC("ScrInterpolateCamera");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	bool bSetPos = true;
	CVector vecFrom, vecDest;
	int time;
	uint8_t mode;

	bsData.Read(bSetPos);
	bsData.Read(vecFrom.x);
	bsData.Read(vecFrom.y);
	bsData.Read(vecFrom.z);
	bsData.Read(vecDest.x);
	bsData.Read(vecDest.y);
	bsData.Read(vecDest.z);
	bsData.Read(time);
	bsData.Read(mode);

	if(mode < 1 || mode > 2)
		mode = 2;

	if(time > 0)
	{
        CLocalPlayer::m_bSpectateProcessed = true;
		if(bSetPos)
		{
			CCamera::InterpolateCameraPos(&vecFrom, &vecDest, time, mode);
		}
		else
			CCamera::InterpolateCameraLookAt(&vecFrom, &vecDest, time, mode);
	}
}

void ScrSetSpawnInfo(RPCParameters *rpcParams)
{
    LOGRPC("ScrSetSpawnInfo");
    unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
    int iBitLength = rpcParams->numberOfBitsOfData;

    PLAYER_SPAWN_INFO spawnInfo;
    RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);
    bsData.Read((char*)&spawnInfo, sizeof(PLAYER_SPAWN_INFO));

    CLocalPlayer::SetSpawnInfo(&spawnInfo);

    return;
}

void ScrAddGangZone(RPCParameters *rpcParams) {
    LOGRPC("ScrAddGangZone");
    unsigned char *Data = reinterpret_cast<unsigned char *>(rpcParams->input);
    int iBitLength = rpcParams->numberOfBitsOfData;

    RakNet::BitStream bsData((unsigned char *) Data, (iBitLength / 8) + 1, false);

    uint16_t wZoneID;
    float minx, miny, maxx, maxy;
    uint32_t dwColor;
    bsData.Read(wZoneID);
    bsData.Read(minx);
    bsData.Read(miny);
    bsData.Read(maxx);
    bsData.Read(maxy);
    bsData.Read(dwColor);

    CGangZonePool::New(wZoneID, minx, miny, maxx, maxy, dwColor);
}

void ScrRemoveGangZone(RPCParameters *rpcParams)
{
    LOGRPC("ScrRemoveGangZone");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	uint16_t wZoneID;
	bsData.Read(wZoneID);

	CGangZonePool::Delete(wZoneID);
}

void ScrFlashGangZone(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrFlashGangZone");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	uint16_t wZoneID;
	uint32_t dwColor;
	bsData.Read(wZoneID);
	bsData.Read(dwColor);
	CGangZonePool::Flash(wZoneID, dwColor);
}

void ScrStopFlashGangZone(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrStopFlashGangZone");

	unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	uint16_t wZoneID;
	bsData.Read(wZoneID);

	CGangZonePool::StopFlash(wZoneID);
}

int iTotalObjects = 0;

void ScrCreateObject(RPCParameters* rpcParams)
{
    LOGRPC("ScrCreateObject");
	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	uint16_t wObjectID;
	uint32 ModelID;
	float fDrawDistance;
	CVector vecPos, vecRot;

	uint8_t bNoCameraCol;
	int16_t attachedVehicleID;
	int16_t attachedObjectID;
	CVector vecAttachedOffset;
	CVector vecAttachedRotation;
	uint8_t bSyncRot;
	uint8_t iMaterialCount;

	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);
	bsData.Read(wObjectID);
	bsData.Read(ModelID);
	bsData.Read(vecPos.x);
	bsData.Read(vecPos.y);
	bsData.Read(vecPos.z);

	bsData.Read(vecRot.x);
	bsData.Read(vecRot.y);
	bsData.Read(vecRot.z);

	bsData.Read(fDrawDistance);

	bsData.Read(bNoCameraCol);
	bsData.Read(attachedVehicleID);
	bsData.Read(attachedObjectID);
	if (attachedObjectID != -1 || attachedVehicleID != -1)
	{
		bsData.Read(vecAttachedOffset);
		bsData.Read(vecAttachedRotation);
		bsData.Read(bSyncRot);
	}
	bsData.Read(iMaterialCount);

    // Vice City map (vicecity/). An object the client has already placed itself
    // from its own copy of the map (same model, same place) is not created a
    // second time: the server's copy would sit inside the client's and count
    // against the object limit of the 0.3.7 protocol.
    if (vc::OnServerObject(static_cast<int32_t>(ModelID), vecPos)) {
        if (CObjectPool::GetAt(wObjectID) && CObjectPool::Delete(wObjectID)) {
            if (iTotalObjects > 0) --iTotalObjects;   // the id was in use for another object
        }
        return;
    }
    // The models of the map keep the (negative) ids the 0.3.DL script gave them
    // with AddSimpleModel; inside the game they have ids of their own, which no
    // server can mean. Every other id comes back unchanged.
    {
        const int resolved = vc::ResolveServerModel(static_cast<int32_t>(ModelID));
        if (resolved < 0) {
            Log("Error ScrCreateObject. No model %d", static_cast<int32_t>(ModelID));
            return;
        }
        ModelID = static_cast<uint32>(resolved);
    }

    // ModelID comes directly from RakNet; GetModelInfo() is an unchecked array
    // access in this client. Validate the full range before touching it.
    if (ModelID >= static_cast<uint32_t>(CModelInfo::NUM_MODEL_INFOS) ||
        !CModelInfo::GetModelInfo(static_cast<int>(ModelID))) {
        Log("Error ScrCreateObject. No model %u", ModelID);
        return;
    }

    const bool replacingObject = (CObjectPool::GetAt(wObjectID) != nullptr);
    if (!CObjectPool::New(wObjectID, ModelID, vecPos, vecRot, fDrawDistance)) {
        Log("Error ScrCreateObject. CObjectPool::New failed for id %u", wObjectID);
        return;
    }

    CObjectSamp* pObject = CObjectPool::GetAt(wObjectID);
    if (!pObject) {
        Log("Error ScrCreateObject. GetAt returned null for id %u", wObjectID);
        return;
    }

    if (!replacingObject) {
        iTotalObjects++;
    }
    //Log("ID: %d, model: %d. iTotalObjects = %d", wObjectID, ModelID, iTotalObjects);

	if (attachedVehicleID != -1)
	{
		pObject->AttachToVehicle(attachedVehicleID, &vecAttachedOffset, &vecAttachedRotation);
	}
	if (iMaterialCount > 0)
	{
        // texturesCount is the number of material slots, but SA-MP encodes the
        // actual texture/text records consecutively after that byte. Parse all
        // complete records present in the RPC, matching the reference client,
        // while only applying material indices 0..15. This prevents a later
        // material/text record from being skipped when the count is stale or
        // when the server mixes texture and text slots.
        uint8_t parsedMaterials = 0;
        while (bsData.GetNumberOfUnreadBits() >= 16 &&
               parsedMaterials < 255)
        {
            ++parsedMaterials;
            uint8_t materialType = 0;
            uint8_t slot = 0;
            bsData.Read(materialType);
            bsData.Read(slot);

            if (materialType == 1) // MATERIAL_TYPE_TEXTURE
            {
                uint16_t modelId = 0xFFFF;
                uint8_t txdLen = 0, texLen = 0;
                // Lengths are uint8, so 256 bytes always fit. The old 32-byte
                // limit aborted the WHOLE material list of the object on the
                // first longer TXD name, leaving every later slot untextured.
                char txd[256]{};
                char tex[256]{};
                uint32_t color = 0;

                bsData.Read(modelId);
                bsData.Read(txdLen);
                bsData.Read(txd, txdLen);
                txd[txdLen] = 0;

                bsData.Read(texLen);
                bsData.Read(tex, texLen);
                tex[texLen] = 0;
                bsData.Read(color);

                // 0xFFFF (and anything out of range) means "no material model":
                // resolve the texture by TXD name only.
                const int resolvedModel = (modelId == 0xFFFF ||
                                           modelId >= CModelInfo::NUM_MODEL_INFOS)
                                              ? -1 : static_cast<int>(modelId);
                if (pObject) pObject->SetMaterial(resolvedModel, slot, txd, tex, color);
            }
            else if (materialType == 2) // MATERIAL_TYPE_TEXT
            {
                uint8_t materialSize = 0, fontLen = 0, fontSize = 0, fontBold = 0, align = 0;
                uint32_t fontColor = 0, bgColor = 0;
                char font[256]{};
                char text[2049]{};

                bsData.Read(materialSize);
                bsData.Read(fontLen);
                bsData.Read(font, fontLen);
                font[fontLen] = 0;
                bsData.Read(fontSize);
                bsData.Read(fontBold);
                bsData.Read(fontColor);
                bsData.Read(bgColor);
                bsData.Read(align);
                stringCompressor->DecodeString(text, 2049, &bsData);
                text[2048] = 0;

                // SA-MP sends material text in the client code page. Convert it
                // once at the RPC boundary like Alyn's ScriptRPC.cpp, then the
                // material-text renderer consumes UTF-8 directly.
                if (pObject) {
                    const std::string utf8Text = cp1251_to_utf8(text);
                    pObject->SetMaterialText(slot, materialSize, font, fontSize,
                                             fontBold, fontColor, bgColor, align, utf8Text.c_str());
                }
            }
            else
            {
                // Unknown material type. Do not try to guess its payload size.
                return;
            }
        }
	}

}

void ScrDestroyObject(RPCParameters *rpcParams)
{
    LOGRPC("ScrDestroyObject");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	uint16_t wObjectID;
	RakNet::BitStream bsData(Data,(iBitLength/8)+1,false);
	bsData.Read(wObjectID);

    // Only decrement the counter when an object was actually alive. Duplicate
    // destroy RPCs are legal during reconnect/resync and must not underflow the
    // global object count.
    if (CObjectPool::GetAt(wObjectID) && CObjectPool::Delete(wObjectID)) {
        if (iTotalObjects > 0) {
            --iTotalObjects;
        }
    }
}

void ScrSetObjectPos(RPCParameters *rpcParams)
{
    LOGRPC("RPC_SCRSETOBJECTPOS");

	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	uint16_t wObjectID;
	float fRotation;
	CVector vecPos, vecRot;
	RakNet::BitStream bsData(Data,(iBitLength/8)+1,false);
	bsData.Read(wObjectID);
	bsData.Read(vecPos.x);
	bsData.Read(vecPos.y);
	bsData.Read(vecPos.z);
	bsData.Read(fRotation);

	//LOGI("id: %d x: %.2f y: %.2f z: %.2f", wObjectID, vecPos.x, vecPos.y, vecPos.z);
	//LOGI("VecRot x: %.2f y: %.2f z: %.2f", vecRot.x, vecRot.y, vecRot.z);

	auto pObject = CObjectPool::GetAt(wObjectID);
	if(pObject)
	{
		pObject->SetPos(vecPos.x, vecPos.y, vecPos.z);
	}
}

void ScrAttachObjectToPlayer(RPCParameters* rpcParams)
{
    LOGRPC("RPC_SCRATTACHOBJECTTOPLAYER");

	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);

	PLAYERID wObjectID, wPlayerID;
	float OffsetX, OffsetY, OffsetZ, rX, rY, rZ;

	bsData.Read(wObjectID);
	bsData.Read(wPlayerID);

	bsData.Read(OffsetX);
	bsData.Read(OffsetY);
	bsData.Read(OffsetZ);

	bsData.Read(rX);
	bsData.Read(rY);
	bsData.Read(rZ);

	CObjectSamp* pObject = CObjectPool::GetAt(wObjectID);
	if (!pObject) return;

	if (wPlayerID == CPlayerPool::GetLocalPlayerID())
	{
		ScriptCommand(&attach_object_to_actor, pObject->m_dwGTAId, CLocalPlayer::GetPlayerPed()->m_dwGTAId,
			OffsetX, OffsetY, OffsetZ, rX, rY, rZ);
	}
	else {
		CRemotePlayer* pPlayer = CPlayerPool::GetSpawnedPlayer(wPlayerID);

		if (!pPlayer)
			return;

		ScriptCommand(&attach_object_to_actor, pObject->m_dwGTAId, pPlayer->GetPlayerPed()->m_dwGTAId,
			OffsetX, OffsetY, OffsetZ, rX, rY, rZ);
	}
}


void ScrPlaySound(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrPlaySound");
//
//	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
//	int iBitLength = rpcParams->numberOfBitsOfData;
//
//	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
//
//	int iSound;
//	float fX, fY, fZ;
//	bsData.Read(iSound);
//	bsData.Read(fX);
//	bsData.Read(fY);
//	bsData.Read(fZ);
//	CGame::PlaySound(iSound, fX, fY, fZ);
}

void ScrSetPlayerWantedLevel(RPCParameters *rpcParams)
{
    LOGRPC("RPC: ScrSetPlayerWantedLevel");

	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);
	
	if(!CGame::GetGameInit()) return;

	uint8_t byteLevel;
	bsData.Read(byteLevel);
	CGame::SetWantedLevel(byteLevel);
}

void ScrGivePlayerWeapon(RPCParameters* rpcParams)
{
    LOGRPC("ScrGivePlayerWeapon");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);

	PLAYERID wPlayerID;
	int iWeaponID;
	int iAmmo;
	bsData.Read(iWeaponID);
	bsData.Read(iAmmo);

	//CChatWindow::AddMessage("giveweapon | weaponid: %d ammo: %d", iWeaponID, iAmmo);

	CLocalPlayer::GetPlayerPed()->m_pPed->GiveWeapon(iWeaponID, iAmmo);
    CLocalPlayer::ammoUpdated = true;
}

void ScrSetWeaponAmmo(RPCParameters* rpcParams)
{
    LOGRPC("ScrSetWeaponAmmo");
	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);

	//PLAYERID wPlayerID;
	uint8_t iWeaponID;
	int16_t iAmmo;
	bsData.Read(iWeaponID);
	bsData.Read(iAmmo);

	//CChatWindow::AddMessage("setweaponammo | weaponid: %d ammo: %d", iWeaponID, iAmmo);


	CGame::FindPlayerPed()->SetWeaponAmmo(iWeaponID, iAmmo);
}

void ScrTogglePlayerControllable(RPCParameters *rpcParams)
{
    LOGRPC("RPC: TogglePlayerControllable");

	unsigned char * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData(Data,(iBitLength/8)+1,false);
	uint8_t byteControllable;
	bsData.Read(byteControllable);
	//Log("controllable = %d", byteControllable);

	CLocalPlayer::GetPlayerPed()->TogglePlayerControllable((bool)byteControllable);
}

#define WEAPONTYPE_PISTOL_SKILL 69
#define WEAPONTYPE_PISTOL_SILENCED_SKILL 70
#define WEAPONTYPE_DESERT_EAGLE_SKILL 71
#define WEAPONTYPE_SHOTGUN_SKILL 72
#define WEAPONTYPE_SAWNOFF_SHOTGUN_SKILL 73
#define WEAPONTYPE_SPAS12_SHOTGUN_SKILL 74
#define WEAPONTYPE_MICRO_UZI_SKILL 75
#define WEAPONTYPE_MP5_SKILL 76
#define WEAPONTYPE_AK47_SKILL 77
#define WEAPONTYPE_M4_SKILL 78
#define WEAPONTYPE_SNIPERRIFLE_SKILL 79

void ScrSetPlayerSkillLevel(RPCParameters *rpcParams)
{
    LOGRPC("ScrSetPlayerSkillLevel");
	int iBitLength = rpcParams->numberOfBitsOfData;
	PLAYERID bytePlayerID;
	unsigned int ucSkillType;
	unsigned short uiSkillLevel;
	RakNet::BitStream bsData(rpcParams->input, (iBitLength / 8) + 1, false);

	bsData.Read(bytePlayerID);
	bsData.Read(ucSkillType);
	bsData.Read(uiSkillLevel);

	if (ucSkillType < 0 || ucSkillType > 10) return;
	if (uiSkillLevel < 0 || uiSkillLevel > 1000) return;

	switch (ucSkillType)
	{
	case 0:
		ucSkillType = WEAPONTYPE_PISTOL_SKILL;
		break;
	case 1:
		ucSkillType = WEAPONTYPE_PISTOL_SILENCED_SKILL;
		break;
	case 2:
		ucSkillType = WEAPONTYPE_DESERT_EAGLE_SKILL;
		break;
	case 3:
		ucSkillType = WEAPONTYPE_SHOTGUN_SKILL;
		break;
	case 4:
		ucSkillType = WEAPONTYPE_SAWNOFF_SHOTGUN_SKILL;
		break;
	case 5:
		ucSkillType = WEAPONTYPE_SPAS12_SHOTGUN_SKILL;
		break;
	case 6:
		ucSkillType = WEAPONTYPE_MICRO_UZI_SKILL;
		break;
	case 7:
		ucSkillType = WEAPONTYPE_MP5_SKILL;
		break;
	case 8:
		ucSkillType = WEAPONTYPE_AK47_SKILL;
		break;
	case 9:
		ucSkillType = WEAPONTYPE_M4_SKILL;
		break;
	case 10:
		ucSkillType = WEAPONTYPE_SNIPERRIFLE_SKILL;
		break;
	default:
		return;
	}
	ScriptCommand(&change_stat, ucSkillType, uiSkillLevel);
	//g_CGame::GetStats()->SetStatValue(ucSkillType, uiSkillLevel);

	unsigned char ucWeaponSkill;
	if (uiSkillLevel >= 0 && uiSkillLevel < 333)
		ucWeaponSkill = 0;
	else if (uiSkillLevel >= 333 && uiSkillLevel < 666)
		ucWeaponSkill = 1;
	else
		ucWeaponSkill = 2;

	CPedSamp *pPlayer = CGame::FindPlayerPed();
	if (pPlayer)
	{
		ScriptCommand(&set_char_weapon_skill, pPlayer->m_dwGTAId, ucWeaponSkill);
	}
}

void ScrResetPlayerWeapons(RPCParameters* rpcParams)
{
    LOGRPC("ScrResetPlayerWeapons");
    uint8_t* Data = reinterpret_cast<uint8_t*>(rpcParams->input);
    int iBitLength = rpcParams->numberOfBitsOfData;
    PlayerID sender = rpcParams->sender;

    CPedSamp* pPlayerPed = CLocalPlayer::GetPlayerPed();
    pPlayerPed->ClearAllWeapons();
}

void ScrShowTextDraw(RPCParameters* rpcParams)
{
    LOGRPC("ScrShowTextDraw");
    unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
    int iBitLength = rpcParams->numberOfBitsOfData;
    RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);

    uint16_t wTextID;
    uint16_t wTextSize;
    TEXT_DRAW_TRANSMIT TextDrawTransmit;
    char cText[MAX_TEXT_DRAW_LINE];

    bsData.Read(wTextID);
    bsData.Read((char*)& TextDrawTransmit, sizeof(TEXT_DRAW_TRANSMIT));
    bsData.Read(wTextSize);
    bsData.Read(cText, wTextSize);
    cText[wTextSize] = 0;
    CTextDrawPool::New(wTextID, &TextDrawTransmit, cText);
}

void ScrHideTextDraw(RPCParameters* rpcParams)
{
    LOGRPC("ScrHideTextDraw");
    unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
    int iBitLength = rpcParams->numberOfBitsOfData;

    RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);
    uint16_t wTextID;
    bsData.Read(wTextID);
    CTextDrawPool::Delete(wTextID);
}

void ScrEditTextDraw(RPCParameters* rpcParams)
{
    LOGRPC("ScrEditTextDraw");
    unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
    int iBitLength = rpcParams->numberOfBitsOfData;
    RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);
    uint16_t wTextID;
    uint16_t wLen;
    bsData.Read(wTextID);
    bsData.Read(wLen);
    uint8_t pStr[256];
    if (wLen >= 255) return;

    bsData.Read((char*)pStr, wLen);
    pStr[wLen] = 0;
    CTextDraw* pTextDraw = CTextDrawPool::GetAt(wTextID);
    if (pTextDraw)
    {
        pTextDraw->SetText((const char*)pStr);
    }
}

void ScrSelectTextDraw(RPCParameters* rpcParams)
{
    Log("RPC: ScrSelectTextDraw");
    unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
    int iBitLength = rpcParams->numberOfBitsOfData;

    bool bEnable = false;
    uint32_t dwColor = 0;
    RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);
    bsData.Read(bEnable);
    bsData.Read(dwColor);

    CTextDrawPool::SetSelectState(bEnable, dwColor);
}

#define ATTACH_BONE_SPINE	1
#define ATTACH_BONE_HEAD	2
#define ATTACH_BONE_LUPPER	3
#define ATTACH_BONE_RUPPER	4
#define ATTACH_BONE_LHAND	5
#define ATTACH_BONE_RHAND	6
#define ATTACH_BONE_LTHIGH	7
#define ATTACH_BONE_RTHIGH	8
#define ATTACH_BONE_LFOOT	9
#define ATTACH_BONE_RFOOT	10
#define ATTACH_BONE_RCALF	11
#define ATTACH_BONE_LCALF	12
#define ATTACH_BONE_LFARM	13
#define ATTACH_BONE_RFARM	14
#define ATTACH_BONE_LSHOULDER	15
#define ATTACH_BONE_RSHOULDER	16
#define ATTACH_BONE_NECK	17
#define ATTACH_BONE_JAW		18

int GetSampIDFromInternalBoneID(int internalBoneID)
{
    switch (internalBoneID)
    {
        case 3:
            return ATTACH_BONE_SPINE;
        case 5:
            return ATTACH_BONE_HEAD;
        case 22:
            return ATTACH_BONE_LUPPER;
        case 32:
            return ATTACH_BONE_RUPPER;
        case 34:
            return ATTACH_BONE_LHAND;
        case 24:
            return ATTACH_BONE_RHAND;
        case 41:
            return ATTACH_BONE_LTHIGH;
        case 51:
            return ATTACH_BONE_RTHIGH;
        case 43:
            return ATTACH_BONE_LFOOT;
        case 53:
            return ATTACH_BONE_RFOOT;
        case 52:
            return ATTACH_BONE_RCALF;
        case 42:
            return ATTACH_BONE_LCALF;
        case 33:
            return ATTACH_BONE_LFARM;
        case 23:
            return ATTACH_BONE_RFARM;
        case 31:
            return ATTACH_BONE_LSHOULDER;
        case 21:
            return ATTACH_BONE_RSHOULDER;
        case 4:
            return ATTACH_BONE_NECK;
        case 8:
            return ATTACH_BONE_JAW;
    }
    return -1; // ��� ������ ��������, ������� ��������� ���������� ������������
}


int GetInternalBoneIDFromSampID(int sampid)
{
	switch (sampid)
	{
	case ATTACH_BONE_SPINE: // 3 or 2
		return 3;
	case ATTACH_BONE_HEAD: // ?
		return 5;
	case ATTACH_BONE_LUPPER: // left upper arm
		return 22;
	case ATTACH_BONE_RUPPER: // right upper arm
		return 32;
	case ATTACH_BONE_LHAND: // left hand
		return 34;
	case ATTACH_BONE_RHAND: // right hand
		return 24;
	case ATTACH_BONE_LTHIGH: // left thigh
		return 41;
	case ATTACH_BONE_RTHIGH: // right thigh
		return 51;
	case ATTACH_BONE_LFOOT: // left foot
		return 43;
	case ATTACH_BONE_RFOOT: // right foot
		return 53;
	case ATTACH_BONE_RCALF: // right calf
		return 52;
	case ATTACH_BONE_LCALF: // left calf
		return 42;
	case ATTACH_BONE_LFARM: // left forearm
		return 33;
	case ATTACH_BONE_RFARM: // right forearm
		return 23;
	case ATTACH_BONE_LSHOULDER: // left shoulder (claviacle)
		return 31;
	case ATTACH_BONE_RSHOULDER: // right shoulder (claviacle)
		return 21;
	case ATTACH_BONE_NECK: // neck
		return 4;
	case ATTACH_BONE_JAW: // jaw ???
		return 8; // i dont know
	}
	return 0;
}

void ScrSetPlayerAttachedObject(RPCParameters* rpcParams)
{
    LOGRPC("ScrSetPlayerAttachedObject");
	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;
	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);

	PLAYERID id;
	uint32_t slot;
	bool create;
	ATTACHED_OBJECT_INFO info;

	bsData.Read(id);
	bsData.Read(slot);
	bsData.Read(create);
	CPedSamp* pPed = nullptr;
	if (id == CPlayerPool::GetLocalPlayerID())
	{
		pPed = CLocalPlayer::GetPlayerPed();
	}
	else
	{
		if (CPlayerPool::GetSpawnedPlayer(id))
		{
			pPed = CPlayerPool::GetSpawnedPlayer(id)->GetPlayerPed();
		}
	}
	if (!pPed) return;
	if (!create)
	{
		pPed->DeattachObject(slot);
		return;
	}
	bsData.Read((char*)& info, sizeof(ATTACHED_OBJECT_INFO));

	pPed->AttachObject(&info, slot);
}

void ScrSetObjectMaterial(RPCParameters* rpcParams)
{
    LOGRPC("ScrSetObjectMaterial");
    if (!rpcParams || !rpcParams->input) return;
    RakNet::BitStream bsData(reinterpret_cast<unsigned char*>(rpcParams->input), (rpcParams->numberOfBitsOfData / 8) + 1, false);
    uint16_t objectId = 0;
    uint8_t materialType = 0, materialIndex = 0, length = 0;
    bsData.Read(objectId);
    bsData.Read(materialType);
    bsData.Read(materialIndex);
    auto* object = CObjectPool::GetAt(objectId);
    if (!object) return;
    if (materialType == 1) {
        uint16_t modelId = 0xFFFF; uint32_t color = 0;
        char txd[256]{}, tex[256]{};   // uint8 lengths always fit
        bsData.Read(modelId); bsData.Read(length);
        bsData.Read(txd, length); txd[length] = 0;
        bsData.Read(length);
        bsData.Read(tex, length); tex[length] = 0;
        bsData.Read(color);

        // 0xFFFF means "no material model": resolve the texture by TXD name
        // only. Anything else out of range would index past CModelInfo.
        int resolvedModel = (modelId == 0xFFFF ||
                               modelId >= CModelInfo::NUM_MODEL_INFOS) ? -1 : static_cast<int>(modelId);

        object->SetMaterial(resolvedModel, materialIndex, txd, tex, color);
    } else if (materialType == 2) {
        uint8_t size=0, fontSize=0, bold=0, align=0; uint32_t fontColor=0, bg=0; char font[256]{}, text[2049]{};
        bsData.Read(size); bsData.Read(length);
        bsData.Read(font, length); font[length]=0;
        bsData.Read(fontSize); bsData.Read(bold); bsData.Read(fontColor); bsData.Read(bg); bsData.Read(align);
        stringCompressor->DecodeString(text, 2049, &bsData);
        text[2048] = 0;

        // Match Alyn: DecodeString() gives the SA-MP client code page; convert
        // once before storing the material text.
        const std::string utf8Text = cp1251_to_utf8(text);
        object->SetMaterialText(materialIndex, size, font, fontSize, bold,
                                fontColor, bg, align, utf8Text.c_str());
    }
}

void ScrRemoveBuilding(RPCParameters* rpcParams)
{
    LOGRPC("ScrRemoveBuilding");
    if (!rpcParams || !rpcParams->input) return;
    RakNet::BitStream bsData(reinterpret_cast<unsigned char*>(rpcParams->input), (rpcParams->numberOfBitsOfData / 8) + 1, false);
    int32_t model=-1; float x=0,y=0,z=0,radius=0;
    bsData.Read(model); bsData.Read(x); bsData.Read(y); bsData.Read(z); bsData.Read(radius);
    if (radius > 0.0f && radius < 100000.0f) RemoveBuilding::Add(model, {x,y,z}, radius);
}

// RPC_ScrSetPlayerObjectMaterial (0x54) and RPC_ScrSetObjectMaterial (84) are
// the SAME id - SA-MP sends player-object materials down the one object
// material RPC, and the client keeps player objects in the same pool. Two
// handlers were registered for it, so whichever registered last won and this
// one, which parsed the packet and then dropped it on the floor, silently ate
// every SetObjectMaterial / SetObjectMaterialText the server sent. It now just
// forwards, so the behaviour no longer depends on registration order.
void ScrSetPlayerObjectMaterial(RPCParameters* rpcParams)
{
	ScrSetObjectMaterial(rpcParams);
}

void ScrSetVehicleZAngle(RPCParameters* rpcParams)
{
	LOGRPC("RPC: ScrSetVehicleZAngle");

	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);
	VEHICLEID VehicleId;
	float fZAngle;
	bsData.Read(VehicleId);
	bsData.Read(fZAngle);

	CVehicleMP* pVeh = CVehiclePool::GetAt(VehicleId);
	if (!pVeh) return;
	if (GamePool_Vehicle_GetAt(pVeh->m_dwGTAId))
	{
		ScriptCommand(&set_car_z_angle, pVeh->m_dwGTAId, fZAngle);
	}
}

void ScrAttachTrailerToVehicle(RPCParameters* rpcParams)
{
    LOGRPC("ScrAttachTrailerToVehicle");
	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;
	PlayerID sender = rpcParams->sender;

	VEHICLEID TrailerID, VehicleID;
	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);
	bsData.Read(TrailerID);
	bsData.Read(VehicleID);
	CVehicleMP* pTrailer = CVehiclePool::GetAt(TrailerID);
	CVehicleMP* pVehicle = CVehiclePool::GetAt(VehicleID);
	if (!pVehicle) return;
	if (!pTrailer) return;
	pVehicle->SetTrailer(pTrailer);
	pVehicle->AttachTrailer();
}

//----------------------------------------------------

void ScrDetachTrailerFromVehicle(RPCParameters* rpcParams)
{
    LOGRPC("ScrDetachTrailerFromVehicle");
	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;
	PlayerID sender = rpcParams->sender;

	VEHICLEID VehicleID;
	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);
	bsData.Read(VehicleID);
	CVehicleMP* pVehicle = CVehiclePool::GetAt(VehicleID);
	if (!pVehicle) return;
	pVehicle->DetachTrailer();
	pVehicle->SetTrailer(NULL);
}

void ScrRemoveComponent(RPCParameters* rpcParams)
{
    LOGRPC("ScrRemoveComponent");
	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;
	
	VEHICLEID vehicleId;
	uint16_t component;
	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);
	bsData.Read(vehicleId);
	bsData.Read(component);

	if (CVehiclePool::GetAt(vehicleId))
    {
        CVehiclePool::GetAt(vehicleId)->RemoveComponent(component);
    }
}

void ScrMoveObject(RPCParameters* rpcParams)
{
    LOGRPC("ScrMoveObject");
	auto Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);

	unsigned short byteObjectID;
	float curx, cury, curz, newx, newy, newz, speed, rotx, roty, rotz;

	bsData.Read(byteObjectID);
	bsData.Read(curx);
	bsData.Read(cury);
	bsData.Read(curz);
	bsData.Read(newx);
	bsData.Read(newy);
	bsData.Read(newz);
	bsData.Read(speed);
	bsData.Read(rotx);
	bsData.Read(roty);
	bsData.Read(rotz);

	CObjectSamp* pObject = CObjectPool::GetAt(byteObjectID);
	if (pObject)
	{
		// Same as the PC client: start from the server's current position.
		// Without this every client-side drift (frame drops, late RPCs, an
		// object streamed in while moving) stayed in the movement and the
		// object ended up visibly offset from where the server has it.
		if (pObject->m_bIsMoving) pObject->StopMoving();
		if (std::isfinite(curx) && std::isfinite(cury) && std::isfinite(curz)) {
			pObject->SetPos(curx, cury, curz);
		}
		pObject->MoveTo(newx, newy, newz, speed, rotx, roty, rotz);
	}
}

// StopObject / StopPlayerObject. This RPC was never registered, so a stopped
// object kept moving on the client to its old target and was out of sync with
// the server (and with every other player) until it was streamed again.
void ScrStopObject(RPCParameters* rpcParams)
{
	LOGRPC("ScrStopObject");
	if (!rpcParams || !rpcParams->input) return;

	RakNet::BitStream bsData(reinterpret_cast<unsigned char*>(rpcParams->input),
	                         (rpcParams->numberOfBitsOfData / 8) + 1, false);

	uint16_t objectId = 0;
	if (!bsData.Read(objectId)) return;

	CObjectSamp* pObject = CObjectPool::GetAt(objectId);
	if (pObject)
	{
		pObject->StopMoving();
	}
}

void ScrSetPlayerDrunkLevel(RPCParameters* rpcParams)
{
    LOGRPC("ScrSetPlayerDrunkLevel");
	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);

	uint32_t dDrunkLevel;

	bsData.Read(dDrunkLevel);
	Log("ScrSetPlayerDrunkLevel %d", dDrunkLevel);

	CPedSamp *pPlayer = CGame::FindPlayerPed();
	if (!pPlayer)
		return;

	// Only the server RPC is allowed to enable the drunk state.
	pPlayer->SetDrunkLevelFromServer(dDrunkLevel);
}
void ScrSetObjectRotation(RPCParameters* rpcParams)
{
    LOGRPC("ScrSetObjectRotation");
	unsigned char* Data = reinterpret_cast<unsigned char*>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData(Data, (iBitLength / 8) + 1, false);

	uint16_t objectId;
	CVector vecRot;
	
	bsData.Read(objectId);
	bsData.Read((char*)&vecRot, sizeof(CVector));

	if (CObjectPool::GetAt(objectId))
	{
		CObjectPool::GetAt(objectId)->InstantRotate(vecRot.x, vecRot.y, vecRot.z);
	}
}

void ScrSetPlayerTeam(RPCParameters *rpcParams)
{
	LOGRPC("RPC: ScrSetPlayerTeam");
	auto * Data = reinterpret_cast<unsigned char *>(rpcParams->input);
	int iBitLength = rpcParams->numberOfBitsOfData;

	RakNet::BitStream bsData((unsigned char*)Data,(iBitLength/8)+1,false);

	PLAYERID playerId;
	uint8_t teamId;

	bsData.Read(playerId);
	bsData.Read(teamId);

	if(playerId == CPlayerPool::GetLocalPlayerID())
		CLocalPlayer::GetPlayerPed()->SetMoveAnim(teamId);
	else
	{
		if(CPlayerPool::GetSpawnedPlayer(playerId))
			CPlayerPool::GetAt(playerId)->GetPlayerPed()->SetMoveAnim(teamId);
	}
}

bool bDisableVehicleCollision = false;
void ScrDisableRemoteVehicleCollision(RPCParameters *rpcParams)
{
    Log("RPC: ScrDisableRemoteVehicleCollisions");
    unsigned char* Data = reinterpret_cast<unsigned char *>(rpcParams->input);
    int iBitLength = rpcParams->numberOfBitsOfData;
    RakNet::BitStream bsData((unsigned char*)Data, (iBitLength / 8) + 1, false);

    bool bDisable;
    bsData.Read(bDisable);
    bDisableVehicleCollision = bDisable;
}

void RegisterScriptRPCs(RakClientInterface* pRakClient)
{
    LOGRPC("Registering ScriptRPC's..");

	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerTeam, ScrSetPlayerTeam);
    pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetSpawnInfo, ScrSetSpawnInfo);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrDisplayGameText, ScrDisplayGameText);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetGravity, ScrSetGravity);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerPos, ScrSetPlayerPos);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetCameraPos, ScrSetCameraPos);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetCameraLookAt, ScrSetCameraLookAt);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerFacingAngle, ScrSetPlayerFacingAngle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetFightingStyle, ScrSetFightingStyle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerSkin, ScrSetPlayerSkin);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrApplyPlayerAnimation, ScrApplyPlayerAnimation);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrClearPlayerAnimations, ScrClearPlayerAnimations);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrCreateExplosion, ScrCreateExplosion);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerHealth, ScrSetPlayerHealth);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerArmour, ScrSetPlayerArmour);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerColor, ScrSetPlayerColor);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerName, ScrSetPlayerName);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerPosFindZ, ScrSetPlayerPosFindZ);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetInterior, ScrSetPlayerInterior);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetMapIcon, ScrSetMapIcon);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrDisableMapIcon, ScrDisableMapIcon);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetCameraBehindPlayer, ScrSetCameraBehindPlayer);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetSpecialAction, ScrSetPlayerSpecialAction);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrTogglePlayerSpectating, ScrTogglePlayerSpectating);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerSpectating, ScrSetPlayerSpectating);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrPlayerSpectatePlayer, ScrPlayerSpectatePlayer);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrPlayerSpectateVehicle, ScrPlayerSpectateVehicle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrPutPlayerInVehicle, ScrPutPlayerInVehicle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrVehicleParams, ScrVehicleParams);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrVehicleParamsEx, ScrVehicleParamsEx);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrHaveSomeMoney, ScrHaveSomeMoney);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrResetMoney, ScrResetMoney);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrLinkVehicle, ScrLinkVehicle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrRemovePlayerFromVehicle, ScrRemovePlayerFromVehicle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetVehicleHealth, ScrSetVehicleHealth);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetVehiclePos, ScrSetVehiclePos);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetVehicleVelocity, ScrSetVehicleVelocity);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrNumberPlate, ScrNumberPlate);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrInterpolateCamera, ScrInterpolateCamera);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrAddGangZone,ScrAddGangZone);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrRemoveGangZone,ScrRemoveGangZone);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrFlashGangZone,ScrFlashGangZone);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrStopFlashGangZone,ScrStopFlashGangZone);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrCreateObject, ScrCreateObject);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetObjectMaterial, ScrSetObjectMaterial);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_RemoveBuilding, ScrRemoveBuilding);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetObjectPos, ScrSetObjectPos);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrDestroyObject, ScrDestroyObject);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrPlaySound, ScrPlaySound);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerWantedLevel, ScrSetPlayerWantedLevel);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrGivePlayerWeapon, ScrGivePlayerWeapon);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetWeaponAmmo, ScrSetWeaponAmmo);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrTogglePlayerControllable, ScrTogglePlayerControllable);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrAttachObjectToPlayer, ScrAttachObjectToPlayer);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrResetPlayerWeapons, ScrResetPlayerWeapons);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerSkillLevel, ScrSetPlayerSkillLevel);

    pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrShowTextDraw, ScrShowTextDraw);
    pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrHideTextDraw, ScrHideTextDraw);
    pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrEditTextDraw, ScrEditTextDraw);
    pRakClient->RegisterAsRemoteProcedureCall(&RPC_ClickTextDraw, ScrSelectTextDraw);

	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerAttachedObject, ScrSetPlayerAttachedObject);


	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetVehicleZAngle, ScrSetVehicleZAngle);

	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrAttachTrailerToVehicle, ScrAttachTrailerToVehicle);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrDetachTrailerFromVehicle, ScrDetachTrailerFromVehicle);

	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrRemoveComponent, ScrRemoveComponent);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrMoveObject, ScrMoveObject);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrStopObject, ScrStopObject);
	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetObjectRotation, ScrSetObjectRotation);

	pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrSetPlayerDrunkLevel, ScrSetPlayerDrunkLevel);

    pRakClient->RegisterAsRemoteProcedureCall(&RPC_ScrDisableRemoteVehicleCollision, ScrDisableRemoteVehicleCollision);
}

void UnRegisterScriptRPCs(RakClientInterface* pRakClient)
{
    LOGRPC("Unregistering ScriptRPC's..");

	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ClickTextDraw);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrAttachTrailerToVehicle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrDetachTrailerFromVehicle);

	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetVehicleZAngle);


	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerAttachedObject);

	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrEditTextDraw);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrShowTextDraw);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrHideTextDraw);

	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerSkillLevel);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrResetPlayerWeapons);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrGivePlayerWeapon);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetWeaponAmmo);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrDisplayGameText);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetGravity);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrForceSpawnSelection);
    pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetSpawnInfo);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerPos);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetCameraPos);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetCameraLookAt);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerFacingAngle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetFightingStyle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerSkin);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrApplyPlayerAnimation);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrClearPlayerAnimations);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrCreateExplosion);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerHealth);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerArmour);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerColor);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerName);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerPosFindZ);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetInterior);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetMapIcon);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrDisableMapIcon);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetCameraBehindPlayer);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetSpecialAction);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrTogglePlayerSpectating);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerSpectating);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrPlayerSpectatePlayer);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrPlayerSpectateVehicle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrPutPlayerInVehicle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrVehicleParams);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrVehicleParamsEx);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrHaveSomeMoney);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrResetMoney);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrLinkVehicle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrRemovePlayerFromVehicle);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetVehicleHealth);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetVehiclePos);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetVehicleVelocity);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrNumberPlate);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrInterpolateCamera);
	
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrAddGangZone);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrRemoveGangZone);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrFlashGangZone);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrStopFlashGangZone);

	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrCreateObject);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetObjectMaterial);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_RemoveBuilding);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrDestroyObject);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetObjectPos);

	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrPlaySound);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetPlayerWantedLevel);

	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrRemoveComponent);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrSetObjectRotation);
	pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrMoveObject);

    pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrEditTextDraw);
    pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrShowTextDraw);
    pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrHideTextDraw);
    pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ClickTextDraw);

    pRakClient->UnregisterAsRemoteProcedureCall(&RPC_ScrDisableRemoteVehicleCollision);
}