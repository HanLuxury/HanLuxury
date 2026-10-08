#define EOS '\0'
#define MAX_PLAYERS 1000
#define PLAYER_STATE_ONFOOT 1
native printf(const format[], {Float,_}:...);
native format(output[], len, const format[], {Float,_}:...);
native strcat(dest[], const source[], maxlength=sizeof dest);
native strcmp(const string1[], const string2[], bool:ignorecase=false, length=cellmax);
native Kick(playerid);
native ResetPlayerMoney(playerid);
native GivePlayerMoney(playerid,money);
native GetPlayerState(playerid);
native GetTickCount();
native Cache:mysql_query(MySQL:handle, const query[], bool:use_cache = true);
native cache_get_row_count(&destination);
native cache_get_value_name_int(row_idx, const column_name[], &destination);
native cache_get_value_name(row_idx, const column_name[], destination[], max_len = sizeof(destination));
native cache_delete(Cache:cache_id);
native Warung_Nearest(playerid);
new MySQL:g_SQL;
enum E_ACC {pID,pMoney,bool:IsLoggedIn};
new AccountData[MAX_PLAYERS][E_ACC];
#define EC_FIELDS (22)
new gECRevision[MAX_PLAYERS],gECEpoch[MAX_PLAYERS],bool:gECLoaded[MAX_PLAYERS],bool:gECCreated[MAX_PLAYERS],bool:gECOwned[MAX_PLAYERS][160],gECLook[MAX_PLAYERS][EC_FIELDS];
new bool:gECOpen[MAX_PLAYERS],bool:gECShop[MAX_PLAYERS],bool:gECCreator[MAX_PLAYERS],gECNonce[MAX_PLAYERS],gECLastAction[MAX_PLAYERS];
enum E_EAGLE_ITEM { ecItem,ecSlot,ecPrice,ecBodies };
new const gECatalog[][E_EAGLE_ITEM] = {{101,9,200,63}};
stock EC_FindItem(item) {for(new i=0;i<sizeof(gECatalog);i++) if(gECatalog[i][ecItem]==item) return i;return -1;}
stock bool:EC_Validate(const look[EC_FIELDS]) {return look[0]>=0;}
stock EC_Notice(playerid,const text[]) {printf("notice %d %s",playerid,text);return 0;}
stock EC_Close(playerid,bool:force=false)
{
#pragma unused force
    return playerid>=0;
}
stock EC_ApplyCarrier(playerid)
{
#pragma unused playerid
}
stock EC_UIState(playerid) return playerid>=0;
stock J_Begin() {}
stock J_Int(const k[],v)
{
#pragma unused k,v
}
stock J_Send(playerid,const e[])
{
#pragma unused e
    return playerid>=0;
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
main()
{
    EC_OnUI(0,"save",0,"1,1,0,1,3,1,1,0,0,0,0,101,0,301,401,0,0,0,0,0,0,0,0,0");
    printf("%d %s", EC_IsCleanRejection(""), EC_RejectionText("insufficient_money"));
}
