/*
	Minimap Vice City untuk server SA-MP 0.3.7 + client HolySAMP (Android).

	Versi 0.3.7 dari vc_minimap.pwn (https://github.com/casualmind/samp-vice-city). Bedanya:
	  - tanpa AddSimpleModel: di client HolySAMP tekstur "mdl-1500:<nama>" diambil dari minimap.txd yang
	    disalin ke TESTLIT/vice_city di HP;
	  - tanpa plugin streamer: bagian peta dihitung dari posisi pemain (hanya butuh a_samp);
	  - minimap hanya tampil selama pemain berada di wilayah Vice City, jadi radar San Andreas tidak tertutup;
	  - ikon pemain hanya dibuat ulang kalau posisinya di minimap memang berubah (lebih hemat jaringan);
	  - hanya untuk client yang melapor versi "0.3.7" persis: client Android ini, dan SA-MP PC 0.3.7-R1
	    (yang tidak mengenal tekstur "mdl-1500" dan akan melihat kotak putih). PC R2 ke atas melapor
	    "0.3.7-R2" dan seterusnya dan tidak diberi minimap. Hapus MINIMAP_ONLY_FOR_VERSION untuk semua.
*/

#include <a_samp>

// Batas wilayah Vice City
#define VICE_CITY_MIN_X 4130.0
#define VICE_CITY_MIN_Y -930.0
#define VICE_CITY_MAX_X 6975.0
#define VICE_CITY_MAX_Y 2665.0

#define MINIMAP_MODEL "mdl-1500"
#define MINIMAP_UPDATE_INTERVAL 250
#define MINIMAP_ONLY_FOR_VERSION "0.3.7"

// Letak dan ukuran textdraw
#define MINIMAP_TEXTDRAW_POS_X 25.0
#define MINIMAP_TEXTDRAW_POS_Y 325.0
#define MINIMAP_TEXTDRAW_SIZE_X 110.0
#define MINIMAP_TEXTDRAW_SIZE_Y 95.0
#define MINIMAP_TEXTDRAW_ICON_SIZE_X 6.4
#define MINIMAP_TEXTDRAW_ICON_SIZE_Y 8.0
#define MINIMAP_TEXTDRAW_BORDER_SIZE 2.0
// Ikon dibuat ulang kalau bergeser sejauh ini (satuan textdraw) atau arahnya berubah.
#define MINIMAP_ICON_STEP 0.3

#define TD_BORDER 0
#define TD_MAP 1
#define TD_ICON 2

// Batas tiap bagian minimap (minX, minY, maxX, maxY); tekstur bagian ke-n bernama "n".
static const Float:minimapAreas_Coords[15][4] =
{
	{4130.000000, -930.000000, 5078.333496, -211.000000},   //1
	{5078.333496, -930.000000, 6026.666503, -211.000000},   //2
	{6026.666503, -930.000000, 6975.000000, -211.000000},   //3
	{4130.000000, -211.000000, 5078.333496, 508.000000},    //4
	{5078.333496, -211.000000, 6026.666503, 508.000000},    //5
	{6026.666503, -211.000000, 6975.000000, 508.000000},    //6
	{4130.000000, 508.000000, 5078.333496, 1227.000000},    //7
	{5078.333496, 508.000000, 6026.666503, 1227.000000},    //8
	{6026.666503, 508.000000, 6975.000000, 1227.000000},    //9
	{4130.000000, 1227.000000, 5078.333496, 1946.000000},   //10
	{5078.333496, 1227.000000, 6026.666503, 1946.000000},   //11
	{6026.666503, 1227.000000, 6975.000000, 1946.000000},   //12
	{4130.000000, 1946.000000, 5078.333496, 2665.000000},   //13
	{5078.333496, 1946.000000, 6026.666503, 2665.000000},   //14
	{6026.666503, 1946.000000, 6975.000000, 2665.000000}    //15
};

forward OnVcMinimapRequestUpdate(playerid);

static
	bool:pVcMinimap[MAX_PLAYERS],
	PlayerText:pVcMinimapTextdraws[MAX_PLAYERS][3],
	pVcMinimapCurrentArea[MAX_PLAYERS],
	pVcMinimapTimer[MAX_PLAYERS] = {-1, ...},
	Float:pVcMinimapIconX[MAX_PLAYERS],
	Float:pVcMinimapIconY[MAX_PLAYERS],
	pVcMinimapIconCompass[MAX_PLAYERS];

public OnFilterScriptInit()
{
	for (new i = 0; i < MAX_PLAYERS; i++)
	{
		for (new j = 0; j < 3; j++) pVcMinimapTextdraws[i][j] = PlayerText:INVALID_TEXT_DRAW;
		if (IsPlayerConnected(i)) CreatePlayerVcMinimap(i);
	}
	return 1;
}

public OnFilterScriptExit()
{
	for (new i = 0; i < MAX_PLAYERS; i++)
	{
		if (pVcMinimap[i]) DestroyPlayerVcMinimap(i);
	}
	return 1;
}

public OnPlayerConnect(playerid)
{
	pVcMinimap[playerid] = false;
	pVcMinimapTimer[playerid] = -1;
	for (new j = 0; j < 3; j++) pVcMinimapTextdraws[playerid][j] = PlayerText:INVALID_TEXT_DRAW;
	return 1;
}

public OnPlayerSpawn(playerid)
{
	if (!pVcMinimap[playerid]) CreatePlayerVcMinimap(playerid);
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	if (pVcMinimap[playerid]) DestroyPlayerVcMinimap(playerid);
	return 1;
}

public OnVcMinimapRequestUpdate(playerid)
{
	if (pVcMinimap[playerid]) UpdateVcMinimap(playerid);
	return 1;
}

// Client yang mengenal tekstur minimap?
static bool:IsVcMinimapClient(playerid)
{
#if defined MINIMAP_ONLY_FOR_VERSION
	new version[32];
	GetPlayerVersion(playerid, version, sizeof version);
	return (version[0] != EOS && !strcmp(version, MINIMAP_ONLY_FOR_VERSION, false));
#else
	#pragma unused playerid
	return true;
#endif
}

// Bagian minimap tempat titik (x, y) berada: 1..15, atau 0 di luar Vice City.
static GetVcMinimapArea(Float:x, Float:y)
{
	if (x < VICE_CITY_MIN_X || x > VICE_CITY_MAX_X || y < VICE_CITY_MIN_Y || y > VICE_CITY_MAX_Y) return 0;
	for (new i = 0; i < sizeof minimapAreas_Coords; i++)
	{
		if (x >= minimapAreas_Coords[i][0] && x <= minimapAreas_Coords[i][2]
			&& y >= minimapAreas_Coords[i][1] && y <= minimapAreas_Coords[i][3]) return i + 1;
	}
	return 0;
}

static CreatePlayerVcMinimap(playerid)
{
	if (pVcMinimap[playerid]) return 0;
	if (!IsVcMinimapClient(playerid)) return 0;

	// Bingkai
	pVcMinimapTextdraws[playerid][TD_BORDER] = CreatePlayerTextDraw(playerid,
		MINIMAP_TEXTDRAW_POS_X - MINIMAP_TEXTDRAW_BORDER_SIZE, MINIMAP_TEXTDRAW_POS_Y - MINIMAP_TEXTDRAW_BORDER_SIZE, "LD_SPAC:white");
	PlayerTextDrawTextSize(playerid, pVcMinimapTextdraws[playerid][TD_BORDER],
		MINIMAP_TEXTDRAW_SIZE_X + (MINIMAP_TEXTDRAW_BORDER_SIZE * 2.0),
		MINIMAP_TEXTDRAW_SIZE_Y + (MINIMAP_TEXTDRAW_BORDER_SIZE * 2.0));
	PlayerTextDrawAlignment(playerid, pVcMinimapTextdraws[playerid][TD_BORDER], 1);
	PlayerTextDrawColor(playerid, pVcMinimapTextdraws[playerid][TD_BORDER], 255);
	PlayerTextDrawSetShadow(playerid, pVcMinimapTextdraws[playerid][TD_BORDER], 0);
	PlayerTextDrawBackgroundColor(playerid, pVcMinimapTextdraws[playerid][TD_BORDER], 255);
	PlayerTextDrawFont(playerid, pVcMinimapTextdraws[playerid][TD_BORDER], 4);
	PlayerTextDrawSetProportional(playerid, pVcMinimapTextdraws[playerid][TD_BORDER], 0);

	// Peta
	pVcMinimapTextdraws[playerid][TD_MAP] = CreatePlayerTextDraw(playerid,
		MINIMAP_TEXTDRAW_POS_X, MINIMAP_TEXTDRAW_POS_Y, MINIMAP_MODEL":1");
	PlayerTextDrawTextSize(playerid, pVcMinimapTextdraws[playerid][TD_MAP], MINIMAP_TEXTDRAW_SIZE_X, MINIMAP_TEXTDRAW_SIZE_Y);
	PlayerTextDrawAlignment(playerid, pVcMinimapTextdraws[playerid][TD_MAP], 1);
	PlayerTextDrawColor(playerid, pVcMinimapTextdraws[playerid][TD_MAP], -1);
	PlayerTextDrawSetShadow(playerid, pVcMinimapTextdraws[playerid][TD_MAP], 0);
	PlayerTextDrawBackgroundColor(playerid, pVcMinimapTextdraws[playerid][TD_MAP], 255);
	PlayerTextDrawFont(playerid, pVcMinimapTextdraws[playerid][TD_MAP], 4);
	PlayerTextDrawSetProportional(playerid, pVcMinimapTextdraws[playerid][TD_MAP], 0);

	pVcMinimapTextdraws[playerid][TD_ICON] = PlayerText:INVALID_TEXT_DRAW;
	pVcMinimapCurrentArea[playerid] = 0;   // di luar Vice City sampai pembaruan pertama: belum ada yang tampil
	pVcMinimapIconCompass[playerid] = -2;

	pVcMinimapTimer[playerid] = SetTimerEx("OnVcMinimapRequestUpdate", MINIMAP_UPDATE_INTERVAL, true, "i", playerid);
	pVcMinimap[playerid] = true;
	UpdateVcMinimap(playerid);
	return 1;
}

static DestroyPlayerVcMinimapIcon(playerid)
{
	if (pVcMinimapTextdraws[playerid][TD_ICON] != PlayerText:INVALID_TEXT_DRAW)
	{
		PlayerTextDrawDestroy(playerid, pVcMinimapTextdraws[playerid][TD_ICON]);
		pVcMinimapTextdraws[playerid][TD_ICON] = PlayerText:INVALID_TEXT_DRAW;
	}
	pVcMinimapIconCompass[playerid] = -2;
}

static DestroyPlayerVcMinimap(playerid)
{
	if (!pVcMinimap[playerid]) return 0;

	if (pVcMinimapTimer[playerid] != -1)
	{
		KillTimer(pVcMinimapTimer[playerid]);
		pVcMinimapTimer[playerid] = -1;
	}
	for (new i = 0; i < 3; i++)
	{
		if (pVcMinimapTextdraws[playerid][i] != PlayerText:INVALID_TEXT_DRAW)
		{
			PlayerTextDrawDestroy(playerid, pVcMinimapTextdraws[playerid][i]);
			pVcMinimapTextdraws[playerid][i] = PlayerText:INVALID_TEXT_DRAW;
		}
	}
	pVcMinimap[playerid] = false;
	return 1;
}

static UpdateVcMinimap(playerid)
{
	new Float:x, Float:y, Float:z;
	if (!GetPlayerPos(playerid, x, y, z)) return 0;

	new area = GetVcMinimapArea(x, y);
	if (area != pVcMinimapCurrentArea[playerid])
	{
		pVcMinimapCurrentArea[playerid] = area;
		if (area == 0)
		{
			// Keluar dari Vice City: minimap disembunyikan, radar San Andreas terlihat lagi.
			PlayerTextDrawHide(playerid, pVcMinimapTextdraws[playerid][TD_BORDER]);
			PlayerTextDrawHide(playerid, pVcMinimapTextdraws[playerid][TD_MAP]);
			DestroyPlayerVcMinimapIcon(playerid);
			return 1;
		}
		new td_str[32];
		format(td_str, sizeof td_str, MINIMAP_MODEL":%d", area);
		PlayerTextDrawSetString(playerid, pVcMinimapTextdraws[playerid][TD_MAP], td_str);
		PlayerTextDrawShow(playerid, pVcMinimapTextdraws[playerid][TD_BORDER]);
		PlayerTextDrawShow(playerid, pVcMinimapTextdraws[playerid][TD_MAP]);
		pVcMinimapIconCompass[playerid] = -2;   // bagian peta berganti: ikon digambar ulang
	}
	if (area == 0) return 1;

	// Ikon pemain
	new Float:td_x, Float:td_y, Float:angle;
	Vc3dTo2d(x, y, td_x, td_y, minimapAreas_Coords[area - 1][0], minimapAreas_Coords[area - 1][1],
		minimapAreas_Coords[area - 1][2], minimapAreas_Coords[area - 1][3]);
	if (IsPlayerInAnyVehicle(playerid)) GetVehicleZAngle(GetPlayerVehicleID(playerid), angle);
	else GetPlayerFacingAngle(playerid, angle);
	new compass = GetCompassByAngle(angle);

	if (compass == pVcMinimapIconCompass[playerid]
		&& floatabs(td_x - pVcMinimapIconX[playerid]) < MINIMAP_ICON_STEP
		&& floatabs(td_y - pVcMinimapIconY[playerid]) < MINIMAP_ICON_STEP) return 1;

	DestroyPlayerVcMinimapIcon(playerid);
	pVcMinimapIconX[playerid] = td_x;
	pVcMinimapIconY[playerid] = td_y;
	pVcMinimapIconCompass[playerid] = compass;

	new icon[40];
	GetPlayerIconByCompass(compass, icon, sizeof icon);
	pVcMinimapTextdraws[playerid][TD_ICON] = CreatePlayerTextDraw(playerid,
		td_x - (MINIMAP_TEXTDRAW_ICON_SIZE_X / 2.0), td_y - (MINIMAP_TEXTDRAW_ICON_SIZE_Y / 2.0), icon);
	PlayerTextDrawTextSize(playerid, pVcMinimapTextdraws[playerid][TD_ICON], MINIMAP_TEXTDRAW_ICON_SIZE_X, MINIMAP_TEXTDRAW_ICON_SIZE_Y);
	PlayerTextDrawFont(playerid, pVcMinimapTextdraws[playerid][TD_ICON], 4);
	PlayerTextDrawColor(playerid, pVcMinimapTextdraws[playerid][TD_ICON], 0xCCCCCCFF);
	PlayerTextDrawShow(playerid, pVcMinimapTextdraws[playerid][TD_ICON]);
	return 1;
}

static GetPlayerIconByCompass(compass, icon[], size)
{
	switch (compass)
	{
		case 0: format(icon, size, MINIMAP_MODEL":player_icon_n");
		case 1: format(icon, size, MINIMAP_MODEL":player_icon_nw");
		case 2: format(icon, size, MINIMAP_MODEL":player_icon_w");
		case 3: format(icon, size, MINIMAP_MODEL":player_icon_sw");
		case 4: format(icon, size, MINIMAP_MODEL":player_icon_s");
		case 5: format(icon, size, MINIMAP_MODEL":player_icon_se");
		case 6: format(icon, size, MINIMAP_MODEL":player_icon_e");
		case 7: format(icon, size, MINIMAP_MODEL":player_icon_ne");
		default: format(icon, size, MINIMAP_MODEL":player_icon");
	}
}

static GetCompassByAngle(Float:angle)
{
	while (angle < 0.0) angle += 360.0;
	while (angle >= 360.0) angle -= 360.0;
	if (angle >= 337.5 || angle <= 22.5) return 0;   // n
	else if (angle <= 67.5) return 1;                // nw
	else if (angle <= 112.5) return 2;               // w
	else if (angle <= 157.5) return 3;               // sw
	else if (angle <= 202.5) return 4;               // s
	else if (angle <= 247.5) return 5;               // se
	else if (angle <= 292.5) return 6;               // e
	return 7;                                        // ne
}

// Posisi dunia -> posisi di textdraw peta, untuk bagian peta dengan batas yang diberikan.
static Vc3dTo2d(Float:x, Float:y, &Float:td_x, &Float:td_y, Float:minX, Float:minY, Float:maxX, Float:maxY)
{
	if (x > maxX) x = maxX;
	else if (x < minX) x = minX;
	if (y > maxY) y = maxY;
	else if (y < minY) y = minY;

	td_x = MINIMAP_TEXTDRAW_POS_X + (x - minX) * (MINIMAP_TEXTDRAW_SIZE_X / (maxX - minX));
	td_y = MINIMAP_TEXTDRAW_POS_Y + (maxY - y) * (MINIMAP_TEXTDRAW_SIZE_Y / (maxY - minY));
}
