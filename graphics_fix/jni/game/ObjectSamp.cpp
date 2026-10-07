#include "../main.h"
#include "game.h"
#include "../net/netgame.h"
#include "util/patch.h"
#include "Timer.h"   
#include "game/Models/ModelInfo.h"
#include "game/RW/rpworld.h"
#include "game/Streaming.h"
#include "game/TxdStore.h"
#include "game/sprite2d.h"
#include "../gui/gui.h"
#include "util/util.h"
#include "Scene.h" 
#include "VisibilityPlugins.h" 
#include "app/app.h"
 
// ============================================================================
//  DIAGNOSTIK & KILL-SWITCH object material (v5)
//  --------------------------------------------------------------------------
//  OBJ_MAT_ENABLE : set 0 untuk MEMATIKAN TOTAL fitur object material/text.
//     Kalau dengan 0 masih FC saat spawn dekat object bermaterial, berarti
//     FC-nya BUKAN dari kode material ini - lapor balik, dicari di tempat lain.
//     Kalau dengan 0 TIDAK FC, berarti benar di material - lanjut pakai trace.
//
//  OBJ_MAT_TRACE  : set 1 supaya tiap tahap menulis baris "MAT:" ke log AXL.
//     Baris "MAT:" TERAKHIR sebelum "libsigchain ... signal 11" di log =
//     fungsi tempat FC terjadi. Kirim ~20 baris terakhir sebelum crash.
//     Set 0 kalau sudah selesai debugging (menghilangkan spam log).
// ============================================================================
#ifndef OBJ_MAT_ENABLE
#define OBJ_MAT_ENABLE 1
#endif
#ifndef OBJ_MAT_TRACE
#define OBJ_MAT_TRACE  0
#endif
#if OBJ_MAT_TRACE
#define MATLOG(...) Log(__VA_ARGS__)
#else
#define MATLOG(...) ((void)0)
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <strings.h>
#include "game/Textures/TextureDatabaseRuntime.h"

extern CGUI* pGUI;
void ImGui_ImplRenderWare_NewFrame();
void ImGui_ImplRenderWare_RenderDrawData(ImDrawData* draw_data);

float fixAngle(float angle)
{
	if (angle > 180.0f) angle -= 360.0f;
	if (angle < -180.0f) angle += 360.0f;
	return angle;
}

float subAngle(float a1, float a2)
{
	return fixAngle(fixAngle(a2) - a1);
}

CObjectSamp::CObjectSamp(int iModel, float fPosX, float fPosY, float fPosZ, CVector vecRot, float fDrawDistance)
{
    // No reserve: only objects with materials push backups (12.8 KB saved per object).
	if(!CModelInfo::GetModelInfo(iModel))
		iModel = 18631; // ��������

	m_pEntity 			= 0;
	m_dwGTAId 			= 0;

	// Remember the network object's model independently of the live GTA object
	// pointer. The GTA streamer may recreate the RW instance later.
	m_iObjectModelId = iModel;

	ScriptCommand(&create_object, iModel, fPosX, fPosY, fPosZ, &m_dwGTAId);
	ScriptCommand(&put_object_at, m_dwGTAId, fPosX, fPosY, fPosZ);

	m_pEntity = GamePool_Object_GetAt(m_dwGTAId);

	m_bIsPlayerSurfing = false;
	m_bNeedRotate = false;

	m_bAttachedType = eObjectAttachType::NONE;
	m_usAttachedVehicle = 0xFFFF;

    if (m_pEntity) {
        // Protect the RW instance from GTA's streaming eviction right away,
        // not only from the first Process() tick.
        m_pEntity->m_bStreamingDontDelete = true;

        auto it = std::find(objectToIdMap.begin(), objectToIdMap.end(), m_pEntity);
        if (it == objectToIdMap.end()) {
            objectToIdMap.push_back(m_pEntity);
        }
    }
	InstantRotate(vecRot.x, vecRot.y, vecRot.z);

	// create_object does not load the model. If the DFF is not resident the
	// entity exists without an RW instance; queue the model now so it shows
	// up as soon as the streamer delivers it (see ProcessStreaming()).
	ProcessStreaming();
}

CObjectSamp::~CObjectSamp()
{
    // Restores the shared geometry and releases every texture / streaming ref.
    // ClearAllMaterials() also releases any model temporarily held for a
    // deferred material-texture request.
    ClearAllMaterials();

    // The GTA object can be recreated by streaming while this wrapper stays
    // alive. Always reacquire the live pool entity before destruction so an old
    // pointer can never make us skip destroy_object and leave an orphaned
    // attachment behind. Orphaned handles are especially visible after the
    // ped's vehicle enter/exit animation changes the hierarchy.
    if (m_dwGTAId) {
        if (CPhysical* liveEntity = GamePool_Object_GetAt(m_dwGTAId))
            m_pEntity = liveEntity;
    }

    if (m_pEntity) {
        if (m_pEntity->IsAdded())
            m_pEntity->Remove();

        ScriptCommand(&destroy_object, m_dwGTAId);
        auto it = std::find(objectToIdMap.begin(), objectToIdMap.end(), m_pEntity);
        if (it != objectToIdMap.end()) objectToIdMap.erase(it);
    }

    // Release our own streaming request only. The old code force-removed the
    // model (RemoveModelIfNoRefs + ClearAllFlags) on every destroy, so an
    // object re-created by the streamer plugin a moment later had to wait for
    // its DFF again (pop-in / flicker), and flags set by other systems for the
    // same model were wiped. GTA's LRU frees unreferenced models on its own.
    if (m_bModelPinned) {
        const int32 modelId = m_iObjectModelId;
        if (modelId > 0 && modelId < CModelInfo::NUM_MODEL_INFOS) {
            CStreaming::SetModelIsDeletable(modelId);
        }
        m_bModelPinned = false;
    }
}

void CObjectSamp::ProcessStreaming()
{
	CEntity* previousEntity = m_pEntity;
	m_pEntity = GamePool_Object_GetAt(m_dwGTAId);
	if (m_pEntity != previousEntity) {
		std::lock_guard<std::recursive_mutex> materialLock(ms_MaterialMutex);
		m_MaterialBackups.clear();
		SyncMaterialRegistration();
	}
	if (!m_pEntity) return;

	// Network objects are persistent from SA-MP's point of view. Protect the
	// entity from GTA's RW-object eviction.
	m_pEntity->m_bStreamingDontDelete = true;

	const int32 modelId = m_iObjectModelId;
	if (modelId <= 0 || modelId >= CModelInfo::NUM_MODEL_INFOS) return;

	if (!CStreaming::IsModelLoaded(modelId)) {
		if (!m_pEntity->m_pRwObject) {
			// Only enqueue. Never LoadAllRequestedModels() here (spawn burst).
			// GAME_REQUIRED only while waiting; the old KEEP_IN_MEMORY flag was
			// never cleared and slowly filled streaming memory until new
			// objects could not load at all.
			if (!m_bModelPinned &&
			    !(CStreaming::GetInfo(modelId).m_nFlags & STREAMING_GAME_REQUIRED)) {
				m_bModelPinned = true;
			}
			CStreaming::RequestModel(modelId, STREAMING_GAME_REQUIRED);
		}
		return;
	}

	// Recreate the RW object if it is missing while the DFF is resident.
	if (!m_pEntity->m_pRwObject && m_pEntity->m_bIsVisible) {
		m_pEntity->CreateRwObject();
		if (m_pEntity->m_pRwObject) {
			m_pEntity->UpdateRW();
			m_pEntity->UpdateRwFrame();
		}
	}

	// Once the RW instance exists it holds a model reference itself, so our
	// GAME_REQUIRED request is no longer needed and must not pin the model.
	if (m_bModelPinned && m_pEntity->m_pRwObject) {
		CStreaming::SetModelIsDeletable(modelId);
		m_bModelPinned = false;
	}
}

void CObjectSamp::Process(float fElapsedTime)
{
	if (m_bAttachedType == eObjectAttachType::TO_VEHICLE)
	{
		CVehicleMP* pVehicle = CVehiclePool::GetAt(m_usAttachedVehicle);
		if (pVehicle && pVehicle->m_pVehicle)
		{
			if (pVehicle->m_pVehicle->IsAdded())
			{
				ProcessAttachToVehicle(pVehicle);
			}
		}
	}

	// Entity pointer, RW instance and model residency. Never calls
	// LoadAllRequestedModels() (spawn burst safety).
	ProcessStreaming();

	// Streaming can hand back a different CObject for the same GTA id, so keep
	// the material lookup table pointing at the entity that is actually live.
	if (m_bHasCustomObjectMaterial || m_pMaterialRegKey) SyncMaterialRegistration();

	if (!m_pEntity) return;
	if (!(m_pEntity->m_matrix)) return;
	if (m_bIsMoving)
	{
		CVector vecSpeed = { 0.0f, 0.0f, 0.0f };
		RwMatrix matEnt;
		matEnt = m_pEntity->GetMatrix().ToRwMatrix();
		float distance = fElapsedTime * m_fMoveSpeed;
		float remaining = DistanceRemaining(&matEnt);
		uint32_t dwThisTick = GetTickCount();

		float posX = matEnt.pos.x;
		float posY = matEnt.pos.y;
		float posZ = matEnt.pos.z;

		float f1 = ((float)(dwThisTick - m_iStartMoveTick)) * 0.001f * m_fMoveSpeed;
		float f2 = m_fDistanceToTargetPoint - remaining;

		if (distance >= remaining)
		{
			m_pEntity->SetVelocity(vecSpeed);
			m_pEntity->SetTurnSpeed(vecSpeed);

			matEnt.pos = m_matTarget.pos;

			if (m_bNeedRotate) {
				m_quatTarget.GetMatrix(&matEnt);
			}

            m_pEntity->Remove();

			m_pEntity->SetMatrix((CMatrix&)matEnt);
			m_pEntity->UpdateRW();
			m_pEntity->UpdateRwFrame();

            m_pEntity->Add();

			StopMoving();
			return;
		}

		if (fElapsedTime <= 0.0f)
			return;

		float delta = 1.0f / (remaining / distance);
		matEnt.pos.x += ((m_matTarget.pos.x - matEnt.pos.x) * delta);
		matEnt.pos.y += ((m_matTarget.pos.y - matEnt.pos.y) * delta);
		matEnt.pos.z += ((m_matTarget.pos.z - matEnt.pos.z) * delta);

		distance = remaining / m_fDistanceToTargetPoint;
		float slerpDelta = 1.0f - distance;

		delta = 1.0f / fElapsedTime;
		vecSpeed.x = (matEnt.pos.x - posX) * delta * 0.02f;
		vecSpeed.y = (matEnt.pos.y - posY) * delta * 0.02f;
		vecSpeed.z = (matEnt.pos.z - posZ) * delta * 0.02f;

		if (FloatOffset(f1, f2) > 0.1f)
		{
			if (f1 > f2)
			{
				delta = (f1 - f2) * 0.1f + 1.0f;
				vecSpeed *= delta;
			}

			if (f2 > f1)
			{
				delta = 1.0f - (f2 - f1) * 0.1f;
				vecSpeed *= delta;
			}
		}

		m_pEntity->SetVelocity(vecSpeed);
		m_pEntity->ApplyMoveSpeed();

		if (m_bNeedRotate)
		{
			float fx, fy, fz;
			GetRotation(&fx, &fy, &fz);
			distance = m_vecRotationTarget.z - distance * m_vecSubRotationTarget.z;
			vecSpeed.x = 0.0f;
			vecSpeed.y = 0.0f;
			vecSpeed.z = subAngle(remaining, distance) * 0.01f;
			if (vecSpeed.z <= 0.001f)
			{
				if (vecSpeed.z < -0.001f)
					vecSpeed.z = -0.001f;
			}
			else
			{
				vecSpeed.z = 0.001f;
			}

			m_pEntity->SetTurnSpeed(vecSpeed);

			m_pEntity->GetMatrix(&matEnt);
			CQuaternion quat;
			quat.Slerp(&m_quatStart, &m_quatTarget, slerpDelta);
			quat.Normalize();
			quat.GetMatrix(&matEnt);
		}
		else
		{
			m_pEntity->GetMatrix(&matEnt);
		}

        m_pEntity->Remove();

		m_pEntity->SetMatrix((CMatrix&)matEnt);
		m_pEntity->UpdateRW();
		m_pEntity->UpdateRwFrame();

        m_pEntity->Add();
	}
}

float CObjectSamp::DistanceRemaining(RwMatrix *matPos)
{

	float	fSX,fSY,fSZ;
	fSX = (matPos->pos.x - m_matTarget.pos.x) * (matPos->pos.x - m_matTarget.pos.x);
	fSY = (matPos->pos.y - m_matTarget.pos.y) * (matPos->pos.y - m_matTarget.pos.y);
	fSZ = (matPos->pos.z - m_matTarget.pos.z) * (matPos->pos.z - m_matTarget.pos.z);
	return (float)sqrt(fSX + fSY + fSZ);
}

void CObjectSamp::SetPos(float x, float y, float z)
{
	if (GamePool_Object_GetAt(m_dwGTAId))
	{
		ScriptCommand(&put_object_at, m_dwGTAId, x, y, z);
	}
}



void CObjectSamp::MoveTo(float fX, float fY, float fZ, float fSpeed, float fRotX, float fRotY, float fRotZ)
{
	m_pEntity = GamePool_Object_GetAt(m_dwGTAId);
	if (!m_pEntity || !m_pEntity->m_matrix) return;

	// A new move replaces the running one from the object's CURRENT
	// transform. The old code first snapped the object to the previous
	// target, so re-issued MoveObject calls (gates reversing, lifts called
	// mid-way) made the object jump before moving again. ScrMoveObject also
	// teleports to the server's current position first, like the PC client.
	if (m_bIsMoving) {
		this->StopMoving();
	}

	m_iStartMoveTick = GetTickCount();
	m_fMoveSpeed = fSpeed;
	m_matTarget.pos = {fX, fY, fZ};

	m_bIsMoving = true;

	if (fRotX <= -999.0f || fRotY <= -999.0f || fRotZ <= -999.0f) {
		m_bNeedRotate = false;
	}
	else
	{
		m_bNeedRotate = true;

		CVector vecRot;
		RwMatrix matrix;
		this->GetRotation(&vecRot.x, &vecRot.y, &vecRot.z);

		m_vecRotationTarget.x = fixAngle(fRotX);
		m_vecRotationTarget.y = fixAngle(fRotY);
		m_vecRotationTarget.z = fixAngle(fRotZ);

		m_vecSubRotationTarget.x = subAngle(vecRot.x, fRotX);
		m_vecSubRotationTarget.y = subAngle(vecRot.y, fRotY);
		m_vecSubRotationTarget.z = subAngle(vecRot.z, fRotZ);

		this->InstantRotate(fRotX, fRotY, fRotZ);
		m_pEntity->GetMatrix(&matrix);

		m_matTarget.right = matrix.right;
		m_matTarget.at = matrix.at;
		m_matTarget.up = matrix.up;

		this->InstantRotate(vecRot.x, vecRot.y, vecRot.z);
		m_pEntity->GetMatrix(&matrix);

		m_quatStart.SetFromMatrix(&matrix);
		m_quatTarget.SetFromMatrix(&m_matTarget);
		m_quatStart.Normalize();
		m_quatTarget.Normalize();
	}

	m_fDistanceToTargetPoint = m_pEntity->GetDistanceFromPoint(m_matTarget.pos.x, m_matTarget.pos.y, m_matTarget.pos.z);

	if (pNetGame) {
		CLocalPlayer::UpdateSurfing();
	}
}

void CObjectSamp::AttachToVehicle(uint16_t usVehID, CVector* pVecOffset, CVector* pVecRot)
{
	m_bAttachedType = eObjectAttachType::TO_VEHICLE;
	m_usAttachedVehicle = usVehID;
	m_vecAttachedOffset.x = pVecOffset->x;
	m_vecAttachedOffset.y = pVecOffset->y;
	m_vecAttachedOffset.z = pVecOffset->z;

	m_vecAttachedRotation.x = pVecRot->x;
	m_vecAttachedRotation.y = pVecRot->y;
	m_vecAttachedRotation.z = pVecRot->z;
}

void CObjectSamp::ProcessAttachToVehicle(CVehicleMP* pVehicle)
{
	if (GamePool_Object_GetAt(m_dwGTAId))
	{
		if (!ScriptCommand(&is_object_attached, m_dwGTAId) || bNeedReAttach)
		{
			ScriptCommand(&attach_object_to_car, m_dwGTAId, pVehicle->m_dwGTAId, m_vecAttachedOffset.x,
				m_vecAttachedOffset.y, m_vecAttachedOffset.z, m_vecAttachedRotation.x, m_vecAttachedRotation.y, m_vecAttachedRotation.z);
		}
	}
}

void CObjectSamp::InstantRotate(float x, float y, float z)
{
	if(!m_pEntity)return;
	x = DegreesToRadians(x);
	y = DegreesToRadians(y);
	z = DegreesToRadians(z);

    m_pEntity->Remove();

	m_pEntity->SetOrientation(x, y, z);

	m_pEntity->UpdateRW();
	m_pEntity->UpdateRwFrame();

    m_pEntity->Add();
}

void CObjectSamp::StopMoving()
{
	m_bIsMoving = false;
	if (!m_pEntity) return;

	CVector vec = { 0.0f, 0.0f, 0.0f };
	this->m_pEntity->ResetMoveSpeed();
	this->m_pEntity->SetTurnSpeed(vec);
}

// ===========================================================================
//  SA-MP object material / material text
//  ---------------------------------------------------------------------------
//  Ported from the Alyn client (Game/Object.cpp, MaterialTextGenerator.cpp,
//  Util.cpp LoadTextureFromTxd). Differences are 64-bit types, references
//  held on shared textures, and the ImGui text drawn in the TXD code page.
// ===========================================================================

// SA-MP/Alyn texture resolution on Android is a two-stage lookup:
// first ask the runtime texture databases, then fall back to the normal GTA
// TXD chain via the '*' dictionary. The server sends a TXD/library name, but
// Alyn's mobile client intentionally resolves the texture by its texture name
// from the currently registered databases. Keep that behaviour while also
// trying the explicitly named TXD as a final fallback.
// Match Alyn's material texture path exactly: switch to the wildcard TXD,
// let CSprite2d/RenderWare perform the normal Android texture lookup, then keep
// one explicit reference for the material slot. This is more reliable than
// manually registering TextureDatabaseRuntime databases from the render path.
static RwTexture* LoadTextureFromTxd(const char* txdName, const char* textureName)
{
    (void)txdName; // SA-MP/Alyn resolves the requested texture through "*".
    if (!textureName || !*textureName) return nullptr;

    const int32_t slot = CTxdStore::FindTxdSlot("*");
    if (slot == -1) return nullptr;

    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(static_cast<uint32_t>(slot));

    // RwTextureRead directly, not CSprite2d::SetTexture: for a name ending in
    // "png" SetTexture (0x6ecce8) reads a PNG FILE (RtPNGImageRead 0x6ecd94)
    // and passes a NULL image on (0x6ecdbc) -> SIGSEGV. A server texture name
    // is never a file. RwTextureRead returns one reference: the slot's.
    RwTexture* texture = RwTextureRead(textureName, nullptr);

    CTxdStore::PopCurrentTxd();
    return texture; // the caller logs a missing texture once (no per-frame log)
}

static RwTexture* GetBlankTexture()
{
    static RwTexture* s_blank = nullptr;
    if (!s_blank) s_blank = LoadTextureFromTxd("*", "blanktex");
    if (s_blank) ++s_blank->refCount;   // reference for the material slot
    return s_blank;
}

static void GetMaterialTextSize(int matSize, int* sizeX, int* sizeY)
{
    static const int sizes[14][2] = {
        {32, 32}, {64, 32}, {64, 64}, {128, 32}, {128, 64}, {128, 128}, {256, 32},
        {256, 64}, {256, 128}, {256, 256}, {512, 64}, {512, 128}, {512, 256}, {512, 512}
    };
    int index = (matSize / 10) - 1;
    if (index < 0) index = 0;
    if (index > 13) index = 13;
    *sizeX = sizes[index][0];
    *sizeY = sizes[index][1];
}

// ---------------------------------------------------------------------------
//  Alyn MaterialTextGenerator: renders the text with ImGui into a camera
//  raster and hands that raster out as a texture.
// ---------------------------------------------------------------------------
namespace {
class MaterialTextGenerator {
public:
    // Alyn creates this helper once during game initialization. Keeping the
    // scene/camera alive avoids creating/destroying RenderWare objects for
    // every SetObjectMaterialText call.
    MaterialTextGenerator() = default;

    RwTexture* Create(int sizeX, int sizeY, uint8_t fontSize, uint32_t fontColor,
                      uint32_t backgroundColor, uint8_t align, const char* text)
    {
        if (!ImGui::GetCurrentContext()) return nullptr;
        if (!pGUI || !pGUI->GetFont()) return nullptr;
        if (sizeX <= 0 || sizeY <= 0 || !text || !*text) return nullptr;
        if (!SetUpScene()) return nullptr;

        // CNetGame::Process() can be reached from different GTA render phases.
        // Never start a nested ImGui frame. Material text will be retried on
        // the next game tick when the normal GUI frame is closed.
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (ctx && ctx->WithinFrameScope) return nullptr;

        RwRaster* raster = RwRasterCreate(
            sizeX, sizeY, 32, rwRASTERFORMAT8888 | rwRASTERTYPECAMERATEXTURE);
        if (!raster) return nullptr;

        RwTexture* texture = RwTextureCreate(raster);
        if (!texture) {
            RwRasterDestroy(raster);
            return nullptr;
        }

        // The material camera must have a Z raster on Android. Without it
        // RwCameraBeginUpdate()/the GLES render path may succeed but discard
        // the 2D draw lists on some devices, producing a permanently blank
        // material-text texture.
        RwRaster* zRaster = GetZRaster(sizeX, sizeY);
        if (!zRaster) {
            RwTextureDestroy(texture);
            return nullptr;
        }

        RwCamera* previousCamera = Scene.m_pRwCamera;
        RwRaster* previousFrameBuffer = m_camera->frameBuffer;
        RwRaster* previousZBuffer = m_camera->zBuffer;

        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 oldDisplaySize = io.DisplaySize;
        const ImVec2 oldFramebufferScale = io.DisplayFramebufferScale;

        // Render the ImGui draw list in the coordinate system of the material
        // raster, not the phone's full screen. This is the important difference
        // from the normal HUD frame: a 256x256 material must get 0..256 clip
        // coordinates or the text can be clipped outside the camera texture.
        io.DisplaySize = ImVec2((float)sizeX, (float)sizeY);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

        // Make sure the RenderWare backend has created the font atlas before
        // the material-text frame is built.
        ImGui_ImplRenderWare_NewFrame();
        if (!io.Fonts || !io.Fonts->TexID) {
            io.DisplaySize = oldDisplaySize;
            io.DisplayFramebufferScale = oldFramebufferScale;
            RwTextureDestroy(texture);
            return nullptr;
        }

        m_camera->frameBuffer = raster;
        m_camera->zBuffer = zRaster;
        CVisibilityPlugins::SetRenderWareCamera(m_camera);

        RwRGBA background{};
        background.red   = static_cast<RwUInt8>((backgroundColor >> 16) & 0xFF);
        background.green = static_cast<RwUInt8>((backgroundColor >> 8) & 0xFF);
        background.blue  = static_cast<RwUInt8>(backgroundColor & 0xFF);
        background.alpha = static_cast<RwUInt8>((backgroundColor >> 24) & 0xFF);

        RwCameraClear(m_camera, &background, 3);
        if (!RwCameraBeginUpdate(m_camera)) {
            m_camera->frameBuffer = previousFrameBuffer;
            m_camera->zBuffer = previousZBuffer;
            if (previousCamera) CVisibilityPlugins::SetRenderWareCamera(previousCamera);
            io.DisplaySize = oldDisplaySize;
            io.DisplayFramebufferScale = oldFramebufferScale;
            RwTextureDestroy(texture);
            return nullptr;
        }

        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, RWRSTATE(FALSE));
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, RWRSTATE(FALSE));
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, RWRSTATE(TRUE));
        RwRenderStateSet(rwRENDERSTATESRCBLEND, RWRSTATE(rwBLENDSRCALPHA));
        RwRenderStateSet(rwRENDERSTATEDESTBLEND, RWRSTATE(rwBLENDINVSRCALPHA));
        RwRenderStateSet(rwRENDERSTATEFOGENABLE, RWRSTATE(FALSE));
        RwRenderStateSet(rwRENDERSTATECULLMODE, RWRSTATE(rwCULLMODENACULLMODE));
        RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, RWRSTATE(rwALPHATESTFUNCTIONGREATER));
        RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, RWRSTATE(1));
        DefinedState();

        Render(sizeX, sizeY, fontSize, fontColor, align, text);
        RwCameraEndUpdate(m_camera);

        // Restore the normal GTA camera and UI dimensions before leaving the
        // helper. The generated texture is independent of the camera after
        // EndUpdate().
        DefinedState2d();
        m_camera->frameBuffer = previousFrameBuffer;
        m_camera->zBuffer = previousZBuffer;
        if (previousCamera) CVisibilityPlugins::SetRenderWareCamera(previousCamera);

        io.DisplaySize = oldDisplaySize;
        io.DisplayFramebufferScale = oldFramebufferScale;

        // RwTextureCreate (0x273f74) starts the texture at refCount 1 (str of
        // 0x1_00000000 at +0x60, 0x273fc4): that reference IS the material
        // slot's. The old extra ++refCount left every material-text render
        // target at 1 after the slot released it (RwTextureDestroy 0x273e2c
        // frees only at 0): a GPU memory leak on every stream-in or text
        // change, until the game was killed near big mappings.
        return texture;
    }

private:
    // One Z raster per material size (at most 14), reused for every text
    // instead of creating and destroying one per call.
    struct ZEntry { int w, h; RwRaster* raster; };
    std::vector<ZEntry> m_zRasters;
    RwRaster* GetZRaster(int w, int h)
    {
        for (const ZEntry& z : m_zRasters)
            if (z.w == w && z.h == h) return z.raster;
        RwRaster* raster = RwRasterCreate(w, h, 0, rwRASTERTYPEZBUFFER);
        if (raster) m_zRasters.push_back({w, h, raster});
        return raster;
    }

    bool SetUpScene()
    {
        if (!Scene.m_pRpWorld) return false;
        if (m_camera && m_frame) return true;

        m_camera = RwCameraCreate();
        m_frame = RwFrameCreate();
        if (!m_camera || !m_frame) {
            if (m_camera) RwCameraDestroy(m_camera);
            if (m_frame) RwFrameDestroy(m_frame);
            m_camera = nullptr;
            m_frame = nullptr;
            return false;
        }

        _rwObjectHasFrameSetFrame(m_camera, m_frame);
        RwCameraSetFarClipPlane(m_camera, 300.0f);
        RwCameraSetNearClipPlane(m_camera, 0.01f);

        // The material texture is a pure 2D render target. A 1:1 view window
        // is enough because ImGui supplies the screen-space vertices itself.
        RwV2d view = { 0.5f, 0.5f };
        RwCameraSetViewWindow(m_camera, &view);
        RwCameraSetProjection(m_camera, rwPERSPECTIVE);
        RpWorldAddCamera(Scene.m_pRpWorld, m_camera);
        return true;
    }

    static void AddLine(ImDrawList* drawList, ImFont* font, float size,
                        const char* line, float x, float y, ImU32 color)
    {
        if (!drawList || !font || !line || !*line) return;
        drawList->AddText(font, size, ImVec2(x, y), color, line);
    }

    static void Render(int sizeX, int sizeY, uint8_t fontSize, uint32_t fontColor,
                       uint8_t align, const char* text)
    {
        if (!pGUI || !pGUI->GetFont() || !text || !*text) return;

        ImGuiIO& io = ImGui::GetIO();
        ImFont* font = pGUI->GetFont();
        const float size = static_cast<float>(std::max<int>(1, fontSize));

        const ImU32 color = IM_COL32(
            (fontColor >> 16) & 0xFF,
            (fontColor >> 8) & 0xFF,
            fontColor & 0xFF,
            (fontColor >> 24) & 0xFF);

        std::vector<std::string> lines;
        std::stringstream stream(text);
        std::string line;
        while (std::getline(stream, line, '\n'))
            lines.push_back(line);
        if (lines.empty()) lines.emplace_back(text);

        ImGui::NewFrame();
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        // Compute total text block height first so multi-line text is centered
        // exactly inside the material raster.
        float totalHeight = 0.0f;
        for (const auto& row : lines) {
            const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, row.c_str());
            totalHeight += std::max(size, ts.y);
        }

        float y = 0.0f;
        if (align == OBJECT_MATERIAL_ALIGN_CENTER) {
            y = std::max(0.0f, (static_cast<float>(sizeY) - totalHeight) * 0.5f);
        } else if (align == OBJECT_MATERIAL_ALIGN_RIGHT) {
            y = 0.0f;
        }

        for (const auto& row : lines) {
            const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, row.c_str());
            float x = 0.0f;
            if (align == OBJECT_MATERIAL_ALIGN_CENTER)
                x = std::max(0.0f, (static_cast<float>(sizeX) - ts.x) * 0.5f);
            else if (align == OBJECT_MATERIAL_ALIGN_RIGHT)
                x = std::max(0.0f, static_cast<float>(sizeX) - ts.x);

            if (!row.empty()) AddLine(drawList, font, size, row.c_str(), x, y, color);
            y += std::max(size, ts.y);
            if (y >= sizeY) break;
        }

        ImGui::EndFrame();
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData) {
            ImGui_ImplRenderWare_RenderDrawData(drawData);
        }

        (void)io;
    }

    RwCamera* m_camera = nullptr;
    RwFrame*  m_frame = nullptr;
};

MaterialTextGenerator g_materialTextGenerator;
}

// ---------------------------------------------------------------------------
//  Public API (Alyn CObject::SetMaterial / SetMaterialText / ProcessMaterialText)
// ---------------------------------------------------------------------------

CObjectSamp* CObjectSamp::FindMaterialObject(const void* pGtaEntity)
{
    if (!pGtaEntity || ms_MaterialObjects.empty()) return nullptr;
    auto it = ms_MaterialObjects.find(pGtaEntity);
    return (it != ms_MaterialObjects.end()) ? it->second : nullptr;
}

void CObjectSamp::SyncMaterialRegistration()
{
    const void* key = (m_bHasCustomObjectMaterial && m_pEntity)
                        ? static_cast<const void*>(m_pEntity) : nullptr;
    if (key == m_pMaterialRegKey) return;

    if (m_pMaterialRegKey) {
        auto it = ms_MaterialObjects.find(m_pMaterialRegKey);
        if (it != ms_MaterialObjects.end() && it->second == this) ms_MaterialObjects.erase(it);
        m_pMaterialRegKey = nullptr;
    }
    if (key) {
        ms_MaterialObjects[key] = this;
        m_pMaterialRegKey = key;
    }
}

static void ReleaseMaterialSourceModel(ObjectMaterialState& material)
{
    if (!material.sourceModelPinned) return;

    const int32_t modelId = material.modelId;
    if (modelId > 0 && modelId < CModelInfo::NUM_MODEL_INFOS) {
        // Only clear the request that this material path owns. GTA will decide
        // when the now-unreferenced source model can actually be evicted.
        CStreaming::SetModelIsDeletable(modelId);
    }
    material.sourceModelPinned = false;
}

// Material-text render targets: at most a few new ones per game tick and a
// cap on the bytes alive at once. Heavy mappings push hundreds of
// SetObjectMaterialText slots in one burst; rendering all of them in one tick
// (each a camera raster + ImGui frame + FBO switch) stalled the game and
// spiked GPU memory. Slots over budget stay pending and are made later.
namespace {
constexpr int    kMaterialTextPerTick  = 3;
constexpr size_t kMaterialTextMaxBytes = size_t(64) << 20;
size_t g_materialTextBytes = 0;
int    g_materialTextLeft  = kMaterialTextPerTick;

bool TakeMaterialTextJob(size_t bytes)
{
    if (CObjectPool::IsSpawnMaterialCooldownActive()) return false;
    if (g_materialTextLeft <= 0) return false;
    if (g_materialTextBytes + bytes > kMaterialTextMaxBytes) return false;
    --g_materialTextLeft;
    return true;
}

void ReleaseMaterialTexture(ObjectMaterialState& material)
{
    if (material.texture) RwTextureDestroy(material.texture);
    material.texture = nullptr;
    g_materialTextBytes -= std::min<size_t>(g_materialTextBytes, material.textBytes);
    material.textBytes = 0;
}
} // namespace

void CObjectSamp::BeginMaterialTextTick()
{
    g_materialTextLeft = kMaterialTextPerTick;
}

void CObjectSamp::SetMaterial(int modelId, uint8_t materialIndex, const char* txdName,
                              const char* textureName, uint32_t color)
{
    if (materialIndex >= OBJECT_MAX_MATERIALS) return;
    if (!txdName || !*txdName || !textureName || !*textureName) return;

    std::lock_guard<std::recursive_mutex> lock(ms_MaterialMutex);
    ChangeToOriginalObjectMaterial();

    ObjectMaterialState& material = m_Material[materialIndex];
    ReleaseMaterialSourceModel(material);
    ReleaseMaterialTexture(material);

    material.type = OBJECT_MATERIAL_TEXTURE;
    material.color = color;
    material.modelId = modelId;
    std::strncpy(material.txdName, txdName, sizeof(material.txdName) - 1);
    material.txdName[sizeof(material.txdName) - 1] = '\0';
    std::strncpy(material.textureName, textureName, sizeof(material.textureName) - 1);
    material.textureName[sizeof(material.textureName) - 1] = '\0';
    material.textureWasCreated = false;
    material.sourceModelPinned = false;
    material.lookupFailures = 0;
    material.nextLookupTick = 0;

    // SA-MP: -1 = keep original texture and optionally change material color;
    // 0 = replace with the transparent/blank texture; >0 = texture comes from
    // a source model and that model must remain resident while the texture is
    // attached. This last point is important on Android: removing the source
    // DFF/TXD immediately after grabbing RwTexture can invalidate the material
    // before the next render pass.
    if (modelId < 0) {
        material.textureWasCreated = true;
    } else if (modelId == 0) {
        material.texture = GetBlankTexture();
        material.textureWasCreated = (material.texture != nullptr);
    } else if (modelId < CModelInfo::NUM_MODEL_INFOS &&
               IsValidModel(static_cast<unsigned int>(modelId))) {
        // Name lookup first: the texture databases load entries on demand.
        // The source model is only requested (asynchronously) when that
        // fails; never LoadAllRequestedModels() inside an RPC (a streamer
        // burst near a big mapping stalled the game for every slot).
        material.texture = LoadTextureFromTxd(material.txdName, material.textureName);
        material.textureWasCreated = (material.texture != nullptr);
        if (!material.textureWasCreated && !CStreaming::IsModelLoaded(modelId)) {
            CStreaming::RequestModel(modelId,
                STREAMING_GAME_REQUIRED | STREAMING_KEEP_IN_MEMORY);
            material.sourceModelPinned = true;
        }
    }

    m_bHasCustomObjectMaterial = true;
    SyncMaterialRegistration();
}

void CObjectSamp::SetMaterialText(uint8_t materialIndex, uint8_t materialSize, const char* fontName,
                                  uint8_t fontSize, uint8_t fontBold, uint32_t fontColor,
                                  uint32_t backgroundColor, uint8_t align, const char* text)
{
    if (materialIndex >= OBJECT_MAX_MATERIALS) return;
    if (!text || !*text) return;

    std::lock_guard<std::recursive_mutex> lock(ms_MaterialMutex);
    ChangeToOriginalObjectMaterial();

    ObjectMaterialState& material = m_Material[materialIndex];
    ReleaseMaterialSourceModel(material);
    ReleaseMaterialTexture(material);

    ObjectMaterialText& info = material.textInfo;
    info.materialSize = materialSize;
    std::strncpy(info.fontName, fontName ? fontName : "", sizeof(info.fontName) - 1);
    info.fontName[sizeof(info.fontName) - 1] = '\0';
    info.fontSize = fontSize;
    info.fontBold = fontBold;
    info.fontColor = fontColor;
    info.backgroundColor = backgroundColor;
    info.align = align;
    info.text.assign(text, strnlen(text, 2048));

    material.type = OBJECT_MATERIAL_TEXT;
    material.color = 0;
    material.modelId = -1;
    material.txdName[0] = '\0';
    material.textureName[0] = '\0';
    material.textureWasCreated = false;

    m_bHasCustomObjectMaterial = true;
    SyncMaterialRegistration();
}

void CObjectSamp::ProcessMaterialText()
{
    std::lock_guard<std::recursive_mutex> lock(ms_MaterialMutex);

    for (int i = 0; i < OBJECT_MAX_MATERIALS; ++i) {
        ObjectMaterialState& material = m_Material[i];

        // A texture replacement may arrive before its source model/TXD is
        // resident. Keep the request alive and retry without losing the RPC.
        if (material.type == OBJECT_MATERIAL_TEXTURE && !material.textureWasCreated) {
            if (material.modelId <= 0 ||
                material.modelId >= CModelInfo::NUM_MODEL_INFOS ||
                !IsValidModel(static_cast<unsigned int>(material.modelId))) {
                // -1 has no replacement texture by design; 0 is handled once
                // by SetMaterial. Nothing else can be resolved here.
                material.textureWasCreated = (material.modelId < 0);
                continue;
            }

            const int32_t modelId = material.modelId;
            const uint32_t now = GetTickCount();
            if (static_cast<int32_t>(material.nextLookupTick - now) > 0) continue;
            if (!CStreaming::IsModelLoaded(modelId)) {
                if (!material.sourceModelPinned) {
                    CStreaming::RequestModel(modelId,
                        STREAMING_GAME_REQUIRED | STREAMING_KEEP_IN_MEMORY);
                    material.sourceModelPinned = true;
                }
                material.nextLookupTick = now + 250u;
                continue;
            }

            RwTexture* texture = LoadTextureFromTxd(material.txdName, material.textureName);
            if (texture) {
                ReleaseMaterialTexture(material);
                material.texture = texture;
                material.textureWasCreated = true;
                // The slot's own reference keeps the texture alive when the
                // source TXD unloads (CTxdStore::RemoveTxd detaches textures
                // with refCount >= 2, 0x6f90f4), so the source model no longer
                // has to stay pinned in streaming memory.
                ReleaseMaterialSourceModel(material);
            } else if (++material.lookupFailures >= 5) {
                Log("ObjectMaterial: texture '%s' not found, keeping the model texture",
                    material.textureName);
                material.textureWasCreated = true; // stop retrying; colour still applies
                ReleaseMaterialSourceModel(material);
            } else {
                material.nextLookupTick = now + 1000u * material.lookupFailures;
            }
            continue;
        }

        if (material.textureWasCreated || material.type != OBJECT_MATERIAL_TEXT)
            continue;
        if (material.textInfo.text.empty())
            continue;

        ObjectMaterialText& info = material.textInfo;
        if (info.materialSize < OBJECT_MATERIAL_SIZE_32X32)
            info.materialSize = OBJECT_MATERIAL_SIZE_32X32;
        else if (info.materialSize > OBJECT_MATERIAL_SIZE_512X512)
            info.materialSize = OBJECT_MATERIAL_SIZE_512X512;

        int sizeX = 0, sizeY = 0;
        GetMaterialTextSize(info.materialSize, &sizeX, &sizeY);
        if (info.align > OBJECT_MATERIAL_ALIGN_RIGHT)
            info.align = OBJECT_MATERIAL_ALIGN_CENTER;

        const uint8_t renderFontSize = static_cast<uint8_t>(std::max<int>(1,
            static_cast<int>(info.fontSize * 0.75f)));

        const size_t bytes = size_t(sizeX) * size_t(sizeY) * 4u;
        if (!TakeMaterialTextJob(bytes)) continue; // stays pending, next tick

        RwTexture* generated = g_materialTextGenerator.Create(
            sizeX, sizeY, renderFontSize, info.fontColor, info.backgroundColor,
            info.align, info.text.c_str());

        if (!generated) continue;

        ReleaseMaterialTexture(material);
        material.texture = generated;
        material.textBytes = static_cast<uint32_t>(bytes);
        g_materialTextBytes += bytes;
        material.textureWasCreated = true;
    }
}

void CObjectSamp::ClearAllMaterials()
{
    std::lock_guard<std::recursive_mutex> lock(ms_MaterialMutex);
    ChangeToOriginalObjectMaterial();

    for (auto& material : m_Material) {
        ReleaseMaterialSourceModel(material);
        ReleaseMaterialTexture(material);
        material = {};
    }
    m_bHasCustomObjectMaterial = false;
    m_MaterialBackups.clear();
    SyncMaterialRegistration();
}

// ---------------------------------------------------------------------------
//  Render-time swap (Alyn ChangeCustom/StoreOriginal/ChangeOriginal callbacks)
// ---------------------------------------------------------------------------

static CObjectSamp::GeometryMaterialBackup* FindGeometryBackup(CObjectSamp* pObject, const void* geometryKey)
{
    if (!pObject || !geometryKey) return nullptr;
    for (auto& backup : pObject->m_MaterialBackups) {
        if (backup.geometry == geometryKey) return &backup;
    }
    return nullptr;
}

static RwObject* ApplyCustomMaterialObjectCB(RwObject* object, void* data)
{
    auto* pObject = static_cast<CObjectSamp*>(data);
    if (!pObject || !object || object->type != rpATOMIC) return object;

    auto* atomic = reinterpret_cast<RpAtomic*>(object);
    RpGeometry* geometry = atomic->geometry;
    if (!geometry || !geometry->matList.materials || geometry->matList.numMaterials <= 0)
        return object;

    // Capture every geometry independently. A single backup array cannot be
    // shared between multiple atomics because their material lists differ.
    if (!FindGeometryBackup(pObject, geometry)) {
        CObjectSamp::GeometryMaterialBackup backup{};
        backup.geometry = geometry;
        backup.flags = geometry->flags;
        backup.materialCount = static_cast<uint8_t>(std::min<int>(geometry->matList.numMaterials,
                                                                    OBJECT_MAX_MATERIALS));

        RpMaterial** materials = geometry->matList.materials;
        for (int i = 0; i < backup.materialCount; ++i) {
            if (!materials[i]) continue;
            backup.textures[i] = materials[i]->texture;
            backup.colors[i] = materials[i]->color;
            backup.surfaceProps[i] = materials[i]->surfaceProps;
        }
        pObject->m_MaterialBackups.push_back(backup);
    }

    RpMaterial** materials = geometry->matList.materials;
    const int count = std::min<int>(geometry->matList.numMaterials, OBJECT_MAX_MATERIALS);
    for (int i = 0; i < count; ++i) {
        if (!materials[i]) continue;

        const ObjectMaterialState& material = pObject->m_Material[i];

        // A real texture pointer means SetObjectMaterial/MaterialText owns the
        // replacement. For modelid == -1, the texture must stay unchanged but
        // the material colour can still be applied (SA-MP semantics).
        if (material.texture) {
            materials[i]->texture = material.texture;
        }

        if (material.type == OBJECT_MATERIAL_TEXTURE && material.color != 0) {
            geometry->flags |= rpGEOMETRYMODULATEMATERIALCOLOR;
            geometry->flags &= 0xFFFFFFF7u;
            materials[i]->color = *reinterpret_cast<const RwRGBA*>(&material.color);
            materials[i]->surfaceProps.ambient = 1.0f;
            materials[i]->surfaceProps.specular = 0.0f;
            materials[i]->surfaceProps.diffuse = 1.0f;
        }
    }

    return object;
}

void CObjectSamp::TryChangeToCustomObjectMaterial()
{
    if (m_bHasCustomObjectMaterial) {
        ChangeToCustomObjectMaterial();
    }
}

void CObjectSamp::ChangeToCustomObjectMaterial()
{
    if (!m_bHasCustomObjectMaterial || !m_pEntity || !m_pEntity->m_pRwObject)
        return;

    // A render scope can be entered more than once by reflection/effect paths.
    // Drop only stale snapshots that are not currently active.
    m_MaterialBackups.clear();

    auto* parent = reinterpret_cast<RwFrame*>(m_pEntity->m_pRwObject->parent);
    if (!parent)
        return;

    RwFrameForAllObjects(parent, ApplyCustomMaterialObjectCB, this);
}

void CObjectSamp::ChangeToOriginalObjectMaterial()
{
    if (m_MaterialBackups.empty()) return;

    for (const auto& backup : m_MaterialBackups) {
        auto* geometry = const_cast<RpGeometry*>(reinterpret_cast<const RpGeometry*>(backup.geometry));
        if (!geometry || !geometry->matList.materials) continue;

        geometry->flags = backup.flags;
        RpMaterial** materials = geometry->matList.materials;
        const int count = std::min<int>(geometry->matList.numMaterials, OBJECT_MAX_MATERIALS);
        const int restoreCount = std::min<int>(count, backup.materialCount);

        for (int i = 0; i < restoreCount; ++i) {
            if (!materials[i]) continue;
            materials[i]->texture = backup.textures[i];
            materials[i]->color = backup.colors[i];
            materials[i]->surfaceProps = backup.surfaceProps[i];
        }
    }

    m_MaterialBackups.clear();
}


void CObjectSamp::SetRot(float &radX, float &radY, float &radZ)
{
    if (!m_pEntity) return;
    m_pEntity->Remove();

	m_pEntity->SetOrientation(radX, radY, radZ);

	m_pEntity->UpdateRW();
	m_pEntity->UpdateRwFrame();

    m_pEntity->Add();
}

void CObjectSamp::GetRotation(float* pfX,float* pfY,float* pfZ)
{
	if (!m_pEntity) return;

	m_pEntity->m_matrix->ConvertToEulerAngles(pfX, pfY, pfZ, 21);

	*pfX = *pfX * 57.295776 * -1.0;
	*pfY = *pfY * 57.295776 * -1.0;
	*pfZ = *pfZ * 57.295776 * -1.0;

}
