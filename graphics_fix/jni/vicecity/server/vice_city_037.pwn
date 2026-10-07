/*
	Vice City untuk server SA-MP 0.3.7 + client HolySAMP (Android, GTA SA 2.10 arm64).

	Di SA-MP 0.3.DL (PC) seluruh map dikirim server: 2412 model lewat AddSimpleModel dan 7498 objek lewat
	plugin streamer. Protokol 0.3.7 tidak punya AddSimpleModel dan hanya mengizinkan 1000 objek per pemain,
	padahal di titik terpadat map ini butuh sekitar 1650 objek sekaligus.

	Karena itu di client HolySAMP map-nya dipasang oleh CLIENT sendiri (jni/vicecity): model, tekstur dan
	collision dibaca dari folder TESTLIT/vice_city di HP, dan penempatan semua objeknya sudah tertanam di
	client. Server cukup memberi tahu bahwa ia memakai map ini. Caranya: server membuat objek yang SAMA
	PERSIS (model dan posisi) dengan salah satu penempatan map. Begitu client melihat objek seperti itu,
	map dipasang (Mode = auto di vice_city.ini), dan objek kiriman server itu sendiri tidak dibuat dobel.

	Filterscript ini membuat dua "suar" seperti itu: dua pohon palem yang memang bagian dari map (baris 7777
	dan 7778 vice_city.pwn asli). Modelnya model San Andreas biasa (620), jadi client lain - termasuk SA-MP
	0.3.7 PC - hanya melihat dua pohon di tengah laut dan tidak terganggu.

	Hanya butuh a_samp (tanpa plugin streamer, tanpa zcmd). Jalan di server SA-MP 0.3.7 dan open.mp.

	Perintah:
	  /gotovc        pindah ke Vice City
	  /vcint [1-29]  pindah ke salah satu interior (tanpa angka: daftar)
	  /day, /night   jam 12:00 / 00:00 (model siang/malam map ikut jam game)

	Sumber map: https://github.com/casualmind/samp-vice-city
	Kredit map: GTA United 1.2 TEAM, Graber, adri1, kaizer.
*/

#include <a_samp>

// Lama pemain ditahan setelah dipindahkan, supaya objek di sekitarnya sempat dibuat client.
#define VC_FREEZE_MS 2000

#define VC_COLOR_INFO 0x95CAFCFF

forward VcUnfreeze(playerid);

enum E_VC_INTERIOR
{
	Float:vcIntX,
	Float:vcIntY,
	Float:vcIntZ,
	Float:vcIntAngle,
	vcIntName[36]
};

// interiors.txt dari repo map. Interior diletakkan tinggi di atas kota (z sekitar 1000).
static const vcInteriors[][E_VC_INTERIOR] =
{
	{6306.95, 933.635, 1048.81, 0.0, "Scarface interior"},
	{6201.99, 587.149, 999.727, 180.0, "Hardware Store Washington Beach"},
	{6228.71, -227.186, 999.922, 75.0, "Ocean View Hotel"},
	{6490.03, 973.854, 998.531, 45.0, "Malibu Club Door 1"},
	{6487.55, 971.518, 998.531, 45.0, "Malibu Club Door 2"},
	{5940.5, -429.63, 997.087, 90.0, "Ammunation"},
	{5318.3, 2251.28, 997.179, 90.0, "Ammunation Downtown"},
	{5621.46, 494.428, 996.133, 180.0, "Mansion Principal Door"},
	{5663.56, 476.213, 1012.12, 270.0, "Mansion Terrace Door"},
	{5672.6, 471.018, 988.161, 270.0, "Mansion Pool Door"},
	{5101.99, 708.993, 994.023, 90.0, "El Banco Corrupto Grande"},
	{6448.64, 2053.69, 1213.64, 0.0, "North Point Mall Door 1"},
	{6379.67, 2052.79, 1213.65, 0.0, "North Point Mall Door 2"},
	{6470.98, 2173.61, 1212.78, 90.0, "North Point Mall Door 3"},
	{6357.09, 2174.61, 1213.48, 270.0, "North Point Mall Door 4"},
	{6448.21, 2299.29, 1213.48, 180.0, "North Point Mall Door 5"},
	{6379.62, 2299.88, 1213.61, 180.0, "North Point Mall Door 6"},
	{6096.09, -404.722, 998.031, 30.0, "Pole Position Club"},
	{6367.55, 2128.26, 997.609, 90.0, "Hardware Store Mall"},
	{5033.35, 354.851, 994.56, 90.0, "Hardware Store Little Havana"},
	{6121.11, 215.509, 1000.0, 250.0, "Ken Rosenberg's office"},
	{5402.77, 1700.65, 995.992, 180.0, "Greasy Chopper (Biker Bar)"},
	{6396.04, 576.625, 996.383, 135.0, "Washington Beach Police HQ"},
	{5037.27, 1197.05, 997.695, 0.0, "Auntie Poulet's"},
	{4935.41, 767.392, 998.069, 90.0, "Print Works"},
	{6407.9, 2082.15, 994.421, 270.0, "GASH Door 1"},
	{6407.75, 2082.1, 1000.73, 270.0, "GASH Door 2"},
	{6464.65, 2058.89, 997.458, 220.0, "Tarbrush Cafe"},
	{4830.08, 441.309, 1001.29, 180.0, "Cafe Robina"}
};

static vcBeacon[2] = {INVALID_OBJECT_ID, INVALID_OBJECT_ID};
static vcFreezeTimer[MAX_PLAYERS] = {-1, ...};

public OnFilterScriptInit()
{
	// Suar untuk client HolySAMP. JANGAN diubah model atau posisinya: harus sama dengan penempatan di map.
	vcBeacon[0] = CreateObject(620, 4764.63, 288.59, 7.11, 0.0, 0.0, 0.0, 300.0);
	vcBeacon[1] = CreateObject(620, 4817.31, 227.21, 5.93, 0.0, 0.0, 0.0, 300.0);
	print("[vice_city_037] Vice City: suar untuk client dibuat. Map dipasang oleh client HolySAMP.");
	return 1;
}

public OnFilterScriptExit()
{
	for (new i = 0; i < sizeof vcBeacon; i++)
	{
		if (vcBeacon[i] != INVALID_OBJECT_ID)
		{
			DestroyObject(vcBeacon[i]);
			vcBeacon[i] = INVALID_OBJECT_ID;
		}
	}
	for (new i = 0; i < MAX_PLAYERS; i++)
	{
		if (vcFreezeTimer[i] != -1)
		{
			KillTimer(vcFreezeTimer[i]);
			vcFreezeTimer[i] = -1;
			if (IsPlayerConnected(i)) TogglePlayerControllable(i, 1);
		}
	}
	return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
	if (vcFreezeTimer[playerid] != -1)
	{
		KillTimer(vcFreezeTimer[playerid]);
		vcFreezeTimer[playerid] = -1;
	}
	return 1;
}

public VcUnfreeze(playerid)
{
	vcFreezeTimer[playerid] = -1;
	if (IsPlayerConnected(playerid)) TogglePlayerControllable(playerid, 1);
	return 1;
}

static VcTeleport(playerid, Float:x, Float:y, Float:z, Float:angle)
{
	SetPlayerInterior(playerid, 0);
	SetPlayerPos(playerid, x, y, z);
	SetPlayerFacingAngle(playerid, angle);
	SetCameraBehindPlayer(playerid);

	// Ditahan sebentar: client membuat tanah dan bangunan di sekitar titik ini begitu pemain tiba.
	TogglePlayerControllable(playerid, 0);
	if (vcFreezeTimer[playerid] != -1) KillTimer(vcFreezeTimer[playerid]);
	vcFreezeTimer[playerid] = SetTimerEx("VcUnfreeze", VC_FREEZE_MS, false, "i", playerid);
}

public OnPlayerCommandText(playerid, cmdtext[])
{
	if (!strcmp(cmdtext, "/gotovc", true))
	{
		VcTeleport(playerid, 4563.418945, 168.347625, 15.361562, 0.0);
		SendClientMessage(playerid, VC_COLOR_INFO, "Selamat datang di Vice City.");
		return 1;
	}
	if (!strcmp(cmdtext, "/vcint", true, 6) && (cmdtext[6] == EOS || cmdtext[6] == ' '))
	{
		new number = (cmdtext[6] == EOS) ? 0 : strval(cmdtext[7]);
		if (number < 1 || number > sizeof vcInteriors)
		{
			new line[144];
			SendClientMessage(playerid, VC_COLOR_INFO, "Pemakaian: /vcint [1-29]");
			for (new i = 0; i < sizeof vcInteriors; i++)
			{
				format(line, sizeof line, "%d. %s", i + 1, vcInteriors[i][vcIntName]);
				SendClientMessage(playerid, -1, line);
			}
			return 1;
		}
		VcTeleport(playerid, vcInteriors[number - 1][vcIntX], vcInteriors[number - 1][vcIntY],
			vcInteriors[number - 1][vcIntZ], vcInteriors[number - 1][vcIntAngle]);
		SendClientMessage(playerid, VC_COLOR_INFO, vcInteriors[number - 1][vcIntName]);
		return 1;
	}
	if (!strcmp(cmdtext, "/day", true))
	{
		SetWorldTime(12);
		return 1;
	}
	if (!strcmp(cmdtext, "/night", true))
	{
		SetWorldTime(0);
		return 1;
	}
	return 0;
}
