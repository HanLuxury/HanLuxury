/* EAGLE modular custom ped system. CEF previews stay entirely on the client.
 * Ped carriers: 158/cwmofr male, 298/cat female. Never model 0/CJ.
 * SQL migration 001_eagle_character.sql is required before this include runs.
 */
#if defined _eagle_modular_character
    #endinput
#endif
#define _eagle_modular_character
#include <YSI/y_hooks>
#include "SERVER/player/character/character_catalog"
#define EC_FIELDS (22)
#define EC_RPC (1161905996)
#define EC_PACKET (251) // client vendor/raknet/PacketEnumerations.h: ID_CUSTOM_RPC
#define EC_SLOT_HAIR (4)
#define EC_SLOT_TOP (9)
#define EC_SLOT_JACKET (10)
#define EC_SLOT_PANTS (11)
#define EC_SLOT_SHOES (12)
#define EC_SLOT_HAT (13)
#define EC_SLOT_GLASSES (14)
#define EC_SLOT_MASK (15)
#define EC_SLOT_WATCH (16)
#define EC_SLOT_NECKLACE (17)
#define EC_SLOT_BAG (18)
#define EC_SLOT_ACCESSORY (19)
new gECLook[MAX_PLAYERS][EC_FIELDS], gECRevision[MAX_PLAYERS], gECSession[MAX_PLAYERS];
new gECNonce[MAX_PLAYERS], gECAccount[MAX_PLAYERS];
new bool:gECLoaded[MAX_PLAYERS], bool:gECOpen[MAX_PLAYERS], bool:gECShop[MAX_PLAYERS], bool:gECCreator[MAX_PLAYERS];
new bool:gECLoading[MAX_PLAYERS], gECGeneration[MAX_PLAYERS], gECSerial=1;
new bool:gECOwned[MAX_PLAYERS][sizeof(gECatalog)];
new gECLastAction[MAX_PLAYERS];
new bool:gECCreated[MAX_PLAYERS], bool:gECSpawnPending[MAX_PLAYERS];
new bool:gECIsolated[MAX_PLAYERS], gECReturnWorld[MAX_PLAYERS];

stock EC_CarrierForGender(gender) {return gender==2 ? EC_FEMALE_PED:EC_MALE_PED;}
stock bool:EC_BlocksSpawn(playerid) {return gECSpawnPending[playerid] || gECOpen[playerid];}

// Called by the existing spawn menu, not by an untrusted CEF request.
stock bool:EC_PrepareSpawn(playerid)
{
    if(!AccountData[playerid][IsLoggedIn]) return false;
    if(gECLoaded[playerid] && gECCreated[playerid]) return false;
    gECSpawnPending[playerid]=true;
    TogglePlayerControllable(playerid,0);
    if(!gECLoaded[playerid]) ClothesCEF_LoadDbState(playerid);
    else EC_ResumeSpawn(playerid);
    return true;
}
stock EC_ResumeSpawn(playerid)
{
    if(!gECSpawnPending[playerid] || !gECLoaded[playerid]) return 0;
    if(gECCreated[playerid])
    {
        gECSpawnPending[playerid]=false;
        return SpawnMenu(playerid,true);
    }
    if(!CEF_IsReady(playerid) || GetPlayerState(playerid)!=PLAYER_STATE_ONFOOT) return 0;
    Auth_Hide(playerid);
    if(!gECOpen[playerid]) return OpenCharacterCreator(playerid);
    return ClothesCEF_OnCEFReady(playerid);
}

stock EC_FindItem(item)
{
    for(new i=0;i<sizeof(gECatalog);i++) if(gECatalog[i][ecItem]==item) return i;
    return -1;
}
stock bool:EC_Validate(const look[EC_FIELDS])
{
    for(new i=0;i<EC_FIELDS;i++) if(look[i]<0 || look[i]>65535) return false;
    if(look[0]>1 || look[1]>2 || look[2]<1 || look[2]>EC_SKIN_TONES || look[3]<1 || look[3]>EC_FACE_COUNT || look[4]>EC_HAIR_COUNT || look[5]>=EC_HAIR_COLORS) return false;
    if(look[6]>2 || look[7]>2 || look[8]>2 || look[20]>2 || look[21]>2 || (look[0] && look[7])) return false;
    for(new f=9;f<=19;f++) if(look[f])
    {
        new row=EC_FindItem(look[f]);
        if(row<0 || gECatalog[row][ecSlot]!=f || !(gECatalog[row][ecBodies] & (1 << (look[0]*3+look[1])))) return false;
    }
    return true;
}
stock bool:EC_Identity(playerid, generation, account)
{
    return IsPlayerConnected(playerid) && gECGeneration[playerid]==generation && AccountData[playerid][pID]==account && AccountData[playerid][IsLoggedIn];
}
stock EC_Notice(playerid,const text[])
{
    J_Begin(); J_Str("message",text); J_Send(playerid,"eagle_character_error");
    ShowTDN(playerid,NOTIFICATION_ERROR,text);return 0;
}
stock EC_UIState(playerid)
{
    if(!gECLoaded[playerid]) return 0;
    J_Begin(); J_Int("revision",gECRevision[playerid]); J_Int("nonce",gECNonce[playerid]);
    J_Int("money",AccountData[playerid][pMoney]); J_Int("catalogRevision",EC_CATALOG_REVISION);
    J_Obj("prices");
    for(new row=0;row<sizeof(gECatalog);row++)
    {
        new key[12];
        valstr(key,gECatalog[row][ecItem]);
        J_Int(key,gECatalog[row][ecPrice]);
    }
    J_End();
    J_Obj("appearance");
    for(new i=0;i<EC_FIELDS;i++)
    {
        J_Int(gECJsonFields[i],gECLook[playerid][i]);
    }
    J_End();
    J_Arr("owned");
    for(new i=0;i<sizeof(gECatalog);i++)
    {
        if(gECOwned[playerid][i])
        {
            J_AInt(gECatalog[i][ecItem]);
        }
    }
    J_End();
    return J_Send(playerid,"eagle_character_server_state");
}
stock EC_Write16(BitStream:bs,value)
{
    BS_WriteUint8(bs,value & 255); BS_WriteUint8(bs,(value >>> 8) & 255);
}
stock EC_Write32(BitStream:bs,value)
{
    for(new shift=0;shift<32;shift+=8) BS_WriteUint8(bs,(value >>> shift) & 255);
}
stock EC_SendAppearance(playerid,target)
{
    if(!gECLoaded[playerid] || !IsPlayerConnected(target)) return 0;
    new BitStream:bs=BS_New(); BS_WriteUint8(bs,EC_PACKET); EC_Write32(bs,EC_RPC);
    BS_WriteUint8(bs,1); BS_WriteUint8(bs,1); EC_Write16(bs,playerid);
    EC_Write32(bs,gECSession[playerid]); EC_Write32(bs,gECRevision[playerid]); BS_WriteUint8(bs,1);
    for(new i=0;i<EC_FIELDS;i++) EC_Write16(bs,gECLook[playerid][i]);
    new result=PR_SendPacket(bs,target,PR_HIGH_PRIORITY,PR_RELIABLE_ORDERED,0); BS_Delete(bs); return result;
}
stock SyncPlayerAppearance(playerid)
{
    EC_SendAppearance(playerid,playerid);
    foreach(new other : Player) if(other!=playerid && IsPlayerStreamedIn(playerid,other)) EC_SendAppearance(playerid,other);
    return 1;
}
stock EC_ApplyCarrier(playerid)
{
    new skin=gECLook[playerid][0] ? EC_FEMALE_PED:EC_MALE_PED;
    AccountData[playerid][pSkin]=skin;AccountData[playerid][pGender]=gECLook[playerid][0]+1;
    // Uniforms intentionally keep their native model; modular rendering only
    // applies to the two carrier models on the client.
    if(!AccountData[playerid][pUsingUniform] && GetPlayerSkin(playerid)!=skin) SetPlayerSkin(playerid,skin);
    SyncPlayerAppearance(playerid);
}
stock ClothesCEF_LoadDbState(playerid)
{
    new account=AccountData[playerid][pID];
    if(account<=0 || !AccountData[playerid][IsLoggedIn] || gECLoading[playerid]) return 0;
    if(gECLoaded[playerid] && gECAccount[playerid]==account) return 1;
    // An account/character switch invalidates callbacks from the previous one.
    gECGeneration[playerid]++;gECLoading[playerid]=true;gECLoaded[playerid]=false;gECAccount[playerid]=account;
    new query[512];
    mysql_format(g_SQL,query,sizeof(query),"INSERT IGNORE INTO player_appearance(player_id,gender) VALUES(%d,%d)",account,(AccountData[playerid][pGender]==2 ? 1:0));mysql_tquery(g_SQL,query);
    mysql_format(g_SQL,query,sizeof(query),"INSERT IGNORE INTO player_clothes_inventory(player_id,item_id) SELECT %d,item_id FROM clothes_catalog WHERE item_id IN(101,301,401)",account);mysql_tquery(g_SQL,query);
    mysql_format(g_SQL,query,sizeof(query),"SELECT a.*,p.Char_ClothesEpoch FROM player_appearance a JOIN player_characters p ON p.pID=a.player_id WHERE a.player_id=%d",account);
    mysql_tquery(g_SQL,query,"EC_OnLoaded","ddd",playerid,gECGeneration[playerid],account);return 1;
}
forward EC_OnLoaded(playerid,generation,account);
public EC_OnLoaded(playerid,generation,account)
{
    if(!EC_Identity(playerid,generation,account)) return 1;
    new rows;cache_get_row_count(rows);if(rows!=1) {gECLoading[playerid]=false;printf("[EagleCharacter] appearance load failed account=%d",account);return EC_Notice(playerid,"Data karakter belum termuat. Periksa migrasi SQL; gunakan /character untuk mencoba lagi.");}
    new look[EC_FIELDS],rev,epoch,created;
    for(new i=0;i<EC_FIELDS;i++) cache_get_value_name_int(0,gECFields[i],look[i]);
    cache_get_value_name_int(0,"revision",rev);cache_get_value_name_int(0,"Char_ClothesEpoch",epoch);
    cache_get_value_name_int(0,"created",created);
    if(!EC_Validate(look) || rev<1 || epoch<0) {gECLoading[playerid]=false;return EC_Notice(playerid,"Data karakter tidak valid; hubungi admin.");}
    gECCreated[playerid]=(created!=0);
    for(new i=0;i<EC_FIELDS;i++) gECLook[playerid][i]=look[i];
    gECRevision[playerid]=rev;gECEpoch[playerid]=epoch;gECSession[playerid]=++gECSerial;
    new query[256];mysql_format(g_SQL,query,sizeof(query),"SELECT item_id FROM player_clothes_inventory WHERE player_id=%d",account);
    mysql_tquery(g_SQL,query,"EC_OnInventory","ddd",playerid,generation,account);return 1;
}
forward EC_OnInventory(playerid,generation,account);
public EC_OnInventory(playerid,generation,account)
{
    if(!EC_Identity(playerid,generation,account)) return 1;
    for(new i=0;i<sizeof(gECatalog);i++) gECOwned[playerid][i]=false;
    new rows;cache_get_row_count(rows);gECLoading[playerid]=false;if(rows>2048) return EC_Notice(playerid,"Inventory pakaian melewati batas.");
    for(new i=0;i<rows;i++) {new item;cache_get_value_name_int(i,"item_id",item);new row=EC_FindItem(item);if(row>=0) gECOwned[playerid][row]=true;}
    gECLoaded[playerid]=true;EC_ApplyCarrier(playerid);EC_UIState(playerid);EC_ResumeSpawn(playerid);return 1;
}
stock ClothesCEF_Open(playerid,bool:shopMode=false)
{
    if(!AccountData[playerid][IsLoggedIn] || !gECLoaded[playerid]) return EC_Notice(playerid,"Karakter sedang dimuat.");
    if(!CEF_IsReady(playerid)) return EC_Notice(playerid,"CEF belum siap. Tunggu lalu gunakan /character lagi.");
    if(AccountData[playerid][pUsingUniform]) return EC_Notice(playerid,"Lepaskan seragam sebelum mengganti pakaian.");
    if(GetPlayerState(playerid)!=PLAYER_STATE_ONFOOT) return EC_Notice(playerid,"Buka menu saat berjalan kaki.");
    if(shopMode && Warung_Nearest(playerid)==-1) return EC_Notice(playerid,"Anda harus berada di toko pakaian.");
    gECOpen[playerid]=true;gECShop[playerid]=shopMode;gECCreator[playerid]=false;
    gECNonce[playerid]=random(1000000000)+1;
    TogglePlayerControllable(playerid,0);AllGui_Status(playerid,false);
    EC_UIState(playerid);J_Begin();J_Str("mode",shopMode ? "shop":"wardrobe");J_Int("nonce",gECNonce[playerid]);J_Send(playerid,"eagle_character_open");return 1;
}
stock OpenCharacterCreator(playerid)
{
    if(!ClothesCEF_Open(playerid,false)) return 0;
    if(!gECOpen[playerid]) return 0;
    gECCreator[playerid]=true;
    if(gECSpawnPending[playerid] && !gECIsolated[playerid])
    {
        gECReturnWorld[playerid]=GetPlayerVirtualWorld(playerid);gECIsolated[playerid]=true;
        SetPlayerVirtualWorld(playerid,60000+playerid);
    }
    J_Begin();J_Str("mode","creator");J_Int("nonce",gECNonce[playerid]);J_Bool("required",gECSpawnPending[playerid] && !gECCreated[playerid]);J_Send(playerid,"eagle_character_open");return 1;
}
stock EC_Close(playerid,bool:force=false)
{
    if(!force && gECSpawnPending[playerid] && !gECCreated[playerid]) return EC_Notice(playerid,"Simpan karakter terlebih dahulu sebelum memilih lokasi spawn.");
    if(!gECOpen[playerid]) return 1;
    gECOpen[playerid]=false;gECNonce[playerid]=0;gECShop[playerid]=false;gECCreator[playerid]=false;
    if(gECIsolated[playerid]) {SetPlayerVirtualWorld(playerid,gECReturnWorld[playerid]);gECIsolated[playerid]=false;}
    if(!force) {TogglePlayerControllable(playerid,1);SetCameraBehindPlayer(playerid);AllGui_Status(playerid,true);}
    J_Begin();J_Bool("show",false);J_Send(playerid,"eagle_character_close");
    if(!force && gECSpawnPending[playerid] && gECCreated[playerid])
    {
        EC_ResumeSpawn(playerid);TimersSpawn(playerid);
    }
    return 1;
}
/* Strict decimal CSV: nonce,revision,22 appearance fields. No path/price accepted. */
stock bool:EC_Parse(const text[],out[24])
{
    new field=0,value=0,digits=0;
    // Same limit as before: up to 299 payload characters followed by EOS.
    for(new p=0;p<300;p++)
    {
        new c=text[p];
        if(c>='0' && c<='9')
        {
            if(++digits>10 || value>214748364 || (value==214748364 && c>'7')) return false;
            value=value*10+c-'0';
        }
        else if(c==',' || c==EOS)
        {
            if(!digits || field>=24) return false;
            out[field++]=value;
            value=0;
            digits=0;
            if(c==EOS) return field==24;
        }
        else
        {
            return false;
        }
    }
    return false;
}
/* Reasons from eagle_commit_appearance that leave the database untouched. stale_revision,
   database_error and a missing answer stay fatal: the saved state is then unknown. */
stock bool:EC_IsCleanRejection(const reason[])
{
    if(reason[0]==EOS) return false;
    return !strcmp(reason,"invalid_appearance") || !strcmp(reason,"unavailable_item") || !strcmp(reason,"price_changed")
        || !strcmp(reason,"already_owned") || !strcmp(reason,"purchase_not_equipped") || !strcmp(reason,"insufficient_money")
        || !strcmp(reason,"not_owned_or_incompatible");
}
stock EC_RejectionText(const reason[])
{
    new text[96];
    if(!strcmp(reason,"insufficient_money")) text="Uang Anda tidak cukup.";
    else if(!strcmp(reason,"already_owned")) text="Pakaian ini sudah Anda miliki.";
    else if(!strcmp(reason,"not_owned_or_incompatible")) text="Ada pakaian yang belum dimiliki atau tidak cocok dengan bentuk tubuh.";
    else if(!strcmp(reason,"price_changed")) text="Harga di database berbeda dengan gamemode. Hubungi admin.";
    else if(!strcmp(reason,"unavailable_item")) text="Pakaian ini tidak tersedia di katalog database. Hubungi admin.";
    else text="Pilihan ditolak server. Coba lagi.";
    return text;
}
stock EC_Commit(playerid,const look[EC_FIELDS],buy=0)
{
    if(!gECLoaded[playerid] || !EC_Validate(look)) return 0;
    for(new f=9;f<=19;f++) if(look[f])
    {
        new row=EC_FindItem(look[f]);if(row<0 || (!gECOwned[playerid][row] && look[f]!=buy)) return EC_Notice(playerid,"Pakaian belum dimiliki.");
    }
    if(buy)
    {
        new row=EC_FindItem(buy);
        if(row<0 || gECOwned[playerid][row] || AccountData[playerid][pMoney]<gECatalog[row][ecPrice]) return EC_Notice(playerid,"Uang kurang atau item sudah dimiliki.");
    }
    new query[1536],part[24],quote=0;
    if(buy) quote=gECatalog[EC_FindItem(buy)][ecPrice];
    format(query,sizeof(query),"CALL eagle_commit_appearance(%d,%d,%d,%d,%d,%d",AccountData[playerid][pID],gECRevision[playerid],gECEpoch[playerid],AccountData[playerid][pMoney],buy,quote);
    for(new i=0;i<EC_FIELDS;i++)
    {
        format(part,sizeof(part),",%d",look[i]);
        strcat(query,part,sizeof(query));
    }
    strcat(query,")",sizeof(query));
    // One synchronous transaction is intentional: other AMX money callbacks
    // cannot run between the validated cash snapshot and its commit result.
    // Legacy async wallet writes carry an epoch guard (see integration patch).
    new Cache:result=mysql_query(g_SQL,query,true);
    new rows=0,ok=0,cash=0,epoch=0,revision=0,reason[32];
    if(result!=Cache:0)
    {
        cache_get_row_count(rows);if(rows) cache_get_value_name_int(0,"ok",ok);
        if(ok) {cache_get_value_name_int(0,"cash",cash);cache_get_value_name_int(0,"epoch",epoch);cache_get_value_name_int(0,"revision",revision);}
        else if(rows) cache_get_value_name(0,"reason",reason);
        cache_delete(result);
    }
    // These answers come after ROLLBACK and change nothing: wallet, inventory and
    // revision are the same as before, so the player stays and can try again.
    if(!ok && EC_IsCleanRejection(reason))
    {
        printf("[EagleCharacter] commit refused account=%d rev=%d buy=%d reason=%s",AccountData[playerid][pID],gECRevision[playerid],buy,reason);
        return EC_Notice(playerid,EC_RejectionText(reason));
    }
    if(!ok)
    {
        // An interrupted connection after COMMIT is ambiguous. Never guess or
        // refund blindly. Re-login reloads the durable wallet/inventory.
        EC_Notice(playerid,"Transaksi tidak disahkan. Login ulang untuk memuat saldo terbaru.");
        printf("[EagleCharacter] commit rejected/uncertain account=%d rev=%d",AccountData[playerid][pID],gECRevision[playerid]);
        EC_Close(playerid,true);gECLoaded[playerid]=false;Kick(playerid);return 0;
    }
    AccountData[playerid][pMoney]=cash;gECEpoch[playerid]=epoch;gECRevision[playerid]=revision;
    gECCreated[playerid]=true;
    ResetPlayerMoney(playerid);GivePlayerMoney(playerid,cash);
    for(new i=0;i<EC_FIELDS;i++) gECLook[playerid][i]=look[i];
    if(buy) gECOwned[playerid][EC_FindItem(buy)]=true;
    EC_ApplyCarrier(playerid);EC_UIState(playerid);
    J_Begin();J_Int("revision",revision);J_Send(playerid,"eagle_character_saved");
    printf("[EagleCharacter] committed account=%d rev=%d buy=%d",AccountData[playerid][pID],revision,buy);return 1;
}
stock EC_OnUI(playerid,const action[],item,const text[])
{
    if(!strcmp(action,"close")) return EC_Close(playerid);
    if(action[0]==EOS || (strcmp(action,"save") && strcmp(action,"buy")) || !gECOpen[playerid]) return 1;
    // The menu shows "Menyimpan..." until it hears back: every save/buy gets an answer.
    if(!gECLoaded[playerid] || !AccountData[playerid][IsLoggedIn]) return EC_Notice(playerid,"Karakter sedang dimuat. Coba lagi.");
    if(GetPlayerState(playerid)!=PLAYER_STATE_ONFOOT) return EC_Notice(playerid,"Simpan pakaian saat berjalan kaki.");
    if(GetTickCount()-gECLastAction[playerid]<500) return EC_Notice(playerid,"Tunggu sebentar, lalu coba lagi.");
    gECLastAction[playerid]=GetTickCount();
    new data[24],look[EC_FIELDS];
    if(!EC_Parse(text,data) || data[0]!=gECNonce[playerid] || data[1]!=gECRevision[playerid]) {EC_UIState(playerid);return EC_Notice(playerid,"Sesi berubah. Coba pilihan Anda lagi.");}
    for(new i=0;i<EC_FIELDS;i++) look[i]=data[i+2];
    if(!EC_Validate(look)) return EC_Notice(playerid,"Pilihan karakter tidak valid.");
    if(!gECCreator[playerid])
    {
        for(new f=0;f<9;f++) if(look[f]!=gECLook[playerid][f]) return EC_Notice(playerid,"Gunakan character creator untuk mengubah tubuh/wajah.");
        if(look[20]!=gECLook[playerid][20] || look[21]!=gECLook[playerid][21]) return EC_Notice(playerid,"Gunakan character creator untuk mengubah tattoo/freckles.");
    }
    if(!strcmp(action,"buy"))
    {
        if(!gECShop[playerid] || Warung_Nearest(playerid)==-1) return EC_Notice(playerid,"Pembelian hanya tersedia di toko.");
        return EC_Commit(playerid,look,item);
    }
    if(EC_Commit(playerid,look,0)) EC_Close(playerid);return 1;
}
stock ClothesCEF_OnUI(playerid,const view[],const action[],item,const text[])
{
    #pragma unused view
    return EC_OnUI(playerid,action,item,text);
}
stock ClothesCEF_OnCEFReady(playerid)
{
    EC_UIState(playerid);SyncPlayerAppearance(playerid);
    if(gECOpen[playerid]) {Auth_Hide(playerid);J_Begin();J_Str("mode",gECCreator[playerid] ? "creator":(gECShop[playerid] ? "shop":"wardrobe"));J_Int("nonce",gECNonce[playerid]);J_Bool("required",gECSpawnPending[playerid] && !gECCreated[playerid]);J_Send(playerid,"eagle_character_open");}
    else EC_ResumeSpawn(playerid);
    return 1;
}
stock EC_SetField(playerid,field,value)
{
    if(!gECLoaded[playerid] || field<0 || field>=EC_FIELDS) return 0;
    new look[EC_FIELDS];for(new i=0;i<EC_FIELDS;i++) look[i]=gECLook[playerid][i];look[field]=value;
    if(field==0 && value==1) look[7]=0;
    return EC_Commit(playerid,look);
}
stock SetPlayerGender(playerid,value) {return EC_SetField(playerid,0,value);}
stock SetPlayerBodyType(playerid,value) {return EC_SetField(playerid,1,value);}
stock SetPlayerSkinTone(playerid,value) {return EC_SetField(playerid,2,value);}
stock SetPlayerFace(playerid,value) {return EC_SetField(playerid,3,value);}
stock SetPlayerHair(playerid,value) {return EC_SetField(playerid,4,value);}
stock SetPlayerHairColor(playerid,value) {return EC_SetField(playerid,5,value);}
stock SetPlayerClothes(playerid,slot,item) {if(slot!=4 && (slot<9 || slot>19)) return 0;return EC_SetField(playerid,slot,item);}
stock RemovePlayerClothes(playerid,slot) {return SetPlayerClothes(playerid,slot,0);}
stock GetPlayerSkinTone(playerid) {return gECLoaded[playerid] ? gECLook[playerid][2]:-1;}
stock GetPlayerFace(playerid) {return gECLoaded[playerid] ? gECLook[playerid][3]:-1;}
stock GetPlayerHair(playerid) {return gECLoaded[playerid] ? gECLook[playerid][4]:-1;}
stock GetPlayerClothes(playerid,slot) {return gECLoaded[playerid] && (slot==4 || (slot>=9 && slot<=19)) ? gECLook[playerid][slot]:-1;}

CMD:character(playerid,params[]) {
    #pragma unused params
    if(gECSpawnPending[playerid]) {if(!gECLoaded[playerid]) ClothesCEF_LoadDbState(playerid);else EC_ResumeSpawn(playerid);return 1;}
    return OpenCharacterCreator(playerid);
}
CMD:clothes(playerid,params[]) {
    #pragma unused params
    return ClothesCEF_Open(playerid,false);
}
CMD:pakaian(playerid,params[]) {
    #pragma unused params
    return ClothesCEF_Open(playerid,false);
}
CMD:baju(playerid,params[]) {
    #pragma unused params
    return ClothesCEF_Open(playerid,true);
}
hook OnPlayerConnect(playerid)
{
    gECGeneration[playerid]++;gECLoaded[playerid]=false;gECLoading[playerid]=false;gECOpen[playerid]=false;gECEpoch[playerid]=0;gECLastAction[playerid]=GetTickCount()-1000;
    gECCreated[playerid]=false;gECSpawnPending[playerid]=false;gECIsolated[playerid]=false;
    gECShop[playerid]=false;gECCreator[playerid]=false;gECNonce[playerid]=0;gECAccount[playerid]=0;
    return 1;
}
hook OnPlayerDisconnect(playerid,reason)
{
    #pragma unused reason
    gECGeneration[playerid]++;gECLoaded[playerid]=false;gECLoading[playerid]=false;gECOpen[playerid]=false;
    // Keep the original-world snapshot until OnPlayerConnect resets this slot:
    // the main disconnect callback may save after this YSI hook.
    gECSpawnPending[playerid]=false;return 1;
}
hook OnPlayerDeath(playerid,killerid,reason)
{
    #pragma unused killerid,reason
    EC_Close(playerid,true);return 1;
}
hook OnPlayerSpawn(playerid)
{
    SetTimerEx("EC_AfterSpawn",250,false,"ddd",playerid,gECGeneration[playerid],AccountData[playerid][pID]);return 1;
}
forward EC_AfterSpawn(playerid,generation,account);
public EC_AfterSpawn(playerid,generation,account)
{
    if(!EC_Identity(playerid,generation,account)) return 1;
    if(gECLoaded[playerid]) {EC_ApplyCarrier(playerid);EC_ResumeSpawn(playerid);}
    else if(!gECLoading[playerid]) ClothesCEF_LoadDbState(playerid);return 1;
}
hook OnPlayerStreamIn(playerid,forplayerid) {EC_SendAppearance(playerid,forplayerid);return 1;}
