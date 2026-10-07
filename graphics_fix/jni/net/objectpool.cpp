#include "../main.h"
#include "../game/game.h"
#include "netgame.h"

namespace {
uint32_t g_objectMaterialCooldownUntil = 0;
}
  
void CObjectPool::SetSpawnMaterialCooldown(uint32_t milliseconds)
{
	const uint32_t now = GetTickCount();
	g_objectMaterialCooldownUntil = now + milliseconds;
}

bool CObjectPool::IsSpawnMaterialCooldownActive()
{
	const uint32_t now = GetTickCount();
	return static_cast<int32_t>(g_objectMaterialCooldownUntil - now) > 0;
}

void CObjectPool::Free()
{
    auto ids = GetAllIds();
    for (auto& id : ids) {
        Delete(id);
    }
}
 
bool CObjectPool::Delete(uint16_t objectId)
{
	if(!GetAt(objectId))
		return false;

	delete list[objectId];
	list.erase(objectId);

	return true;
}

bool CObjectPool::New(uint16_t objectId, int iModel, CVector vecPos, CVector vecRot, float fDrawDistance)
{
	if(GetAt(objectId))
		Delete(objectId);

	CObjectSamp* pObject = CGame::NewObject(iModel, vecPos.x, vecPos.y, vecPos.z, vecRot, fDrawDistance);

	// Never store a null entry: Process() and ProcessMaterialText() walk this
	// map every frame and would dereference it.
	if (!pObject)
		return false;

	list[objectId] = pObject;
	return true;
}

CObjectSamp *CObjectPool::GetObjectFromGtaPtr(CEntity *pGtaObject)
{
	for(auto &pair : list) {
		auto pObject = pair.second;
		if(!pObject) continue;

		if(pObject->m_pEntity == pGtaObject)
		{
			return pObject;
		}
	}
	return nullptr;
}

uint16_t CObjectPool::FindIDFromGtaPtr(CEntity* pGtaObject)
{
	for(auto &pair : list) {
		auto pObject = pair.second;
		if(!pObject) continue;

		if(pObject->m_pEntity == pGtaObject)
		{
			return pair.first;
		}
	}

	return INVALID_OBJECT_ID;
}

void CObjectPool::Process()
{
	static unsigned long s_ulongLastCall = 0;
	if (!s_ulongLastCall) s_ulongLastCall = GetTickCount();
	unsigned long ulongTick = GetTickCount();
	float fElapsedTime = ((float)(ulongTick - s_ulongLastCall)) / 1000.0f;
	// Get elapsed time in seconds

	for(auto &pair : list) {
		auto pObject = pair.second;
		if(!pObject) continue;

		pObject->Process(fElapsedTime);

		// The native streamer may recreate the GTA entity/RW object during the
		// same tick. Re-assert persistence here so it cannot be selected for
		// aggressive RW eviction before the next Process() call.
		if (pObject->m_pEntity) {
			pObject->m_pEntity->m_bStreamingDontDelete = true;
		}
	}

	s_ulongLastCall = ulongTick;

	// Material textures are resolved in ProcessMaterialText() (render phase),
	// never from an RPC callback and never by loading the material's DFF.
}

void CObjectPool::ProcessMaterialText()
{
	// Alyn: every frame, right after CObjectPool::Process() in CGame::Process.
	CObjectSamp::BeginMaterialTextTick();
	for (auto &pair : list) {
		if (pair.second) pair.second->ProcessMaterialText();
	}
}
