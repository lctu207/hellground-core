/*
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * Copyright (C) 2008-2009 Trinity <http://www.trinitycore.org/>
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
    \ingroup u2w
*/

#include "WorldSocket.h"                                    // must be first to make ACE happy with ACE includes in it
#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "Guild.h"
#include "World.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "BattleGroundMgr.h"
#include "OutdoorPvPMgr.h"
#include "Language.h"                                       // for CMSG_CANCEL_MOUNT_AURA handler
#include "Chat.h"
#include "SocialMgr.h"
#include "WardenWin.h"
#include "WardenMac.h"

/// WorldSession constructor
WorldSession::WorldSession(uint32 id, WorldSocket *sock, uint32 sec, uint8 expansion, LocaleConstant locale, time_t mute_time, std::string mute_reason, uint64 accFlags, uint16 opcDisabled) :
LookingForGroup_auto_join(false), LookingForGroup_auto_add(false), m_muteTime(mute_time), m_muteReason(mute_reason),
_player(NULL), m_Socket(sock), _security(sec), _accountId(id), m_expansion(expansion), m_opcodesDisabled(opcDisabled),
m_sessionDbcLocale(sWorld.GetAvailableDbcLocale(locale)), m_sessionDbLocaleIndex(objmgr.GetIndexForLocale(locale)),
_logoutTime(0), m_inQueue(false), m_playerLoading(false), m_playerLogout(false), m_playerSave(false), m_playerRecentlyLogout(false), m_latency(0),
m_kickTimer(MINUTE * 15 * 1000), m_accFlags(accFlags), m_Warden(NULL)
{
    if (sock)
    {
        m_Address = sock->GetRemoteAddress ();
        sock->AddReference ();
        LoginDatabase.PExecute("UPDATE account SET online = 1 WHERE id = %u;", GetAccountId());
    }
}

/// WorldSession destructor
WorldSession::~WorldSession()
{
    ///- unload player if not unloaded
    if (_player)
        LogoutPlayer(true);

    /// - If have unclosed socket, close it
    if (m_Socket)
    {
        m_Socket->CloseSocket ();
        m_Socket->RemoveReference ();
        m_Socket = NULL;
    }

    if (m_Warden)
        delete m_Warden;

    WorldPacket* packet;
    while (_recvThreadSafeQueue.next(packet))
        delete packet;

    while (_recvThreadUnsafeQueue.next(packet))
        delete packet;

    LoginDatabase.PExecute("UPDATE account SET online = 0 WHERE id = %u;", GetAccountId());
    CharacterDatabase.PExecute("UPDATE characters SET online = 0 WHERE account = %u;", GetAccountId());
}

void WorldSession::SizeError(WorldPacket const& packet, uint32 size) const
{
    sLog.outError("Client (account %u) send packet %s (%u) with size %u but expected %u (attempt crash server?), skipped",
        GetAccountId(),LookupOpcodeName(packet.GetOpcode()),packet.GetOpcode(),packet.size(),size);
}

/// Get the player name
char const* WorldSession::GetPlayerName() const
{
    return GetPlayer() ? GetPlayer()->GetName() : "<none>";
}

void WorldSession::SetOpcodeDisableFlag(uint16 flag)
{
    m_opcodesDisabled |= flag;
    LoginDatabase.PExecute("UPDATE account SET opcodesDisabled ='%u' WHERE id = '%u'", m_opcodesDisabled, GetAccountId());
}

void WorldSession::RemoveOpcodeDisableFlag(uint16 flag)
{
    m_opcodesDisabled &= ~flag;
    LoginDatabase.PExecute("UPDATE account SET opcodesDisabled ='%u' WHERE id = '%u'", m_opcodesDisabled, GetAccountId());
}

/// Send a packet to the client
void WorldSession::SendPacket(WorldPacket const* packet)
{
    if (!m_Socket)
        return;

    #ifdef TRINITY_DEBUG

    // Code for network use statistic
    static uint64 sendPacketCount = 0;
    static uint64 sendPacketBytes = 0;

    static time_t firstTime = time(NULL);
    static time_t lastTime = firstTime;                     // next 60 secs start time

    static uint64 sendLastPacketCount = 0;
    static uint64 sendLastPacketBytes = 0;

    time_t cur_time = time(NULL);

    if ((cur_time - lastTime) < 60)
    {
        sendPacketCount+=1;
        sendPacketBytes+=packet->size();

        sendLastPacketCount+=1;
        sendLastPacketBytes+=packet->size();
    }
    else
    {
        uint64 minTime = uint64(cur_time - lastTime);
        uint64 fullTime = uint64(lastTime - firstTime);
        sLog.outDetail("Send all time packets count: " I64FMT " bytes: " I64FMT " avr.count/sec: %f avr.bytes/sec: %f time: %u",sendPacketCount,sendPacketBytes,float(sendPacketCount)/fullTime,float(sendPacketBytes)/fullTime,uint32(fullTime));
        sLog.outDetail("Send last min packets count: " I64FMT " bytes: " I64FMT " avr.count/sec: %f avr.bytes/sec: %f",sendLastPacketCount,sendLastPacketBytes,float(sendLastPacketCount)/minTime,float(sendLastPacketBytes)/minTime);

        lastTime = cur_time;
        sendLastPacketCount = 1;
        sendLastPacketBytes = packet->wpos();               // wpos is real written size
    }

    #endif                                                  // !TRINITY_DEBUG

    if (m_Socket->SendPacket(*packet) == -1)
        m_Socket->CloseSocket();
}

/// Add an incoming packet to the queue
void WorldSession::QueuePacket(WorldPacket* new_packet)
{
    if (!new_packet)
        return;

    OpcodeHandler& opHandle = opcodeTable[new_packet->GetOpcode()];

    switch (opHandle.packetProcessing)
    {
        case PROCESS_INPLACE:
        case PROCESS_THREADSAFE:
            _recvThreadSafeQueue.add(new_packet);
            break;
        case PROCESS_THREADUNSAFE:
            _recvThreadUnsafeQueue.add(new_packet);
            break;
    }
}

/// Logging helper for unexpected opcodes
void WorldSession::logUnexpectedOpcode(WorldPacket* packet, const char *reason)
{
    sLog.outDebug("SESSION: received unexpected opcode %s (0x%.4X) %s",
        LookupOpcodeName(packet->GetOpcode()),
        packet->GetOpcode(),
        reason);
}

void WorldSession::ProcessPacket(WorldPacket* packet)
{
    if (!packet)
        return;

    if (packet->GetOpcode() >= NUM_MSG_TYPES)
    {
        sLog.outError("SESSION: received non-existed opcode %s (0x%.4X)",
            LookupOpcodeName(packet->GetOpcode()),
            packet->GetOpcode());
    }
    else
    {
        OpcodeHandler& opHandle = opcodeTable[packet->GetOpcode()];
        switch (opHandle.status)
        {
            case STATUS_LOGGEDIN:
                if (!_player)
                {
                    // skip STATUS_LOGGEDIN opcode unexpected errors if player logout sometime ago - this can be network lag delayed packets
                    if (!m_playerRecentlyLogout)
                        logUnexpectedOpcode(packet, "the player has not logged in yet");
                }
                else if (_player->IsInWorld())
                    (this->*opHandle.handler)(*packet);

                // lag can cause STATUS_LOGGEDIN opcodes to arrive after the player started a transfer
                break;
            case STATUS_TRANSFER_PENDING:
                if (!_player)
                    logUnexpectedOpcode(packet, "the player has not logged in yet");
                else if (_player->IsInWorld())
                    logUnexpectedOpcode(packet, "the player is still in world");
                else
                    (this->*opHandle.handler)(*packet);

                break;
            case STATUS_AUTHED:
                // prevent cheating with skip queue wait
                if (m_inQueue)
                {
                    logUnexpectedOpcode(packet, "the player not pass queue yet");
                    break;
                }

                m_playerRecentlyLogout = false;
                (this->*opHandle.handler)(*packet);

                break;
            case STATUS_NEVER:
                if (packet->GetOpcode() != CMSG_MOVE_NOT_ACTIVE_MOVER)
                    sLog.outError("SESSION: received not allowed opcode %s (0x%.4X)",
                        LookupOpcodeName(packet->GetOpcode()),
                        packet->GetOpcode());
                break;
        }
    }
}

/// Update the WorldSession (triggered by World update)
bool WorldSession::Update(uint32 diff, bool threadSafe)
{
    if (!m_inQueue && !m_playerLoading && (!_player || !_player->IsInWorld()))
    {
        if (m_kickTimer < diff)
            KickPlayer();
        else
            m_kickTimer -= diff;
    }
    else
        m_kickTimer = MINUTE * 15 * 1000;

    sWorld.RecordTimeDiff(NULL);

    ///- Retrieve packets from the receive queue and call the appropriate handlers
    /// not proccess packets if socket already closed
    WorldPacket* packet;

    ACE_Based::LockedQueue<WorldPacket*, ACE_Thread_Mutex> * _recvQueue = NULL;

    if (threadSafe)
        _recvQueue = &_recvThreadSafeQueue;
    else
        _recvQueue = &_recvThreadUnsafeQueue;

    if (!_recvQueue)
    {
        KickPlayer();
        return false;
    }

    while (m_Socket && !m_Socket->IsClosed() && _recvQueue->next(packet))
    {
        ProcessPacket(packet);
        delete packet;
    }

    bool overtime = false;
    if (sWorld.RecordTimeDiff("WorldSession:Update: packets. Accid %u | ", GetAccountId()) > sWorld.getConfig(CONFIG_SESSION_UPDATE_MAX_TIME))
        overtime = true;

    if (m_Socket && m_Warden)
        m_Warden->Update();

    if (sWorld.RecordTimeDiff("WorldSession:Update: warden. Accid %u | ", GetAccountId()) > sWorld.getConfig(CONFIG_SESSION_UPDATE_MAX_TIME))
        overtime = true;

    if (overtime)
    {
        switch (sWorld.getConfig(CONFIG_SESSION_UPDATE_OVERTIME_METHOD))
        {
            case OVERTIME_IPBAN:
                LoginDatabase.PExecute("INSERT INTO ip_banned VALUES ('%s', NOW(), NOW(), 'CONSOLE', 'bye bye')", GetRemoteAddress().c_str());
            case OVERTIME_ACCBAN:
                LoginDatabase.PExecute("INSERT INTO account_banned VALUES ('%u', NOW(), NOW(), 'CONSOLE', 'bye bye', 1)", GetAccountId());
            case OVERTIME_KICK:
                KickPlayer();
            case OVERTIME_LOG:
                sLog.outError("WorldSession::Update: session for account %u was too long", GetAccountId());
            default:
                break;
        }
    }

    //check if we are safe to proceed with logout
    //logout procedure should happen only in World::UpdateSessions() method!!!
    if (!threadSafe)
    {
        ///- If necessary, log the player out
        time_t currTime = time(NULL);
        if (!m_Socket || (ShouldLogOut(currTime) && !m_playerLoading))
            LogoutPlayer(true);
    }

    ///- Cleanup socket pointer if need
    if (m_Socket && m_Socket->IsClosed())
    {
        m_Socket->RemoveReference();
        m_Socket = NULL;
    }

    if (!m_Socket)
        return false;                                       //Will remove this session from the world session map

    return true;
}

/// %Log the player out
void WorldSession::LogoutPlayer(bool Save)
{
    if (m_playerRecentlyLogout)
        return;

    // finish pending transfers before starting the logout
    while (_player && _player->IsBeingTeleported())
        HandleMoveWorldportAckOpcode();

    m_playerLogout = true;
    m_playerSave = Save;

    if (_player)
    {
        _player->updateMutex.acquire();

        if (uint64 lguid = GetPlayer()->GetLootGUID())
            DoLootRelease(lguid);

        ///- If the player just died before logging out, make him appear as a ghost
        //FIXME: logout must be delayed in case lost connection with client in time of combat
        if (_player->GetDeathTimer())
        {
            _player->getHostilRefManager().deleteReferences();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();
        }
        else
        {
            InstanceMap *pTempMap = NULL;
            if (_player->GetMap() && _player->GetMap()->IsDungeon())
                pTempMap = ((InstanceMap*)_player->GetMap());

            if (!_player->getAttackers().empty() || (pTempMap && pTempMap->EncounterInProgress(_player)))
            {
                _player->CombatStop();
                _player->getHostilRefManager().setOnlineOfflineState(false);
                _player->RemoveAllAurasOnDeath();

                // build set of player who attack _player or who have pet attacking of _player
                std::set<Player*> aset;
                if (!_player->getAttackers().empty())
                {
                    for (Unit::AttackerSet::const_iterator itr = _player->getAttackers().begin(); itr != _player->getAttackers().end(); ++itr)
                    {
                        // including player controlled case
                        if (Unit* owner = (*itr)->GetOwner())
                        {
                            if (owner->GetTypeId()==TYPEID_PLAYER)
                                aset.insert((Player*)owner);
                        }
                        else
                            if ((*itr)->GetTypeId()==TYPEID_PLAYER)
                                aset.insert((Player*)(*itr));
                    }
                }

                _player->SetPvPDeath(!aset.empty());
                _player->KillPlayer();
                _player->BuildPlayerRepop();
                _player->RepopAtGraveyard();

                // give honor to all attackers from set like group case
                for (std::set<Player*>::const_iterator itr = aset.begin(); itr != aset.end(); ++itr)
                    (*itr)->RewardHonor(_player,aset.size());

                // give bg rewards and update counters like kill by first from attackers
                // this can't be called for all attackers.
                if (!aset.empty())
                {
                    if (BattleGround *bg = _player->GetBattleGround())
                        bg->HandleKillPlayer(_player,*aset.begin());
                }
            }
            else if (_player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
            {
                // this will kill character by SPELL_AURA_SPIRIT_OF_REDEMPTION
                _player->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
                //_player->SetDeathPvP(*); set at SPELL_AURA_SPIRIT_OF_REDEMPTION apply time
                _player->KillPlayer();
                _player->BuildPlayerRepop();
                _player->RepopAtGraveyard();
            }
        }

        //drop a flag if player is carrying it
        if (BattleGround *bg = _player->GetBattleGround())
            bg->EventPlayerLoggedOut(_player);

        sOutdoorPvPMgr.HandlePlayerLeave(_player);

        for (int i=0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; i++)
        {
            if (int32 bgTypeId = _player->GetBattleGroundQueueId(i))
            {
                _player->RemoveBattleGroundQueueId(bgTypeId);
                sBattleGroundMgr.m_BattleGroundQueues[ bgTypeId ].RemovePlayer(_player->GetGUID(), true);
            }
        }

        ///- If the player is in a guild, update the guild roster and broadcast a logout message to other guild members
        Guild *guild = objmgr.GetGuildById(_player->GetGuildId());
        if (guild)
        {
            guild->LoadPlayerStatsByGuid(_player->GetGUID());
            guild->UpdateLogoutTime(_player->GetGUID());

            WorldPacket data(SMSG_GUILD_EVENT, (1+1+12+8)); // name limited to 12 in character table.
            data << uint8(GE_SIGNED_OFF);
            data << uint8(1);
            data << _player->GetName();
            data << _player->GetGUID();
            guild->BroadcastPacket(&data);
        }

        ///- Remove pet
        _player->RemovePet(NULL,PET_SAVE_AS_CURRENT, true);

        ///- empty buyback items and save the player in the database
        // some save parts only correctly work in case player present in map/player_lists (pets, etc)
        if (Save)
        {
            uint32 eslot;
            for (int j = BUYBACK_SLOT_START; j < BUYBACK_SLOT_END; j++)
            {
                eslot = j - BUYBACK_SLOT_START;
                _player->SetUInt64Value(PLAYER_FIELD_VENDORBUYBACK_SLOT_1+eslot*2,0);
                _player->SetUInt32Value(PLAYER_FIELD_BUYBACK_PRICE_1+eslot,0);
                _player->SetUInt32Value(PLAYER_FIELD_BUYBACK_TIMESTAMP_1+eslot,0);
            }
            _player->SaveToDB();
        }

        ///- Leave all channels before player delete...
        _player->CleanupChannels();

        ///- If the player is in a group (or invited), remove him. If the group if then only 1 person, disband the group.
        _player->UninviteFromGroup();

        ///- Send update to group
        if (_player->GetGroup())
        {
            _player->GetGroup()->CheckLeader(_player->GetGUID(), true); //logout check leader
            _player->GetGroup()->SendUpdate();
        }

        ///- Broadcast a logout message to the player's friends
        sSocialMgr.SendFriendStatus(_player, FRIEND_OFFLINE, _player->GetGUIDLow(), true);
        sSocialMgr.RemovePlayerSocial(_player->GetGUIDLow ());


        ///- Delete the player object
        _player->CleanupsBeforeDelete();

        ///- Remove the player from the world
        // the player may not be in the world when logging out
        // e.g if he got disconnected during a transfer to another map
        // calls to GetMap in this case may cause crashes
        if (_player->IsInWorld())
            _player->GetMap()->Remove(_player, false);

        // RemoveFromWorld does cleanup that requires the player to be in the accessor
        ObjectAccessor::Instance().RemovePlayer(_player);

        delete _player;
        _player = NULL;

        ///- Send the 'logout complete' packet to the client
        WorldPacket data(SMSG_LOGOUT_COMPLETE, 0);
        SendPacket(&data);

        ///- Since each account can only have one online character at any given time, ensure all characters for active account are marked as offline
        //No SQL injection as AccountId is uint32
        CharacterDatabase.PExecute("UPDATE characters SET online = 0 WHERE account = '%u'",
            GetAccountId());
        sLog.outDebug("SESSION: Sent SMSG_LOGOUT_COMPLETE Message");
    }

    m_playerLogout = false;
    m_playerSave = false;
    m_playerRecentlyLogout = true;
    LogoutRequest(0);
}

/// Kick a player out of the World
void WorldSession::KickPlayer()
{
    if (m_Socket)
        m_Socket->CloseSocket ();
}

/// Cancel channeling handler

void WorldSession::SendAreaTriggerMessage(const char* Text, ...)
{
    va_list ap;
    char szStr [1024];
    szStr[0] = '\0';

    va_start(ap, Text);
    vsnprintf(szStr, 1024, Text, ap);
    va_end(ap);

    uint32 length = strlen(szStr)+1;
    WorldPacket data(SMSG_AREA_TRIGGER_MESSAGE, 4+length);
    data << length;
    data << szStr;
    SendPacket(&data);
}

void WorldSession::SendNotification(const char *format,...)
{
    if (format)
    {
        va_list ap;
        char szStr [1024];
        szStr[0] = '\0';
        va_start(ap, format);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        WorldPacket data(SMSG_NOTIFICATION, (strlen(szStr)+1));
        data << szStr;
        SendPacket(&data);
    }
}

void WorldSession::SendNotification(int32 string_id,...)
{
    char const* format = GetTrinityString(string_id);
    if (format)
    {
        va_list ap;
        char szStr [1024];
        szStr[0] = '\0';
        va_start(ap, string_id);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        WorldPacket data(SMSG_NOTIFICATION, (strlen(szStr)+1));
        data << szStr;
        SendPacket(&data);
    }
}

const char * WorldSession::GetTrinityString(int32 entry) const
{
    return objmgr.GetTrinityString(entry,GetSessionDbLocaleIndex());
}

void WorldSession::Handle_NULL(WorldPacket& recvPacket)
{
    sLog.outDebug("SESSION: received unhandled opcode %s (0x%.4X)",
        LookupOpcodeName(recvPacket.GetOpcode()),
        recvPacket.GetOpcode());
}

void WorldSession::Handle_EarlyProccess(WorldPacket& recvPacket)
{
    sLog.outError("SESSION: received opcode %s (0x%.4X) that must be processed in WorldSocket::OnRead",
        LookupOpcodeName(recvPacket.GetOpcode()),
        recvPacket.GetOpcode());
}

void WorldSession::Handle_ServerSide(WorldPacket& recvPacket)
{
    sLog.outError("SESSION: received server-side opcode %s (0x%.4X)",
        LookupOpcodeName(recvPacket.GetOpcode()),
        recvPacket.GetOpcode());
}

void WorldSession::Handle_Deprecated(WorldPacket& recvPacket)
{
    sLog.outError("SESSION: received deprecated opcode %s (0x%.4X)",
        LookupOpcodeName(recvPacket.GetOpcode()),
        recvPacket.GetOpcode());
}

void WorldSession::SendAuthWaitQue(uint32 position)
{
    if (position == 0)
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
        packet << uint8(AUTH_OK);
        SendPacket(&packet);
    }
    else
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 5);
        packet << uint8(AUTH_WAIT_QUEUE);
        packet << uint32 (position);
        SendPacket(&packet);
    }
}

void WorldSession::InitWarden(BigNumber *K, uint8& OperatingSystem)
{
    switch (OperatingSystem)
    {
        case 0:
            m_Warden = (WardenBase*)new WardenWin();
            break;
        case 1:
//            m_Warden = (WardenBase*)new WardenMac();
            break;
        default:
            sLog.outWarden("Client %u got unsupported operating system (%i)", GetAccountId(), OperatingSystem);
            if (sWorld.getConfig(CONFIG_WARDEN_KICK))
                KickPlayer();
            return;
    }

    if (m_Warden)
        m_Warden->Init(this, K);
}
