#include "main.h"
#include "CSettings.h"
#include "game/game.h"
#include "vendor/ini/config.h"
#include "java_systems/HUD.h"
#include "util/patch.h"
#include "CDebugInfo.h"
#include "graphics/GraphicsSettings.h"
#include <cerrno>
#include <mutex>
#include <unistd.h>

stSettings CSettings::m_Settings;

namespace {
// save() runs on the UI thread (login, client settings dialog, pause) and on
// the game thread (GRAFIS menu, /fpslimit); toDefaults re-enters LoadSettings.
std::recursive_mutex g_settingsLock;
// Nothing is written before the file was read once: a save with empty
// m_Settings would replace the player's file with defaults.
bool g_settingsLoaded = false;

void SettingsPath(char* out, size_t size) {
	snprintf(out, size, "%sSAMP/settings.ini", g_pszStorage ? g_pszStorage : "");
}

// Write <file>.tmp, then rename over the file: a reader (or the next start
// after the app was killed mid-save) sees the old file or the new one, never
// a missing or half-written one. The old code deleted the file first.
bool WriteSettingsFile(ini_table_s* config, const char* path) {
	char tmp[0x200];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (ini_table_write_to_file(config, tmp)) {
		if (rename(tmp, path) == 0) return true;
		unlink(tmp);
	}
	// Storage that cannot rename over an existing file.
	return ini_table_write_to_file(config, path);
}

void SaveSettings(int iIgnoreCategory, bool keepUnknownKeys);
}

static void ClearBackslashN(char *pStr, size_t size) {
	for (size_t i = 0; i < size; i++) {
		if (pStr[i] == '\n' || pStr[i] == 13)
		{
			pStr[i] = 0;
		}
	}
}

void CSettings::toDefaults(int iCategory)
{
	std::lock_guard<std::recursive_mutex> lock(g_settingsLock);
	// Same result as before (keys of the category are left out, the reload
	// fills in defaults), written atomically instead of truncating the file.
	SaveSettings(iCategory, false);
	LoadSettings(m_Settings.szNickName);

	CHUD::ChangeChatTextSize(m_Settings.iChatFontSize);

}

void CSettings::save(int iIgnoreCategory)
{
	// Normal save: keys written by the launcher/Java (host, tutorial_done,
	// voice, ...) are kept; the client's own keys are overwritten.
	SaveSettings(iIgnoreCategory, iIgnoreCategory == 0);
}

namespace {
void SaveSettings(int iIgnoreCategory, bool keepUnknownKeys)
{
	using CS = CSettings;
	auto& m_Settings = CS::m_Settings;
	std::lock_guard<std::recursive_mutex> lock(g_settingsLock);
	if (!g_settingsLoaded || !g_pszStorage) {
		Log("Settings: not loaded yet, save skipped");
		return;
	}
	char buff[0x200];
	SettingsPath(buff, sizeof(buff));

	ini_table_s *config = ini_table_create();
	if (keepUnknownKeys) ini_table_read_from_file(config, buff);

	ini_table_create_entry(config, "client", "name", m_Settings.szNickName);
	ini_table_create_entry(config, "client", "ip", m_Settings.szIp);
	ini_table_create_entry_as_int(config, "client", "port", m_Settings.port);
	ini_table_create_entry(config, "client", "password", m_Settings.szPassword);
	ini_table_create_entry(config, "client", "player_password", m_Settings.player_password);
	
	ini_table_create_entry_as_int(config, "client", "autologin", m_Settings.szAutoLogin);

	ini_table_create_entry_as_int(config, "gui", "hparmourtext", m_Settings.iHPArmourText);
	ini_table_create_entry_as_int(config, "gui", "damageinformer", m_Settings.iIsEnableDamageInformer);
	ini_table_create_entry_as_int(config, "gui", "text3dinveh", m_Settings.iIsEnable3dTextInVehicle);

	ini_table_create_entry_as_int(config, "client", "server", m_Settings.szServer);
	ini_table_create_entry_as_int(config, "client", "debug", m_Settings.szDebug);
	ini_table_create_entry_as_int(config, "client", "headmove", m_Settings.szHeadMove);
	ini_table_create_entry_as_int(config, "client", "dl", m_Settings.szDL);
	ini_table_create_entry_as_int(config, "client", "timestamp", m_Settings.szTimeStamp);
	ini_table_create_entry_as_int(config, "client", "test", m_Settings.isTestMode);

	ini_table_create_entry(config, "gui", "Font", m_Settings.szFont);

	ini_table_create_entry_as_float(config, "gui", "FontSize", m_Settings.fFontSize);
	ini_table_create_entry_as_int(config, "gui", "FontOutline", m_Settings.iFontOutline);

	ini_table_create_entry_as_int(config, "gui", "fps", m_Settings.iFPS);

	if (iIgnoreCategory != 1)
	{
		ini_table_create_entry_as_int(config, "gui", "ChatFontSize", m_Settings.iChatFontSize);
		ini_table_create_entry_as_int(config, "gui", "ChatMaxMessages", m_Settings.iChatMaxMessages);

		ini_table_create_entry_as_int(config, "gui", "androidKeyboard", m_Settings.iAndroidKeyboard);
		ini_table_create_entry_as_int(config, "gui", "outfit", m_Settings.iOutfitGuns);
	}

	// Client graphics options (GRAFIS tab), [graphics] section.
	GraphicsSettings::Save(config);

	if (!WriteSettingsFile(config, buff))
		Log("Settings: cannot write %s (errno %d)", buff, errno);
	ini_table_destroy(config);
}
}


// TODO: json
extern bool g_bIsTestMode;
extern void ApplyFPSPatch(uint8_t fps);
void CSettings::LoadSettings(const char *szNickName, int iChatLines)
{
	char tempNick[40];
	if (szNickName)
	{
		strcpy(tempNick, szNickName);
	}

	Log("Loading settings..");
	std::lock_guard<std::recursive_mutex> lock(g_settingsLock);

	char buff[0x200];
	SettingsPath(buff, sizeof(buff));

	ini_table_s *config = ini_table_create();
	Log("Opening settings: %s", buff);
	if (!ini_table_read_from_file(config, buff))
	{
		// A save interrupted by the old remove()+rewrite left only the temp file.
		char tmp[0x200];
		snprintf(tmp, sizeof(tmp), "%s.tmp", buff);
		ini_table_destroy(config);
		config = ini_table_create();
		if (!ini_table_read_from_file(config, tmp) || rename(tmp, buff) != 0)
		{
			ini_table_destroy(config);
			Log("Cannot load settings, exiting...");
			CGame::exitGame();
			return;
		}
		Log("Settings restored from %s", tmp);
	}

	snprintf(m_Settings.szNickName, sizeof(m_Settings.szNickName), "__android_%d%d", rand() % 1000, rand() % 1000);
	memset(m_Settings.szPassword, 0, sizeof(m_Settings.szPassword));
	memset(m_Settings.player_password, 0, sizeof(m_Settings.player_password));

	snprintf(m_Settings.szFont, sizeof(m_Settings.szFont), "visby-round-cf-extra-bold.ttf");

	memset(m_Settings.szIp, 0, sizeof(m_Settings.szIp));
	const char *szIp = ini_table_get_entry(config, "client", "ip");
	if (szIp) strncpy(m_Settings.szIp, szIp, sizeof(m_Settings.szIp) - 1);

	m_Settings.port = ini_table_get_entry_as_int(config, "client", "port", 7777);

	const char *pName = ini_table_get_entry(config, "client", "name");
	std::string szName = pName ? pName : "";
	const char *szPassword = ini_table_get_entry(config, "client", "password");
	const char *pPassword = ini_table_get_entry(config, "client", "player_password");

	m_Settings.szAutoLogin = ini_table_get_entry_as_int(config, "client", "autologin", 0);
	m_Settings.szServer = ini_table_get_entry_as_int(config, "client", "server", 0);

	m_Settings.szDebug = ini_table_get_entry_as_int(config, "client", "debug", 0);
	CDebugInfo::SetDrawFPS(CSettings::m_Settings.szDebug);

	m_Settings.szHeadMove = ini_table_get_entry_as_int(config, "client", "headmove", 0);
	m_Settings.szDL = ini_table_get_entry_as_int(config, "client", "dl", 0);
	m_Settings.szTimeStamp = ini_table_get_entry_as_int(config, "client", "timestamp", 0);
	m_Settings.isTestMode = ini_table_get_entry_as_int(config, "client", "test", 0);
	g_bIsTestMode = (bool)m_Settings.isTestMode;

	const char *pFontName = ini_table_get_entry(config, "gui", "Font");
	std::string szFontName = pFontName ? pFontName : "";

	if(pPassword)
	{
		strncpy(m_Settings.player_password, pPassword, sizeof(m_Settings.player_password) - 1);
	}
	if ( !szName.empty() )
	{
		strncpy(m_Settings.szNickName, szName.c_str(), sizeof(m_Settings.szNickName) - 1);
	}
	if (szPassword)
	{
		strncpy(m_Settings.szPassword, szPassword, sizeof(m_Settings.szPassword) - 1);
	}
	if ( !szFontName.empty() )
	{
		strncpy(m_Settings.szFont, szFontName.c_str(), sizeof(m_Settings.szFont) - 1);
	}

	ClearBackslashN(m_Settings.szNickName, sizeof(m_Settings.szNickName));
	ClearBackslashN(m_Settings.szPassword, sizeof(m_Settings.szPassword));
	ClearBackslashN(m_Settings.szFont, sizeof(m_Settings.szFont));
	ClearBackslashN(m_Settings.player_password, sizeof(m_Settings.player_password));

	if (szNickName)
	{
		strcpy(m_Settings.szNickName, tempNick);
	}

	m_Settings.fFontSize = ini_table_get_entry_as_float(config, "gui", "FontSize", 30.0f);
	m_Settings.iChatFontSize = ini_table_get_entry_as_int(config, "gui", "ChatFontSize", -1);
	m_Settings.iFontOutline = ini_table_get_entry_as_int(config, "gui", "FontOutline", 2);

	m_Settings.iChatMaxMessages = ini_table_get_entry_as_int(config, "gui", "ChatMaxMessages", -1);

	m_Settings.iFPS = ini_table_get_entry_as_int(config, "gui", "fps", 60);
	if( m_Settings.iFPS < 20 ) m_Settings.iFPS = 60;
	ApplyFPSPatch(m_Settings.iFPS);

	m_Settings.iAndroidKeyboard = ini_table_get_entry_as_int(config, "gui", "androidKeyboard", 0);

	m_Settings.iOutfitGuns = ini_table_get_entry_as_int(config, "gui", "outfit", 1);
	CWeaponsOutFit::SetEnabled(CSettings::m_Settings.iOutfitGuns);

	m_Settings.iIsEnableDamageInformer = ini_table_get_entry_as_int(config, "gui", "damageinformer", 1);
	m_Settings.iIsEnable3dTextInVehicle = ini_table_get_entry_as_int(config, "gui", "text3dinveh", 1);

	m_Settings.iHPArmourText = ini_table_get_entry_as_int(config, "gui", "hparmourtext", 0);

	// Client graphics options; missing keys keep the defaults ("Sedang").
	GraphicsSettings::Load(config);

	ini_table_destroy(config);
	g_settingsLoaded = true;
}

bool CSettings::SaveNow()
{
	std::lock_guard<std::recursive_mutex> lock(g_settingsLock);
	if (!g_settingsLoaded) return false;
	GraphicsSettings::ConsumeDirty();
	save();
	return true;
}

extern "C"
{

JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_ChatFontSizeChanged(JNIEnv *env,
																				   jobject thiz,
																				   jint size) {
	CSettings::m_Settings.iChatFontSize = size;
	CSettings::save();
	CHUD::ChangeChatTextSize(size);
}

JNIEXPORT jboolean JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_getNativeDamageInformer(JNIEnv *env,
																					   jobject thiz) {
	return CSettings::m_Settings.iIsEnableDamageInformer;
}

JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_setNativeDamageInformer(JNIEnv *env,
																					   jobject thiz,
																					   jboolean state) {
	CSettings::m_Settings.iIsEnableDamageInformer = state;
	CSettings::save();
}
}
extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_setNativeShow3dText(JNIEnv *env,
                                                                                   jobject thiz,
                                                                                   jboolean state) {
	CSettings::m_Settings.iIsEnable3dTextInVehicle = state;
	CSettings::save();
}
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_getNativeShow3dText(JNIEnv *env,
																				   jobject thiz) {
	return CSettings::m_Settings.iIsEnable3dTextInVehicle;
}
extern "C"
JNIEXPORT jint JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_getNativeFpsLimit(JNIEnv *env,
                                                                                 jobject thiz) {
	return CSettings::m_Settings.iFPS;
}

extern void ApplyFPSPatch(uint8_t fps);

extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_setNativeFpsCount(JNIEnv *env,
																				 jobject thiz,
																				 jint fps) {
	CSettings::m_Settings.iFPS = fps;
	CSettings::save();
	ApplyFPSPatch(fps);
}
extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_setNativeFpsCounterSettings(
        JNIEnv *env, jobject thiz, jboolean b) {
	CSettings::m_Settings.szDebug = b;

	CDebugInfo::SetDrawFPS(b);
}
extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_setNativeOutfitGunsSettings(
		JNIEnv *env, jobject thiz, jboolean b) {
	CSettings::m_Settings.iOutfitGuns = b;

	CWeaponsOutFit::SetEnabled(b);
}
extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettings_onSettingsWindowDefaults(JNIEnv *env, jobject thiz,
																		  jint category) {
	CSettings::toDefaults(category);
}
extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettings_onSettingsWindowSave(JNIEnv *env, jobject thiz) {
	CSettings::save();
}
extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_setNativeHpArmourText(JNIEnv *env,
																					 jobject thiz,
																					 jboolean b) {
	CSettings::m_Settings.iHPArmourText = b;
	CHUD::ToggleProgressTexts(b);
	CSettings::save();
}
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_getNativeFpsCounterSettings(
		JNIEnv *env, jobject thiz) {
	return CSettings::m_Settings.szDebug;
}
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_getNativeOutfitGunsSettings(
		JNIEnv *env, jobject thiz) {
	return CSettings::m_Settings.iOutfitGuns;
}
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_getNativeHpArmourText(JNIEnv *env,
																					 jobject thiz) {
	return CSettings::m_Settings.iHPArmourText;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_setNativeTexts(JNIEnv *env,
                                                                              jobject thiz,
                                                                              jboolean b) {
	CSettings::m_Settings.i3dTextsDisable = b;
}
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_holy_game_core_DialogClientSettingsCommonFragment_getNativeTexts(JNIEnv *env,
																			  jobject thiz) {
	return CSettings::m_Settings.i3dTextsDisable;
}