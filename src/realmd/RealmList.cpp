/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** \file
    \ingroup realmd
*/

#include "Common.h"
#include "RealmList.h"
#include "AuthCodes.h"
#include "Util.h"                                           // for Tokens typedef
#include "Policies/Singleton.h"
#include "Database/DatabaseEnv.h"

INSTANTIATE_SINGLETON_1( RealmList );

extern DatabaseType LoginDatabase;

// will only support WoW 1.12.1/1.12.2 , WoW:TBC 2.4.3 , WoW:WotLK 3.3.5a , WoW:Cataclysm 4.3.4 and official release for WoW:MoP and later, client builds 16057, 15595, 12340, 8606, 6005, 5875
// if you need more from old build then add it in cases in realmd sources code
// list sorted from high to low build and first build used as low bound for accepted by default range (any > it will accepted by realmd at least)

static RealmBuildInfo ExpectedRealmdClientBuilds[] =
{
    {16357, 5, 1, 0, 'a'},
    {16135, 5, 0, 5, 'b'},
    {15595, 4, 3, 4, ' '},
    {15050, 4, 3, 0, ' '},
    {13623, 4, 0, 6, 'a'},
    {12340, 3, 3, 5, 'a'},
    {8606,  2, 4, 3, ' '},
    {6005,  1,12, 2, ' '},
    {5875,  1,12, 1, ' '},
    {0,     0, 0, 0, ' '}                                   // terminator
};

RealmBuildInfo const* FindBuildInfo(uint16 _build)
{
    // first build is low bound of always accepted range
    if (_build >= ExpectedRealmdClientBuilds[0].build)
        return &ExpectedRealmdClientBuilds[0];

    // continue from 1 with explicit equal check
    for(int i = 1; ExpectedRealmdClientBuilds[i].build; ++i)
        if(_build == ExpectedRealmdClientBuilds[i].build)
            return &ExpectedRealmdClientBuilds[i];

    // none appropriate build
    return NULL;
}

RealmList::RealmList( ) : m_UpdateInterval(0), m_NextUpdateTime(time(NULL))
{
}

RealmList& sRealmList
{
    static RealmList realmlist;
    return realmlist;
}

/// Load the realm list from the database
void RealmList::Initialize(uint32 updateInterval)
{
    m_UpdateInterval = updateInterval;

    ///- Get the content of the realmlist table in the database
    UpdateRealms(true);
}

void RealmList::UpdateRealm( uint32 ID, const std::string& name, const std::string& address, uint32 port, uint8 icon, RealmFlags realmflags, uint8 timezone, AccountTypes allowedSecurityLevel, float popu, const std::string& builds)
{
    ///- Create new if not exist or update existed
    Realm& realm = m_realms[name];

    realm.m_ID       = ID;
    realm.icon       = icon;
    realm.realmflags = realmflags;
    realm.timezone   = timezone;
    realm.allowedSecurityLevel = allowedSecurityLevel;
    realm.populationLevel      = popu;

    Tokens tokens(builds, ' ');
    Tokens::iterator iter;

    for (iter = tokens.begin(); iter != tokens.end(); ++iter)
    {
        uint32 build = atol(*iter);
        realm.realmbuilds.insert(build);
    }

    uint16 first_build = !realm.realmbuilds.empty() ? *realm.realmbuilds.begin() : 0;

    realm.realmBuildInfo.build = first_build;
    realm.realmBuildInfo.major_version = 0;
    realm.realmBuildInfo.minor_version = 0;
    realm.realmBuildInfo.bugfix_version = 0;
    realm.realmBuildInfo.hotfix_version = ' ';

    if (first_build)
        if (RealmBuildInfo const* bInfo = FindBuildInfo(first_build))
            if (bInfo->build == first_build)
                realm.realmBuildInfo = *bInfo;

    ///- Append port to IP address.
    std::ostringstream ss;
    ss << address << ':' << port;
    realm.address   = ss.str();
}

void RealmList::UpdateIfNeed()
{
    // maybe disabled or updated recently
    if(!m_UpdateInterval || m_NextUpdateTime > time(NULL))
        return;

    m_NextUpdateTime = time(NULL) + m_UpdateInterval;

    // Clears Realm list
    m_realms.clear();

    // Get the content of the realmlist table in the database
    UpdateRealms(false);
}

void RealmList::UpdateRealms(bool init)
{
    DETAIL_LOG("Updating Realm List...");

    ////                                               0   1     2        3     4     5           6         7                     8           9
    QueryResult *result = LoginDatabase.Query( "SELECT id, name, address, port, icon, realmflags, timezone, allowedSecurityLevel, population, realmbuilds FROM realmlist WHERE (realmflags & 1) = 0 ORDER BY name" );

    ///- Circle through results and add them to the realm map
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();

            uint32 Id                  = fields[0].GetUInt32();
            std::string name           = fields[1].GetCppString();
            std::string address        = fields[2].GetCppString();
            uint32 port                = fields[3].GetUInt32();
            uint8 realmflags           = fields[5].GetUInt8();
            uint8 allowedSecurityLevel = fields[7].GetUInt8();

            if (realmflags & ~(REALM_FLAG_OFFLINE | REALM_FLAG_NEW_PLAYERS | REALM_FLAG_RECOMMENDED | REALM_FLAG_SPECIFYBUILD))
            {
                sLog.outError("Realm (id %u, name '%s') can only be flagged as OFFLINE (mask 0x02), NEWPLAYERS (mask 0x20), RECOMMENDED (mask 0x40), or SPECIFICBUILD (mask 0x04) in DB", Id, name.c_str());
                realmflags &= (REALM_FLAG_OFFLINE | REALM_FLAG_NEW_PLAYERS | REALM_FLAG_RECOMMENDED | REALM_FLAG_SPECIFYBUILD);
            }

            UpdateRealm(
                Id, name, address, port,
                fields[4].GetUInt8(), RealmFlags(realmflags), fields[6].GetUInt8(),
                (allowedSecurityLevel <= SEC_ADMINISTRATOR ? AccountTypes(allowedSecurityLevel) : SEC_ADMINISTRATOR),
                fields[8].GetFloat(), fields[9].GetCppString());

            if(init)
                sLog.outString("RealmList::UpdateRealms added realm id %u, name %s, BindAddress %s:%u, flags %u, Allowed level %u", Id, name.c_str(), address.c_str(), port, realmflags, allowedSecurityLevel);

        } while( result->NextRow() );
        delete result;
    }
}
