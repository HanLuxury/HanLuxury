#include "game/character/CharacterManager.h"
#include "../ClothesSystem.h"
#include "../main.h"
#include "game.h"
#include "../net/netgame.h"
#include "common.h"
      
#include "..//CDebugInfo.h"
#include "util/patch.h"
#include "Enums/ePedState.h"
#include "game/Models/ModelInfo.h"
#include "game/RW/rphanim.h"
#include "RwHelper.h"
#include "World.h"
#include "game/Animation/AnimManager.h"
#include "java_systems/HUD.h"
#include "game/Plugins/RpAnimBlendPlugin/RpAnimBlend.h"
#include "CarEnterExit.h"
#include "Timer.h"
#include "WeaponInfo.h"
#include "CrossHair.h"
#include "scripting.h"
#include "Entity/Ped/Ped.h"
#include "PedIntelligence.h" 

CPedSamp* g_pCurrentFiredPed;

CPedSamp::CPedSamp()
{
	drunk_level = 0;
	m_bDrunkServerAuthorized = false;
	m_dwGTAId = 1;
	m_pPed = (CPed*)GamePool_FindPlayerPed();
	m_bHaveBulletData = false;

	m_bytePlayerNumber = 0;
	SetPlayerPedPtrRecord(m_bytePlayerNumber,(uintptr_t)m_pPed);
	ScriptCommand(&set_actor_weapon_droppable, m_dwGTAId, 1);
	ScriptCommand(&set_char_never_targeted, m_dwGTAId, 1);
	ScriptCommand(&set_actor_can_be_decapitated, m_dwGTAId, 0);
	m_dwArrow = 0;
}

CPedSamp::CPedSamp(uint8_t bytePlayerNumber, int iSkin, float fX, float fY, float fZ, float fRotation)
{
    drunk_level = 0;
    m_bDrunkServerAuthorized = false;
    static int iPlayerNum;
    iPlayerNum = bytePlayerNumber; // ty killman <3


    m_pPed = nullptr;
	m_dwGTAId = 0;
	m_dwArrow = 0;
	m_bHaveBulletData = false;

	ScriptCommand(&create_player, &iPlayerNum, fX, fY, fZ, &m_dwGTAId);
	ScriptCommand(&create_actor_from_player, &iPlayerNum, &m_dwGTAId);

	m_pPed = GamePool_Ped_GetAt(m_dwGTAId);

	m_bytePlayerNumber = bytePlayerNumber;

	SetPlayerPedPtrRecord(m_bytePlayerNumber, (uintptr_t)m_pPed);
	m_pPed->bDoesntDropWeaponsWhenDead = true;

	ScriptCommand(&set_actor_immunities, m_dwGTAId, 0, 0, 0, 0, 0);
	ScriptCommand(&set_actor_can_be_decapitated, m_dwGTAId, 0); // ����� �����
	m_pPed->bNeverEverTargetThisPed = true;

	ScriptCommand(&set_actor_money, m_dwGTAId, 0); // ������ ������ ��� ������

	SetModelIndex(iSkin);
	ForceTargetRotation(fRotation);
	RwMatrix mat;
	m_pPed->GetMatrix(&mat);
	mat.pos.x = fX;
	mat.pos.y = fY;
	mat.pos.z = fZ+ 0.15f;
	m_pPed->SetMatrix((CMatrix&)mat);

	memset(&RemotePlayerKeys[m_bytePlayerNumber], 0, sizeof(PAD_KEYS));
}

bool IsValidGamePed(CPed* pPed);

CPedSamp::~CPedSamp()
{
	memset(&RemotePlayerKeys[m_bytePlayerNumber], 0, sizeof(PAD_KEYS));

	SetPlayerPedPtrRecord(m_bytePlayerNumber, 0);

	if(!m_dwGTAId)return;
	if(!GamePool_Ped_GetAt(m_dwGTAId))return;

	if (m_pPed && IsValidGamePed(m_pPed))
	{
        Eagle::Character::CharacterManager::BeforePedDestroyed(m_pPed);
        const auto modelId = m_pPed->m_nModelIndex;
		FlushAttach();
		if (m_pPed->bInVehicle) {
			m_pPed->RemoveFromVehicle();

		//	ClearAllTasks();
		}
		// m_nPlayerGroup is left alone: ~CPlayerPed frees the group this ped's constructor took
		// (CPedGroups::AddGroup 0x5c0c4c, RemoveGroup 0x5c0f98). When all 8 were taken it holds -1, and
		// RemoveGroup(-1) only reads ms_activeGroups[-1], the zero top byte of the allocator vtable pointer
		// before it, and returns. Group 0 must not be forced: it is normally the local player's group. The old
		// "m_pPlayerData + 76" stepped 76 CPlayerPedData (16416 bytes) into another CWorld::Players entry.
		// CPedSamp::Destructor

		//(( void (*)(CPed*))(*(void**)(m_pPed->vtable+0x4)))(m_pPed);
		((void (*)(uintptr_t))(g_libGTASA + (VER_x32 ? 0x004CE6A0 + 1 : 0x5CDC64)))((uintptr_t)m_pPed);
		//ScriptCommand(&DELETE_CHAR, m_dwGTAId);

		m_pPed = nullptr;
		m_dwGTAId = 0;
        CStreaming::RemoveModelIfNoRefs(modelId);
	}
	else
	{
		m_pPed = nullptr;
		m_dwGTAId = 0;
	}
}

void CPedSamp::ClearDrunkState()
{
	drunk_level = 0;
	m_bDrunkServerAuthorized = false;
	ScriptCommand(&SET_PLAYER_DRUNKENNESS, m_bytePlayerNumber, 0);
}

void CPedSamp::SetDrunkLevelFromServer(uint32_t level)
{
	if (level > 10000)
		level = 10000;

	drunk_level = static_cast<int>(level);
	m_bDrunkServerAuthorized = (level != 0);

	// Apply immediately only for an explicitly received server RPC.
	ScriptCommand(&SET_PLAYER_DRUNKENNESS, m_bytePlayerNumber,
	              static_cast<int>(level / 100));
}

CAMERA_AIM * CPedSamp::GetCurrentAim()
{
	return GameGetInternalAim();
}

void CPedSamp::SetCurrentAim(CAMERA_AIM * pAim)
{
	GameStoreRemotePlayerAim(m_bytePlayerNumber, pAim);
}

uint16_t CPedSamp::GetCameraMode()
{
	return GameGetLocalPlayerCameraMode();
}

void CPedSamp::SetCameraMode(uint16_t byteCamMode)
{
	GameSetPlayerCameraMode(byteCamMode, m_bytePlayerNumber);
}

float CPedSamp::GetCameraExtendedZoom()
{
	return GameGetLocalPlayerCameraExtZoom();
}

void CPedSamp::ApplyCrouch()
{
	
	if (!m_pPed || !m_dwGTAId)
	{
		return;
	}
	if (!GamePool_Ped_GetAt(m_dwGTAId))
	{
		return;
	}

	if (!(m_pPed->bIsDucking))
	{
		if (!IsCrouching())
		{
			if (m_pPed->m_pIntelligence)
			{
				((int (*)(CPedIntelligence*, uint16_t))(g_libGTASA + (VER_x32 ? 0x004C07B0 + 1 : 0x5BCE70)))(m_pPed->m_pIntelligence, 0);
			}
		}
	}
}

void CPedSamp::ResetCrouch()
{
	if (!m_pPed || !m_dwGTAId)
	{
		return;
	}
	if (!GamePool_Ped_GetAt(m_dwGTAId))
	{
		return;
	}
	m_pPed->bIsDucking = false;
	if (m_pPed->m_pIntelligence)
	{
		((int (*)(CPedIntelligence*))(g_libGTASA + (VER_x32 ? 0x004C08A8 + 1 : 0x5BCFF8)))(m_pPed->m_pIntelligence);
	}
}

bool CPedSamp::IsCrouching()
{
	
	if (!m_pPed || !m_dwGTAId)
	{
		return false;
	}
	if (!GamePool_Ped_GetAt(m_dwGTAId))
	{
		return false;
	}
	return m_pPed->bIsDucking;
}

void CPedSamp::SetDead()
{
	
	if (!m_dwGTAId || !m_pPed)
	{
		return;
	}
	if (!GamePool_Ped_GetAt(m_dwGTAId))
	{
		return;
	}
	
	RwMatrix mat;
	m_pPed->GetMatrix(&mat);
	// will reset the tasks
	m_pPed->Teleport(CVector(mat.pos), false);
	SetHealth(0.f);

	uint8_t old = CWorld::PlayerInFocus;
	CWorld::PlayerInFocus = m_bytePlayerNumber;
	ScriptCommand(&kill_actor, m_dwGTAId);
	CWorld::PlayerInFocus = 0;
}


void CPedSamp::SetWeaponAmmo(int iWeaponID, int iAmmo)
{
	if (!m_pPed || !m_dwGTAId)
	{
		return;
	}

	if (!GamePool_Ped_GetAt(m_dwGTAId))
	{
		return;
	}

	int iModelID = 0;
	iModelID = CUtil::GameGetWeaponModelIDFromWeaponID(iWeaponID);

	if (iModelID == -1) return;

	ScriptCommand(&SET_CHAR_AMMO, m_dwGTAId, iWeaponID, iAmmo);
	SetArmedWeapon(iWeaponID);
}

void CPedSamp::SetArmedWeapon(int iWeaponID) const
{
	if (!m_pPed || !m_dwGTAId)
	{
		return;
	}

	if (!GamePool_Ped_GetAt(m_dwGTAId))
	{
		return;
	}

	((int(*)(uintptr_t, unsigned int))(g_libGTASA + (VER_x32 ? 0x004A521C + 1 : 0x59B86C)))((uintptr_t)m_pPed, iWeaponID);	// CPed::SetCurrentWeapon(thisptr, weapid)
}

void CPedSamp::ApplyCommandTask(char* a2, int a4, int a5, int a6, CVector* a7, char a8, float a9, int a10, int a11, char a12)
{
	uintptr dwPed = (uintptr)m_pPed;
	if (!dwPed) return;
	// 00958484 - g_ikChainManager
	// 00463188 addr
	((int(*)(uintptr_t a1, char* a2, uint32_t a3, int a4, int a5, int a6, CVector* a7, char a8, float a9, int a10, int a11, char a12))(g_libGTASA + (VER_x32 ? 0x004D36E2 + 1 : 0x5D3884)))
		(g_libGTASA + (VER_x32 ? 0x009F9D4C : 0xC98340), a2, dwPed, a4, a5, a6, a7, a8, a9, a10, a11, a12);

}

BYTE CPedSamp::GetCurrentWeapon()
{
    return GetCurrentWeaponSlot()->m_nType;
}

// 0.3.7
CVehicle* CPedSamp::GetGtaVehicle()
{
	return m_pPed->pVehicle;
}

// 0.3.7
void CPedSamp::SetInitialState() const
{
	DLOG("CPedSamp::SetInitialState()");

    CHook::CallFunction<void>(g_libGTASA + (VER_x32 ? 0x004C37B4 + 1 : 0x5C0D50), m_pPed);
//	//TODO:����������?
//	CTimer::ms_fTimeScale = 1.0;
//	m_pPed->ClearAimFlag();
//	m_pPed->ClearLookFlag();
//	m_pPed->SetPedState(PEDSTATE_IDLE);
//	m_pPed->SetMoveState(PEDMOVE_STILL);
//
//	CPlayerInfoGta& playerInfo = FindPlayerInfo();
//	playerInfo.PlayerState = PLAYERSTATE_PLAYING;
//
//	m_pPed->m_pIntelligence->FlushIntelligence();
//	RpAnimBlendClumpRemoveAllAssociations(m_pPed->m_pRwClump);
//
//	auto &taskMgr = m_pPed->m_pIntelligence->m_TaskMgr;
//	taskMgr.SetTask(
//			new CTaskSimplePlayerOnFoot(),
//			TASK_PRIMARY_DEFAULT
//			);
//
//	taskMgr.SetTask(
//			new CTaskSimpleStandStill{ 0, true },
//			TASK_PRIMARY_PRIMARY
//	);
}

// 0.3.7
void CPedSamp::SetHealth(float fHealth) const
{
	if(!m_pPed) return;
	if (IsValidGamePed(m_pPed))
	{
		m_pPed->m_fHealth = fHealth;

		CHUD::updateBars();
	}

}

// 0.3.7
float CPedSamp::GetHealth()
{
	if(!m_pPed) return 0.0f;
	return m_pPed->m_fHealth;
}

// 0.3.7
void CPedSamp::SetArmour(float fArmour)
{
	if(!m_pPed) return;
	m_pPed->m_fArmour = fArmour;

	CHUD::updateBars();
}

float CPedSamp::GetArmour()
{
	if(!m_pPed) return 0.0f;
	return m_pPed->m_fArmour;
}

void CPedSamp::EnterVehicle(int iVehicleID, bool bPassenger)
{
	if (!m_pPed) return;
	if(!m_dwGTAId)return;
	if (!IsValidGamePed(m_pPed) || !GamePool_Ped_GetAt(m_dwGTAId)) {
		return;
	}

	CVehicle* ThisVehicleType;
	if((ThisVehicleType = GamePool_Vehicle_GetAt(iVehicleID)) == 0) return;
	if (ThisVehicleType->fHealth == 0.0f) return;

	// Alyn clears the parachute before starting the vehicle entry task.
	if (GetCurrentWeapon() == WEAPON_PARACHUTE)
		SetArmedWeapon(0);

	if(bPassenger)
	{
		if (ThisVehicleType->m_nModelIndex == TRAIN_PASSENGER &&
			m_pPed == GamePool_FindPlayerPed())
			ScriptCommand(&put_actor_in_car2, m_dwGTAId, iVehicleID, -1);
		else
			ScriptCommand(&send_actor_to_car_passenger,m_dwGTAId,iVehicleID, 3000, -1);
	}
	else{
		ScriptCommand(&TASK_ENTER_CAR_AS_DRIVER, m_dwGTAId, iVehicleID, 3000);
	}

}

// Ported from alyn (CPlayerPed::PutDirectlyInVehicle): warp the ped onto a seat
// without an animation.  byteSeatID is the SA-MP seat id, 0 = driver.
bool CPedSamp::PutDirectlyInVehicle(int iVehicleID, uint8_t byteSeatID)
{
	if (!m_pPed || !m_dwGTAId) return false;
	if (!IsValidGamePed(m_pPed) || !GamePool_Ped_GetAt(m_dwGTAId)) return false;

	CVehicle* pGtaVehicle = GamePool_Vehicle_GetAt(iVehicleID);
	if (!pGtaVehicle) return false;
	if (pGtaVehicle->fHealth == 0.0f) return false;

	if (GetCurrentWeapon() == WEAPON_PARACHUTE)
		SetArmedWeapon(0);

	// A seat the model does not have (cars and bikes, like alyn).
	const uintptr_t vptr = *reinterpret_cast<uintptr_t*>(pGtaVehicle) - g_libGTASA;
	const bool bCarOrBike = vptr == (VER_x32 ? 0x0066D678 : 0x83BB50) ||	// CAutomobile
							vptr == (VER_x32 ? 0x0066D7F0 : 0x83BE40);		// CBike
	if (bCarOrBike && byteSeatID > pGtaVehicle->m_nMaxPassengers)
		return false;

	if (byteSeatID == 0)
	{
		// Somebody else is still behind the wheel: putting a second driver in
		// leaves the first one in the vehicle without a seat.
		CPed* pDriver = pGtaVehicle->pDriver;
		if (pDriver && pDriver != m_pPed && pDriver->IsInVehicle())
			return false;

		ScriptCommand(&put_actor_in_car, m_dwGTAId, iVehicleID);
	}
	else
	{
		ScriptCommand(&put_actor_in_car2, m_dwGTAId, iVehicleID, byteSeatID - 1);
	}

	return m_pPed->IsInVehicle();
}

// 0.3.7
void CPedSamp::ExitCurrentVehicle()
{
	DLOG("ExitCurrentVehicle");

	if (!m_pPed) return;
	if (!m_dwGTAId) return;
	if (!IsValidGamePed(m_pPed) || !GamePool_Ped_GetAt(m_dwGTAId)) {
		return;
	}

	if (!m_pPed->bInVehicle || !m_pPed->pVehicle) {
		return;
	}

	// Use the same GTA script command as the Alyn reference instead of
	// TASK_LEAVE_ANY_CAR (0x0633). The latter schedules a generic leave task;
	// during the enter/exit transition this client can leave the ped/object
	// hierarchy in the wrong state for one or more frames. The 0x05CD command
	// targets the current vehicle explicitly and lets GTA perform the normal
	// door/seat/task cleanup.
	const uint32_t gtaVehicleId = GamePool_Vehicle_GetIndex(m_pPed->pVehicle);
	if (gtaVehicleId == 0 || gtaVehicleId == INVALID_VEHICLE_ID) {
		return;
	}

	CVehicle* pVehicle = GamePool_Vehicle_GetAt(gtaVehicleId);
	if (!pVehicle) {
		return;
	}

	// Trains use different enter/exit handling; preserve the behavior from
	// the Alyn implementation and do not send make_actor_leave_car for the
	// train passenger models.
	if (pVehicle->m_nModelIndex == TRAIN_PASSENGER ||
		pVehicle->m_nModelIndex == TRAIN_PASSENGER_LOCO) {
		return;
	}

	ScriptCommand(&make_actor_leave_car, m_dwGTAId, gtaVehicleId);
}

// 0.3.7
VEHICLEID CPedSamp::GetCurrentSampVehicleID()
{
	if(!m_pPed) return INVALID_VEHICLE_ID;
	if(!pNetGame)return INVALID_VEHICLE_ID;

	if(!m_pPed->pVehicle)return INVALID_VEHICLE_ID;

	return CVehiclePool::FindIDFromGtaPtr((CVehicle *)m_pPed->pVehicle);
}

CVehicleMP* CPedSamp::GetCurrentVehicle()
{
	if(!m_pPed || !m_pPed->IsInVehicle())
        return nullptr;

    uint32_t vehicleId = CVehiclePool::GetEntity(m_pPed->pVehicle);
    if (vehicleId != INVALID_VEHICLE_ID) {
        return CVehiclePool::GetAt(vehicleId);
    }
	return nullptr;
}

CVehicle* CPedSamp::GetCurrentGtaVehicle()
{
	if(!m_pPed) return nullptr;

	return (CVehicle *)m_pPed->pVehicle;
}

uint32_t CPedSamp::GetCurrentGTAVehicleID() {
	if(!m_pPed) return 0;
	return GamePool_Vehicle_GetIndex(reinterpret_cast<CVehicle *>(m_pPed->pVehicle));
}

// 0.3.7
void CPedSamp::TogglePlayerControllable(bool bToggle, bool isTemp)
{
    if(!isTemp) lToggle = bToggle;

    if (!m_pPed || !m_dwGTAId) return;
    if (!IsValidGamePed(m_pPed) || !GamePool_Ped_GetAt(m_dwGTAId)) return;

    if(!bToggle)
    {
        ScriptCommand(&toggle_player_controllable, m_bytePlayerNumber, 0);
        ScriptCommand(&lock_actor, m_dwGTAId, 1);
    }
    else
    {
        ScriptCommand(&toggle_player_controllable, m_bytePlayerNumber, 1);
        ScriptCommand(&lock_actor, m_dwGTAId, 0);
    }
}

// 0.3.7
void CPedSamp::SetModelIndex(uint32 uiModel)
{
	if(!GamePool_Ped_GetAt(m_dwGTAId)) return;

    // Mobile compatibility: SAMP skin ID 1 is represented by native CJ.
    if(uiModel == 1)
        uiModel = 0;

	if(!IsPedModel(uiModel))
		uiModel = 0;

	if(m_pPed)
	{
        auto oldModelId = m_pPed->m_nModelIndex;

        if (!CStreaming::TryLoadModel(uiModel))
            return;

        // Callbacks go back on the live clump first, then the clump goes, as in SA-MP and the game's
        // own player model change (0x40a0d4). CEntity::CreateRwObject stores the new clump over the old
        // one without destroying it (0x4cb2d8), so without this every skin change leaked a clump and
        // kept the old model referenced, and RemoveModelIfNoRefs below never freed it.
        Eagle::Character::CharacterManager::BeforePedDestroyed(m_pPed);
        CClothesSystem::BeforeClumpReplaced(m_pPed);
        m_pPed->DeleteRwObject();
        m_pPed->m_nModelIndex = uiModel;

        m_pPed->SetModelIndex(uiModel);
        CClothesSystem::AfterClumpReplaced(m_pPed);

        CStreaming::RemoveModelIfNoRefs(oldModelId);
	}
}

bool CPedSamp::IsAnimationPlaying(char* szAnimName)
{
	if (!m_pPed) return false;
	if (!GamePool_Ped_GetAt(m_dwGTAId)) return false;
	if (!szAnimName || !strlen(szAnimName)) return false;

	if (ScriptCommand(&is_char_playing_anim, m_dwGTAId, szAnimName)) {
		return true;
	}

	return false;
}

void CPedSamp::ClearAllTasks()
{
	if (!GamePool_Ped_GetAt(m_dwGTAId) || !m_pPed) {
		return;
	}

	ScriptCommand(&clear_char_tasks, m_dwGTAId);
}

void CPedSamp::ClearAnimations()
{
	m_pPed->ResetMoveSpeed();

	auto pos = m_pPed->GetPosition();
	if(m_pPed->IsInVehicle())
		m_pPed->RemoveFromVehicleAndPutAt(pos);
	else
		m_pPed->Teleport(pos, false);

	DLOG("ClearAnimations");
}

void CPedSamp::DestroyFollowPedTask()
{
    ClearAllTasks();
}

void CPedSamp::ClearAllWeapons()
{
	uintptr_t dwPedPtr = (uintptr_t)m_pPed;
	uint8_t old = CWorld::PlayerInFocus;
	CWorld::PlayerInFocus = m_bytePlayerNumber;

	((uint32_t(*)(uintptr_t, int, int, int))(g_libGTASA + (VER_x32 ? 0x0049F836 + 1 : 0x595604)))(dwPedPtr, 1, 1, 1); // CPed::ClearWeapons(void)

	CWorld::PlayerInFocus = old;

    CHUD::updateAmmo();
}


CVector* CPedSamp::GetCurrentWeaponFireOffset() {
	if (!IsValidGamePed(m_pPed) || !GamePool_Ped_GetAt(m_dwGTAId)) {
		return nullptr;
	}

	CWeapon* pSlot = GetCurrentWeaponSlot();
	if (pSlot) {
        return &CWeaponInfo::GetWeaponInfo(pSlot->m_nType, eWeaponSkill::STD)->m_vecFireOffset;
	}
	return nullptr;
}

void CPedSamp::GetWeaponInfoForFire(int bLeft, CVector* vecBone, CVector* vecOut) {
	if (!IsValidGamePed(m_pPed) || !GamePool_Ped_GetAt(m_dwGTAId)) {
		return;
	}

	CVector* pFireOffset = GetCurrentWeaponFireOffset();
	if (pFireOffset && vecBone && vecOut) {
		vecOut->x = pFireOffset->x;
		vecOut->y = pFireOffset->y;
		vecOut->z = pFireOffset->z;

        ePedBones boneId = ePedBones::BONE_R_HAND;
        if (bLeft) {
            boneId = ePedBones::BONE_L_HAND;
        }

		m_pPed->GetBonePosition(vecBone, boneId, false);

		vecBone->z += pFireOffset->z + 0.15f;

		// CPed::GetTransformedBonePosition
		((void (*)(CPed*, CVector*, int, bool))(g_libGTASA + (VER_x32 ? 0x004A24A8 + 1 : 0x598670)))(m_pPed, vecOut, boneId, false);
	}
}

extern uint32_t (*CWeapon__FireInstantHit)(CWeapon* thiz, CPed* pFiringEntity, CVector* vecOrigin, CVector* muzzlePosn, CEntity* targetEntity, CVector *target, CVector* originForDriveBy, int arg6, int muzzle);
extern uint32_t (*CWeapon__FireSniper)(CWeapon *pWeaponSlot, CPed *pFiringEntity, CEntity *a3, CVector *vecOrigin);

void CPedSamp::FireInstant() {
	if(!m_pPed || !GamePool_Ped_GetAt(m_dwGTAId)) {
		return;
	}

	uint8_t byteCameraMode;
	if(m_bytePlayerNumber != 0) {
		byteCameraMode = *pbyteCameraMode;
		*pbyteCameraMode = GameGetPlayerCameraMode(m_bytePlayerNumber);

		// wCameraMode2
		GameStoreLocalPlayerCameraExtZoom();
		GameSetRemotePlayerCameraExtZoom(m_bytePlayerNumber);

		GameStoreLocalPlayerAim();
		GameSetRemotePlayerAim(m_bytePlayerNumber);
	}
	g_pCurrentFiredPed = this;

	CWeapon *pSlot = GetCurrentWeaponSlot();
	if(pSlot) {
		if(GetCurrentWeapon() == WEAPON_SNIPERRIFLE)
		{
			if(m_pPed)
				CWeapon__FireSniper(pSlot, m_pPed, nullptr, nullptr);
			else
				CWeapon__FireSniper(nullptr, m_pPed, nullptr, nullptr);
		}
		else
		{
			CVector vecBonePos;
			CVector vecOut;

			GetWeaponInfoForFire(false, &vecBonePos, &vecOut);

			if(m_pPed)
				CWeapon__FireInstantHit(pSlot, m_pPed, &vecBonePos, &vecOut, nullptr, nullptr, nullptr, 0, 1);
			else
				CWeapon__FireInstantHit(nullptr, m_pPed, &vecBonePos, &vecOut, nullptr, nullptr, nullptr, 0, 1);
		}
	}

	g_pCurrentFiredPed = nullptr;

	if(m_bytePlayerNumber != 0) {
		*pbyteCameraMode = byteCameraMode;

		// wCamera2
		GameSetLocalPlayerCameraExtZoom();
		GameSetLocalPlayerAim();
	}
}

void CPedSamp::ResetDamageEntity()
{
	Log("ResetDamageEntity");
	m_pPed->m_pLastEntityDamage = nullptr;
}

// 0.3.7
void CPedSamp::RestartIfWastedAt(const CVector vec, float fRotation)
{	
	ScriptCommand(&restart_if_wasted_at, vec.x, vec.y, vec.z, fRotation, 0);
}

// 0.3.7
void CPedSamp::ForceTargetRotation(float fRotation)
{
	if(!m_pPed) return;
	if(!GamePool_Ped_GetAt(m_dwGTAId)) return;

	m_pPed->m_fCurrentRotation = DegToRad(fRotation);
	m_pPed->m_fAimingRotation = DegToRad(fRotation);

	ScriptCommand(&set_actor_z_angle,m_dwGTAId,fRotation);
}

void CPedSamp::SetRotation(float fRotation)
{
	if(!m_pPed) return;
	if(!GamePool_Ped_GetAt(m_dwGTAId)) return;

	m_pPed->m_fCurrentRotation = DegToRad(fRotation);
	m_pPed->m_fAimingRotation = DegToRad(fRotation);
}

// 0.3.7
uint8_t CPedSamp::GetActionTrigger()
{
	return (uint8_t)m_pPed->m_nPedState;
}

void CPedSamp::SetActionTrigger(ePedState action)
{
	m_pPed->m_nPedState = action;
}

void CPedSamp::AttachObject(ATTACHED_OBJECT_INFO* pInfo, int iSlot)
{
    if (!pInfo || iSlot < 0) return;
    if (iSlot >= 64) return;

    // SA-MP attached-object bone ids are already the GTA ped node indexes
    // used by m_apBones (1..18). The previous implementation converted them
    // to ePedBones ids (for example 2 -> BONE_PELVIS/head mappings) and then
    // fed those ids back into the animation hierarchy. During vehicle enter /
    // exit the hierarchy changes and that produced an object jumping onto a
    // random bone or appearing as an extra object on the player. Alyn uses the
    // original SA-MP id directly.
    if (pInfo->dwBoneId_MP == 0 || pInfo->dwBoneId_MP >= TOTAL_PED_NODES) return;

    if (GetAttachedObject(iSlot))
    {
        DeattachObject(iSlot);
    }

    auto& attach = m_aAttachedObject[iSlot] = CAttachedPlayerObject();

    attach.vecRotation  = pInfo->vecRotation;
    attach.vecOffset    = pInfo->vecOffset;
    attach.dwModelId    = pInfo->dwModelId;
    attach.vecScale     = pInfo->vecScale;
    attach.dwColor[0]   = pInfo->dwColor[0];
    attach.dwColor[1]   = pInfo->dwColor[1];
    attach.dwSampBone   = pInfo->dwBoneId_MP;
    attach.dwBone       = pInfo->dwBoneId_MP;

    CVector vecPos = m_pPed->GetPosition();
    CVector vecRot{ 0.0f, 0.0f, 0.0f };
    attach.pObject = new CObjectSamp(pInfo->dwModelId, vecPos.x, vecPos.y, vecPos.z, vecRot, 200.0f);
    if (!attach.pObject || !attach.pObject->IsValid()) {
        delete attach.pObject;
        attach.pObject = nullptr;
        m_aAttachedObject.erase(iSlot);
        return;
    }

    if (attach.pObject->m_pEntity)
        attach.pObject->m_pEntity->m_bUsesCollision = false;
}

void CPedSamp::SetAttachOffset(int iSlot, CVector pos, CVector rot)
{
    auto attach = GetAttachedObject(iSlot);
    if(attach) {
        attach->vecOffset = pos;
        attach->vecRotation = rot;
    }
}

void CPedSamp::DeattachObject(int iSlot)
{
    auto attach = GetAttachedObject(iSlot);
    if(attach)
	{
		delete attach->pObject;
        m_aAttachedObject.erase(iSlot);
	}

}

bool CPedSamp::IsValidAttach(int iSlot)
{
    auto attach = GetAttachedObject(iSlot);

    return attach != nullptr;
}
void CPedSamp::FlushAttach()
{
    std::vector<uint32> keysToDelete;

    for (auto& element : m_aAttachedObject) {
        keysToDelete.push_back(element.first);
    }

	for (auto& id : keysToDelete)
	{
		DeattachObject(id);
	}
}

void CPedSamp::ProcessAttach()
{
    if (!m_pPed) return;

    const bool isAdded = m_pPed->IsAdded();
    const bool isLocal = GamePool_FindPlayerPed() == m_pPed;
    const bool needRender = !(isLocal && CCrossHair::m_UsedCrossHair);
    const bool firstPerson = isLocal && CFirstPersonCamera::IsEnabled();
    bool animUpdated = false;
    RpHAnimHierarchy* hierarchy = nullptr;

    const auto finiteVector = [](const CVector& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };

    for (auto& entry : m_aAttachedObject) {
        auto& attach = entry.second;
        CObjectSamp* object = attach.pObject;
        if (!object) continue;
        object->ProcessStreaming();
        auto* entity = object->m_pEntity;
        if (!entity) continue;

        // Do not move a recycled pool entry belonging to another model.
        if (object->m_iObjectModelId > 0 && entity->m_nModelIndex !=
            static_cast<uint16_t>(object->m_iObjectModelId)) continue;

        if (entity->IsAdded()) entity->Remove();
        if (!isAdded || !m_pPed->m_pRwClump || !needRender ||
            (firstPerson && attach.dwSampBone == 2)) continue;

        if (!animUpdated) {
            // Alyn's UpdateElementRpHAnim: native hierarchy update once for
            // this ped, then the CJ shoulder correction, also while entering
            // or leaving a vehicle. Do not use the partial local implementation.
            CHook::CallFunction<void>("_ZN7CEntity13UpdateRpHAnimEv", m_pPed);
            if (m_pPed->m_nModelIndex == 0 && m_pPed->m_pRwClump)
                CHook::CallFunction<void>("_ZN4CPed20ShoulderBoneRotationEP7RpClump", m_pPed->m_pRwClump);
            hierarchy = GetAnimHierarchyFromSkinClump(m_pPed->m_pRwClump);
            animUpdated = true;
        }
        if (!hierarchy || !hierarchy->pMatrixArray || !hierarchy->pNodeInfo ||
            hierarchy->numNodes <= 0 || attach.dwSampBone == 0 ||
            attach.dwSampBone >= TOTAL_PED_NODES) continue;

        AnimBlendFrameData* bone = m_pPed->m_apBones[attach.dwSampBone];
        if (!bone) continue;
        const int index = RpHAnimIDGetIndex(hierarchy, bone->m_nNodeId);
        if (index < 0 || index >= hierarchy->numNodes) continue;

        CMatrix matrix;
        memcpy(&matrix, &hierarchy->pMatrixArray[index], sizeof(RwMatrix));
        if (!finiteVector(matrix.m_right) || !finiteVector(matrix.m_forward) ||
            !finiteVector(matrix.m_up) || !finiteVector(matrix.m_pos) ||
            !finiteVector(attach.vecOffset) || !finiteVector(attach.vecRotation) ||
            !finiteVector(attach.vecScale)) continue;

        // Alyn order: bone-local offset, X/Y/Z pre-rotation, then local scale.
        matrix.m_pos = matrix.TransformPoint(attach.vecOffset);
        matrix.RotateFromDegrees(attach.vecRotation);
        matrix.Scale(attach.vecScale);
        // Match Alyn's RwMatrixScale: a scaled matrix is no longer guaranteed
        // to be an identity or orthonormal matrix to RenderWare.
        matrix.flags &= 0xFFFDFFFCu;
        if (!finiteVector(matrix.m_right) || !finiteVector(matrix.m_forward) ||
            !finiteVector(matrix.m_up) || !finiteVector(matrix.m_pos)) continue;

        entity->SetMatrix(matrix);
        entity->UpdateRW();
        entity->UpdateRwFrame();
        entity->Add();
    }

    if (isAdded)
        ProcessHeadMatrix();
}

void CPedSamp::ProcessHeadMatrix()
{
	if (!m_pPed || !m_pPed->m_pRwClump) return;
	auto hierarchy = GetAnimHierarchyFromSkinClump(m_pPed->m_pRwClump);

	if(!hierarchy || !hierarchy->pMatrixArray || !hierarchy->pNodeInfo ||
       hierarchy->numNodes <= 0)
		return;

	uint32_t bone = 4;
	int iID = RpHAnimIDGetIndex(hierarchy, bone);

	if (iID < 0 || iID >= hierarchy->numNodes)
	{
		return;
	}

	memcpy(&m_HeadBoneMatrix, &hierarchy->pMatrixArray[iID], sizeof(RwMatrix));
}

bool CPedSamp::IsPlayingAnim(int idx)
{
	
	if (!m_pPed || !m_dwGTAId || (idx == 0) )
	{
		return 0;
	}
	if (!GamePool_Ped_GetAt(m_dwGTAId))
	{
		return 0;
	}
	if (!m_pPed->m_pRwObject)
	{
		return 0;
	}

	const char* pAnim = GetAnimByIdx(idx - 1);
	if (!pAnim)
	{
		return false;
	}
	const char* pNameAnim = strchr(pAnim, ':') + 1;

	uintptr_t blendAssoc = ((uintptr_t(*)(RwObject* clump, const char* szName))(g_libGTASA + (VER_x32 ? 0x00390A24 + 1 : 0x46AAF4)))(m_pPed->m_pRwObject, pNameAnim);	// RpAnimBlendClumpGetAssociation

	if (blendAssoc)
	{
		return true;
	}
	else
	{
		return false;
	}
}

void CPedSamp::PlayAnimByIdx(int idx, float BlendData, bool loop, bool freeze, uint8_t time)
{
	
	if (!idx)
	{
		RwMatrix mat;
		m_pPed->GetMatrix(&mat);
		m_pPed->Teleport(CVector(mat.pos), false);
		return;
	}
	std::string szAnim;
	std::string szBlock;

	char pszAnim[40];
	char pszBlock[40];

	memset(&pszAnim[0], 0, 40);
	memset(&pszBlock[0], 0, 40);

	bool bTest = false;
	const char* pBegin = GetAnimByIdx(idx - 1);
	if (!pBegin)
	{
		return;
	}
	while (*pBegin)
	{
		if (*pBegin == ':')
		{
			pBegin++;
			bTest = true;
			continue;
		}
		if (!bTest)
		{
			szBlock += *pBegin;
		}
		if (bTest)
		{
			szAnim += *pBegin;
		}
		pBegin++;
	}

	strcpy(&pszAnim[0], szAnim.c_str());
	strcpy(&pszBlock[0], szBlock.c_str());
	ApplyAnimation(&pszAnim[0], &pszBlock[0], BlendData, loop, 1, 1, freeze, time);
}

bool IsBlendAssocGroupLoaded(int iGroup)
{
	auto pBlendAssocGroup = CAnimManager::ms_aAnimAssocGroups[iGroup].m_pAnimBlock;

	return pBlendAssocGroup != nullptr;
}

void CPedSamp::SetMoveAnim(int iAnimGroup)
{
	if (iAnimGroup == ANIM_GROUP_DEFAULT || iAnimGroup == 255)
	{
		auto pModel = CModelInfo::GetModelInfo(m_pPed->m_nModelIndex)->AsPedModelInfoPtr();
		iAnimGroup = pModel->m_nAnimType;
	}
	const char* strBlockName = nullptr;
	switch (iAnimGroup) {
		case 55:
		case 58:
		case 61:
		case 64:
		case 67:
			strBlockName = "fat";
			break;

		case 56:
		case 59:
		case 62:
		case 65:
		case 68:
			strBlockName = "muscular";
			break;
		case 138:
			strBlockName = "skate";
			break;

		default:
			strBlockName = "ped";
			break;
	}

	if (!m_dwGTAId || !m_pPed) return;

	if (!IsBlendAssocGroupLoaded(iAnimGroup))
	{
		Log("Animgrp %d not loaded", iAnimGroup);
		auto pAnimBlock = CAnimManager::GetAnimationBlock(strBlockName);

		if (!pAnimBlock) return;

		if (!pAnimBlock->bLoaded)
		{
			auto idx = CAnimManager::GetAnimationBlockIndex(pAnimBlock);

			auto modelId = IFPToModelId(idx);

			if (!CStreaming::TryLoadModel(modelId))
				return;
		}
		if (!IsBlendAssocGroupLoaded(iAnimGroup))
		{
			Log("not loaded %d", iAnimGroup);
			return;
		}
		Log("animgrp %d loaded", iAnimGroup);
	}

	m_pPed->m_nAnimGroup = static_cast<AssocGroupId>(iAnimGroup);

	auto pPlayerPed = (CPlayerPed*)m_pPed;
	pPlayerPed->ReApplyMoveAnims();
}



// 0.3.7
bool CPedSamp::IsDead() const
{
	if(!m_pPed) return false;

	return m_pPed->m_fHealth <= 0.0f || m_pPed->m_nPedState == PEDSTATE_DIE || m_pPed->m_nPedState == PEDSTATE_DEAD;
}

// 0.3.7
void CPedSamp::ShowMarker(uint32_t iMarkerColorID)
{
	if(m_dwArrow) HideMarker();
	ScriptCommand(&create_arrow_above_actor, m_dwGTAId, &m_dwArrow);
	ScriptCommand(&set_marker_color, m_dwArrow, iMarkerColorID);
	ScriptCommand(&show_on_radar2, m_dwArrow, 2);
}

// 0.3.7
void CPedSamp::HideMarker()
{
	if(m_dwArrow) ScriptCommand(&disable_marker, m_dwArrow);
	m_dwArrow = 0;
}

void CPedSamp::SetFightingStyle(int iStyle)
{
	
	if(!m_pPed || !m_dwGTAId) return;
	if (!GamePool_Ped_GetAt(m_dwGTAId))
	{
		return;
	}
	//CChatWindow::AddMessage("set fighting style %d", iStyle);
	ScriptCommand( &set_fighting_style, m_dwGTAId, iStyle, 6 );
}

// 0.3.7

void CPedSamp::ApplyAnimation(char* szAnimName, char* szAnimFile, float fDelta, bool bLoop, bool bLockX, bool bLockY, bool bFreeze, int uiTime)
{

    if (!m_pPed) return;
    if (!GamePool_Ped_GetAt(m_dwGTAId)) return;

	if(!CGame::IsAnimationLoaded(szAnimFile)) {
		CGame::RequestAnimation(szAnimFile);

		ScriptCommand(&apply_animation, m_dwGTAId, szAnimName, szAnimFile, fDelta, bLoop, bLockX, bLockY, bFreeze, uiTime);
	//	usleep(100000);
	}

//	animFlagTime = (uint8_t)uiTime;
//	animFlagFreeze = bFreeze;
//	animFlagLoop = bLoop;

	ScriptCommand(&apply_animation, m_dwGTAId, szAnimName, szAnimFile, fDelta, bLoop, bLockX, bLockY, bFreeze, uiTime);
}

uint8_t CPedSamp::GetExtendedKeys()
{
	uint8_t result = 0;
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_YES])
	{
		LocalPlayerKeys.bKeys[ePadKeys::KEY_YES] = false;
		result = 1;
	}
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_NO])
	{
		LocalPlayerKeys.bKeys[ePadKeys::KEY_NO] = false;
		result = 2;
	}
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_CTRL_BACK])
	{
		LocalPlayerKeys.bKeys[ePadKeys::KEY_CTRL_BACK] = false;
		result = 3;
	}

	return result;
}

uint16_t CPedSamp::GetKeys(uint16_t *lrAnalog, uint16_t *udAnalog)
{
	*lrAnalog = LocalPlayerKeys.wKeyLR;
	*udAnalog = LocalPlayerKeys.wKeyUD;
	uint16_t wRet = 0;

	// KEY_ANALOG_RIGHT
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_ANALOG_RIGHT]) wRet |= 1;
	wRet <<= 1;
	// KEY_ANALOG_LEFT
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_ANALOG_LEFT]) wRet |= 1;
	wRet <<= 1;
	// KEY_ANALOG_DOWN
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_ANALOG_DOWN]) wRet |= 1;
	wRet <<= 1;
	// KEY_ANALOG_UP
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_ANALOG_UP]) wRet |= 1;
	wRet <<= 1;
	// KEY_WALK
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_WALK]) wRet |= 1;
	wRet <<= 1;
	// KEY_SUBMISSION
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_SUBMISSION]) wRet |= 1;
	wRet <<= 1;
	// KEY_LOOK_LEFT
	if (LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_LEFT]) wRet |= 1;
	wRet <<= 1;

	// On foot the aim key follows the weapon camera (0x35 = aim weapon).  In a
	// vehicle KEY_HANDBRAKE is the handbrake recorded by the CPad::GetHandBrake
	// hook: overwriting it here meant other players never saw the handbrake.
	if (!m_pPed || !m_pPed->IsInVehicle())
	{
		LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE] = (GetCameraMode() == 0x35);
	}

	// The jetpack thrust is sent as KEY_SPRINT, the key CPad::GetAccelerate
	// answers with on the other clients.
	if (m_bJetpackAscendButton && IsInJetpack())
	{
		LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT] = true;
	}

	// KEY_HANDBRAKE
	if(LocalPlayerKeys.bKeys[ePadKeys::KEY_HANDBRAKE]/*true*/) wRet |= 1;
	wRet <<= 1;
	// KEY_LOOK_RIGHT
	if(LocalPlayerKeys.bKeys[ePadKeys::KEY_LOOK_RIGHT]) wRet |= 1;
	wRet <<= 1;
	// KEY_JUMP
	if(LocalPlayerKeys.bKeys[ePadKeys::KEY_JUMP]) wRet |= 1;
	wRet <<= 1;
	// KEY_SECONDARY_ATTACK
	if(LocalPlayerKeys.bKeys[ePadKeys::KEY_SECONDARY_ATTACK]) wRet |= 1;
	wRet <<= 1;
	// KEY_SPRINT
	if(LocalPlayerKeys.bKeys[ePadKeys::KEY_SPRINT]) wRet |= 1;
	wRet <<= 1;
	// KEY_FIRE
	if(LocalPlayerKeys.bKeys[ePadKeys::KEY_FIRE]) wRet |= 1;
	wRet <<= 1;
	// KEY_CROUCH
	if(LocalPlayerKeys.bKeys[ePadKeys::KEY_CROUCH]) wRet |= 1;
	wRet <<= 1;
	// KEY_ACTION
	if(LocalPlayerKeys.bKeys[ePadKeys::KEY_ACTION]) wRet |= 1;

	memset(LocalPlayerKeys.bKeys, 0, ePadKeys::SIZE);

	return wRet;
}

CWeapon * CPedSamp::GetCurrentWeaponSlot()
{
	if (m_pPed) 
	{
		return &m_pPed->m_aWeapons[m_pPed->m_nActiveWeaponSlot];
	}
	return NULL;
}


void CPedSamp::SetKeys(uint16_t wKeys, uint16_t lrAnalog, uint16_t udAnalog)
{
	PAD_KEYS *pad = &RemotePlayerKeys[m_bytePlayerNumber];

	// LEFT/RIGHT
	pad->wKeyLR = lrAnalog;
	// UP/DOWN
	pad->wKeyUD = udAnalog;

	// KEY_ACTION
	pad->bKeys[ePadKeys::KEY_ACTION] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_CROUCH
	pad->bKeys[ePadKeys::KEY_CROUCH] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_FIRE
	pad->bKeys[ePadKeys::KEY_FIRE] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_SPRINT
	pad->bKeys[ePadKeys::KEY_SPRINT] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_SECONDARY_ATTACK
	pad->bKeys[ePadKeys::KEY_SECONDARY_ATTACK] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_JUMP
	pad->bKeys[ePadKeys::KEY_JUMP] = (wKeys & 1);
	if(!pad->bKeys[ePadKeys::KEY_JUMP]) pad->bIgnoreJump = false;
	wKeys >>= 1;
	// KEY_LOOK_RIGHT
	pad->bKeys[ePadKeys::KEY_LOOK_RIGHT] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_HANDBRAKE
	pad->bKeys[ePadKeys::KEY_HANDBRAKE] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_LOOK_LEFT
	pad->bKeys[ePadKeys::KEY_LOOK_LEFT] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_SUBMISSION
	pad->bKeys[ePadKeys::KEY_SUBMISSION] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_WALK
	pad->bKeys[ePadKeys::KEY_WALK] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_ANALOG_UP
	pad->bKeys[ePadKeys::KEY_ANALOG_UP] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_ANALOG_DOWN
	pad->bKeys[ePadKeys::KEY_ANALOG_DOWN] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_ANALOG_LEFT
	pad->bKeys[ePadKeys::KEY_ANALOG_LEFT] = (wKeys & 1);
	wKeys >>= 1;
	// KEY_ANALOG_RIGHT
	pad->bKeys[ePadKeys::KEY_ANALOG_RIGHT] = (wKeys & 1);

	return;
}

void CPedSamp::SetAimZ(float fAimZ)
{
	if (!m_pPed)
	{
		return;
	}
    m_pPed->m_pPlayerData->m_fLookPitch = fAimZ;
}

float CPedSamp::GetAimZ()
{
	if (!m_pPed)
	{
		return 0.0f;
	}
	return m_pPed->m_pPlayerData->m_fLookPitch;
}



void CPedSamp::StartJetpack()
{
    if (!m_pPed || !m_pPed->m_pIntelligence || m_pPed->IsInVehicle() || m_dwGTAId == 0)
        return;

    if (!GamePool_Ped_GetAt(m_dwGTAId))
        return;

    // Jetpack model (370) must be resident before the native opcode creates
    // CTaskSimpleJetPack on GTA SA 2.1 x64.
    if (!CStreaming::IsModelLoaded(370))
    {
        CStreaming::RequestModel(370, STREAMING_GAME_REQUIRED | STREAMING_PRIORITY_REQUEST);
        CStreaming::LoadRequestedModels();
    }

    ScriptCommand(&actor_task_jetpack, m_dwGTAId);
    m_dwJetpackRequestTick = GetTickCount();
}

void CPedSamp::StopJetpack()
{
    if (!m_pPed || !m_pPed->m_pIntelligence || m_pPed->IsInVehicle() || m_dwGTAId == 0)
        return;

    if (!GamePool_Ped_GetAt(m_dwGTAId))
        return;

    auto* task = m_pPed->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[TASK_PRIMARY_PRIMARY];
    if (!task)
        return;

    const uintptr_t jetpackVtable = g_libGTASA + (VER_x32 ? 0x66CC28 : 0x83A6C0);
    if (*reinterpret_cast<uintptr_t*>(task) != jetpackVtable)
        return;

    CHook::CallFunction<void>("_ZN18CTaskSimpleJetPackD0Ev", task);
    m_pPed->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[TASK_PRIMARY_PRIMARY] = nullptr;
}

bool CPedSamp::IsInJetpack() const
{
    if (!m_pPed || !m_pPed->m_pIntelligence || m_pPed->IsInVehicle())
        return false;

    auto* task = m_pPed->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[TASK_PRIMARY_PRIMARY];
    if (!task)
        return false;

    const uintptr_t jetpackVtable = g_libGTASA + (VER_x32 ? 0x66CC28 : 0x83A6C0);
    return *reinterpret_cast<const uintptr_t*>(task) == jetpackVtable;
}

void CPedSamp::ProcessSpecialAction(BYTE byteSpecialAction, bool bApplyJetpack) {

	if (bApplyJetpack) {
		if (byteSpecialAction == SPECIAL_ACTION_USEJETPACK) {
			if (!IsInJetpack()) StartJetpack();
		} else if (IsInJetpack()) {
			StopJetpack();
		}
	}

	if (byteSpecialAction == SPECIAL_ACTION_CARRY && !IsAnimationPlaying("CRRY_PRTIAL"))
	{
		ApplyAnimation("CRRY_PRTIAL", "CARRY", 4.1, 0, 0, 0, 1, 1);
		IsActionCarry = true;
	}
	if(IsActionCarry && byteSpecialAction != SPECIAL_ACTION_CARRY)
	{
		ApplyAnimation("crry_prtial", "CARRY", 4.00, false, false, false, false, 1);
		IsActionCarry = false;
	}

    // pissing:start
    if(!m_iPissingState && byteSpecialAction == SPECIAL_ACTION_PISSING) {
        ApplyAnimation("PISS_LOOP", "PAULNMAC", 4.0f, 1, true, true, true, -1);

        ScriptCommand(&attach_particle_to_actor2,"PETROLCAN",m_dwGTAId,
                      0.0f, 0.58f, -0.08f, 0.0f, 0.01f, 0.0f, 1, &m_dwPissParticlesHandle);

        ScriptCommand(&make_particle_visible,m_dwPissParticlesHandle);

        m_iPissingState = true;
    }
	if(!m_pPed->bUsingMobilePhone && byteSpecialAction == SPECIAL_ACTION_USECELLPHONE) {
		m_pPed->bUsingMobilePhone = true;
		ScriptCommand(&TASK_USE_MOBILE_PHONE, m_dwGTAId, true);
	}
	if(m_pPed->bUsingMobilePhone && byteSpecialAction == SPECIAL_ACTION_STOPUSECELLPHONE) {
		m_pPed->bUsingMobilePhone = false;
		ScriptCommand(&TASK_USE_MOBILE_PHONE, m_dwGTAId, false);
	}

    // pissing:stop
    if(m_iPissingState && byteSpecialAction != SPECIAL_ACTION_PISSING) {
        if(m_dwPissParticlesHandle) {
            ScriptCommand(&destroy_particle,m_dwPissParticlesHandle);
            m_dwPissParticlesHandle = 0;
        }
        ClearAnimations();
        m_iPissingState = false;
    }
}
