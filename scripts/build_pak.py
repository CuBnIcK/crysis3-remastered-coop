"""Build build/c3coop.pak: data overrides that make a networked (bMultiplayer) game use the SP content.

The remaster ships the SP campaign only; network game contexts still take the MP branches of the game's
data (GAME="MP" elements, MP HUD/menus, MP player model and team materials), whose assets were removed.
Sources are read from the installed game paks (read-only), newest first: Patch/patch2 > Patch/patch1 > C3.

  python scripts/build_pak.py [--game <game folder>]   (default: C3_GAME_DIR, else found in the Steam libraries)
"""
import argparse
import collections
import glob
import io
import math
import os
import re
import sys
import xml.etree.ElementTree as ET
import zipfile
from types import SimpleNamespace

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'c3coop.pak')
SOURCES = ['Patch/patch2.pak', 'Patch/patch1.pak', 'C3/Scripts.pak', 'C3/Libs.pak', 'C3/GameData.pak'] + [
    f'C3/Objects-part{i}.pak' for i in range(6)]

MP_SUIT_DIR = 'Objects/characters/human/us/nanosuit_mp/'
SP_SUIT_MTL = 'Objects/characters/human/us/nanosuit_v2/nanosuit_v2.mtl'


class GameFiles:
    """Newest copy of each file across the game paks (local headers use backslashes, so read raw)."""

    def __init__(self, game_dir):
        self.index = {}
        for rel in SOURCES:
            path = os.path.join(game_dir, rel)
            z = zipfile.ZipFile(path)
            for info in z.infolist():
                key = info.filename.replace('\\', '/').lower()
                if key not in self.index:  # SOURCES is ordered newest first
                    self.index[key] = (path, info)

    def names(self):
        return self.index.keys()

    def read(self, name):
        import struct, zlib
        path, info = self.index[name.lower()]
        with open(path, 'rb') as f:
            f.seek(info.header_offset)
            hdr = f.read(30)
            nlen, xlen = struct.unpack_from('<HH', hdr, 26)
            f.seek(info.header_offset + 30 + nlen + xlen)
            raw = f.read(info.compress_size)
        return raw if info.compress_type == 0 else zlib.decompress(raw, -15)

    def original_name(self, name):
        return self.index[name.lower()][1].filename.replace('\\', '/')


def spify(xml_bytes):
    """Drop GAME="MP" elements and strip GAME="SP" so the SP variant is used in every game mode."""
    text = xml_bytes.decode('utf-8-sig', errors='replace')
    root = ET.fromstring(text)
    removed = 0

    def walk(node):
        nonlocal removed
        for child in list(node):
            game = child.attrib.get('GAME')
            if game is not None and game.upper() == 'MP':
                node.remove(child)
                removed += 1
                continue
            if game is not None and game.upper() == 'SP':
                del child.attrib['GAME']
            walk(child)

    walk(root)
    return ET.tostring(root, encoding='utf-8', xml_declaration=False), removed


PLAYER_LUA_PATCH = (
    'C3COOP_PLAYER_LUA = 1 -- marker: proves the pak overrides files that also exist in the game paks\n'
    'function Player:SetIsMultiplayer()\n'
    '\t-- c3coop: keep the SP player setup in network games (the MP nanosuit and MP damage files are not\n'
    '\t-- shipped with the remaster).\n'
    'end\n'
)


# Co-op RMIs on the player entity (Net.Expose script RMIs: entity ids are mapped between machines, both directions):
#   server -> clients  player.allClients:C3Revive(pos, yaw)   revive this player on every machine
#   client -> server   g_localActor.server:C3ToServer(text)   handed to g_gameRules:C3CoopOnClientMessage
#   server -> clients  player.allClients:C3SetName(name)       nickname (Steam persona name) of this player
#   server -> clients  hostPlayer.allClients:C3Tokens(text)    shared game token values "name=value|..."
#   server -> clients  hostPlayer.allClients:C3Trig(text)      trigger events "kind~trigger~sender~count|..." (Lockstep.lua)
#   server -> clients  player.allClients:C3Teleport(pos, yaw)   move this player (its owner moves itself)
#   server -> clients  hostPlayer.allClients:C3Hud(text)        "player,player~text" gate message for those players
PLAYER_RMI_CLIENT = ('\t\t\tC3Revive\t\t\t\t= { RELIABLE_ORDERED, POST_ATTACH, VEC3, FLOAT },\n'
                     '\t\t\tC3SetName\t\t\t\t= { RELIABLE_ORDERED, POST_ATTACH, STRING },\n'
                     '\t\t\tC3Tokens\t\t\t\t= { RELIABLE_ORDERED, POST_ATTACH, STRING },\n'
                     '\t\t\tC3Objective\t\t\t\t= { RELIABLE_ORDERED, POST_ATTACH, STRING, STRING, BOOL },\n'
                     '\t\t\tC3Trig\t\t\t\t= { RELIABLE_ORDERED, POST_ATTACH, STRING },\n'
                     '\t\t\tC3Teleport\t\t\t\t= { RELIABLE_ORDERED, POST_ATTACH, VEC3, FLOAT },\n'
                     '\t\t\tC3Hud\t\t\t\t= { RELIABLE_ORDERED, POST_ATTACH, STRING },\n'
                     '\t\t\tC3UseUpgrade\t\t\t\t= { RELIABLE_ORDERED, POST_ATTACH, STRING },\n')
PLAYER_RMI_SERVER = '\t\t\tC3ToServer\t\t\t\t= { RELIABLE_ORDERED, POST_ATTACH, STRING },\n'
PLAYER_RMI_LUA = '''
-- c3coop: the server forwards a remote player's use of a suit upgrade: this machine collects its own copy
function Player.Client:C3UseUpgrade(name)
	local upgrade = System.GetEntityByName(name);
	if (upgrade and upgrade.C3OrigOnUsed and upgrade.used == 0) then
		Log("[c3coop] collecting suit upgrade %s", name);
		upgrade:C3OrigOnUsed(self, 0);
	end
end
-- c3coop: revive on this machine (the server revives through CGameRules::RevivePlayer, which sends no RMI)
function Player.Client:C3Revive(pos, yaw)
	if (self.actor:GetHealth() <= 0) then
		-- same order as CActor::NetReviveAt: StartRevive restores the health, then Revive
		self.actor:SetHealth(self.actor:GetMaxHealth());
		self.actor:Revive();
	end
	-- also for other players: their entity stays where the ragdoll ended up until their input aspect changes
	self:SetWorldPos(pos);
	if (self.id == g_localActorId) then
		self.actor:PlayerSetViewAngles({x = 0, y = 0, z = yaw});
		System.ExecuteCommand("c3coop hud state default");
	end
	Log("[c3coop] revived %s at %.1f %.1f %.1f", self:GetName(), pos.x, pos.y, pos.z);
end

function Player.Client:C3SetName(name)
	self.c3coopNick = name;
	self.c3coopLabel = nil;
end

-- story state from the server: apply each token with the type it has here
function Player.Client:C3Tokens(text)
	if (CryAction.IsServer()) then
		return;
	end
	local changed = false;
	for pair in string.gmatch(text, "[^|]+") do
		local name, value = string.match(pair, "^([^=]+)=(.*)$");
		if (name) then
			local current = GameToken.GetToken(name);
			if (type(current) == "boolean") then
				value = (value == "true");
			elseif (type(current) == "number") then
				value = tonumber(value);
			end
			if (value ~= nil and current ~= value) then
				Log("[c3coop] token %s: %s -> %s", name, tostring(current), tostring(value));
				GameToken.SetToken(name, value);
				changed = true;
				if (C3CoopTokenApplied) then
					C3CoopCall("token applied", C3CoopTokenApplied, name, tostring(value));
				end
			end
		end
	end
	-- story state jumps (joining a game in progress) can leave the HUD in a scripted malfunction state whose
	-- reboot was already consumed: boot it once things settle
	if (changed) then
		C3COOP_HUD_SETTLE = (C3COOP_HUD_SETTLE or 0) + 1;
		local settle = C3COOP_HUD_SETTLE;
		Script.SetTimer(9000, function()
			if (settle == C3COOP_HUD_SETTLE) then
				System.ExecuteCommand("c3coop hud reboot 1");
				System.ExecuteCommand("c3coop hud state default");
			end
		end);
	end
end

-- mission objective state from the server (MissionObjective entities have the same names everywhere)
function Player.Client:C3Objective(name, state, silent)
	if (CryAction.IsServer()) then
		return;
	end
	local objective = System.GetEntityByName(name);
	if (objective and objective.GotoState and objective:GetState() ~= state) then
		objective.silent = silent;
		objective:GotoState(state);
	end
end

function Player.Server:C3ToServer(text)
	if (g_gameRules and g_gameRules.C3CoopOnClientMessage) then
		g_gameRules:C3CoopOnClientMessage(self, text);
	end
end

-- trigger events decided by the server (Scripts/c3coop/Lockstep.lua)
function Player.Client:C3Trig(text)
	if (CryAction.IsServer() or not C3CoopTrigApply) then
		return;
	end
	for ev in string.gmatch(text, "[^|]+") do
		C3CoopCall("trigger event " .. ev, C3CoopTrigApply, ev);
	end
end

-- co-op gate / catch-up teleport: the server already moved its copy; the owner moves itself (its position is its own)
function Player.Client:C3Teleport(pos, yaw)
	if (CryAction.IsServer()) then
		return;
	end
	self:SetWorldPos(pos);
	if (self.id == g_localActorId) then
		self.actor:PlayerSetViewAngles({x = 0, y = 0, z = yaw});
		Log("[c3coop] teleported to %.2f %.2f %.2f", pos.x, pos.y, pos.z);
	end
end

function Player.Client:C3Hud(text)
	if (not CryAction.IsServer() and C3CoopHudReceive) then
		C3CoopHudReceive(text);
	end
end
'''


MISSION_OBJECTIVE_LUA = '''
-- c3coop: in a network game the server's objective states are mirrored on every client (see Player C3Objective)
for _, state in ipairs(MissionObjective.States) do
	local handlers = MissionObjective[state];
	local onBegin = handlers.OnBeginState;
	handlers.OnBeginState = function(self)
		onBegin(self);
		if (CryAction.IsServer() and g_gameRules and g_gameRules.game:IsMultiplayer() and g_localActor and g_localActor.allClients) then
			g_localActor.allClients:C3Objective(self:GetName(), state, self.silent and true or false);
		end
	end
end
'''


def patch_mission_objective_lua(src):
    text = src.decode('latin-1')
    nl = '\r\n' if '\r\n' in text else '\n'
    return (text + MISSION_OBJECTIVE_LUA.replace('\n', nl)).encode('latin-1')


# NanosuitUpgrade.lua: the upgrade points go to the machine that runs OnUsed (CNanosuitUpgrade::OnUsed ->
# CPersistantStats::IncrementClientStats), so a remote player's use is forwarded to that player's own machine, which
# collects its own copy; every player can collect every upgrade, as in single player.
NANOSUIT_UPGRADE_LUA = '''
-- c3coop: every player collects their own copy of a suit upgrade
NanosuitUpgrade.C3OrigOnUsed = NanosuitUpgrade.OnUsed;
function NanosuitUpgrade:OnUsed(user, idx)
	if (CryAction.IsServer() and g_gameRules and g_gameRules.game:IsMultiplayer() and user and user.id ~= g_localActorId
		and user.onClient and user.actor) then
		Log("[c3coop] suit upgrade %s goes to %s", self:GetName(), user:GetName());
		user.onClient:C3UseUpgrade(user.actor:GetChannel(), self:GetName());
		return;
	end
	self:C3OrigOnUsed(user, idx);
end
'''


def patch_nanosuit_upgrade_lua(src):
    text = src.decode('latin-1')
    nl = '\r\n' if '\r\n' in text else '\n'
    return (text + NANOSUIT_UPGRADE_LUA.replace('\n', nl)).encode('latin-1')


# BasicActor.lua: single player behaviour in network games (pain sounds and ShouldIgnoreHit for scripted invulnerable
# NPCs in Server:OnHit, vehicles reacting to a dead driver, heavy landing explosions)
BASIC_ACTOR_SP = [
    ('\tlocal isMultiplayer = System.IsMultiplayer();', '\tlocal isMultiplayer = false; -- c3coop: SP hit handling'),
    ('\tif ( g_gameRules.game:IsMultiplayer() == false ) then', '\tif (true) then -- c3coop: SP passenger death'),
    ('\t\t\tif (event == "heavylanded" and (not g_gameRules.game:IsMultiplayer())) then',
     '\t\t\tif (event == "heavylanded") then -- c3coop: SP heavy landing'),
]


# Destroyable objects on a listen server (DestroyableObject.lua, HuskReplenisher.lua, PressurizedObject.lua):
#   - Explode() sends ClExplode only "if not CryAction.IsClient()" (a dedicated server): the host is a client too, so a
#     prop shot to pieces on the server stayed whole, with collision, on the remote machines. Their script state is not
#     networked either, so ClExplode now takes the client to its Dead state (Client.Dead explodes it there and fires
#     the flowgraph "Explode" output), unless the client already blew it up itself (flowgraph-driven Explode inputs
#     run on every machine).
#   - "damaged only by the player" / "only the player destroys it" were SP-only branches comparing the shooter with
#     g_localActorId: any player counts now, and AI can no longer destroy player-only objects.
DESTROYABLE_FIXES = {
    'destroyableobject': [
        ('if( not CryAction.IsClient() ) then', 'if( CryAction.IsServer() ) then -- c3coop: the host is a client too'),
        ('(not System.IsMultiplayer()) and pass and NumberToBool(self.Properties.bPlayerOnly) and',
         'pass and NumberToBool(self.Properties.bPlayerOnly) and -- c3coop: any player'),
        ('(hit.shooterId and (hit.shooterId ~= g_localActorId))) then', '(not C3CoopIsPlayerId(hit.shooterId))) then'),
        ('if (not System.IsMultiplayer()) then', 'if (true) then -- c3coop: only players destroy it fully'),
        ('if (hit.shooterId ~= g_localActorId) then', 'if (not C3CoopIsPlayerId(hit.shooterId)) then'),
        ('function DestroyableObject.Client:ClExplode()', 'function DestroyableObject.Client:C3OrigClExplode()'),
    ],
    'huskreplenisher': [
        ('if( not CryAction.IsClient() ) then', 'if( CryAction.IsServer() ) then -- c3coop: the host is a client too'),
        ('(not System.IsMultiplayer()) and pass and NumberToBool(self.Properties.bPlayerOnly) and',
         'pass and NumberToBool(self.Properties.bPlayerOnly) and -- c3coop: any player'),
        ('(hit.shooterId and (hit.shooterId ~= g_localActorId))) then', '(not C3CoopIsPlayerId(hit.shooterId))) then'),
        ('function HuskReplenisher.Client:ClExplode()', 'function HuskReplenisher.Client:C3OrigClExplode()'),
    ],
    'pressurizedobject': [
        ('local playerHit = hit.shooterId == g_localActorId;', 'local playerHit = C3CoopIsPlayerId(hit.shooterId); -- c3coop'),
    ],
}

DESTROYABLE_LUA = '''
-- c3coop: is this entity id a player (any machine's)?
function C3CoopIsPlayerId(id)
	local e = id and System.GetEntity(id);
	return e ~= nil and e.class == "Player";
end
'''

DESTROYABLE_CLEXPLODE_LUA = '''
-- c3coop: the server's explosion reaches a client that has not blown it up itself: Dead state = explode + outputs
function %s.Client:ClExplode()
	if (not CryAction.IsServer() and not self.dead and not self.exploded) then
		self.health = 0;
		self:GotoState("Dead");
	end
end
'''


# Elevator.lua: Server:OnInitClient (a player joining mid-ride) sends ClInitMoving(currFloor, goalFloor, speed,
# acceleration), and Client:ClInitMoving takes those four, but Net.Expose declared VEC3 first: the RMI was malformed.
ELEVATOR_FIXES = [
    ('ClInitMoving = { RELIABLE_UNORDERED, POST_ATTACH, VEC3, INT8, INT8, FLOAT, FLOAT },',
     'ClInitMoving = { RELIABLE_UNORDERED, POST_ATTACH, INT8, INT8, FLOAT, FLOAT }, -- c3coop: as sent and handled'),
]


def patch_elevator_lua(src):
    text = src.decode('latin-1')
    for old, new in ELEVATOR_FIXES:
        if text.count(old) != 1:
            raise SystemExit('elevator.lua: expected exactly one ' + repr(old))
        text = text.replace(old, new)
    return text.encode('latin-1')


# Physics props of the campaign levels have MultiplayerOptions.bNetworked 0, so their OnSpawn makes them
# ENTITY_FLAG_CLIENT_ONLY: every machine simulates its own copy and nothing the host picks up, holds or throws moves on
# the other machines. In a network game the dynamic ones are networked instead (server physics, synced like MP props);
# static ones stay local. The rule only reads the entity's properties, so every machine decides the same.
NET_PROP_CHECK = 'if (self.Properties.MultiplayerOptions.bNetworked == 0) then'
NET_PROP_LUA = '''
-- c3coop: dynamic physics props are networked in a network game (C3COOP_NET_PROPS = false before the level load: off)
function C3CoopNetProp(ent)
	if (C3COOP_NET_PROPS == false) then
		return false;
	end
	local mp = System.IsMultiplayer();
	if (mp == nil or mp == false or mp == 0) then
		return false;
	end
	local ph = ent.Properties and ent.Properties.Physics;
	if (not ph or tonumber(ph.bPhysicalize or 1) == 0) then
		return false;
	end
	return ent.class == "RigidBodyEx" or tonumber(ph.bRigidBody or 0) ~= 0;
end
'''


def patch_stalker_tree(src):
    """Stalkers start with a sound stimulus at g_localActorId, on the host always the host's player: the nearest living
    player instead (C3CoopSeedStalker, Lockstep.lua)."""
    text = src.decode('latin-1')
    new, n = re.subn(r'code="AI\.CreateStimulusEvent\(entity\.id, g_localActorId, [^"]*"',
                     'code="C3CoopSeedStalker(entity)"', text)
    if n != 1:
        raise SystemExit('StalkerTree.xml: local player stimulus not found')
    return new.encode('latin-1')


def patch_net_prop_lua(src, name):
    text = src.decode('latin-1')
    nl = '\r\n' if '\r\n' in text else '\n'
    if text.count(NET_PROP_CHECK) != 1:
        raise SystemExit(f'{name}: expected exactly one {NET_PROP_CHECK!r}')
    text = text.replace(NET_PROP_CHECK, 'if (self.Properties.MultiplayerOptions.bNetworked == 0 and not C3CoopNetProp(self)) then')
    return (text.replace('\r\n', '\n') + NET_PROP_LUA).replace('\n', nl).encode('latin-1')


def patch_destroyable_lua(src, kind):
    text = src.decode('latin-1')
    nl = '\r\n' if '\r\n' in text else '\n'
    for old, new in DESTROYABLE_FIXES[kind]:
        if text.count(old) != 1:
            raise SystemExit(f'{kind}.lua: expected exactly one {old!r}')
        text = text.replace(old, new)
    text += DESTROYABLE_LUA
    if kind != 'pressurizedobject':
        cls = {'destroyableobject': 'DestroyableObject', 'huskreplenisher': 'HuskReplenisher'}[kind]
        text += DESTROYABLE_CLEXPLODE_LUA % cls
    return text.replace('\r\n', '\n').replace('\n', nl).encode('latin-1')


def patch_basic_actor_lua(src):
    text = src.decode('latin-1')
    for old, new in BASIC_ACTOR_SP:
        if text.count(old) != 1:
            raise SystemExit('basicactor.lua: expected exactly one ' + repr(old))
        text = text.replace(old, new)
    return text.encode('latin-1')


def patch_player_lua(src):
    text = src.decode('latin-1')
    nl = '\r\n' if '\r\n' in text else '\n'
    patch = PLAYER_LUA_PATCH.replace('\n', nl)
    new, n = re.subn(r'function Player:SetIsMultiplayer\(\).*?\r?\nend\r?\n', lambda m: patch, text, count=1, flags=re.S)
    if n != 1:
        raise SystemExit('player.lua: SetIsMultiplayer not found')
    for table, line in (('ClientMethods', PLAYER_RMI_CLIENT), ('ServerMethods', PLAYER_RMI_SERVER)):
        new, n = re.subn(table + r' = \{\r?\n', lambda m: m.group(0) + line.replace('\n', nl), new, count=1)
        if n != 1:
            raise SystemExit('player.lua: Net.Expose ' + table + ' not found')
    return (new + PLAYER_RMI_LUA.replace('\n', nl)).encode('latin-1')


# Scripts/Network/CompressionPolicy.xml is read by CryNetwork (0x37A67800, gEnv->pSystem->LoadXmlFromFile) each time a
# network game context starts (CNetContext ctor 0x37A9E410 -> 0x37A67800), long after the early pak mount, so the
# override is used. Host and every client must run the SAME c3coop.pak: the bit layout is not negotiated.
POLICY_OVERRIDES = {
    # Player input positions (SerialiseRelativePosition 0x3861F5E0, 'wrld') were sized for the MP maps (0..700 /
    # 0..800 / 0..300, clamped). Campaign terrains reach 2560 m (EndGame 80 x 32) and heights up to 1024. ~4 mm steps.
    'wrld': '\t<Policy name="wrld" impl="QuantizedVec3">\n'
            '\t\t<XParams min="0" max="4096.0" nbits="20"/>\n'
            '\t\t<YParams min="0" max="4096.0" nbits="20"/>\n'
            '\t\t<ZParams min="0" max="1024.0" nbits="18"/>\n'
            '\t</Policy>',
    'lwld': '\t<Policy name="lwld" impl="QuantizedVec3">\n'
            '\t\t<XParams min="-20" max="4096.0" nbits="20"/>\n'
            '\t\t<YParams min="-20" max="4096.0" nbits="20"/>\n'
            '\t\t<ZParams min="-20" max="1024.0" nbits="18"/>\n'
            '\t</Policy>',
    # SSerializedPlayerInput::stance: 0..6 clamped AI HIGH_COVER (7) and SWIM (8) to ALERTED. The DLL (anim.cpp,
    # `!anim stance on|off`) decides at runtime whether 7..9 are sent and never sends STANCE_NULL (0xFF).
    'stnc': '\t<Policy name="stnc" impl="RangedInt">\n'
            '\t\t<Range min="0" max="9"/>\n'
            '\t</Policy>',
}


def patch_compression_policy(src):
    text = src.decode('utf-8-sig')
    for name, block in POLICY_OVERRIDES.items():
        pattern = r'[ \t]*<Policy name="%s"[^>]*>.*?</Policy>' % re.escape(name)
        text, n = re.subn(pattern, lambda m: block, text, count=1, flags=re.S)
        if n != 1:
            raise SystemExit(f'CompressionPolicy.xml: policy {name} not found')
    if '\r\n' in src.decode('utf-8-sig'):
        text = re.sub(r'(?<!\r)\n', '\r\n', text)
    ET.fromstring(text)  # still well-formed
    return text.encode('utf-8')


COOP_RULES_LUA = '''
----------------------------------------------------------------------------------------------------
-- c3coop: co-op respawn (server only). A dead player is revived C3COOP_RESPAWN_DELAY seconds after death,
-- 1.5 m behind a living teammate (or at a free spot next to them), or where they stood 4 s before dying when nobody is
-- alive. Change the delay from the console with `#C3COOP_RESPAWN_DELAY = 5`; 0 disables respawning.
C3COOP_RESPAWN_DELAY = C3COOP_RESPAWN_DELAY or 8;

-- SpawnPoint "Spawn" starts the level's start chain (intro sequence, fader, action filter): once per level on the
-- server, not again for every joining player; clients joining while the level starts get event "P" (C3CoopJoinReplay)
function C3CoopSpawned(rules, spawn, player)
	rules.c3coopSpawnFired = rules.c3coopSpawnFired or {};
	if (rules.c3coopSpawnFired[spawn:GetName()]) then
		return;
	end
	rules.c3coopSpawnFired[spawn:GetName()] = true;
	rules.c3coopStartedAt = System.GetCurrTime();
	spawn:Spawned(player);
end
-- trigger lockstep, co-op gates, teleports
if (not C3COOP_LOCKSTEP) then
	Script.ReloadScript("Scripts/c3coop/Lockstep.lua");
end
-- network games switch a killed player to third person and do not always switch back on revive
System.ExecuteCommand("c3coop dev pl_switchTPOnKill 0"); -- cheat cvar: through c3coop.dll
-- network games clone a dying player into a client-only "Corpse" entity on revive (remaster default
-- pl_spawnCorpseOnDeath 1; the clone stays until 6 exist). 0: the player entity itself stands up, as in SP
System.ExecuteCommand("c3coop dev pl_spawnCorpseOnDeath 0");
-- MP suit/melee settings left over from the network game: one shared energy bar for cloak and nanovision, boosted
-- melee as in SP
System.ExecuteCommand("c3coop dev g_nanosuitUsesSpecificEnergyBars 0");
System.ExecuteCommand("c3coop dev pl_boostedMelee_allowInMP 1");

function SinglePlayer:C3CoopStart()
	if (self.c3coopRunning) then
		return;
	end
	self.c3coopRunning = true;
	self.c3coopDeadSince = {};
	-- Lua globals outlive the level: trigger events of the previous level must not be applied by name in this one,
	-- and the start gates wait for the players of a co-op level change only, not after a session ended
	C3COOP_TRIG_QUEUE = {};
	if (C3COOP_EXPECTED_FROM_CHANGE) then
		C3COOP_EXPECTED_FROM_CHANGE = nil;
	else
		C3COOP_EXPECTED = nil;
	end
	Script.SetTimer(1000, function() self:C3CoopTick() end);
	Script.SetTimer(300, function() self:C3CoopTokenTick() end);
	Script.SetTimer(100, function() self:C3CoopTrigTick() end);
	Script.SetTimer(500, function() self:C3CoopGateTick() end);
	C3COOP_CARRY = {};
	Script.SetTimer(50, function() self:C3CoopHoldTick() end);
	C3COOP_LOOT = nil; -- scanned at the first tick
	Script.SetTimer(300, function() self:C3CoopAwareTick() end);
end

-- threat meter of the remote players: the host computes their AI awareness (c3coop.dll "aware", sent to each client)
function SinglePlayer:C3CoopAwareTick()
	Script.SetTimer(300, function() self:C3CoopAwareTick() end);
	if (not C3CoopNet() or not CryAction.IsServer()) then
		return;
	end
	local list = {};
	for _, p in pairs(System.GetEntitiesByClass("Player") or {}) do
		if (p.id ~= g_localActorId and p.actor and p.actor:GetHealth() > 0) then
			local channel = p.actor:GetChannel() or 0;
			if (channel > 0) then
				list[#list + 1] = p:GetRawId() .. ":" .. channel;
			end
		end
	end
	if (#list > 0) then
		System.ExecuteCommand("c3coop aware " .. table.concat(list, " "));
	end
end

-- held and carried objects (Lockstep.lua)
function SinglePlayer:C3CoopHoldTick()
	Script.SetTimer(50, function() self:C3CoopHoldTick() end);
	if (C3CoopHeldTick) then
		C3CoopCall("hold tick", C3CoopHeldTick);
	end
end

-- trigger events for the clients, a few messages per tick (Lockstep.lua)
function SinglePlayer:C3CoopTrigTick()
	Script.SetTimer(100, function() self:C3CoopTrigTick() end);
	if (C3CoopTrigFlush) then
		C3CoopCall("trigger flush", C3CoopTrigFlush);
	end
end

-- co-op gates: quorum, waiting messages, release (Lockstep.lua)
function SinglePlayer:C3CoopGateTick()
	Script.SetTimer(500, function() self:C3CoopGateTick() end);
	if (C3CoopGateTick and C3CoopNet()) then
		C3CoopCall("gate tick", C3CoopGateTick);
	end
end

----------------------------------------------------------------------------------------------------
-- story state: the server's values of the level's shared game tokens win (clients run the same flowgraphs, but
-- their triggers fire for their own player only)
Script.ReloadScript("Scripts/c3coop/LevelTokens.lua");

function SinglePlayer:C3CoopLevelTokens()
	if (not self.c3coopTokens) then
		local level = C3CoopLevel and C3CoopLevel();
		local list = level and C3COOP_LEVEL_TOKENS and C3COOP_LEVEL_TOKENS[level];
		if (not list) then
			return {}; -- level not known yet: try again next tick
		end
		self.c3coopTokens = list;
		Log("[c3coop] syncing %d game tokens of %s", #list, level);
	end
	return self.c3coopTokens;
end

-- Trigger events (C3Trig) and mirrored flowgraph outputs (fgmirror.cpp) can make a client's flowgraph test game tokens
-- at once: the token changes made before them go out first (C3CoopTokenFlush), not up to 300 ms later.
function C3CoopTokenFlush()
	local rules = g_gameRules;
	if (rules and rules.C3CoopTokenCollect and CryAction.IsServer()) then
		rules:C3CoopTokenCollect();
		rules:C3CoopTokenSend(8);
	end
end

-- fgmirror.cpp, after a late joiner's replay: the story state again (the replay may have rewritten tokens there)
function C3CoopTokenResyncFor(channel)
	if (g_gameRules) then
		g_gameRules.c3coopTokenResync = true;
		Log("[c3coop] token resync after the flowgraph replay for channel %s", tostring(channel));
	end
end

function SinglePlayer:C3CoopTokenTick()
	Script.SetTimer(300, function() self:C3CoopTokenTick() end);
	self:C3CoopTokenCollect();
	-- one-way tokens of co-op gates: bring everybody along
	if (C3CoopGateTokens) then
		C3CoopCall("gate tokens", C3CoopGateTokens);
	end
	self:C3CoopTokenSend(3);
end

-- a few queued messages: tokens first, then the objectives of a resync
function SinglePlayer:C3CoopTokenSend(max)
	local carrier = g_localActor;
	if (not carrier or not carrier.allClients) then
		return;
	end
	for i = 1, max do
		local text = table.remove(self.c3coopTokenQueue or {}, 1);
		if (text) then
			carrier.allClients:C3Tokens(text);
		else
			local objective = table.remove(self.c3coopObjectiveQueue or {}, 1);
			if (not objective) then
				break;
			end
			carrier.allClients:C3Objective(objective[1], objective[2], true);
		end
	end
end

-- collect changed tokens (all of them after a player joined) into a queue of short messages
function SinglePlayer:C3CoopTokenCollect()
	local carrier = g_localActor;
	if (not carrier or not carrier.allClients) then
		return;
	end
	self.c3coopTokenValues = self.c3coopTokenValues or {};
	self.c3coopTokenQueue = self.c3coopTokenQueue or {};
	local all = self.c3coopTokenResync;
	self.c3coopTokenResync = false;
	for _, name in ipairs(self:C3CoopLevelTokens()) do
		local value = tostring(GameToken.GetToken(name));
		if (all or self.c3coopTokenValues[name] ~= value) then
			self.c3coopTokenValues[name] = value;
			local queue = self.c3coopTokenQueue;
			local last = queue[#queue];
			local pair = name .. "=" .. value;
			-- one RMI string must stay small (a long one hung the server)
			if (last and string.len(last) + string.len(pair) < 200) then
				queue[#queue] = last .. "|" .. pair;
			else
				queue[#queue + 1] = pair;
			end
		end
	end
	if (all) then
		-- queued, not sent in one burst (a burst of RMIs stalled the server before)
		self.c3coopObjectiveQueue = {};
		for _, objective in pairs(System.GetEntitiesByClass("MissionObjective") or {}) do
			if (objective:GetState() ~= "Deactivated") then
				table.insert(self.c3coopObjectiveQueue, {objective:GetName(), objective:GetState()});
			end
		end
	end
end

-- players spawned or staged on the same spot stand inside each other and can't move: after 2 s of overlap the
-- later one (never the host) is moved next to the other, to a spot with no wall in between and a floor under it
function SinglePlayer:C3CoopSeparate(players)
	self.c3coopOverlap = self.c3coopOverlap or {};
	local free = {};
	for _, p in ipairs(players) do
		-- riders (vehicles, the Fields train attaches everyone to one spot) stay where they are
		local vehicle = p.actor.GetLinkedVehicleId and p.actor:GetLinkedVehicleId();
		if (not vehicle and not p:GetParent()) then
			free[#free + 1] = p;
		end
	end
	players = free;
	for i = 1, #players do
		for j = i + 1, #players do
			-- b is moved, so never the host
			local a, b = players[i], players[j];
			if (b.id == g_localActorId) then
				a, b = b, a;
			end
			local pa, pb = a:GetWorldPos(), b:GetWorldPos();
			local key = tostring(a.id) .. "~" .. tostring(b.id);
			if ((pa.x - pb.x) ^ 2 + (pa.y - pb.y) ^ 2 < 0.36 and math.abs(pa.z - pb.z) < 1.5) then
				self.c3coopOverlap[key] = (self.c3coopOverlap[key] or 0) + 1;
			else
				self.c3coopOverlap[key] = nil;
			end
			if ((self.c3coopOverlap[key] or 0) >= 2) then
				self.c3coopOverlap[key] = -8;	-- a player pinned in place (PlayerStaging) is retried every 10 s, not every 2
				local pos = C3CoopFreeSpot(pa, 1.1, j, a.id, b.id);
				if (pos) then
					C3CoopTeleport(b, pos, b:GetWorldAngles().z, "overlapping " .. a:GetName());
				end
			end
		end
	end
end

-- A spot about `radius` from `center` (a player's feet) with no wall in between and a floor at most 1.5 m below,
-- trying 8 directions starting at `seed`; nil when there is none. skipA/skipB: entities the rays ignore.
function C3CoopFreeSpot(center, radius, seed, skipA, skipB)
	local hits = {};
	local from = {x = center.x, y = center.y, z = center.z + 1};
	for k = 0, 7 do
		local ang = ((seed or 0) + k) * math.pi / 4;
		local dir = {x = radius * math.cos(ang), y = radius * math.sin(ang), z = 0};
		if (Physics.RayWorldIntersection(from, dir, 1, ent_all, skipA, skipB, hits) == 0) then
			local top = {x = from.x + dir.x, y = from.y + dir.y, z = from.z};
			if (Physics.RayWorldIntersection(top, {x = 0, y = 0, z = -2.5}, 1, ent_all, skipA, skipB, hits) > 0
				and hits[1] and hits[1].pos) then
				return {x = top.x, y = top.y, z = hits[1].pos.z + 0.1};
			end
		end
	end
	return nil;
end

function SinglePlayer:C3CoopTick()
	Script.SetTimer(1000, function() self:C3CoopTick() end);
	local living = {};
	self.c3coopTrail = self.c3coopTrail or {};
	for _, p in pairs(System.GetEntitiesByClass("Player") or {}) do
		-- remote players are created without ENTITY_FLAG_TRIGGER_AREAS (CActor::Init: channel 0 only): on the server
		-- they never entered an area trigger (zones, layer streaming)
		if (CryAction.IsServer() and math.floor(p:GetFlags() / 16384) % 2 == 0) then
			p:SetFlags(16384, 0);
			Log("[c3coop] %s triggers areas now", p:GetName());
		end
		if (p.actor and p.actor:GetHealth() > 0 and p.actor:GetSpectatorMode() == 0) then
			living[#living + 1] = p;
			-- the last 4 positions, one per second (revive spot when nobody is alive)
			local trail = self.c3coopTrail[p.id] or {};
			local wp = p:GetWorldPos();
			trail[#trail + 1] = {x = wp.x, y = wp.y, z = wp.z + 0.2};
			if (#trail > 4) then
				table.remove(trail, 1);
			end
			self.c3coopTrail[p.id] = trail;
		end
	end
	C3CoopCall("separate", self.C3CoopSeparate, self, living);
	if (C3CoopLootTick) then
		C3CoopCall("loot", C3CoopLootTick);
	end
	if (C3COOP_RESPAWN_DELAY <= 0) then
		return;
	end
	local now = System.GetCurrTime();
	local players = System.GetEntitiesByClass("Player") or {};
	local alive = {};
	for _, p in pairs(players) do
		if (p.actor and p.actor:GetHealth() > 0 and p.actor:GetSpectatorMode() == 0) then
			alive[#alive + 1] = p;
			self:C3CoopRememberItems(p);
		end
	end
	for _, p in pairs(players) do
		if (p.actor and p.actor:GetHealth() <= 0) then
			local since = self.c3coopDeadSince[p.id];
			if (not since) then
				self.c3coopDeadSince[p.id] = now;
			elseif (now - since >= C3COOP_RESPAWN_DELAY) then
				self.c3coopDeadSince[p.id] = nil;
				-- nobody alive: where this player stood 4 s before dying (not the body: a fall or a pit kills again)
				local trail = self.c3coopTrail and self.c3coopTrail[p.id];
				local pos = trail and trail[1] or p:GetWorldPos();
				local angles = p:GetWorldAngles();
				local mate;
				for _, m in ipairs(alive) do
					local vehicle = m.actor.GetLinkedVehicleId and m.actor:GetLinkedVehicleId();
					if (not vehicle and not m:GetParent()) then
						mate = m;
						break;
					end
				end
				if (mate) then
					-- behind the teammate when nothing is in between, else any free spot around them
					local mp = mate:GetWorldPos();
					local dir = mate:GetDirectionVector(1);
					local hits = {};
					local behind = {x = mp.x - dir.x * 1.5, y = mp.y - dir.y * 1.5, z = mp.z + 0.2};
					if (Physics.RayWorldIntersection({x = mp.x, y = mp.y, z = mp.z + 1}, {x = -dir.x * 1.5, y = -dir.y * 1.5, z = 0},
							1, ent_all, mate.id, p.id, hits) == 0) then
						pos = behind;
					else
						pos = C3CoopFreeSpot(mp, 1.3, 0, mate.id, p.id) or {x = mp.x, y = mp.y, z = mp.z + 0.3};
					end
					angles = mate:GetWorldAngles();
				end
				Log("[c3coop] reviving %s next to %s", p:GetName(), mate and mate:GetName() or "where they stood");
				self.game:RevivePlayer(p.id, pos, angles, 0, false);
				-- death sends AIEVENT_AGENTDIED/DISABLE to the player's AI object and a revive sends nothing back (SP
				-- reloads a checkpoint): the AI never perceived a revived remote player again
				p:TriggerEvent(AIEVENT_ENABLE);
				if (mate and C3CoopZoneInherit) then
					C3CoopCall("zone inherit", C3CoopZoneInherit, p, mate);
				end
				-- RevivePlayer sends no RMI and health sync ignores dead -> alive: revive it on every client
				p.allClients:C3Revive(pos, angles.z);
				local playerId = p.id;
				Script.SetTimer(500, function() self:C3CoopGiveItemsBack(playerId) end);
			end
		else
			self.c3coopDeadSince[p.id] = nil;
		end
	end
end

-- messages from clients (g_localActor.server:C3ToServer(text), see player.lua)
function SinglePlayer:C3CoopOnClientMessage(player, text)
	Log("[c3coop] message from %s: %s", player:GetName(), tostring(text));
	-- teleports, uses, visibility reports: run outside this RMI handler
	local verb = string.match(text, "^(%S+)");
	if (verb == "fg") then
		-- flowgraph outputs that fired on this client (fgmirror.cpp uplink): the DLL applies them on the server
		local channel = player.actor and player.actor:GetChannel() or 0;
		local records = string.sub(text, 4);
		Script.SetTimer(1, function()
			System.ExecuteCommand("c3coop fgmirror u " .. tostring(channel) .. " " .. records);
		end);
		return;
	end
	if (C3CoopServerRequest and (verb == "tp" or verb == "goto" or verb == "use" or verb == "cv" or verb == "ioex" or verb == "act" or
			verb == "tok" or verb == "special" or verb == "pick" or verb == "pickn" or verb == "ptdrop")) then
		local playerId = player.id;
		Script.SetTimer(1, function()
			local p = System.GetEntity(playerId);
			if (p) then
				C3CoopCall("request " .. text, C3CoopServerRequest, p, text);
			end
		end);
		return;
	end
	local nick = string.match(text, "^nick (.+)$");
	if (nick) then
		-- a (re)joining player: send the whole story state too (from the token tick, not inside this RMI)
		self.c3coopTokenResync = true;
		if (C3CoopTrigResync) then
			C3CoopCall("trigger resync", C3CoopTrigResync);
		end
		-- trigger events this player missed that only he needs (level start of EndGame, current streaming zones)
		if (C3CoopJoinReplay) then
			local joinId = player.id;
			Script.SetTimer(2000, function()
				local p = System.GetEntity(joinId);
				if (p) then
					C3CoopCall("join replay", C3CoopJoinReplay, p);
				end
			end);
		end
		self.c3coopNicks = self.c3coopNicks or {};
		self.c3coopNicks[player.id] = nick;
		-- everyone (late joiners included) gets every known nick again
		for id, name in pairs(self.c3coopNicks) do
			local p = System.GetEntity(id);
			if (p and p.allClients) then
				p.allClients:C3SetName(name);
			end
		end
	end
end

----------------------------------------------------------------------------------------------------
-- the label font has no Cyrillic: transliterate UTF-8 Cyrillic, other non-ASCII characters become '?'
C3COOP_LATIN = {"A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
	"R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Sch", "", "Y", "", "E", "Yu", "Ya"};

function C3CoopLatin(text)
	local out = {};
	local i = 1;
	local n = string.len(text);
	while (i <= n) do
		local b = string.byte(text, i);
		if (b < 128) then
			out[#out + 1] = string.char(b);
			i = i + 1;
		elseif ((b == 208 or b == 209) and i < n) then
			local cp = math.fmod(b, 32) * 64 + math.fmod(string.byte(text, i + 1), 64);
			if (cp >= 1040 and cp <= 1071) then
				out[#out + 1] = C3COOP_LATIN[cp - 1039];
			elseif (cp >= 1072 and cp <= 1103) then
				out[#out + 1] = string.lower(C3COOP_LATIN[cp - 1071]);
			elseif (cp == 1025) then
				out[#out + 1] = "Yo";
			elseif (cp == 1105) then
				out[#out + 1] = "yo";
			else
				out[#out + 1] = "?";
			end
			i = i + 2;
		else
			out[#out + 1] = "?";
			i = i + 1;
			while (i <= n and string.byte(text, i) >= 128 and string.byte(text, i) < 192) do
				i = i + 1;
			end
		end
	end
	return table.concat(out);
end

----------------------------------------------------------------------------------------------------
-- c3coop client side: send our nick once per local player, draw teammates' nicks above their heads
function SinglePlayer:C3CoopClientStart()
	if (self.c3coopClientRunning) then
		return;
	end
	self.c3coopClientRunning = true;
	self.c3coopNextHello = 0;
	-- the level start flowgraphs restore the inventory stored at the end of the previous level when
	-- Game.Global.Previous_Level names it; a network game stores nothing, so the players started empty-handed.
	-- Without the token they take the level's start pack (Mission_<level>), as Jailbreak does
	if (C3CoopNet()) then
		GameToken.SetToken("Game.Global.Previous_Level", "");
	end
	Script.SetTimer(0, function() self:C3CoopClientTick() end);
end

function SinglePlayer:C3CoopClientTick()
	Script.SetTimer(0, function() self:C3CoopClientTick() end);
	local me = g_localActor;
	if (not me) then
		return;
	end
	local now = System.GetCurrTime();
	if (not me.c3coopNick and now >= self.c3coopNextHello) then
		self.c3coopNextHello = now + 5;
		System.ExecuteCommand("c3coop hello");
	end
	if (C3CoopHudDraw) then
		C3CoopHudDraw(); -- co-op gate messages
		if (now >= (self.c3coopNextRoomTick or 0)) then
			self.c3coopNextRoomTick = now + 0.5;
			if (C3CoopNet()) then
				C3CoopCall("room tick", C3CoopRoomTick);
				C3CoopCall("uplink tick", C3CoopUplinkTick);
				C3CoopCall("special tick", C3CoopSpecialTick);
			end
		end
	end
	local eye = me:GetWorldPos();
	for _, p in pairs(System.GetEntitiesByClass("Player") or {}) do
		if (p.id ~= me.id and p.c3coopNick and p.actor and p.actor:GetHealth() > 0) then
			local pos = p:GetWorldPos();
			if (DistanceSqVectors(pos, eye) < 10000) then
				pos.z = pos.z + 2.1;
				p.c3coopLabel = p.c3coopLabel or C3CoopLatin(p.c3coopNick);
				System.DrawLabel(pos, 1.4, p.c3coopLabel, 0.55, 0.85, 1.0, 1.0);
			end
		end
	end
end

-- a dead player drops everything: remember the item classes they carry (held items sit on the player) and give
-- them back after the revive
-- item classes of this level (every item entity, refreshed every 30 s): the inventory is asked for each. Owned items
-- are not near their owner in the remaster (a 3 m sphere found none: "gave back 0 items" after every death)
function C3CoopItemClasses()
	local now = System.GetCurrTime();
	if (not C3COOP_ITEM_CLASSES or now - (C3COOP_ITEM_CLASSES_AT or 0) > 30) then
		local set = {};
		for _, e in pairs(System.GetEntities() or {}) do
			-- not the hands, nor the weapons of vehicles (VehicleHMG, VehicleSpammer: owned while seated)
			if (e.item and e.class ~= "NoWeapon" and e.class ~= "PickAndThrowWeapon" and not string.find(e.class, "^Vehicle")) then
				set[e.class] = true;
			end
		end
		C3COOP_ITEM_CLASSES, C3COOP_ITEM_CLASSES_AT = {}, now;
		for class in pairs(set) do
			C3COOP_ITEM_CLASSES[#C3COOP_ITEM_CLASSES + 1] = class;
		end
	end
	return C3COOP_ITEM_CLASSES;
end

function SinglePlayer:C3CoopRememberItems(p)
	local items = {};
	if (p.inventory) then
		for _, class in ipairs(C3CoopItemClasses()) do
			if (p.inventory:GetItemByClass(class)) then
				items[#items + 1] = class;
			end
		end
	end
	local current = p.inventory and p.inventory:GetCurrentItem();
	self.c3coopItems = self.c3coopItems or {};
	self.c3coopItems[p.id] = {classes = items, current = current and current.class};
end

function SinglePlayer:C3CoopGiveItemsBack(playerId)
	local saved = self.c3coopItems and self.c3coopItems[playerId];
	if (not saved) then
		return;
	end
	for _, class in ipairs(saved.classes) do
		ItemSystem.GiveItem(class, playerId, false, class == saved.current);
	end
	Log("[c3coop] gave back %d items (current %s)", #saved.classes, tostring(saved.current));
end
'''


def patch_singleplayer_lua(src):
    """Append the co-op respawn loop and start it with the level (server side)."""
    text = src.decode('latin-1')
    nl = '\r\n' if '\r\n' in text else '\n'
    start = '\tself:C3CoopStart();' + nl
    new, n = re.subn(r'function SinglePlayer\.Server:OnStartLevel\(\)\r?\n', lambda m: m.group(0) + start, text, count=1)
    if n != 1:
        raise SystemExit('SinglePlayer.lua: Server:OnStartLevel not found')
    start = '\tself:C3CoopClientStart();' + nl
    new, n = re.subn(r'function SinglePlayer\.Client:OnStartLevel\(\)\r?\n', lambda m: m.group(0) + start, new, count=1)
    if n != 1:
        raise SystemExit('SinglePlayer.lua: Client:OnStartLevel not found')
    # every player is spawned as "Dude": give each a unique name (co-op messages address players by name)
    new, n = re.subn(r'name\s*=\s*"Dude",', lambda m: 'name     = "Player" .. tostring(channelId),', new, count=1)
    if n != 1:
        raise SystemExit('SinglePlayer.lua: player name in OnClientConnect not found')
    new, n = re.subn(r'spawn:Spawned\(player\);', lambda m: 'C3CoopSpawned(self, spawn, player);', new, count=1)
    if n != 1:
        raise SystemExit('SinglePlayer.lua: spawn:Spawned in OnClientConnect not found')
    return (new + COOP_RULES_LUA.replace('\n', nl)).encode('latin-1')


def level_paks(game_dir):
    """(level name in lower case, path of its level.pak) for every campaign level."""
    for pak in sorted(glob.glob(os.path.join(game_dir, 'C3', 'Levels', '*', 'level.pak'))):
        yield os.path.basename(os.path.dirname(pak)).lower(), pak


def read_level_token_libs(pak):
    """Every game token of a level pak: gametokens/*.xml, one GameTokensLibrary per file. Returns [(full name, library,
    LocalOnly)] in file order; full name = "<library Name attribute>.<token Name>"."""
    z = zipfile.ZipFile(pak)
    out = []
    for n in sorted(z.namelist()):
        key = n.replace('\\', '/').lower()
        if not (key.startswith('gametokens/') and key.endswith('.xml')):
            continue
        root = ET.fromstring(z.read(n).decode('utf-8-sig', errors='replace'))
        lib = root.attrib.get('Name')
        if not lib:
            raise SystemExit(f'{pak}: {n} has no library name')
        for t in root.iter('GameToken'):
            out.append((lib + '.' + t.attrib['Name'], lib, t.attrib.get('LocalOnly', '0') == '1'))
    return out


# Story state sync (server -> clients, SinglePlayer.lua C3CoopTokenTick). Default per level: every token of the level's
# libraries that is not LocalOnly, minus
#   - libraries that are per machine or cross-level state (TOKEN_LIBS_SKIP),
#   - tokens of other libraries than "Level" that the level's flowgraphs never reference (the Cinematics/Dialog_Tokens
#     libraries are shared by all levels),
#   - tokens only cosmetic triggers set (classify_triggers),
#   - the level's deny patterns;
# plus the LocalOnly tokens matching the level's allow patterns. LocalOnly means nothing in single player, and in
# Fields..Cave the designers left most story tokens LocalOnly (Fields 209/240, Cave 245/257 of the Level library), so
# those levels allow their story groups back explicitly. deny always wins. Every per-level pattern must match a token of
# that level (typos fail the build).
#   uplink   {token: value}: a client whose own flowgraph reaches that value tells the server, which sets it (results of
#            per-machine minigames that the server's story waits for).
TOKEN_LIBS_SKIP = {'Lighting', 'Lighting_debug', 'Music', 'Game'}
TOKEN_POLICY = {
    'jailbreak': dict(
        allow=[],
        # the player's own suit/weapon state and the tutorial counters (prompts, dialog lines) count on each machine for
        # its own player; synced, the host's cloak would trigger the clients' tutorial prompts, and GameTokenModify
        # counters would count twice (local add + server value)
        deny=['Level.Mission.Player_*', 'Level.Tutorial.*', 'Level.AB1.Bow_Hit', 'Level.AB1.Bow_Miss',
              'Level.Dialog.Info_Stream']),
    'fields': dict(
        allow=['Level.*', 'Cinematics.Fields.*'],
        deny=['Level.Debug.*', 'Level.Music.*', 'Level.Global.Player_Weapon',
              # the player's own suit state (dialog / tutorial checks)
              'Level.Global.ArmorModeActive', 'Level.Misc.PlayerCloaked',
              # tower searchlight / kill line: per player (the kill lines are server-side kill volumes)
              'Level.Tower.DetectsPlayer', 'Level.Tower.PlayerEscaped']),
    'canyon': dict(
        allow=['Level.*'], deny=[],
        # CY10: the nexus gates open on the machine whose player pressed use at the remote (Input:Action); the other
        # machines play the opening when the token reaches them (TOKEN_SEQUENCES)
        # CYH: the laser grid is per player now (TRIGGER_OVERRIDES cosmetic): a client's laser hit deploys the turret on
        # the server too
        uplink={'Level.AB3.Nexus_Gate_Opened': 'true', 'Level.Dam.Turret_Deploy_Right': 'true'}),
    'swamp': dict(allow=['Level.*', 'Cinematics.Swamp.*'],
                  deny=['Level.AB2.Suit_LastMode']),  # suit mode restored after the mind carrier: per player
    'river': dict(allow=['Level.*', 'Cinematics.River.*'],
                  deny=['Level.Mission.PlayerInVehicle', 'Level.Mission.DebugLogic', 'Level.Suit.*', 'Level.Tutorial.*',
                        'Level.Achievement.*']),
    'river2': dict(allow=['Level.*', 'Cinematics.River.*'],
                   deny=['Level.Mission.PlayerInVehicle', 'Level.Mission.DebugLogic', 'Level.Tutorial.*',
                         'Level.Achievement.*', 'Level.AB4.TowerBattlePlayerPosition']),
    'islands': dict(
        allow=['Level.*', 'Cinematics.Islands.*'],
        deny=['Level.Debug.*', 'Level.Layerswitch.Lighting*', 'Level.Layerswitch.Sound', 'Level.Layerswitch.Cine_20_Lighting',
              # Mastermind mind battle: only the host's machine runs it (the token starts the button mash on every
              # machine that sees it); its result reaches the clients through the other tokens
              'Level.AB3.Mastermind_Start_Minigame',
              # VTOL door-gunner ride: every machine rides its own copy; a host death must not fail the clients' ride
              'Level.AB3.cine_13_vTol_vs_Gunnship_PlayerDeath', 'Level.AB3.cine_13_current_gunship_health'],
        uplink={'Level.AB3.cine_13_vTol_vs_Gunship_finished': 'true'}),
    'cave': dict(
        # cave_gates.md section 4
        allow=['Level.Mission.*', 'Level.AB1.*', 'Level.AB2.*', 'Level.AB3.*', 'Level.AB3C_Finishing.State', 'Cinematics.Cave.*'],
        deny=['Level.Global.*', 'Level.LayerSwitch.*', 'Level.Flash.*', 'Level.suit.*', 'Level.Bow.*', 'Level.Debug.*',
              'Level.Savegames.*', 'Level.Dialog.*', 'Level.AB3B_Tornado.*', 'Level.AB3C_Finishing.debug_*',
              # boss duel: only the host's machine runs the tractor beam / button mash (Player_Boss_Control_active starts
              # it on every machine that sees it); per-player values stay per machine
              'Cave.AB3_Boss.Player_*', 'Cave.AB3_Boss.debug_*'],
        uplink={'Cave.AB3_Boss.Task': 'Finished'}),  # bow shot: a client's hit finishes the boss on the server too
    'endgame': dict(
        allow=[],
        # the finale runs on every machine for its own player (satellite aim input, hack minigame): nothing is shared
        deny=['Level.Satellite.*', 'Cinematics.EndGame.*']),
}


# Track view sequences a flowgraph plays only on the machine where a local input happened: every machine that gets the
# token value from the network (server: uplink, clients: token sync) plays the sequence too (Movie.PlaySequence).
TOKEN_SEQUENCES = {
    'canyon': {'Level.AB3.Nexus_Gate_Opened': ('true', 'AB3_gates_Open')},
}


def _token_match(pattern, name):
    import fnmatch
    return fnmatch.fnmatchcase(name, pattern)


def select_level_tokens(level, tokens, refs, local_tokens):
    """The tokens the server syncs for a level (see TOKEN_POLICY). Returns (names, stats)."""
    policy = TOKEN_POLICY.get(level, {})
    allow, deny = policy.get('allow', []), policy.get('deny', [])
    names = {t for t, _, _ in tokens}
    for pattern in allow + deny + list(policy.get('uplink', {})):
        if not any(_token_match(pattern, n) for n in names):
            raise SystemExit(f'TOKEN_POLICY[{level}]: {pattern!r} matches no token of the level')
    out, stats = [], collections.Counter()
    for name, lib, local_only in tokens:
        if lib in TOKEN_LIBS_SKIP:
            continue
        if lib != 'Level' and name not in refs:
            continue
        if any(_token_match(p, name) for p in deny):
            stats['denied'] += 1
            continue
        if local_only and not any(_token_match(p, name) for p in allow):
            stats['localonly'] += 1
            continue
        if name in local_tokens:
            stats['cosmetic'] += 1
            continue
        stats['allowed localonly' if local_only else 'shared'] += 1
        out.append(name)
    return out, stats


def level_signatures(all_tokens, old_shared):
    """A token of each level's "Level" library that exists in no other level (GameToken.GetToken is nil elsewhere), to
    tell which level is loaded. Prefers the first unique token of the old non-LocalOnly list (same choice as before)."""
    seen = collections.Counter()
    for level, tokens in all_tokens.items():
        for name in {t for t, _, _ in tokens}:
            seen[name] += 1
    sig = {}
    for level, tokens in all_tokens.items():
        candidates = old_shared.get(level, []) + [t for t, lib, _ in tokens if lib == 'Level']
        unique = [t for t in candidates if seen[t] == 1 and t.startswith('Level.')]
        if unique:
            sig[level] = unique[0]
    return sig


# output port index of the fade-in sources start_fade_records accepts (one-output classes and their first output)
FADE_SOURCE_PORTS = {'Time:Delay': ('out', 0), 'Logic:Gate': ('Out', 0), 'Logic:Any': ('out', 0), 'Logic:Once': ('Out', 0)}


def start_fade_records(pak):
    """fgmirror records ("owner id,node,port,v,class,owner name") that fade the screen back in for a player who joins
    after the level start: in every graph where Game:Start fades the screen out (Image:ScreenFader.FadeOut), the outputs
    that feed only FadeIn inputs of its faders. Node numbers are runtime indices (comment nodes do not count)."""
    z = zipfile.ZipFile(pak)
    names = [n for n in z.namelist() if n.replace('\\', '/').lower() == 'mission_mission0.xml']
    objects = ET.fromstring(z.read(names[0])).find('Objects') if names else None
    records = []
    for e in (objects.iter('Entity') if objects is not None else ()):
        fg = e.find('FlowGraph')
        owner, owner_id = e.get('Name') or '', e.get('EntityId')
        if fg is None or not owner_id or not re.fullmatch(r'[A-Za-z0-9_.\-]+', owner):
            continue
        cls, index = {}, {}
        for n in fg.iter('Node'):
            c = n.get('Class') or ''
            cls[n.get('Id')] = c
            if not c.startswith('_comment'):
                index[n.get('Id')] = len(index)
        edges = [(ed.get('nodeOut'), ed.get('portOut'), ed.get('nodeIn'), ed.get('portIn')) for ed in fg.iter('Edge')
                 if ed.get('enabled') != '0']
        blackout = any(cls.get(a) == 'Game:Start' and cls.get(b) == 'Image:ScreenFader' and pi == 'FadeOut'
                       for a, _, b, pi in edges)
        if not blackout:
            continue
        for a, po, b, pi in edges:
            src = FADE_SOURCE_PORTS.get(cls.get(a))
            if cls.get(b) != 'Image:ScreenFader' or pi != 'FadeIn' or not src or src[0] != po or a not in index:
                continue
            if all(cls.get(b2) == 'Image:ScreenFader' and pi2 == 'FadeIn' for a2, po2, b2, pi2 in edges
                   if a2 == a and po2 == po):
                rec = f'{owner_id},{index[a]},{src[1]},v,{cls[a]},{owner}'
                if rec not in records:
                    records.append(rec)
    return records


def start_fades_lua(fades):
    """C3COOP_START_FADEIN = { <level> = "<records>" }: sent to a player who joins after the level start."""
    lines = ['C3COOP_START_FADEIN = {']
    for level, recs in fades.items():
        if recs:
            lines.append('\t%s = "%s",' % (level, '|'.join(recs)))
    lines.append('};')
    return ('\n'.join(lines) + '\n').encode('utf-8')


def level_tokens_lua(synced, signatures, uplinks):
    """C3COOP_LEVEL_TOKENS = { <level> = { "<Library>.<token>", ... } }: the tokens the server syncs to the clients
    (select_level_tokens). C3COOP_LEVEL_SIGNATURE = { <level> = "Level.<token>" }: a token no other level has, to tell
    which level is loaded. C3COOP_TOKEN_UPLINK = { <level> = { [token] = value } } (TOKEN_POLICY uplink)."""
    lines = ['-- generated by scripts/build_pak.py: shared game tokens of each campaign level (TOKEN_POLICY)',
             'C3COOP_LEVEL_TOKENS = {']
    for level, names in synced.items():
        lines.append('\t%s = {%s},' % (level, ', '.join('"%s"' % t for t in names)))
    lines.append('};')
    lines.append('C3COOP_LEVEL_SIGNATURE = {')
    for level, token in signatures.items():
        lines.append('\t%s = "%s",' % (level, token))
    lines.append('};')
    lines.append('C3COOP_TOKEN_SEQUENCES = {')
    for level, seqs in TOKEN_SEQUENCES.items():
        lines.append('\t%s = {%s},' % (level, ', '.join('[%s] = {value = %s, seq = %s}' % (
            _lua_value(t), _lua_value(v), _lua_value(q)) for t, (v, q) in seqs.items())))
    lines.append('};')
    lines.append('C3COOP_TOKEN_UPLINK = {')
    for level, up in uplinks.items():
        lines.append('\t%s = {%s},' % (level, ', '.join('[%s] = %s' % (_lua_value(k), _lua_value(v)) for k, v in up.items())))
    lines.append('};')
    return ('\n'.join(lines) + '\n').encode('utf-8')


# ---------------------------------------------------------------- level missions: entities, volumes, flowgraphs
def _vec(text, default):
    if not text:
        return list(default)
    return [float(v) for v in text.split(',')]


def _qrot(q, v):
    """Rotates vector v by quaternion q = (w, x, y, z)."""
    w, x, y, z = q
    ix = w * v[0] + y * v[2] - z * v[1]
    iy = w * v[1] + z * v[0] - x * v[2]
    iz = w * v[2] + x * v[1] - y * v[0]
    iw = -x * v[0] - y * v[1] - z * v[2]
    return [ix * w - iw * x - iy * z + iz * y,
            iy * w - iw * y - iz * x + ix * z,
            iz * w - iw * z - ix * y + iy * x]


def _qmul(a, b):
    w1, x1, y1, z1 = a
    w2, x2, y2, z2 = b
    return [w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2]


def _yaw(q):
    """CryEngine Ang3 z (yaw, radians) of a rotation: atan2(m10, m00)."""
    w, x, y, z = q
    return math.atan2(2 * (x * y + w * z), 1 - 2 * (y * y + z * z))


TRIGGER_CLASSES = ('ProximityTrigger', 'AreaTrigger', 'CinematicTrigger')


class LevelModel:
    """Entities (world transforms, properties, area shapes) and flowgraphs of a level's mission_mission0.xml."""

    def __init__(self, pak):
        self.by_id, self.by_guid = {}, {}
        self.by_name = collections.defaultdict(list)
        self.nodes = {}  # (graph index, node id) -> (class, entity or None, inputs)
        self.out = collections.defaultdict(list)  # node key -> [(port out, node key, port in)]
        self.token_listeners = collections.defaultdict(list)
        self.token_setters = collections.defaultdict(list)
        self.entity_nodes = collections.defaultdict(list)  # entity id -> node keys of entity:<class> nodes
        self.shapes_of = collections.defaultdict(list)  # trigger entity id -> area entities linked to it
        z = zipfile.ZipFile(pak)
        names = [n for n in z.namelist() if n.replace('\\', '/').lower() == 'mission_mission0.xml']
        if not names:
            return
        objects = ET.fromstring(z.read(names[0])).find('Objects')
        if objects is None:
            return
        for e in objects.iter('Entity'):
            cls = e.get('EntityClass')
            if cls is None:  # <Entity Id=".."/> references inside areas and links
                continue
            o = SimpleNamespace(name=e.get('Name') or '', cls=cls, id=e.get('EntityId'), guid=e.get('EntityGuid'),
                                parent=e.get('ParentId'), lpos=_vec(e.get('Pos'), (0, 0, 0)),
                                lrot=_vec(e.get('Rotate'), (1, 0, 0, 0)), lscale=_vec(e.get('Scale'), (1, 1, 1)),
                                props={}, area=None, fg=e.find('FlowGraph'))
            p = e.find('Properties')
            if p is not None:
                for el in p.iter():
                    for k, v in el.attrib.items():
                        o.props[k if el is p else el.tag + '.' + k] = v
            a = e.find('Area')
            if a is not None:
                o.area = {'height': float(a.get('Height') or 0),
                          'points': [_vec(pt.get('Pos'), (0, 0, 0)) for pt in a.iter('Point')],
                          'box': (_vec(a.get('BoxMin'), (0, 0, 0)), _vec(a.get('BoxMax'), (0, 0, 0)))
                          if a.get('BoxMin') else None,
                          'radius': float(a.get('Radius')) if a.get('Radius') else None,
                          'targets': [t.get('Id') for t in a.iter('Entity')]}
            self.by_id[o.id] = o
            if o.guid:
                self.by_guid[o.guid] = o
            self.by_name[o.name].append(o)
        for o in self.by_id.values():
            o.pos, o.rot, o.scale = self._world(o)
            if o.area:
                for target in o.area['targets']:
                    self.shapes_of[target].append(o)
        for gi, owner in enumerate(o for o in self.by_id.values() if o.fg is not None):
            for n in owner.fg.iter('Node'):
                ent = owner if n.get('GraphEntity') is not None else self.by_guid.get(n.get('EntityGUID_64') or '')
                inp = n.find('Inputs')
                key = (gi, n.get('Id'))
                cls = n.get('Class') or ''
                inputs = dict(inp.attrib) if inp is not None else {}
                self.nodes[key] = (cls, ent, inputs)
                token = inputs.get('gametoken_Token') or inputs.get('Token')
                if token and cls == 'Mission:GameToken':
                    self.token_listeners[token].append(key)
                if token and cls in ('Mission:GameTokenSet', 'Mission:GameTokenModify'):
                    self.token_setters[token].append(key)
                if ent is not None and cls == 'entity:' + ent.cls:
                    self.entity_nodes[ent.id].append(key)
            for ed in owner.fg.iter('Edge'):
                if ed.get('enabled') == '0':
                    continue
                self.out[(gi, ed.get('nodeOut'))].append((ed.get('portOut'), (gi, ed.get('nodeIn')), ed.get('portIn')))

    def _world(self, o, depth=0):
        parent = self.by_id.get(o.parent) if o.parent else None
        if parent is None or depth > 20:
            return o.lpos, o.lrot, o.lscale
        pp, pr, ps = self._world(parent, depth + 1)
        rp = _qrot(pr, [o.lpos[i] * ps[i] for i in range(3)])
        return [pp[i] + rp[i] for i in range(3)], _qmul(pr, o.lrot), [ps[i] * o.lscale[i] for i in range(3)]

    def entity(self, name):
        found = self.by_name.get(name) or []
        if len(found) != 1:
            raise SystemExit(f'level entity {name!r}: {len(found)} matches')
        return found[0]

    def volume(self, o):
        """Lua zone of a trigger: {box = {x1, y1, x2, y2}, z1 =, z2 =} / {poly = {x1, y1, ...}, z1 =, z2 =} /
        {any = {zone, ...}} (AreaTrigger with several shapes). Rotation of proximity boxes is ignored (not rotatable)."""
        if o.cls in ('ProximityTrigger', 'CinematicTrigger'):
            f = 'f' if o.cls == 'CinematicTrigger' else ''
            dims = [float(o.props.get(f + 'Dim' + c, 5)) for c in 'XYZ']
            return {'box': [o.pos[0] - dims[0] / 2, o.pos[1] - dims[1] / 2, o.pos[0] + dims[0] / 2, o.pos[1] + dims[1] / 2],
                    'z1': o.pos[2] - dims[2] / 2, 'z2': o.pos[2] + dims[2] / 2}
        zones = []
        for s in self.shapes_of.get(o.id, []):
            area = s.area
            height = area['height'] if area['height'] > 0 else 1000.0
            if area['box']:
                lo, hi = area['box']
                corners = [[x, y, zz] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for zz in (lo[2], hi[2])]
                world = [[s.pos[i] + _qrot(s.rot, c)[i] for i in range(3)] for c in corners]
                zones.append({'box': [min(c[0] for c in world), min(c[1] for c in world),
                                      max(c[0] for c in world), max(c[1] for c in world)],
                              'z1': min(c[2] for c in world), 'z2': max(c[2] for c in world)})
            elif area['radius']:
                r = area['radius']
                zones.append({'box': [s.pos[0] - r, s.pos[1] - r, s.pos[0] + r, s.pos[1] + r],
                              'z1': s.pos[2] - r, 'z2': s.pos[2] + r})
            elif len(area['points']) >= 3:
                poly = []
                for pt in area['points']:
                    w = [s.pos[i] + _qrot(s.rot, pt)[i] for i in range(3)]
                    poly += [w[0], w[1]]
                zones.append({'poly': poly, 'z1': s.pos[2], 'z2': s.pos[2] + height})
        if not zones:
            return None
        return zones[0] if len(zones) == 1 else {'any': zones}


# Flowgraph classification of trigger outputs (see classify_triggers). A trigger is cosmetic when every node reachable
# from its outputs within FG_MAX_HOPS hops (edges, plus Mission:GameTokenSet -> Mission:GameToken listeners of that token)
# is either plumbing (FG_TRANSPARENT*) or a local effect (FG_COSMETIC*); anything else, or a chain longer than the hop
# budget, makes it a gameplay trigger.
FG_MAX_HOPS = 10
FG_TRANSPARENT = {
    'Time:Delay', 'Time:Timer', 'Time:RandomDelay', 'Time:TimedCounter', 'Game:CheckPlatform', 'Game:IsDemo',
    'Game:DifficultyLevel', 'Entity:GetPos', 'Entity:EntityInfo', 'Entity:EntityId', 'Entity:PropertyGet',
    'Entity:CheckDistance', 'Entity:EntitiesInRange', 'Actor:LocalPlayer', 'Actor:HealthGet', 'Inventory:ItemCheck',
    'Inventory:AmmoCheck', 'Weapon:ZoomCheck', 'NanoSuit:NanoSuitGet', 'Input:CheckInvertedYAxis'}
FG_TRANSPARENT_PREFIXES = ('Logic:', 'Math:', 'Vec3:', 'String:', 'Interpol:', 'Mission:GameToken')
FG_COSMETIC = {'Game:ForceFeedback', 'Entity:Material', 'Entity:MaterialParam', 'Entity:RenderParams',
               'Entity:MaterialSerialize'}
FG_COSMETIC_PREFIXES = ('Sound:', 'Music:', 'Image:', 'CrysisFX:', 'Environment:', 'Time:TimeOfDay', 'Camera:', 'HUD:',
                        'Debug:')
FG_GAMEPLAY = {'Sound:PlayDialog', 'Sound:Dialog'}  # story dialogs: everybody hears them when the server fires
FG_COSMETIC_ENTITIES = {
    'Light', 'EnvironmentLight', 'DestroyableLight', 'AmbientVolume', 'SoundSpot', 'SoundEventSpot', 'RandomSoundVolume',
    'ParticleEffect', 'FogVolume', 'Rain', 'Snow', 'Lightning', 'LightningArc', 'WindArea', 'MusicLogicTrigger'}
COSMETIC_LAYER = re.compile(r'(^|_)(lighting|particles|clouds)(_|$)', re.I)  # Engine:LayerSwitch of these is local


def _node_kind(cls, ent, inputs, trigger):
    if cls in ('_comment', '_commentbox'):
        return None
    if cls.startswith('entity:'):
        if ent is trigger:
            return 'transparent'  # the trigger's own ports (Enable/Disable itself)
        return 'cosmetic' if cls[7:] in FG_COSMETIC_ENTITIES else 'gameplay'
    if cls == 'Engine:LayerSwitch':
        return 'cosmetic' if COSMETIC_LAYER.search(inputs.get('Layer', '')) else 'gameplay'
    if cls in FG_GAMEPLAY:
        return 'gameplay'
    if cls in FG_TRANSPARENT or cls.startswith(FG_TRANSPARENT_PREFIXES):
        return 'transparent'
    if cls in FG_COSMETIC or cls.startswith(FG_COSMETIC_PREFIXES):
        return 'cosmetic'
    return 'gameplay'


def _reach(model, trigger, ports=None):
    """Walks the flowgraph downstream of the trigger's entity nodes. Returns (kinds seen, {(class, kind)} seen, setter
    nodes reached, truncated by the hop budget)."""
    kinds, classes, setters, truncated = set(), set(), set(), False
    frontier = collections.deque()
    for key in model.entity_nodes.get(trigger.id, []):
        for port, nxt, _ in model.out.get(key, []):
            if ports is None or port in ports:
                frontier.append((nxt, 1))
    seen = set()
    while frontier:
        key, depth = frontier.popleft()
        if key in seen or key not in model.nodes:
            continue
        seen.add(key)
        cls, ent, inputs = model.nodes[key]
        kind = _node_kind(cls, ent, inputs, trigger)
        if kind is None:
            continue
        kinds.add(kind)
        classes.add((cls, kind))
        # every input counts, also Close/Reset/Disable ones: they change what a stateful node lets through later
        nexts = [nxt for _, nxt, _ in model.out.get(key, [])]
        if cls in ('Mission:GameTokenSet', 'Mission:GameTokenModify'):
            setters.add(key)
            nexts += model.token_listeners.get(inputs.get('gametoken_Token') or inputs.get('Token'), [])
        if nexts and depth >= FG_MAX_HOPS:
            truncated = True
            continue
        frontier.extend((nxt, depth + 1) for nxt in nexts)
    return kinds, classes, setters, truncated


def _is_kill_volume(model, trigger):
    """Enter reaches Actor:KillPlayer and nothing else that matters (debug skip triggers that also reach a kill node
    somewhere downstream stay gameplay triggers)."""
    _, classes, _, truncated = _reach(model, trigger, {'Enter'})
    if ('Actor:KillPlayer', 'gameplay') not in classes:
        return False
    other = {c for c, kind in classes if kind == 'gameplay'} - {'Actor:KillPlayer'}
    if other or truncated:
        print(f'  note: {trigger.name} reaches Actor:KillPlayer among other gameplay nodes: treated as a gameplay trigger')
        return False
    return True


# Per-level corrections of classify_triggers:
#   kill       kill volumes whose Actor:KillPlayer sits beyond the hop budget or behind effects (rockets, explosions):
#              as gameplay triggers their replicated Enter would kill every machine's own player
#   cosmetic   per-player triggers: each machine evaluates only its own player (suit scanners: as gameplay triggers their
#              replicated Enter would disable / damage every machine's own player)
TRIGGER_OVERRIDES = {
    'fields': dict(kill=['pTrigger_AB1_Deathline', 'aTrigger_TowerKillLine']),  # FDK1/FDK2 (tower rocket kill lines)
    'canyon': dict(kill=['kill_trigger_boundary'],                               # CYK2 (tower rockets behind the dam)
                   # CYH1 laser grid (raycast timers -> Actor:Damage 75 + NanoSuit:Disable on the local player), the
                   # reactor EMP scanner plane (NanoSuit:Disable on the local player) and CYH4 the scanner door prefabs
                   # (6 + 4 copies of one name: Actor:Damage, PlayerStumble, NanoSuit:Disable, chroma filter, all on
                   # the local player; left "original", the host's copy also hit the host when a client walked through)
                   cosmetic=['trigger_scanner_4', 'trigger_scanner_5', 'turret_scanner_trigger_1',
                             'dam_scanner_door_prefab', 'dam_scanner_door_prefab2']),
}


def classify_triggers(model):
    """Co-op handling of each local (bNetworked=0) trigger of a level:
      cosmetic  everything downstream is a local effect: stays local on every machine, local player only
      kill      Enter reaches Actor:KillPlayer: the server kills the entering player
      original  the name is not unique (events are keyed by name): left as in the game
      gameplay  (not listed) server-authoritative lockstep
    anyplayer lists the gameplay/kill triggers with bOnlyMyPlayer / bOnlyLocalPlayer: in single player that meant "the
    player"; on the server it would mean the host only, so the server ignores the flag for them.
    Also returns the tokens set only by cosmetic chains (not synced by the server)."""
    result = {'cosmetic': set(), 'kill': set(), 'original': set(), 'anyplayer': set()}
    local_player_only = set()
    stats = collections.Counter()
    per_name = collections.defaultdict(list)
    cosmetic_setters, gameplay_setters = set(), set()
    for o in model.by_id.values():
        if o.cls not in TRIGGER_CLASSES or o.props.get('MultiplayerOptions.bNetworked', '0') != '0':
            continue
        kinds, classes, setters, truncated = _reach(model, o)
        script = (o.props.get('ScriptCommand') or '') + (o.props.get('PlaySequence') or '') + (o.props.get('Sequence') or '')
        if o.props.get('bOnlyMyPlayer', '0') != '0' or o.props.get('bOnlyLocalPlayer', '0') != '0':
            local_player_only.add(o.name)
        if o.cls != 'CinematicTrigger' and _is_kill_volume(model, o):
            mode = 'kill'
        elif 'gameplay' in kinds or truncated or script.strip():
            mode = 'gameplay'
        else:
            mode = 'cosmetic'
        (cosmetic_setters if mode == 'cosmetic' else gameplay_setters).update(setters)
        per_name[o.name].append(mode)
    for name, modes in per_name.items():
        if len(modes) > 1 and set(modes) != {'cosmetic'}:
            mode = 'original'
        else:
            mode = modes[0]
        stats[mode] += 1
        if mode != 'gameplay':
            result[mode].add(name)
        if mode in ('gameplay', 'kill') and name in local_player_only:
            result['anyplayer'].add(name)
            stats['anyplayer'] += 1
    local_tokens = set()
    for token, keys in model.token_setters.items():
        if keys and all(k in cosmetic_setters for k in keys) and not any(k in gameplay_setters for k in keys):
            local_tokens.add(token)
    return result, local_tokens, stats


def _lua_value(v, indent=''):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, (int, float)):
        return ('%.3f' % v).rstrip('0').rstrip('.') if isinstance(v, float) else str(v)
    if isinstance(v, str):
        return '"%s"' % v.replace('\\', '\\\\').replace('"', '\\"')
    if isinstance(v, (list, tuple)):
        return '{' + ', '.join(_lua_value(x, indent) for x in v) + '}'
    if isinstance(v, dict):
        return '{' + ', '.join('%s = %s' % (k, _lua_value(x, indent)) for k, x in v.items()) + '}'
    raise TypeError(type(v))


# ---------------------------------------------------------------- co-op gates (Scripts/c3coop/Gates_<level>.lua)
# One-way moments of a level (doors that close behind the players, geometry hidden behind them). The server holds the
# gated activation until the quorum of alive players (C3COOP_GATE_QUORUM) is in `zone`, then brings everybody else to
# `target` and fires it (Lockstep.lua).
#   trigger / use   name of the gated trigger (its Enter is held) / InteractiveEntity (its use is held)
#   zone            None: the trigger's own volume; ('trigger', name): another trigger's volume;
#                   ('box', (x1, y1, z1), (x2, y2, z2))
#   target          teleport point: a level entity name (floor position and yaw taken from it) or (x, y, z)
#   yaw             degrees, for (x, y, z) targets
#   commit          (token, value): one-way token behind the gate; when it takes that value on the server, everybody
#                   still outside zone/keep and farther than radius from the target is brought along (safety net for
#                   paths that bypass the trigger, e.g. Psycho closing a door)
#   keep            ('box', ...): area ahead of the gate; players there are never pulled back
#   estimate        what is not taken from level data (check these in game)
# Added for the other levels:
#   switch          name of a Switch entity (Net.Exposed, Switch.lua): its server-side SvRequestUse is held
#   zone            also ('near', entity, r): within r m of that entity's position at run time (moving entities);
#                   ('any', zone, zone, ...): inside any of them (players may split between two lifts)
#   target          also ('dynamic', entity): that entity's position at run time
#   ring            radius of the ring of teleport points around the target (default 0.9 m)
#   quorum          'all': every alive player must be in the zone (ignores C3COOP_GATE_QUORUM)
#   group           gates sharing one state (one of two lifts; once one fired the others no longer hold)
#   commit_target   where the commit token brings stragglers (default: target), e.g. the top of a lift that has left
#   start           level start: hold until as many players as the previous level had are back (rejoin after the host's
#                   level change) or C3COOP_START_TIMEOUT s; no zone, no teleports
#   replay          a player who joins after the gate fired gets its Enter replayed for himself (EndGame: the finale
#                   only starts from it)
# Commit-only gates (no trigger/use/switch) just bring stragglers when the commit token changes.
GATES = {
    'jailbreak': [
        dict(id='JB04', what='silencer room: outer door closes behind', trigger='PlayerPos_Dock_A_Pos_pTrigger',
             target='SpawnPoint_03_Dock_A', commit=('Level.AB1.Dock_A_OuterDoor_Closes', 'true'), radius=6),
        dict(id='JB05', what='Dock A security door closes behind Psycho',
             trigger='Psycho_Dock_A_CloseSecurityDoor_pTriggerOnce', zone=('trigger', 'PlayerPos_Dock_A_Security_aTrigger'),
             target='Psycho_Dock_A_Security_P1', radius=4),
        dict(id='JB07', what='bow room cutscene (door closes, Dock A hidden)', trigger='Start_Cine_BowIntro_aTriggerOnce',
             zone=('box', (379, 234, 127.5), (389, 244, 132.5)), target=(381.9, 236.5, 128.7), yaw=-45,
             commit=('Level.Cine.BowIntro_Started', 'true'), radius=6, estimate='zone box, target position'),
        dict(id='JB09', what='Dock B back gate closes, Dock B hidden', trigger='PlayerPos_Dock_B_Exit_pTrigger',
             target='SpawnPoint_06_Platform_Exit', commit=('Level.AB1.Dock_B_BackgateDoor_Closes', 'true'), radius=6),
        dict(id='JB11', what='Airlock A inner door closes, AB1 hidden', trigger='PlayerPos_Airlock_A_Door_pTrigger',
             target='SpawnPoint_07_Airlock_A', commit=('Level.AB2.Airlock_A_InnerDoor_Closes', 'true'), radius=6),
        dict(id='JB14', what='Airlock B outer doors close, bridge hidden', trigger='Airlock_B_PlayerEnter_pTriggerOnce',
             zone=('box', (247.5, 440, 114.5), (259.5, 446.5, 119.5)), target=(252.0, 442.4, 115.8), yaw=60,
             commit=('Level.AB3.Airlock_B_OuterDoor_Closes', 'true'), radius=6,
             keep=('box', (219, 440, 110), (260, 462, 122)), estimate='zone y extent, target floor, keep box'),
        dict(id='JB15', what='airlock2 closes behind (scanner tunnel)', trigger='scanner_trigger_1',
             zone=('box', (219.5, 454.5, 112), (237, 461, 118)), target='TagPoint35', radius=4,
             estimate='zone box (tunnel between the airlocks), target floor'),
        dict(id='JB16', what='Airlock B switch: airlock1 closes behind', use='Cell_Switch_Airlock_B',
             zone=('box', (208, 450, 112), (217.5, 467.5, 118)), target='SpawnPoint_08_Airlock_B', radius=4,
             estimate='zone box (inner chamber)'),
        dict(id='JB19', what='base end room: back gate 1 closes', trigger='Player_MeetPsycho_AB3_TriggerOnce',
             zone=('box', (111.8, 599, 112), (119, 609, 118)), target='SpawnPoint_09_BaseEnd', radius=4,
             estimate='zone box (room between the back gates)'),
        dict(id='JB20', what='back gate 2 closes, AB3 base hidden', trigger='Elevator_Close_Backgate2_pTriggerOnce',
             zone=('box', (119.5, 598, 112), (131, 609.5, 118)), target=(124.6, 603.7, 113.6), yaw=-90,
             commit=('Level.AB3.Elevator_Backgate_Closed', 'true'), radius=4,
             keep=('box', (119.5, 560, 105), (140, 612, 125)), estimate='zone box, target floor, keep box'),
        dict(id='JB21', what='tower elevator ride (level end)', trigger='TowerElevator_Player_pTrigger',
             target=(129.0, 574.25, 113.6), yaw=30, radius=2, estimate='target floor height'),
    ],
    # FD07/FD08 layer streaming: ZONES (Level.Global.PlayerPosition).
    'fields': [
        dict(id='FD01', what='level start in the elevator (intro dialog, PlayerStaging)', trigger='pTrigger_level_start',
             start=True),
        dict(id='FD03', what='airlock 2 closes behind, dome door opens', trigger='pTrigger_OpenDomeExteriorDoor',
             target='LeadIn_Prophet_WarpForHandsOff', radius=3),
        dict(id='FD04', what='dome door closes behind, minefield armed', trigger='pTrigger_AB0_PsychoMove1',
             zone=('box', (1985, 400, 140), (2035, 478, 165)), target='LeadIn_Psycho_goto_transition', radius=4,
             estimate='zone box (everything south of the dome door)'),
        dict(id='FD06', what='Cine_10 trainyard reveal (beam 10 m down, drop point explodes)',
             trigger='pTrigger_Trainyard_Cinematic_Start', target=(1865.5, 325.12, 152.25), yaw=-90, radius=4,
             estimate='target height (highline deck)'),
        dict(id='FD07', what='trainyard start: roof blocker, checkpoint', trigger='pTrigger_AB1_Start',
             zone=('trigger', 'PlayerPosition_AB1_TrainyardStart'), target='SpawnPoint2', radius=6),
        dict(id='FD09', what='stalker intro switch (Cine_20, beams everybody)', switch='Switch1',
             zone=('near', 'Switch1', 15), target='Cine_20_PlayerSpawn', radius=4),
        dict(id='FD10', what='shed entry: fake door closes, AB1_Transition hidden', trigger='pTrigger_AB2_Start',
             zone=('trigger', 'PlayerPosition_AB2_ShedStairs'), target='SpawnPoint5',
             commit=('Level.LayerSwitch.Enable_AB1_Transition', 'false'), radius=4,
             estimate='SpawnPoint5 assumed past the kick door'),
        dict(id='FD11', what='shed exit: AB2 (shed) hidden, back-tracking blocker', trigger='pTrigger_AB2_Unload',
             target='AB3_StalkerA_4_start', commit=('Level.LayerSwitch.Enable_AB2', 'false'), radius=6),
        dict(id='FD13', what='tanker wagon push (train ride, TrainSequence attaches every machine\'s player)',
             switch='AB3_TrainPushSwitch', zone=('trigger', 'AB3_PushDialogTrigger'), target='Cine_40_DebugStart_Pos',
             commit=('Level.AB3.TrainRideStarted', 'true'), radius=10),
        # riders count while they are on the wagon (PlayerLink_POS is a child of Tunnel_Train, carried by WagonRoll),
        # so a held Enter is not dropped when the wagon leaves the trigger box
        dict(id='FD14', what='tunnel end: beam off the wagon, Cine_40_Extro (level end)',
             trigger='TrainRide_ProxTrigger_EndTransition',
             zone=('any', ('trigger', 'TrainRide_ProxTrigger_EndTransition'), ('near', 'PlayerLink_POS', 8)),
             target='TrainRideOver_BeamPlayerPos', radius=6, estimate='8 m around the wagon link point'),
    ],
    # CY08/CY09 reversible hides: ZONES. CYK1 floor kill volume: KILL_SAFE.
    'canyon': [
        dict(id='CY01', what='level start (Cine_20 meet rebels)', trigger='Master_Spawn_Trigger', start=True),
        dict(id='CY02', what='cliff edge: Cine_25_Observe (every machine holsters and beams its player)',
             trigger='Start_observe_scene', zone=('near', 'observe_end_pos', 10), target='observe_end_pos', radius=4,
             estimate='zone: 10 m around the cliff-edge end position (the trigger is a 3x1 m strip)'),
        dict(id='CY03', what='creek cabin falls 65 m, upper transition hidden', trigger='elevator_creek_trigger-2',
             zone=('box', (923.2, 851.6, 147.5), (929.3, 857.6, 152.5)), target=(926.25, 854.62, 150.6), yaw=0,
             ring=0.8, commit=('Level.Mission.Backgate_AB1', 'true'), commit_target='SpawnPoint3', radius=3,
             estimate='cabin box and roof height'),
        # players may split between the two dam towers: the quorum counts both cabins; whichever cabin goes first
        # finishes the group (the other cabin still works, its switch is no longer held)
        dict(id='CY04', what='right dam tower cabin up (canyon floor hidden at z 96, trap, vent)', group='dam',
             use='Switches.Cell_Switch_Once2',
             zone=('any', ('box', (761.0, 687.3, 78.0), (765.4, 691.7, 82.5)), ('box', (939.4, 678.7, 78.0), (943.8, 683.1, 82.5))),
             target=(763.3, 689.6, 79.3), yaw=0, ring=0.8, commit=('Level.AB2.Player_At_Dam', 'true'),
             commit_target='SpawnPoint4', radius=4, estimate='cabin boxes and floor height'),
        dict(id='CY05', what='left dam tower cabin up', group='dam', use='Switches.Cell_Switch_Once7',
             zone=('any', ('box', (761.0, 687.3, 78.0), (765.4, 691.7, 82.5)), ('box', (939.4, 678.7, 78.0), (943.8, 683.1, 82.5))),
             target=(941.6, 680.9, 79.3), yaw=0, ring=0.8, radius=4, estimate='cabin boxes and floor height'),
        dict(id='CY07', what='dam C4 (Cine_Dam_Escape beams everybody downstream, flood)', switch='Dam_Explosive_Position_Final',
             zone=('near', 'Dam_Explosive_Position_Final', 15), target=('dynamic', 'Dam_Explosive_Position_Final'),
             ring=2.0, radius=4, estimate='ring around the C4 point'),
        dict(id='CY11L', what='nexus left cabin down (AB3_Upper_Art hidden 10 s later)', group='nexus',
             use='nexus_top_elev_left_switch',
             zone=('any', ('box', (500.8, 1358.8, 57.0), (504.4, 1362.5, 61.0)), ('box', (488.8, 1358.8, 57.0), (492.4, 1362.5, 61.0))),
             target=(502.61, 1360.65, 58.6), yaw=0, ring=0.8, commit=('Level.AB3.Elevator_to_Reactor_Used', 'true'),
             commit_target='SpawnPoint7', radius=4, estimate='cabin boxes and floor height'),
        dict(id='CY11R', what='nexus right cabin down', group='nexus', use='nexus_top_elev_right_switch',
             zone=('any', ('box', (500.8, 1358.8, 57.0), (504.4, 1362.5, 61.0)), ('box', (488.8, 1358.8, 57.0), (492.4, 1362.5, 61.0))),
             target=(490.62, 1360.65, 58.5), yaw=0, ring=0.8, radius=4, estimate='cabin boxes and floor height'),
        dict(id='CY12', what='reactor centre lift (whole reactor hidden at z 78)', use='Switches.Cell_Switch_Once10',
             zone=('box', (493.0, 1334.5, 1.5), (501.0, 1342.5, 7.0)), target=(496.98, 1338.47, 3.2), yaw=0, ring=1.2,
             commit=('Level.Relay_Elevator.Elevator_Finished', 'true'), commit_target='TagPoint395', radius=4,
             estimate='platform box and height'),
        # the switch keyboard is on the upper deck (z 136.4), above dialog_end_ininside_check (z 129.4-134.4): the
        # player using it was never in the zone
        dict(id='CY13', what='mainframe (Cine_30, level end)', switch='mainframe_switch',
             zone=('any', ('trigger', 'dialog_end_ininside_check'), ('near', 'mainframe_switch', 12)),
             target='TagPoint395', radius=4),
    ],
    # SW02 streaming strips: ZONES.
    'swamp': [
        dict(id='SW01', what='level start (cine_10 intro)', trigger='10_Intro', start=True),
        dict(id='SW03', what='end of the ruins: AB1 geometry and collision hidden', trigger='AB1_End',
             zone=('box', (380, 296, 50), (393, 312, 60)), target='SpawnPoint2', commit=('Level.AB1.AB1_End', 'true'),
             radius=4, estimate='zone box (transition corridor)'),
        dict(id='SW04', what='System X window (blocking door, cutscene beams everybody)', use='cine_20_starttrigger',
             zone=('box', (388, 307, 52), (402, 338, 66)), target='cine_20_player_spawn', radius=4,
             estimate='zone box (building interior)'),
        dict(id='SW07', what='Scorcher door: burningman_scorchered (every machine holsters and beams its player)',
             trigger='Scorcher_Start', zone=('near', 'BurningMan_end_position', 10), target='BurningMan_end_position',
             radius=4, estimate='zone: 10 m around the end position in front of the door (the trigger is a doorway strip)'),
        dict(id='SW08', what='collapsed hallway AB2 -> AB3, swamp hidden', trigger='AB2End',
             zone=('box', (364, 612, 50), (386, 632, 58)), target='SpawnPoint3', commit=('Level.AB2.AB2_End', 'true'),
             radius=4, estimate='zone box (hallway west of the collapse)'),
        dict(id='SW10', what='collapsed hallway 2, maze hidden', trigger='AB3Part1End',
             zone=('box', (224, 584, 50), (247.5, 602, 58)), target='SpawnPoint4', commit=('Level.AB3.AB3_Art_End', 'true'),
             radius=4, estimate='zone box (west of the collapse block)'),
        dict(id='SW12', what='tower lift down, yard hidden', trigger='Lift_Start',
             zone=('box', (168.5, 475, 55.5), (175.5, 481, 60)), target='Psycho_P01', ring=0.8,
             commit=('Level.AB3.AB3_Yard_End', 'true'), commit_target='Psycho_P25', radius=3,
             estimate='cabin box'),
        dict(id='SW13', what='lab entrance door snaps shut, tower base hidden', trigger='Psycho_PlayerCheck_Room1',
             zone=('box', (166, 503, 14), (178, 520, 21)), target='Psycho_P24', radius=4,
             estimate='zone box (lab corridor)'),
        dict(id='SW15', what='last lab room: doors close 3 s after the switch', trigger='AB3_Psycho_weaponlower2',
             target='Cine_30_DebugStart_Pos', radius=4),
        dict(id='SW16', what='Psycho argument (cine_30, level end)', trigger='Cine_30_StartTrigger_PT',
             zone=('trigger', 'AB3_Psycho_weaponlower2'), target='Cine_30_DebugStart_Pos', radius=4),
    ],
    # RV02/RV05/RV08 Enter/Leave streaming and AI zones: player-only AreaTriggers, the
    # lockstep's occupancy rule (Leave only when the last player is out) already covers them.
    'river': [
        dict(id='RV01', what='level start (intro pan)', trigger='Cine_Intro_pTrigger', start=True),
        dict(id='RV03', what='Hive Mind hologram (cutscene beams everybody)', switch='Hologram_switch2',
             zone=('box', (1583, 145, 140), (1664, 211, 170)), target='cine_10_player_spawn_GunPickup', radius=4,
             estimate='zone box around the node'),
        dict(id='RV09', what='drop into the bridge pillar (level end)', trigger='EndLevel_pTrigger',
             target=(691.0, 1105.4, 97.0), yaw=0, ring=2.0, radius=6, estimate='target floor height'),
    ],
    # RVL1/RVA1 streaming and AI zones: occupancy rule (player-only AreaTriggers).
    'river2': [
        dict(id='RV01', what='level start (Claire rescue cutscene)', trigger='Cine_20_StartTrigger_PT', start=True),
        dict(id='RV11', what='tower elevator: blocker, doors close, everything below hidden', trigger='AB4_Elevator_aTrigger',
             target='SpawnPoint8', ring=0.8, commit=('Level.AB4.EnteredElevator', 'true'), radius=2.5),
        dict(id='RV12', what='Rasch reveal cutscene (level end)', trigger='betrayal_start _trigger',
             target='Cine_30_ElevatorCenter_Pos', ring=0.8, radius=3),
    ],
    # ISL1 Player_at streaming: ZONES. IS08 Mastermind: TOKEN_POLICY (host only).
    'islands': [
        dict(id='IS01', what='level start (intro dogfight + dialog)', trigger='AB1_Intro_pTrigger', start=True),
        # AAA energy sources: the interaction cutscene locks and beams every machine's own player
        dict(id='IS02', what='AB1 AAA energy source (cine_02 interaction, beams everybody)',
             trigger='AB1_AAA_Interact_pTrigger', target='AB1_AAA_PlayerPos_after_cine', radius=4),
        dict(id='IS03', what='AB2 left AAA energy source (cine_02 interaction, beams everybody)',
             trigger='AB2Left_AAA_Interact_pTrigger', target='AB2Left_AAA_PlayerPos_after_cine', radius=4),
        dict(id='IS04', what='AB2 right AAA energy source (cine_02 interaction, beams everybody)',
             trigger='AB2Right_AAA_Interact_pTrigger', target='AB2Right_AAA_PlayerPos_after_cine', radius=4),
        # the cockpit is a child of the VTOL path point that is beamed to the last destroyed AAA's landing site
        dict(id='IS05', what='Psycho VTOL cockpit (take-off, door-gunner ride, ground layers hidden)',
             use='AB2_Rendezevous_cockpit_InteractiveObject', quorum='all',
             zone=('near', 'AB2_Rendezevous_cockpit_InteractiveObject', 12),
             target=('dynamic', 'AB2_Rendezevous_cockpit_InteractiveObject'), ring=2.5, radius=12,
             estimate='ring around the cockpit (VTOL hull may be in the way)'),
        dict(id='IS06', what='VTOL landing on AB3: everybody to the AGL deck',
             commit=('Level.AB3.cine_13_vTol_vs_Gunship_finished', 'true'), target='Player_Pos_After_Cine_15',
             ring=1.5, radius=15),
        dict(id='IS07', what='enter the VTOL: Mastermind resurrection (beams everybody)',
             trigger='Objective_55_enter_VTOL_pTrigger', zone=('near', 'cine_15_vTol_landing_pos', 10),
             target='Player_Pos_After_Cine_15_2', radius=4),
        dict(id='IS09', what='reach the beam: world collapse (level end)', trigger='AB3_EndCinematic_pTrigger',
             zone=('near', 'AB3_EndCinematic_pTrigger', 9), target='SpawnPoint8', radius=4),
    ],
    # CV16 PlayerAt streaming: ZONES. CV12 duel: TOKEN_POLICY (host only).
    'cave': [
        dict(id='CV01', what='level start (intro, suit malfunction)', trigger='pTrigger_Start_AB1', start=True),
        dict(id='CV04', what='rock bridge collapses into Waterworld', trigger='pTrigger_AB1_tremor2_collapse',
             zone=('box', (570, 796, 357), (583, 812, 368)), target=(576.5, 765.3, 310.4), yaw=180, ring=1.5, radius=8,
             estimate='bridge box, landing point'),
        # mind carrier pads: Cine_Hivemind_Event_0x locks, supercharges and beams every machine's own player. Pylon A is
        # not a once trigger (Logic:Once behind it): its commit token ends the gate after the first firing.
        dict(id='CV05', what='pylon A pad (Hivemind vision 1, beams everybody)', trigger='AB1_aTrigger_Pylon_A',
             target='AB1_Pylon_A_Switch_PlayerPos', commit=('Level.Mission.Beam_A_PlayerEnter', 'true'), radius=4),
        dict(id='CV08', what='pylon B pad (Hivemind vision 2, beams everybody)', trigger='AB2_aTrigger_Pylon_B',
             target='AB2_Pylon_B_Switch_PlayerPos', radius=4),
        dict(id='CV09', what='pylon C pad (Hivemind vision 3, beams everybody)', trigger='AB2_aTrigger_Pylon_C',
             target='AB2_Pylon_C_Switch_PlayerPos', radius=4),
        dict(id='CV10', what='jump into the boss arena (Cine_20 beams ~690 m)', trigger='20_Boss', quorum='all',
             target='cine_20_boss_player_start2', radius=3,
             commit=('Cinematics.Cave.20_BossIntro_done', 'true'), commit_target='SpawnPoint_AB3A'),
        dict(id='CV13', what='boss down: bow finale, everybody to the bow position',
             commit=('Level.AB3C_Finishing.State', 'Finishing_Intro'), target='Cine_27_PlayerPos', ring=1.5, radius=6),
    ],
    # the finale runs on every machine for its own player
    'endgame': [
        dict(id='EG01', what='finale start (Cine_20, hack, satellite)', trigger='pTrigger_SceneStart', start=True,
             replay=True),
    ],
}

# Zone tokens (Scripts/c3coop/Lockstep.lua, "zones"): groups of non-once triggers of which only the latest one entered
# counts ("last writer wins": a single token like Level.Global.PlayerPosition, or a hide/unhide strip pair). In a network
# game their own Enter/Leave is ignored on every machine; the server records the last zone of each alive player and fires
# the trigger of the REARMOST zone (lowest rank; equal ranks: the one entered last) for everybody, so nothing a player
# still needs is unloaded behind him.
#   token      token the triggers write: the value of each trigger is read from the flowgraph and checked against order
#   order      values (token groups) or trigger names (strip groups) in mission order; a nested list gives equal ranks
#   commits    values written by other flowgraph paths (cutscenes) that nobody returns from: once the token takes one,
#              players whose zone ranks lower count as that point
#   nil_zone   zone (value / trigger) of a player who has entered none yet (default: not counted)
#   gates      {value: dict(target=, radius=, quorum=)}: a boundary nobody crosses alone; when a quorum is past it the
#              others are brought to target (forward streaming: the area ahead only loads once the rear player is in)
ZONES = {
    'fields': [
        # fields_gates.md 4: leadin < trainyardstart < huts < tunnel < shed < fields < hill < tunnel < end. The tunnel
        # value hides AB1 and is the only one that loads AB2, so nobody may stay behind (catalog FD08).
        dict(name='PlayerPosition', token='Level.Global.PlayerPosition',
             order=['leadin', 'trainyardstart', 'hut_1', 'hut_2', 'hut_3', 'hut_4', 'ab1_ab2_tunnel', 'ab2_shed_a',
                    'ab2_shed_stairs', 'ab2_shed_b', 'ab3_vvv', 'ab3_fields_a', 'ab3_fields_b', 'ab3_hill', 'ab3_tunnel',
                    'ab3_end'],
             gates={'ab1_ab2_tunnel': dict(target='SpawnPoint3', radius=6)}),
    ],
    'canyon': [
        # CY08: hide AB1/AB2 at the nexus side only when nobody is behind; unhide when anybody goes back
        dict(name='AB1_river', order=['ab3_unhide_ab1_trigger', 'ab3_hide_ab1_trigger'], nil_zone='ab3_unhide_ab1_trigger'),
        # CY09: two paths (right/left) with a hide and an unhide strip each
        dict(name='AB2_transition', order=[['unhide_ab2_trans_trigger_right', 'unhide_ab2_trans_trigger_left'],
                                           ['hide_ab2_trans_trigger_right', 'hide_ab2_trans_trigger_left']],
             nil_zone='unhide_ab2_trans_trigger_right'),
    ],
    'swamp': [
        # SW02: unhide strips rank first, so a layer stays visible while anybody last crossed its unhide strip
        dict(name='AB1_Geo_start', order=['AB1_Geo_start_UnHide_aTrigger', 'AB1_Geo_start_Hide_aTrigger']),
        dict(name='AB1_Geo_end', order=['AB1_Geo_end_UnHide_aTrigger', 'AB1_Geo_end_Hide_aTrigger']),
    ],
    'islands': [
        # Hub: AB1 -> flatlands -> AB2 left / right (any order) -> VTOL (Cine13) -> AB3 island (Cine15) -> Cine20.
        # Strict order (left before right): players split between left and right get the left values (deterministic,
        # no flip-flopping); see the report.
        dict(name='Player_at', token='Level.Mission.Player_at',
             order=['cine10', 'Intro_crashsite', 'Intro_roof', 'Intro_bridge', 'Intro_VVV',
                    'AB1_distant', 'AB1_close_building_right', 'AB1_close_building_left', 'AB1_close_bottom',
                    'AB1_close_center', 'AB1_close_AAA_top',
                    'FlatlandTransitionLeft', 'FlatlandTransitionRight', 'FlatlandMiddle', 'FlatlandLeft', 'FlatlandRight',
                    'AB2_Left_distant', 'AB2_Left_SecObjective', 'AB2_Left_close_bottom', 'AB2_Left_close_top_right',
                    'AB2_Left_close_top_left', 'AB2_Left_close_top_entrance', 'AB2_Left_close_top_battery',
                    'AB2_Right_distant', 'AB2_Right_closing_in', 'AB2_Right_close_bottom', 'AB2_Right_close_top_right',
                    'AB2_Right_close_top_center', 'AB2_Right_close_top_bridge', 'AB2_Right_close_top_AAA',
                    'Cine13', 'Cine15', 'AB3_close_left', 'AB3_close_right', 'AB3_close_back', 'Cine20'],
             commits=['Cine13', 'Cine15', 'Cine20']),
    ],
    'cave': [
        # AB1 is linear; AB2 (pylons B and C in any order, bridge parts) in path order; the AB3 boss arena slices are
        # one rank: there the last slice any player entered counts (boss start needs A2/C2 like in single player).
        dict(name='PlayerAt', token='Level.Global.PlayerAt',
             order=['AB1_StartArea', 'AB1_WaterworldDrop', 'AB1_Waterworld', 'AB1_TransTo_B', 'AB1_RiftVista',
                    'AB1_ToPylonA_tunnel1', 'AB1_ToPylonA_tunnel2', 'AB1_Pylon_CaveSide1', 'AB1_Pylon_Structure',
                    'AB1_TransistionToAB2', 'AB2_EnterVista', 'AB1_AlternativeToAB2', 'AB2_Waterfall_high',
                    'AB2_NaturalArea', 'AB2_Natural_Vista', 'AB2_CenterVista', 'AB2_Waterfall_low', 'AB2_Center',
                    'AB2_CenterCrack', 'AB2_TurretA_area', 'AB2_PylonB_area', 'AB2_Center_tunnel', 'AB2_CenterBridgeside',
                    'AB2_CenterBridge', 'AB2_PylonC_area', 'AB2_BridgePartA', 'AB2_BridgePartB', 'AB2_BridgePartC',
                    'AB2_BridgePartD', 'AB2_RiftSide_area', 'AB2_TransistionToAB3_B', 'AB2_TransistionToAB3_A',
                    ['AB3_Slice_A1', 'AB3_Slice_A2', 'AB3_Slice_B1A', 'AB3_Slice_B1B', 'AB3_Slice_B2', 'AB3_Slice_C1',
                     'AB3_Slice_C2', 'AB3_Slice_D1A', 'AB3_Slice_D1B', 'AB3_Slice_D2', 'AB3_Slice_E1', 'AB3_Slice_E2',
                     'AB3_Slice_F1A', 'AB3_Slice_F1B', 'AB3_Slice_F2']]),
    ],
}

# Kill volumes that spare players standing in these zones (Lockstep.lua C3CoopKillVolume)
KILL_SAFE = {
    # CYK1 covers the canyon floor in front of the dam (z 55..100) 10 s after somebody reached the dam top; a teammate
    # still in the other tower's cabin (or riding it up) stands inside the polygon
    'canyon': {'Dam_Top_Kill_Trigger': ('any', ('box', (760.5, 686.8, 70), (765.9, 692.2, 135)),
                                        ('box', (938.9, 678.2, 70), (944.3, 683.6, 135)))},
}

# Level specials (Lockstep.lua C3CoopSpecialTick):
#   restart   when a machine's own token reaches value (its player lost) and nobody has won, the host restarts the map
#             for everybody after delay s (EndGame: the lost hack kills the player, single player reloads the level)
#   won       token/value of a machine's own win (a later loss then only affects that machine)
LEVEL_SPECIAL = {
    'endgame': dict(restart=dict(token='Level.Satellite.PlayerLost', value='true', delay=12, map='EndGame'),
                    won=dict(token='Level.Satellite.isChargingDone', value='true')),
}

# Rooms whose AI waves must stay enabled while a player is inside (Lockstep.lua C3CoopRoomDefer): the level disables
# a room's waves when "the" player crosses a transition trigger, which in co-op can be anybody.
ROOMS = {
    'jailbreak': [
        dict(name='AB3 room 1 (reactor, Base A)', token='Level.AB3.AI_Room_1_Enabled',
             waves=['AB3_Base_A', 'AB3_Base_A_Reinforcements'],
             zone=('box', (180.5, 468, 108), (241, 563, 142))),
        dict(name='AB3 room 2 (bassin, Base B)', token='Level.AB3.AI_Room_2_Enabled',
             waves=['AB3_Base_B', 'AB3_Base_B_Reinforcements', 'AB3_Base_B_Guards'],
             zone=('box', (75, 468, 108), (155, 563, 142))),
    ],
}


def _zone(model, spec, default=None):
    if spec is None:
        return default
    if spec[0] == 'trigger':
        t = model.entity(spec[1])
        if t.cls not in TRIGGER_CLASSES:
            raise SystemExit(f'gate zone: {spec[1]} is a {t.cls}, not a trigger')
        zone = model.volume(t)
        if zone is None:
            raise SystemExit(f'gate zone: trigger {spec[1]} has no volume')
        return zone
    if spec[0] == 'box':
        (x1, y1, z1), (x2, y2, z2) = spec[1], spec[2]
        if not (x1 < x2 and y1 < y2 and z1 < z2):
            raise SystemExit(f'gate zone: box {spec!r} is not min, max')
        return {'box': [x1, y1, x2, y2], 'z1': z1, 'z2': z2}
    if spec[0] == 'near':
        model.entity(spec[1])
        return {'near': spec[1], 'r': float(spec[2])}
    if spec[0] == 'any':
        return {'any': [_zone(model, s) for s in spec[1:]]}
    raise SystemExit(f'gate zone: bad spec {spec!r}')


ENTITY_GATE_CLASSES = {'use': ('InteractiveEntity',), 'switch': ('Switch',), 'trigger': TRIGGER_CLASSES}


def _target(model, target, yaw_deg):
    """Lua fields of a teleport target: pos + yaw (entity at build time / coordinates) or posEntity (run time)."""
    if isinstance(target, str):
        t = model.entity(target)
        return {'pos': {'x': t.pos[0], 'y': t.pos[1], 'z': t.pos[2]}, 'yaw': _yaw(t.rot)}, 'entity ' + target
    if isinstance(target, tuple) and len(target) == 2 and target[0] == 'dynamic':
        model.entity(target[1])
        return {'posEntity': target[1]}, 'entity %s at run time' % target[1]
    if isinstance(target, (tuple, list)) and len(target) == 3:
        return {'pos': {'x': target[0], 'y': target[1], 'z': target[2]}, 'yaw': math.radians(yaw_deg or 0)}, 'coordinates'
    raise SystemExit(f'gate target: bad spec {target!r}')


def _check_token(level, tokens, name):
    if name not in tokens:
        raise SystemExit(f'{level}: game token {name!r} does not exist in the level')
    return name


def zone_writers(model, token):
    """{trigger name: value} of the triggers whose Enter writes the token (Mission:GameTokenSet within 8 hops, edges
    only). A trigger writing several different values fails the build."""
    setters = set(model.token_setters.get(token, []))
    out = {}
    for o in model.by_id.values():
        if o.cls not in TRIGGER_CLASSES:
            continue
        frontier = collections.deque()
        for key in model.entity_nodes.get(o.id, []):
            for port, nxt, _ in model.out.get(key, []):
                if port == 'Enter':
                    frontier.append((nxt, 1))
        seen, values = set(), set()
        while frontier:
            key, depth = frontier.popleft()
            if key in seen or key not in model.nodes or depth > 8:
                continue
            seen.add(key)
            if key in setters:
                values.add(model.nodes[key][2].get('token_Value', model.nodes[key][2].get('Value', '')))
                continue
            frontier.extend((nxt, depth + 1) for _, nxt, _ in model.out.get(key, []))
        if len(values) > 1:
            raise SystemExit(f'zone token {token}: trigger {o.name} writes several values {sorted(values)}')
        if values:
            if o.name in out:
                raise SystemExit(f'zone token {token}: trigger name {o.name} is not unique')
            out[o.name] = values.pop()
    return out


def zones_lua(level, model, tokens):
    """C3COOP_ZONES.<level> (see ZONES)."""
    lines = ['C3COOP_ZONES.%s = {' % level]
    for g in ZONES.get(level, []):
        ranks = []  # (rank, key)
        for i, item in enumerate(g['order']):
            for key in (item if isinstance(item, list) else [item]):
                ranks.append((i + 1, key))
        keys = [k for _, k in ranks]
        if len(set(keys)) != len(keys):
            raise SystemExit(f'ZONES[{level}] {g["name"]}: duplicate entries')
        rank_of = dict((k, r) for r, k in ranks)
        zones, commits = {}, {}
        if g.get('token'):
            _check_token(level, tokens, g['token'])
            writers = zone_writers(model, g['token'])
            values = set(writers.values())
            for trig, value in sorted(writers.items()):
                if value not in rank_of:
                    raise SystemExit(f'ZONES[{level}] {g["name"]}: {trig} writes {value!r}, which is not in order')
                if model.entity(trig).props.get('bTriggerOnce', '0') != '0':
                    raise SystemExit(f'ZONES[{level}] {g["name"]}: {trig} is a once trigger')
                zones[trig] = {'rank': rank_of[value], 'value': value}
            for c in g.get('commits', []):
                if c not in rank_of:
                    raise SystemExit(f'ZONES[{level}] {g["name"]}: commit {c!r} not in order')
                commits[c] = rank_of[c]
            for k in keys:
                if k not in values and k not in commits:
                    raise SystemExit(f'ZONES[{level}] {g["name"]}: no trigger writes {k!r} (typo?)')
            by_value = collections.defaultdict(list)
            for trig, value in writers.items():
                by_value[value].append(trig)
        else:
            for trig in keys:
                o = model.entity(trig)
                if o.cls not in TRIGGER_CLASSES or o.props.get('bTriggerOnce', '0') != '0':
                    raise SystemExit(f'ZONES[{level}] {g["name"]}: {trig} is not a non-once trigger')
                zones[trig] = {'rank': rank_of[trig], 'value': trig}
            by_value = {t: [t] for t in keys}
        out = {'name': g['name']}
        if g.get('token'):
            out['token'] = g['token']
        if g.get('nil_zone'):
            nz = by_value.get(g['nil_zone'])
            if not nz:
                raise SystemExit(f'ZONES[{level}] {g["name"]}: nil_zone {g["nil_zone"]!r} unknown')
            out['nilZone'] = sorted(nz)[0]
        gates = {}
        for value, spec in g.get('gates', {}).items():
            if value not in by_value:
                raise SystemExit(f'ZONES[{level}] {g["name"]}: gate value {value!r} has no trigger')
            t, _ = _target(model, spec['target'], spec.get('yaw'))
            t.update({'rank': rank_of[value], 'trig': sorted(by_value[value])[0], 'radius': spec.get('radius', 4),
                      'value': value})
            gates[value] = t
        lines.append('\t-- %s: %d triggers%s' % (g['name'], len(zones), ' writing ' + g['token'] if g.get('token') else ''))
        body = _lua_value(out)[:-1]
        body += ', zones = {%s}' % ', '.join('[%s] = %s' % (_lua_value(t), _lua_value(z)) for t, z in sorted(zones.items()))
        body += ', commits = {%s}' % ', '.join('[%s] = %d' % (_lua_value(c), r) for c, r in sorted(commits.items()))
        body += ', gates = {%s}' % ', '.join('[%s] = %s' % (_lua_value(v), _lua_value(t)) for v, t in sorted(gates.items()))
        lines.append('\t' + body + '},')
    lines.append('};')
    return lines


def gates_lua(level, model, tokens):
    lines = ['-- generated by scripts/build_pak.py (GATES / ROOMS / ZONES / KILL_SAFE / LEVEL_SPECIAL): co-op data of %s,'
             % level, '-- see Scripts/c3coop/Lockstep.lua',
             'C3COOP_GATES = C3COOP_GATES or {};', 'C3COOP_ROOMS = C3COOP_ROOMS or {};', 'C3COOP_ZONES = C3COOP_ZONES or {};',
             'C3COOP_KILL_SAFE = C3COOP_KILL_SAFE or {};', 'C3COOP_SPECIAL = C3COOP_SPECIAL or {};',
             'C3COOP_GATES.%s = {' % level]
    ids = set()
    for g in GATES.get(level, []):
        if g['id'] in ids:
            raise SystemExit(f'gate {g["id"]}: duplicate id')
        ids.add(g['id'])
        out = {'id': g['id']}
        keys = [k for k in ('trigger', 'use', 'switch') if k in g]
        if len(keys) > 1:
            raise SystemExit(f'gate {g["id"]}: more than one of trigger/use/switch')
        default_zone = None
        if keys:
            key = keys[0]
            ent = model.entity(g[key])
            if ent.cls not in ENTITY_GATE_CLASSES[key]:
                raise SystemExit(f'gate {g["id"]}: {g[key]} is a {ent.cls}, expected {ENTITY_GATE_CLASSES[key]}')
            out[key] = g[key]
            if key == 'trigger':
                default_zone = model.volume(ent)
        elif not g.get('commit'):
            raise SystemExit(f'gate {g["id"]}: needs a trigger, use, switch or commit')
        if g.get('start'):
            if g.get('zone') or g.get('commit') or 'trigger' not in g:
                raise SystemExit(f'gate {g["id"]}: start gates take a trigger, no zone/commit')
            out['start'] = True
            if g.get('replay'):
                out['replay'] = True
            lines.append('\t-- %s: %s; START gate (waits for the players of the previous level)' % (g['id'], g['what']))
            lines.append('\t' + _lua_value(out) + ',')
            continue
        out['zone'] = _zone(model, g.get('zone'), default_zone)
        if out['zone'] is None and keys:
            raise SystemExit(f'gate {g["id"]}: no zone')
        if out['zone'] is None:
            del out['zone']
        if g.get('keep'):
            out['keep'] = _zone(model, g['keep'])
        t, source = _target(model, g['target'], g.get('yaw'))
        out.update(t)
        out['radius'] = g.get('radius', 4)
        if g.get('ring'):
            out['ring'] = g['ring']
        if g.get('quorum'):
            if g['quorum'] != 'all':
                raise SystemExit(f'gate {g["id"]}: quorum must be "all"')
            out['quorumAll'] = True
        if g.get('group'):
            out['group'] = g['group']
        if g.get('commit'):
            out['commit'], out['commitValue'] = g['commit']
            _check_token(level, tokens, g['commit'][0])
            if g.get('commit_target'):
                ct, _ = _target(model, g['commit_target'], g.get('commit_yaw'))
                if 'posEntity' in ct:
                    raise SystemExit(f'gate {g["id"]}: commit_target must be static')
                out['commitPos'], out['commitYaw'] = ct['pos'], ct['yaw']
        lines.append('\t-- %s: %s; target from %s%s' % (g['id'], g['what'], source,
                                                        '; ESTIMATED: ' + g['estimate'] if g.get('estimate') else ''))
        lines.append('\t' + _lua_value(out) + ',')
    lines.append('};')
    lines.append('C3COOP_ROOMS.%s = {' % level)
    for r in ROOMS.get(level, []):
        out = {'name': r['name'], 'token': r['token'], 'zone': _zone(model, r['zone'])}
        _check_token(level, tokens, r['token'])
        for w in r['waves']:
            model.entity(w)
        lines.append('\t' + _lua_value(out)[:-1] + ', waves = {%s}},' % ', '.join('[%s] = 1' % _lua_value(w) for w in r['waves']))
    lines.append('};')
    lines += zones_lua(level, model, tokens)
    lines.append('C3COOP_KILL_SAFE.%s = {' % level)
    for trig, spec in KILL_SAFE.get(level, {}).items():
        if model.entity(trig).cls not in TRIGGER_CLASSES:
            raise SystemExit(f'KILL_SAFE[{level}]: {trig} is not a trigger')
        lines.append('\t[%s] = %s,' % (_lua_value(trig), _lua_value(_zone(model, spec))))
    lines.append('};')
    special = LEVEL_SPECIAL.get(level)
    if special:
        for part in special.values():
            _check_token(level, tokens, part['token'])
        lines.append('C3COOP_SPECIAL.%s = %s;' % (level, _lua_value(special)))
    return ('\n'.join(lines) + '\n').encode('utf-8')


def apply_trigger_overrides(level, model, result):
    # a name may stand for several triggers (copies of one prefab): the mode then applies to every copy, so it is only
    # allowed when all of them are triggers (the mode is looked up by name at runtime)
    for mode, names in TRIGGER_OVERRIDES.get(level, {}).items():
        for name in names:
            found = model.by_name.get(name) or []
            if not found:
                raise SystemExit(f'TRIGGER_OVERRIDES[{level}]: no entity named {name}')
            if any(o.cls not in TRIGGER_CLASSES for o in found):
                raise SystemExit(f'TRIGGER_OVERRIDES[{level}]: {name} is not a trigger (every entity of that name must be)')
            for other in ('cosmetic', 'kill', 'original'):
                result[other].discard(name)
            if mode != 'gameplay':
                result[mode].add(name)


def lua_utf8(text):
    """Lua string literal of UTF-8 text with every non-ASCII byte escaped (the patched scripts are latin-1 files)."""
    return '"' + ''.join(chr(b) if 32 <= b < 127 and chr(b) not in '"\\' else '\\%03d' % b
                         for b in text.encode('utf-8')) + '"'


# Scripts/c3coop/Lockstep.lua: shared by the patched trigger scripts (ProximityTrigger, AreaTrigger, CinematicTrigger),
# InteractiveEntity, AIWave and SinglePlayer.lua. @@...@@ placeholders are filled in by lockstep_lua().
LOCKSTEP_LUA = r'''----------------------------------------------------------------------------------------------------
-- c3coop lockstep (generated by scripts/build_pak.py)
--
-- In a network game every machine runs the whole level flowgraph, but the level's triggers are local entities that
-- fire on each machine on their own. Here the server decides:
--   gameplay triggers   only the server evaluates them (for every player). Each Enter/Leave/... it fires goes to the
--                       clients as an event keyed by the trigger name (host player RMI C3Trig) and drives the same
--                       outputs there; what the clients detect locally is ignored.
--   cosmetic triggers   (lighting, sound, ambience; Scripts/c3coop/LevelTriggers.lua) stay local on every machine and
--                       react only to that machine's own player.
--   kill volumes        the server kills the player who entered (Actor:KillPlayer kills each machine's own player).
--   InteractiveEntity   a client's use goes to the server; the server's Use is replayed on every client.
--   co-op gates         (Scripts/c3coop/Gates_<level>.lua) one-way moments wait for a quorum of players inside the
--                       gate zone, then everybody else is brought to the gate's target and the activation fires.
--   AB3-like rooms      an AI wave of a room stays enabled while a player is in that room.
--   zone tokens         ("last writer wins" streaming triggers, Gates_<level>.lua C3COOP_ZONES) are ignored on every
--                       machine; the server fires the trigger of the rearmost player's zone for everybody.
--   Switch              (Net.Exposed) a gated switch's SvRequestUse is held on the server like an InteractiveEntity use.
--   InteractiveObjectEx (kick doors) the user's machine plays the interaction; its Started/Finished/Aborted outputs are
--                       mirrored on every other machine.
-- Outside network games every patched function calls the original one.
----------------------------------------------------------------------------------------------------
C3COOP_LOCKSTEP = 1;
if (C3COOP_GATE_QUORUM == nil) then C3COOP_GATE_QUORUM = -1; end -- -1: alive players - 1, 0: everybody, k: k players
if (C3COOP_START_TIMEOUT == nil) then C3COOP_START_TIMEOUT = 45; end -- level start gates: max seconds to wait for rejoins
if (C3COOP_TP_COOLDOWN == nil) then C3COOP_TP_COOLDOWN = 15; end -- seconds between two `c3coop tp` of a player
if (C3COOP_TRIGGER_LOG == nil) then C3COOP_TRIGGER_LOG = 1; end -- log every trigger event sent / applied
C3COOP_TRIG_QUEUE = C3COOP_TRIG_QUEUE or {};
C3COOP_TEXT_WAIT = @@TEXT_WAIT@@;
C3COOP_TEXT_WAITED = @@TEXT_WAITED@@;

Script.ReloadScript("Scripts/c3coop/LevelTokens.lua");
Script.ReloadScript("Scripts/c3coop/LevelTriggers.lua");
@@GATE_FILES@@

function C3CoopNet()
	local mp = System.IsMultiplayer();
	return (mp ~= nil and mp ~= false and mp ~= 0);
end

-- Lua errors are not logged by this build: protected calls log them here
function C3CoopErr(where, err)
	local text = string.gsub(tostring(err), "%%", "pct");
	Log("[c3coop] Lua error in %s: %s", tostring(where), text);
end

function C3CoopCall(where, f, ...)
	local ok, err = pcall(f, ...);
	if (not ok) then
		C3CoopErr(where, err);
	end
	return ok;
end

-- level loaded right now (a token only this level has, see LevelTokens.lua); cached on the level's gamerules table
function C3CoopLevel()
	local rules = g_gameRules;
	if (rules and rules.c3coopLevel) then
		return rules.c3coopLevel;
	end
	local signatures = C3COOP_LEVEL_SIGNATURE or {};
	local level = C3COOP_LEVEL_CACHE;
	if (not (level and signatures[level] and GameToken.GetToken(signatures[level]) ~= nil)) then
		level = nil;
		for name, token in pairs(signatures) do
			if (GameToken.GetToken(token) ~= nil) then
				level = name;
				Log("[c3coop] level %s", level);
				break;
			end
		end
		C3COOP_LEVEL_CACHE = level;
	end
	if (rules and level) then
		rules.c3coopLevel = level;
	end
	return level;
end

-- entity of an id passed around by triggers (flowgraph inputs may pass anything)
function C3CoopEntity(id)
	local t = type(id);
	if (t ~= "userdata" and t ~= "number") then
		return nil;
	end
	return System.GetEntity(id);
end

function C3CoopEntName(id)
	local e = C3CoopEntity(id);
	return e and e:GetName() or "-";
end

-- network key of an entity: its name, or "name@x,y,z" when other entities of its class share the name (prefab copies:
-- Jailbreak has 7 air vents named air_vent_grate_1/2, Cave 138 Ledge11); C3CoopEntByKey takes the nearest copy
function C3CoopEntKey(ent)
	if (ent.c3coopKey) then
		return ent.c3coopKey;
	end
	local name, n = ent:GetName(), 0;
	for _, e in pairs(System.GetEntitiesByClass(ent.class) or {}) do
		if (e:GetName() == name) then
			n = n + 1;
			if (n > 1) then
				break;
			end
		end
	end
	local key = name;
	if (n > 1) then
		local p = ent:GetWorldPos();
		key = string.format("%s@%.2f,%.2f,%.2f", name, p.x, p.y, p.z);
	end
	ent.c3coopKey = key;
	return key;
end

function C3CoopEntByKey(key)
	local name, x, y, z = string.match(key or "", "^(.*)@(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+)$");
	if (not name) then
		return System.GetEntityByName(key or "");
	end
	local any = System.GetEntityByName(name);
	if (not any) then
		return nil;
	end
	x, y, z = tonumber(x), tonumber(y), tonumber(z);
	local best, bestD;
	for _, e in pairs(System.GetEntitiesByClass(any.class) or {}) do
		if (e:GetName() == name) then
			local p = e:GetWorldPos();
			local d = (p.x - x) ^ 2 + (p.y - y) ^ 2 + (p.z - z) ^ 2;
			if (not bestD or d < bestD) then
				best, bestD = e, d;
			end
		end
	end
	return best or any;
end

function C3CoopIsPlayerId(id)
	local e = C3CoopEntity(id);
	return (e ~= nil and e.actor ~= nil and e.actor:IsPlayer()) and true or false;
end

-- one player in the game (single player session hosted without clients): the level runs as in the original
function C3CoopSolo()
	return #(System.GetEntitiesByClass("Player") or {}) <= 1;
end

function C3CoopAlivePlayers()
	local list = {};
	for _, p in pairs(System.GetEntitiesByClass("Player") or {}) do
		if (p.actor and p.actor:GetHealth() > 0 and p.actor:GetSpectatorMode() == 0) then
			list[#list + 1] = p;
		end
	end
	return list;
end

-- zones: {box = {x1, y1, x2, y2}, z1 =, z2 =}, {poly = {x1, y1, x2, y2, ...}, z1 =, z2 =}, {any = {zone, ...}} or
-- {near = <entity name>, r = <m>} (around that entity where it is now)
function C3CoopInZone(zone, pos)
	if (not zone or not pos) then
		return false;
	end
	if (zone.any) then
		for _, z in ipairs(zone.any) do
			if (C3CoopInZone(z, pos)) then
				return true;
			end
		end
		return false;
	end
	if (zone.near) then
		local e = System.GetEntityByName(zone.near);
		if (not e) then
			return false;
		end
		return DistanceSqVectors(pos, e:GetWorldPos()) <= (zone.r or 5) * (zone.r or 5);
	end
	-- feet position: a little tolerance below the volume
	if (zone.z1 and (pos.z < zone.z1 - 0.5 or pos.z > zone.z2)) then
		return false;
	end
	local b = zone.box;
	if (b) then
		return pos.x >= b[1] and pos.x <= b[3] and pos.y >= b[2] and pos.y <= b[4];
	end
	local p = zone.poly;
	if (not p) then
		return false;
	end
	local n = math.floor(#p / 2);
	local inside = false;
	local j = n;
	for i = 1, n do
		local xi, yi, xj, yj = p[2 * i - 1], p[2 * i], p[2 * j - 1], p[2 * j];
		if (((yi > pos.y) ~= (yj > pos.y)) and (pos.x < (xj - xi) * (pos.y - yi) / (yj - yi) + xi)) then
			inside = not inside;
		end
		j = i;
	end
	return inside;
end

----------------------------------------------------------------------------------------------------
-- teleports (server). A client's position is its own: C3Teleport makes the owning client move itself.
function C3CoopTeleport(p, pos, yaw, why)
	yaw = yaw or 0;
	Log("[c3coop] teleport %s to %.2f %.2f %.2f (%s)", p:GetName(), pos.x, pos.y, pos.z, tostring(why));
	p:SetWorldPos(pos);
	if (p.id == g_localActorId) then
		p.actor:PlayerSetViewAngles({x = 0, y = 0, z = yaw});
	end
	if (p.allClients) then
		p.allClients:C3Teleport(pos, yaw);
	end
end

-- i-th of n points in a small ring (radius r, default 0.9 m) around the target, a bit above the floor
function C3CoopRing(center, i, n, r)
	local pos = {x = center.x, y = center.y, z = center.z + 0.3};
	if (n > 1 or (r and r > 1.5)) then
		local a = (i - 1) * 2 * math.pi / math.max(n, 1);
		pos.x = pos.x + (r or 0.9) * math.cos(a);
		pos.y = pos.y + (r or 0.9) * math.sin(a);
	end
	return pos;
end

-- `c3coop tp [host|<player name>]`: next to the nearest living teammate (or the one named)
function C3CoopCatchUp(p, who)
	if (not p.actor or p.actor:GetHealth() <= 0) then
		return;
	end
	local now = System.GetCurrTime();
	if (p.c3coopTpAt and now - p.c3coopTpAt < C3COOP_TP_COOLDOWN) then
		Log("[c3coop] tp for %s refused: wait %.0f s", p:GetName(), C3COOP_TP_COOLDOWN - (now - p.c3coopTpAt));
		return;
	end
	local pos = p:GetWorldPos();
	local best, bestD;
	for _, m in ipairs(C3CoopAlivePlayers()) do
		if (m.id ~= p.id) then
			local d = DistanceSqVectors(pos, m:GetWorldPos());
			if (who == "host" and m.id == g_localActorId) then
				d = -1;
			elseif (who and who ~= "" and who ~= "host" and m:GetName() == who) then
				d = -1;
			end
			if (not best or d < bestD) then
				best, bestD = m, d;
			end
		end
	end
	if (not best) then
		Log("[c3coop] tp for %s: no other living player", p:GetName());
		return;
	end
	p.c3coopTpAt = now;
	local dir = best:GetDirectionVector(1);
	local target = best:GetWorldPos();
	target = {x = target.x - dir.x * 1.5, y = target.y - dir.y * 1.5, z = target.z + 0.3};
	C3CoopTeleport(p, target, best:GetWorldAngles().z, "tp to " .. best:GetName());
end

----------------------------------------------------------------------------------------------------
-- Held objects (pick and throw). A held object hangs on the player's hand with its physics asleep, so the network does
-- not move it on the other machines: the server streams its position ("H" trigger events, 20 per second). A client's
-- use key on a grabbable object asks the host to carry it ("pick", C3CoopClientPick from c3coop.dll): the host keeps
-- it in front of that player and throws it at the next press.
C3COOP_CARRY = C3COOP_CARRY or {}; -- server: player id -> carried entity id (players on clients)

function C3CoopPlayerByRawId(raw)
	for _, p in pairs(System.GetEntitiesByClass("Player") or {}) do
		if (p:GetRawId() == raw) then
			return p;
		end
	end
end

-- prefab copies share names: the copy of that name nearest pos
function C3CoopNearestByName(name, pos)
	return C3CoopEntByKey(string.format("%s@%.2f,%.2f,%.2f", name, pos.x, pos.y, pos.z));
end

function C3CoopHeldSend(carrier, ent)
	local pos, ang = ent:GetWorldPos(), ent:GetWorldAngles();
	carrier.allClients:C3Trig(string.format("H~%s~%.2f,%.2f,%.2f,%.3f,%.3f,%.3f~0", C3CoopEntKey(ent), pos.x, pos.y, pos.z,
		ang.x, ang.y, ang.z));
end

-- server, every 50 ms (SinglePlayer:C3CoopHoldTick)
function C3CoopHeldTick()
	if (not C3CoopNet() or not CryAction.IsServer()) then
		return;
	end
	local ids = {};
	for _, p in pairs(System.GetEntitiesByClass("Player") or {}) do
		ids[#ids + 1] = tostring(p:GetRawId());
	end
	if (#ids > 0) then
		System.ExecuteCommand("c3coop held " .. table.concat(ids, " ")); -- answers with C3CoopHeldUpdate
	end
	C3CoopCarryUpdate();
end

-- server: {{player raw id, held entity name}, ...} of the players holding something through pick and throw
function C3CoopHeldUpdate(list)
	local carrier = g_localActor;
	if (not carrier or not carrier.allClients) then
		return;
	end
	for _, h in ipairs(list or {}) do
		local p = C3CoopPlayerByRawId(h[1]);
		local ent = p and C3CoopNearestByName(h[2], p:GetWorldPos());
		if (ent and not ent.actor) then
			C3CoopHeldSend(carrier, ent);
		end
	end
end

-- client: a held object's position from the server
function C3CoopHeldApply(ent, s)
	local x, y, z, ax, ay, az = string.match(s or "",
		"^(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+)$");
	if (x) then
		-- the object in this player's own hands: its hand holds it here (the host's position would pull it away)
		local me = g_localActor;
		if (C3COOP_MY_HELD == ent:GetName() and me and DistanceSqVectors(me:GetWorldPos(), ent:GetWorldPos()) < 16) then
			return;
		end
		ent:SetWorldPos({x = tonumber(x), y = tonumber(y), z = tonumber(z)});
		ent:SetWorldAngles({x = tonumber(ax), y = tonumber(ay), z = tonumber(az)});
	end
end

-- client (c3coop.dll): the use key on a grabbable object that only the host can pick up. native: this machine's
-- player already holds it (EnterPickAndThrow), the host holds it for that player too ("pickn"); otherwise the host
-- carries it in front of the player ("pick")
function C3CoopClientPick(name, native)
	local me = g_localActor;
	local ent = me and C3CoopNearestByName(name, me:GetWorldPos());
	if (ent) then
		if (native) then
			C3COOP_MY_HELD = name;
		end
		C3CoopToServer((native and "pickn " or "pick ") .. C3CoopEntKey(ent));
	end
end

-- client (c3coop.dll): its player dropped or threw the object it held
function C3CoopClientDrop(name)
	local me = g_localActor;
	C3COOP_MY_HELD = nil;
	local ent = me and C3CoopNearestByName(name, me:GetWorldPos());
	if (ent) then
		-- where it left the hand and how it flies here: the host throws it from there (it carried it 1.3 m ahead)
		local pos, v = ent:GetWorldPos(), ent:GetVelocity() or {x = 0, y = 0, z = 0};
		if (v.x * v.x + v.y * v.y + v.z * v.z < 4) then
			local dir = me.actor:GetHeadDir() or me:GetDirectionVector(1);
			v = {x = dir.x * 12, y = dir.y * 12, z = dir.z * 12 + 2};
		end
		C3CoopToServer(string.format("ptdrop %s|%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", C3CoopEntKey(ent), pos.x, pos.y, pos.z,
			v.x, v.y, v.z));
	end
end

-- server: a client's native pickup (in its own hands on its machine). Pick and throw on the host's copy of a remote
-- player does not stick (its current item follows the client: "c3coop enterpt" left it on its weapon), so the host
-- carries the object in front of that player for everybody else ("H" events; the holder ignores them)
function C3CoopNativePick(p, key)
	local ent = C3CoopEntByKey(key);
	if (not ent or ent.actor or DistanceSqVectors(ent:GetWorldPos(), p:GetWorldPos()) > 36) then
		return;
	end
	for _, eid in pairs(C3COOP_CARRY) do
		if (eid == ent.id) then
			return;
		end
	end
	Log("[c3coop] %s holds %s (host carries it for the others)", p:GetName(), ent:GetName());
	C3COOP_CARRY[p.id] = ent.id;
	ent:EnablePhysics(false);
end

-- server: the client threw or dropped it ("<key>|x,y,z,vx,vy,vz": its position and velocity on the client's machine)
function C3CoopNativeDrop(p, rest)
	local cur = C3COOP_CARRY[p.id] and System.GetEntity(C3COOP_CARRY[p.id]);
	if (not cur) then
		return;
	end
	local x, y, z, vx, vy, vz = string.match(rest or "", "|(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+)$");
	if (not x) then
		C3CoopCarryThrow(p, cur);
		return;
	end
	C3COOP_CARRY[p.id] = nil;
	cur:SetWorldPos({x = tonumber(x), y = tonumber(y), z = tonumber(z)});
	cur:EnablePhysics(true);
	cur:AwakePhysics(1);
	cur:SetVelocity({x = tonumber(vx), y = tonumber(vy), z = tonumber(vz)});
	Log("[c3coop] %s throws %s (client velocity %s,%s,%s)", p:GetName(), cur:GetName(), vx, vy, vz);
end

function C3CoopPtThrow(playerRawId, name)
	local p = C3CoopPlayerByRawId(playerRawId);
	local ent = p and C3CoopNearestByName(name, p:GetWorldPos());
	if (ent) then
		C3CoopCarryThrow(p, ent);
	end
end

-- server: a player's "pick": carry it, or throw what they carry
function C3CoopCarryRequest(p, key)
	local cur = C3COOP_CARRY[p.id] and System.GetEntity(C3COOP_CARRY[p.id]);
	if (cur) then
		C3CoopCarryThrow(p, cur);
		return;
	end
	local ent = C3CoopEntByKey(key);
	if (not ent or ent.actor or DistanceSqVectors(ent:GetWorldPos(), p:GetWorldPos()) > 25) then
		return;
	end
	for _, eid in pairs(C3COOP_CARRY) do
		if (eid == ent.id) then
			return;
		end
	end
	Log("[c3coop] %s carries %s", p:GetName(), ent:GetName());
	C3COOP_CARRY[p.id] = ent.id;
	ent:EnablePhysics(false); -- moved by hand; the "H" events move it on the clients
end

function C3CoopCarryThrow(p, ent)
	C3COOP_CARRY[p.id] = nil;
	ent:EnablePhysics(true);
	ent:AwakePhysics(1);
	local dir = (p.actor and p.actor:GetHeadDir()) or p:GetDirectionVector(1);
	ent:SetVelocity({x = dir.x * 12, y = dir.y * 12, z = dir.z * 12 + 2});
	Log("[c3coop] %s throws %s", p:GetName(), ent:GetName());
end

function C3CoopCarryUpdate()
	local carrier = g_localActor;
	for pid, eid in pairs(C3COOP_CARRY) do
		local p, ent = System.GetEntity(pid), System.GetEntity(eid);
		if (not p or not ent or not p.actor or p.actor:GetHealth() <= 0) then
			if (ent) then
				ent:EnablePhysics(true);
			end
			C3COOP_CARRY[pid] = nil;
		else
			local dir = p.actor:GetHeadDir() or p:GetDirectionVector(1);
			local pos = p:GetWorldPos();
			ent:SetWorldPos({x = pos.x + dir.x * 1.3, y = pos.y + dir.y * 1.3, z = pos.z + 1.3 + dir.z * 1.3});
			ent:SetWorldAngles(p:GetWorldAngles());
			if (carrier and carrier.allClients) then
				C3CoopHeldSend(carrier, ent);
			end
		end
	end
end

----------------------------------------------------------------------------------------------------
-- Level weapons for every player: an item lying in the level at its start (no owner) comes back at the same spot after
-- a pickup, until as many players as are in the game took it. Items dropped later (AI, players) stay single.
C3COOP_LOOT = C3COOP_LOOT or nil; -- server: list of {id, class, name, pos, angles, dir, props, taken}
C3COOP_LOOT_SKIP = {PickAndThrowWeapon = true, NoWeapon = true};

function C3CoopCopyTable(t)
	local c = {};
	for k, v in pairs(t) do
		c[k] = type(v) == "table" and C3CoopCopyTable(v) or v;
	end
	return c;
end

function C3CoopLootOwned(e)
	local owner = e.item and e.item:GetOwnerId();
	return owner ~= nil and owner ~= 0 and owner ~= NULL_ENTITY;
end

-- weapons and ammo boxes; not attachments (children of a weapon) or mounted guns (owned while used)
function C3CoopLootKind(e)
	if (not e.item or C3COOP_LOOT_SKIP[e.class] or string.find(e.class, "Turret") or e:GetParent()) then
		return false;
	end
	return e.weapon ~= nil or string.find(e.class, "Ammo$") ~= nil;
end

function C3CoopLootScan()
	C3COOP_LOOT = {};
	for _, e in pairs(System.GetEntities() or {}) do
		if (C3CoopLootKind(e) and not C3CoopLootOwned(e)) then
			C3COOP_LOOT[#C3COOP_LOOT + 1] = {id = e.id, class = e.class, name = e:GetName(), pos = e:GetWorldPos(),
				angles = e:GetWorldAngles(), dir = e:GetDirectionVector(1),
				props = e.Properties and C3CoopCopyTable(e.Properties), taken = 0};
		end
	end
	Log("[c3coop] loot: %d level items, one per player", #C3COOP_LOOT);
end

-- server, every second (SinglePlayer:C3CoopTick)
function C3CoopLootTick()
	if (not C3CoopNet() or not CryAction.IsServer()) then
		return;
	end
	if (not C3COOP_LOOT) then
		C3CoopLootScan();
		return;
	end
	local players = #(System.GetEntitiesByClass("Player") or {});
	for i = #C3COOP_LOOT, 1, -1 do
		local rec = C3COOP_LOOT[i];
		local e = System.GetEntity(rec.id);
		if (not e or C3CoopLootOwned(e)) then
			rec.taken = rec.taken + 1;
			if (rec.taken < players) then
				local copy = System.SpawnEntity({class = rec.class, name = rec.name .. "_c3coop" .. rec.taken,
					position = rec.pos, orientation = rec.dir, properties = rec.props});
				if (copy) then
					copy:SetWorldAngles(rec.angles);
					rec.id = copy.id;
					Log("[c3coop] loot: %s taken %d/%d, copy placed", rec.name, rec.taken, players);
				else
					table.remove(C3COOP_LOOT, i);
				end
			else
				table.remove(C3COOP_LOOT, i);
			end
		end
	end
end

-- StalkerTree.xml (patched): a new Stalker gets "where the player is" as a sound; the nearest living player, not the host
function C3CoopSeedStalker(entity)
	local best, bestD;
	for _, p in ipairs(C3CoopAlivePlayers()) do
		local d = DistanceSqVectors(p:GetWorldPos(), entity:GetWorldPos());
		if (not best or d < bestD) then
			best, bestD = p, d;
		end
	end
	best = best or System.GetEntity(g_localActorId);
	if (best) then
		AI.CreateStimulusEvent(entity.id, best.id, "SoundMovement",
			{threat = AITHREAT_SUSPECT, type = EST_Sound, pos = best:GetWorldPos(), radius = 9999});
	end
end

-- `c3coop acc`: accessory state of every player on this machine
function C3CoopAccessoryStatus()
	for _, p in ipairs(C3CoopAlivePlayers()) do
		local hammerId = p.inventory and p.inventory:GetItemByClass("Hammer");
		local hammer = hammerId and System.GetEntity(hammerId);
		Log("[c3coop] acc %s: inventory SilencerPistol %s, Hammer %s, attached %s", p:GetName(),
			tostring(p.inventory and p.inventory:HasAccessory("SilencerPistol")), hammer and hammer:GetName() or "none",
			tostring(hammer and hammer.item and hammer.item:HasAccessory("SilencerPistol")));
	end
end

-- `c3coop goto <x> <y> <z> [yaw degrees]` or `c3coop goto <entity name>` (debug)
function C3CoopGoto(p, rest)
	local x, y, z, yaw = string.match(rest or "", "^(%-?[%d%.]+)[%s,]+(%-?[%d%.]+)[%s,]+(%-?[%d%.]+)[%s,]*(%-?[%d%.]*)%s*$");
	local pos, a;
	if (x) then
		pos = {x = tonumber(x), y = tonumber(y), z = tonumber(z)};
		a = math.rad(tonumber(yaw) or 0);
	else
		local e = System.GetEntityByName(rest or "");
		if (not e) then
			Log("[c3coop] goto: no entity named %s", tostring(rest));
			return;
		end
		pos = e:GetWorldPos();
		pos = {x = pos.x, y = pos.y, z = pos.z + 0.3};
		a = e:GetWorldAngles().z;
	end
	if (not pos.x or not pos.y or not pos.z) then
		Log("[c3coop] goto: bad position %s", tostring(rest));
		return;
	end
	C3CoopTeleport(p, pos, a, "goto");
end

-- requests from players (server side; clients send them with C3ToServer, see SinglePlayer C3CoopOnClientMessage)
function C3CoopServerRequest(player, line)
	local verb, rest = string.match(line or "", "^(%S+)%s*(.*)$");
	if (verb == "tp") then
		C3CoopCatchUp(player, rest);
	elseif (verb == "goto") then
		C3CoopGoto(player, rest);
	elseif (verb == "use") then
		local ent = C3CoopEntByKey(rest);
		if (ent and ent.OnUsed) then
			Log("[c3coop] %s uses %s", player:GetName(), rest);
			ent:OnUsed(player, 0);
		else
			Log("[c3coop] use request for unknown entity %s", tostring(rest));
		end
	elseif (verb == "cv") then
		local visible, name = string.match(rest, "^(%d) (.+)$");
		local ent = name and C3CoopEntByKey(name);
		if (ent and ent.Visible) then
			if (visible == "1") then
				ent:Visible(player.id);
			else
				ent:Invisible(player.id);
			end
		end
	elseif (verb == "ioex") then
		local code, name = string.match(rest, "^(%d) (.+)$");
		local ent = name and C3CoopEntByKey(name);
		if (ent and ent.c3coopOrig) then
			C3CoopIoExMirror(ent, tonumber(code), player.id);
		end
	elseif (verb == "act") then
		local ent = C3CoopEntByKey(rest);
		if (ent and ent.c3coopActionKey) then
			C3CoopActionMirror(ent, player.id);
		else
			Log("[c3coop] action report for unknown entity %s", tostring(rest));
		end
	elseif (verb == "pick") then
		C3CoopCarryRequest(player, rest);
	elseif (verb == "pickn") then
		C3CoopNativePick(player, rest);
	elseif (verb == "ptdrop") then
		C3CoopNativeDrop(player, rest);
	elseif (verb == "tok") then
		C3CoopUplinkReceive(player, rest);
	elseif (verb == "special") then
		C3CoopSpecialReceive(player, rest);
	else
		return false;
	end
	return true;
end

-- console `c3coop <verb> ...` lines handed to Lua by c3coop.dll (coop.cpp)
function C3CoopCommand(line)
	local verb, rest = string.match(line or "", "^(%S+)%s*(.*)$");
	if (verb == "tp" or verb == "goto") then
		if (CryAction.IsServer()) then
			if (g_localActor) then
				C3CoopServerRequest(g_localActor, line);
			end
		elseif (g_localActor and g_localActor.server) then
			Log("[c3coop] asking the server: %s", line);
			g_localActor.server:C3ToServer(line);
		end
	elseif (verb == "gates") then
		C3CoopGateStatus();
	elseif (verb == "acc") then
		C3CoopAccessoryStatus();
	elseif (verb == "trig") then
		C3CoopTrigStatus(rest);
	elseif (verb == "quorum") then
		C3COOP_GATE_QUORUM = tonumber(rest) or -1;
		Log("[c3coop] gate quorum rule %d (-1: alive players - 1, 0: all, k: k players)", C3COOP_GATE_QUORUM);
	elseif (verb == "pos") then
		local me = g_localActor;
		if (me) then
			local pos = me:GetWorldPos();
			Log("[c3coop] %s at %.2f %.2f %.2f yaw %.0f", me:GetName(), pos.x, pos.y, pos.z, math.deg(me:GetWorldAngles().z));
		end
	elseif (verb == "zones") then
		C3CoopZoneStatus();
	elseif (verb == "start") then
		C3CoopStartNow();
	elseif (verb == "token") then
		-- c3coop token <name> [value]: log a token; with a value (server) set it, typed like the current value, to
		-- unblock a story step by hand
		local name, value = string.match(rest or "", "^(%S+)%s*(.*)$");
		local current = name and GameToken.GetToken(name);
		if (name and value ~= "" and CryAction.IsServer()) then
			local typed = value;
			if (type(current) == "boolean") then
				typed = (value == "true");
			elseif (type(current) == "number") then
				typed = tonumber(value);
			end
			GameToken.SetToken(name, typed);
			Log("[c3coop] token %s: %s -> %s (by command)", name, tostring(current), value);
		else
			Log("[c3coop] token %s = %s", tostring(name), tostring(current));
		end
	else
		Log("[c3coop] unknown Lua command %s", tostring(line));
	end
end

----------------------------------------------------------------------------------------------------
-- trigger modes and events
function C3CoopTriggerMode(trig)
	if (not C3CoopNet()) then
		return nil;
	end
	local mode = trig.c3coopMode;
	if (mode == nil) then
		if (not trig.localOnly) then
			mode = false; -- bNetworked trigger: the game's own replication
		else
			local level = C3CoopLevel();
			local info = level and C3COOP_LEVEL_TRIGGERS and C3COOP_LEVEL_TRIGGERS[level];
			local name = trig:GetName();
			mode = "gameplay";
			if (level and C3CoopZoneOf(name)) then
				info = nil;
				mode = "zone"; -- server-computed "last writer wins" streaming trigger (zones below)
			end
			if (info) then
				if (info.cosmetic[name]) then
					mode = "cosmetic";
				elseif (info.kill[name]) then
					mode = "kill";
				elseif (info.original[name]) then
					mode = false;
				end
				if (mode and info.anyplayer and info.anyplayer[name]) then
					trig.c3coopAnyPlayer = true;
				end
			end
			if (not level) then
				return mode or nil; -- not cached before the level is known
			end
		end
		trig.c3coopMode = mode;
	end
	return mode or nil;
end

function C3CoopPlayerOnly(trig)
	local p = trig.Properties or {};
	return (tonumber(p.bOnlyPlayer or 0) or 0) ~= 0 or (tonumber(p.bOnlyPlayers or 0) or 0) ~= 0;
end

-- server: queue an event for the clients ("kind~trigger~sender~insideCount", sent in small batches by C3CoopTrigFlush)
function C3CoopTrigSend(kind, ent, senderId, extra, count)
	local ev = kind .. "~" .. C3CoopEntKey(ent) .. "~" .. (extra or C3CoopEntName(senderId)) .. "~" .. tostring(count or ent.insideCount or 0);
	local q = C3COOP_TRIG_QUEUE;
	if (#q < 1000) then
		q[#q + 1] = ev;
	end
	if (C3COOP_TRIGGER_LOG ~= 0) then
		Log("[c3coop] trigger event sent: %s", ev);
	end
end

-- server, every 100 ms: a few short messages per tick (one long burst of RMIs once hung the server)
function C3CoopTrigFlush()
	local q = C3COOP_TRIG_QUEUE;
	local carrier = g_localActor;
	if (#q == 0 or not carrier or not carrier.allClients) then
		return;
	end
	C3CoopTokenFlush();
	for i = 1, 4 do
		local msg = table.remove(q, 1);
		if (not msg) then
			break;
		end
		while (q[1] and string.len(msg) + string.len(q[1]) < 190) do
			msg = msg .. "|" .. table.remove(q, 1);
		end
		carrier.allClients:C3Trig(msg);
	end
end

-- server: current state of the gameplay triggers for a (re)joining client
function C3CoopTrigResync()
	local n = 0;
	for _, class in ipairs({"ProximityTrigger", "AreaTrigger", "CinematicTrigger"}) do
		for _, ent in pairs(System.GetEntitiesByClass(class) or {}) do
			if (C3CoopTriggerMode(ent) == "gameplay" and (ent.triggered or ent.triggeredOnce or (ent.insideCount or 0) > 0)) then
				local flags = (ent.triggered and "t" or "") .. (ent.triggeredOnce and "o" or "");
				C3CoopTrigSend("S", ent, nil, flags);
				n = n + 1;
			end
		end
	end
	Log("[c3coop] trigger resync: %d triggers with state", n);
end

-- client: one event from the server
function C3CoopTrigApply(ev)
	local kind, name, senderName, count = string.match(ev, "^(%a)~([^~]*)~([^~]*)~(%-?%d+)$");
	if (not kind) then
		Log("[c3coop] bad trigger event %s", ev);
		return;
	end
	count = tonumber(count) or 0;
	local ent = C3CoopEntByKey(name);
	if (not ent) then
		Log("[c3coop] trigger event for unknown entity %s", name);
		return;
	end
	local senderId = NULL_ENTITY;
	local sender = (senderName ~= "" and senderName ~= "-") and System.GetEntityByName(senderName);
	if (sender) then
		senderId = sender.id;
	end
	if (C3COOP_TRIGGER_LOG ~= 0 and kind ~= "H") then -- held object positions: 20 per second
		Log("[c3coop] trigger event applied: %s", ev);
	end
	if (kind == "E") then
		ent.insideCount = count;
		ent.triggered = true;
		ent.triggeredOnce = true;
		ent.Client.ClEnter(ent, senderId, count);
	elseif (kind == "L") then
		ent.insideCount = count;
		if (count <= 0) then
			ent.triggered = nil;
		end
		ent.Client.ClLeave(ent, senderId, count);
	elseif (kind == "V") then
		ent.triggered = true;
		ent.triggeredOnce = true;
		ent.Client.ClVisible(ent, senderId);
	elseif (kind == "I") then
		ent.triggered = nil;
		ent.Client.ClInvisible(ent, senderId);
	elseif (kind == "T") then
		ent.Client.ClTrigger(ent, senderId);
	elseif (kind == "H") then
		-- a held object's position (the server streams it; its physics sleeps while held)
		if (not CryAction.IsServer()) then
			C3CoopHeldApply(ent, senderName);
		end
	elseif (kind == "U") then
		ent.Use(ent); -- InteractiveEntity: same effects and Used output as on the server
	elseif (kind == "X") then
		-- InteractiveObjectEx outputs of another player's interaction (its own machine played it already)
		if (senderId ~= g_localActorId) then
			C3CoopIoExOutputs(ent, count, senderId);
		end
	elseif (kind == "O") then
		-- another player's hack / zipline / ledge / crate / battery (C3COOP_ACTIONS)
		if (senderId ~= g_localActorId) then
			C3CoopActionReplay(ent, senderId);
		end
	elseif (kind == "P") then
		-- the level's start chain (SpawnPoint "Spawn": intro sequence, fader) on this machine's flowgraph
		BroadcastEvent(ent, "Spawn");
	elseif (kind == "S") then
		ent.insideCount = count;
		ent.triggered = string.find(senderName, "t", 1, true) and true or nil;
		ent.triggeredOnce = string.find(senderName, "o", 1, true) and true or nil;
	end
end

function C3CoopTrigStatus(name)
	local ent = System.GetEntityByName(name or "");
	if (not ent) then
		Log("[c3coop] trig: no entity named %s", tostring(name));
		return;
	end
	Log("[c3coop] trig %s (%s): mode %s, enabled %s, triggered %s, once %s, inside %s, server %s", name, tostring(ent.class),
		tostring(C3CoopTriggerMode(ent)), tostring(ent.enabled), tostring(ent.triggered), tostring(ent.triggeredOnce),
		tostring(ent.insideCount), tostring(CryAction.IsServer()));
end

-- client: send a use / visibility report of the local player to the server
function C3CoopToServer(text)
	local me = g_localActor;
	if (me and me.server) then
		me.server:C3ToServer(text);
	end
end

-- kill volume (server): kill the player who entered, keep the volume armed
function C3CoopKillVolume(trig, entityId)
	local p = C3CoopEntity(entityId);
	if (not (p and p.actor and p.actor:IsPlayer()) or p.actor:GetHealth() <= 0) then
		return;
	end
	local game = g_gameRules and g_gameRules.game;
	if (not game) then
		return;
	end
	local level = C3CoopLevel();
	local safe = level and C3COOP_KILL_SAFE and C3COOP_KILL_SAFE[level] and C3COOP_KILL_SAFE[level][trig:GetName()];
	if (safe and C3CoopInZone(safe, p:GetWorldPos())) then
		Log("[c3coop] kill volume %s: %s is in a safe zone (lift cabin), spared", trig:GetName(), p:GetName());
		return;
	end
	local hitType = game:GetHitTypeId("punish");
	if (not hitType or hitType < 0) then
		hitType = game:GetHitTypeId("normal") or 0;
	end
	Log("[c3coop] kill volume %s: killing %s", trig:GetName(), p:GetName());
	local id = p.id;
	game:ServerHit(id, id, id, 10000, 0, 0, -1, hitType);
	Script.SetTimer(300, function()
		local q = System.GetEntity(id);
		if (q and q.actor and q.actor:GetHealth() > 0) then
			Log("[c3coop] kill volume %s: second hit for %s", trig:GetName(), q:GetName());
			game:ServerHit(id, id, id, 10000, 0, 0, -1, hitType);
		end
	end);
end

----------------------------------------------------------------------------------------------------
-- co-op gates (server)
function C3CoopGateList()
	local level = C3CoopLevel();
	return (level and C3COOP_GATES and C3COOP_GATES[level]) or {};
end

function C3CoopGateFor(key, name)
	for _, gate in ipairs(C3CoopGateList()) do
		if (gate[key] == name) then
			return gate;
		end
	end
	return nil;
end

-- gates of one group (one of two lifts) share their state
function C3CoopGateState(gate)
	local rules = g_gameRules;
	if (not rules) then
		return {};
	end
	rules.c3coopGates = rules.c3coopGates or {};
	local key = gate.group or gate.id;
	local st = rules.c3coopGates[key];
	if (not st) then
		st = {};
		rules.c3coopGates[key] = st;
	end
	return st;
end

function C3CoopQuorum(alive)
	local q = tonumber(C3COOP_GATE_QUORUM) or -1;
	if (alive <= 1) then
		return 1;
	elseif (q < 0) then
		return math.max(1, alive - 1);
	elseif (q == 0) then
		return alive;
	end
	return math.min(q, alive);
end

-- players a gate waits for: start gates count the players of the previous level, quorum = "all" gates everybody alive
function C3CoopGateNeed(gate, alive)
	if (gate.start) then
		return math.max(1, tonumber(C3COOP_EXPECTED) or 1);
	elseif (gate.quorumAll) then
		return math.max(1, alive);
	end
	return C3CoopQuorum(alive);
end

function C3CoopGateCount(gate)
	local alive = C3CoopAlivePlayers();
	local inside = {};
	for _, p in ipairs(alive) do
		if (gate.start or C3CoopInZone(gate.zone, p:GetWorldPos())) then
			inside[#inside + 1] = p;
		end
	end
	return inside, alive;
end

-- teleport target of a gate (commit: where the commit token brings stragglers)
function C3CoopGatePos(gate, commit)
	if (commit and gate.commitPos) then
		return gate.commitPos, gate.commitYaw or 0;
	end
	if (gate.posEntity) then
		local e = System.GetEntityByName(gate.posEntity);
		if (e) then
			local p = e:GetWorldPos();
			return {x = p.x, y = p.y, z = p.z}, e:GetWorldAngles().z;
		end
		return nil, 0;
	end
	return gate.pos, gate.yaw or 0;
end

-- every alive player outside the gate zone (and the keep area) farther than the radius goes to the target
function C3CoopGateBring(gate, why, commit)
	if (gate.start or C3CoopSolo()) then
		return 0; -- alone: nobody to bring (and the original never teleports the player)
	end
	local center, yaw = C3CoopGatePos(gate, commit);
	if (not center) then
		Log("[c3coop] gate %s: no teleport target (%s)", gate.id, tostring(gate.posEntity));
		return 0;
	end
	local list = {};
	local r2 = (gate.radius or 0) * (gate.radius or 0);
	for _, p in ipairs(C3CoopAlivePlayers()) do
		local pos = p:GetWorldPos();
		if (not C3CoopInZone(gate.zone, pos) and not C3CoopInZone(gate.keep, pos) and DistanceSqVectors(pos, center) > r2) then
			list[#list + 1] = p;
		end
	end
	for i, p in ipairs(list) do
		C3CoopTeleport(p, C3CoopRing(center, i, #list, gate.ring), yaw, "gate " .. gate.id .. " " .. why);
	end
	return #list;
end

function C3CoopGateOpened(gate, st, ent)
	st.pending = nil;
	-- once triggers and switches do not come back; gates with a commit token are done when it is set
	if (gate.use or gate.switch or gate.start or (ent and ent.triggerOnce)) then
		st.done = true;
	end
	if (gate.start) then
		C3COOP_EXPECTED = nil; -- the rejoin wait is used up
	end
	if (gate.replay) then
		st.fired = true;
	end
end

-- true: the gate holds this activation and fires it later (C3CoopGateTick)
function C3CoopGateHold(key, ent, senderId)
	if (not C3CoopNet() or not CryAction.IsServer()) then
		return false;
	end
	local gate = C3CoopGateFor(key, ent:GetName());
	if (not gate) then
		return false;
	end
	local st = C3CoopGateState(gate);
	if (st.done or st.releasing) then
		return false;
	end
	if (key == "trigger" and not ent.enabled) then
		return false; -- a disabled trigger ignores the Enter anyway
	end
	local inside, alive = C3CoopGateCount(gate);
	local need = C3CoopGateNeed(gate, #alive);
	if (#inside >= need) then
		local n = C3CoopGateBring(gate, "quorum");
		Log("[c3coop] gate %s open: %d/%d players in the zone (%d alive), %d brought, %s fires (%s)", gate.id, #inside, need,
			#alive, n, ent:GetName(), C3CoopEntName(senderId));
		C3CoopGateOpened(gate, st, ent);
		return false;
	end
	if (not st.pending) then
		Log("[c3coop] gate %s holds %s (%s): %d/%d players in the zone (%d alive)", gate.id, ent:GetName(),
			C3CoopEntName(senderId), #inside, need, #alive);
		st.since = System.GetCurrTime();
		st.pending = {ent = ent.id, sender = senderId, player = C3CoopIsPlayerId(senderId), gate = gate.id};
	end
	return true;
end

function C3CoopHudSend(names, text)
	local msg = table.concat(names, ",") .. "~" .. text;
	C3CoopHudReceive(msg); -- the host's own player
	local carrier = g_localActor;
	if (carrier and carrier.allClients) then
		carrier.allClients:C3Hud(msg);
	end
end

-- any machine: show the text when the local player is listed (drawn by C3CoopHudDraw)
function C3CoopHudReceive(msg)
	local list, text = string.match(msg or "", "^([^~]*)~(.*)$");
	local me = g_localActor;
	if (not list or not me) then
		return;
	end
	local myName = me:GetName();
	for name in string.gmatch(list, "[^,]+") do
		if (name == myName) then
			C3COOP_HUD_TEXT = C3CoopLatin and C3CoopLatin(text) or text; -- the label font has no Cyrillic
			C3COOP_HUD_UNTIL = System.GetCurrTime() + 1.7;
			return;
		end
	end
end

function C3CoopHudDraw()
	if (not C3COOP_HUD_TEXT or System.GetCurrTime() > (C3COOP_HUD_UNTIL or 0)) then
		return;
	end
	local cam = System.GetViewCameraPos();
	local dir = System.GetViewCameraDir();
	if (cam and dir) then
		local pos = {x = cam.x + dir.x * 2, y = cam.y + dir.y * 2, z = cam.z + dir.z * 2 + 0.45};
		System.DrawLabel(pos, 1.7, C3COOP_HUD_TEXT, 1.0, 0.8, 0.25, 1.0);
	end
end

-- waiting messages to players in (WAIT) and out of (WAITED) a zone, once a second
function C3CoopWaitHud(st, label, inside, alive, need)
	local now = System.GetCurrTime();
	if (st.hudAt and now - st.hudAt < 1) then
		return;
	end
	st.hudAt = now;
	if (not st.logAt or now - st.logAt >= 10) then
		st.logAt = now;
		Log("[c3coop] %s waits: %d/%d players, held for %.0f s", label, #inside, need, now - (st.since or now));
	end
	local isIn, inNames, outNames = {}, {}, {};
	for _, p in ipairs(inside) do
		isIn[p:GetName()] = true;
		inNames[#inNames + 1] = p:GetName();
	end
	for _, p in ipairs(alive) do
		if (not isIn[p:GetName()]) then
			outNames[#outNames + 1] = p:GetName();
		end
	end
	local count = tostring(#inside) .. "/" .. tostring(need);
	if (#inNames > 0) then
		C3CoopHudSend(inNames, C3COOP_TEXT_WAIT .. " " .. count);
	end
	if (#outNames > 0) then
		C3CoopHudSend(outNames, C3COOP_TEXT_WAITED .. " " .. count .. " (c3coop tp)");
	end
end

function C3CoopGateWaiting(gate, st, inside, alive, need)
	C3CoopWaitHud(st, "gate " .. gate.id, inside, alive, need);
end

function C3CoopGateRelease(gate, st, why)
	local p = st.pending;
	local ent = p and System.GetEntity(p.ent);
	st.pending = nil;
	if (not ent) then
		return;
	end
	-- the player who opened it may have left meanwhile: a player in the zone (or any living one) opens it instead
	local sender = p.sender;
	if (not C3CoopEntity(sender)) then
		local inside, alive = C3CoopGateCount(gate);
		local sub = inside[1] or alive[1];
		if (sub) then
			Log("[c3coop] gate %s: %s is gone, %s opens it", gate.id, C3CoopEntName(sender), sub:GetName());
			sender = sub.id;
		end
	end
	local n = C3CoopGateBring(gate, why);
	Log("[c3coop] gate %s released (%s): %d brought, %s fires (%s)", gate.id, why, n, ent:GetName(), C3CoopEntName(sender));
	st.releasing = true;
	local ok;
	if (gate.use) then
		local user = C3CoopEntity(sender);
		ok = C3CoopCall("gate " .. gate.id .. " use", function() ent:OnUsed(user or ent, 0); end);
	elseif (gate.switch) then
		ok = C3CoopCall("gate " .. gate.id .. " switch", function() ent.Server.SvRequestUse(ent, sender); end);
	else
		ok = C3CoopCall("gate " .. gate.id .. " enter", function() ent:Event_Enter(sender); end);
	end
	st.releasing = false;
	if (not ok) then
		-- not done: the next activation holds it again (a failed release must not leave the door shut for good)
		Log("[c3coop] gate %s: release failed, the gate stays armed", gate.id);
		return;
	end
	C3CoopGateOpened(gate, st, ent);
end

-- server, every 0.5 s
function C3CoopGateTick()
	local now = System.GetCurrTime();
	for _, gate in ipairs(C3CoopGateList()) do
		local st = C3CoopGateState(gate);
		-- shared state of a group: only the gate whose activation is pending ticks it
		if (st.pending and (st.pending.gate == nil or st.pending.gate == gate.id)) then
			local inside, alive = C3CoopGateCount(gate);
			local need = C3CoopGateNeed(gate, #alive);
			if (st.committed) then
				C3CoopGateRelease(gate, st, "committed");
			elseif (#inside >= need) then
				C3CoopGateRelease(gate, st, "quorum " .. #inside .. "/" .. need);
			elseif (gate.start and now - (st.since or now) >= (tonumber(C3COOP_START_TIMEOUT) or 45)) then
				C3CoopGateRelease(gate, st, "start timeout, " .. #inside .. "/" .. need .. " players");
			elseif (gate.start and st.forced) then
				C3CoopGateRelease(gate, st, "c3coop start");
			elseif (#inside == 0 and st.pending.player and not gate.start) then
				Log("[c3coop] gate %s: nobody waits in the zone any more, hold dropped", gate.id);
				st.pending = nil;
			else
				C3CoopGateWaiting(gate, st, inside, alive, need);
			end
		end
	end
	C3CoopZoneTick();
end

-- `c3coop start`: release the level start gates now (host alone after a rejoin that never comes)
function C3CoopStartNow()
	C3COOP_EXPECTED = nil;
	for _, gate in ipairs(C3CoopGateList()) do
		if (gate.start) then
			C3CoopGateState(gate).forced = true;
		end
	end
	Log("[c3coop] start gates released by command");
end

-- server, from the token tick: a gate's one-way token changed -> bring everybody along
function C3CoopGateTokens()
	for _, gate in ipairs(C3CoopGateList()) do
		if (gate.commit) then
			local st = C3CoopGateState(gate);
			local value = tostring(GameToken.GetToken(gate.commit));
			local prev = st.commitPrev;
			st.commitPrev = value;
			if (prev ~= nil and prev ~= value and value == gate.commitValue and not st.committed) then
				st.committed = true;
				st.done = true;
				local n = C3CoopGateBring(gate, "commit", true);
				Log("[c3coop] gate %s committed (%s = %s): %d players brought", gate.id, gate.commit, value, n);
			end
		end
	end
	C3CoopZoneTokens();
end

function C3CoopGateStatus()
	local alive = C3CoopAlivePlayers();
	Log("[c3coop] gates of %s: quorum rule %d, %d alive players, expected %s, server %s", tostring(C3CoopLevel()),
		tonumber(C3COOP_GATE_QUORUM) or -1, #alive, tostring(C3COOP_EXPECTED), tostring(CryAction.IsServer()));
	for _, gate in ipairs(C3CoopGateList()) do
		local st = C3CoopGateState(gate);
		local inside = C3CoopGateCount(gate);
		Log("[c3coop]   %s %s: %d in zone, done %s, committed %s, pending %s", gate.id,
			tostring(gate.trigger or gate.use or gate.switch or ("commit " .. tostring(gate.commit))), #inside,
			tostring(st.done), tostring(st.committed), st.pending and C3CoopEntName(st.pending.sender) or "no");
	end
end

-- server: a (re)joining player gets the Enter of "replay" gates that already fired, and the current zone of every
-- zone group, for himself only (EndGame: the finale starts from its start trigger)
function C3CoopJoinReplay(player)
	local carrier = g_localActor;
	if (not carrier or not carrier.onClient or player.id == g_localActorId or not player.actor) then
		return;
	end
	local channel = player.actor:GetChannel();
	local rules = g_gameRules;
	if (not channel or channel <= 0 or not rules) then
		return;
	end
	-- once per player entity (a later `c3coop nick` sends the nick again)
	rules.c3coopReplayed = rules.c3coopReplayed or {};
	if (rules.c3coopReplayed[player.id]) then
		return;
	end
	rules.c3coopReplayed[player.id] = true;
	local sent = 0;
	for _, gate in ipairs(C3CoopGateList()) do
		if (gate.replay and gate.trigger and C3CoopGateState(gate).fired) then
			carrier.onClient:C3Trig(channel, "E~" .. gate.trigger .. "~" .. player:GetName() .. "~1");
			sent = sent + 1;
		end
	end
	for _, g in ipairs(C3CoopZoneGroups()) do
		local st = C3CoopZoneState(g);
		if (st.published and g.zones[st.published]) then
			carrier.onClient:C3Trig(channel, "E~" .. st.published .. "~" .. player:GetName() .. "~1");
			sent = sent + 1;
		end
	end
	-- the level's start chain (SpawnPoint "Spawn": River's intro sequence, faders) for a player who joins while the level
	-- is still starting, i.e. the rejoin after a level change; not mid-level
	local rules = g_gameRules;
	if (rules and rules.c3coopSpawnFired and rules.c3coopStartedAt and System.GetCurrTime() - rules.c3coopStartedAt < 120) then
		for name in pairs(rules.c3coopSpawnFired) do
			carrier.onClient:C3Trig(channel, "P~" .. name .. "~" .. player:GetName() .. "~1");
			sent = sent + 1;
		end
	end
	-- hacked panels and minefields (C3COOP_ACTIONS keep)
	for name in pairs(rules and rules.c3coopKeptActions or {}) do
		carrier.onClient:C3Trig(channel, "O~" .. name .. "~-~1");
		sent = sent + 1;
	end
	-- opened vents and other once-only InteractiveEntity uses
	for key in pairs(rules and rules.c3coopKeptUses or {}) do
		carrier.onClient:C3Trig(channel, "U~" .. key .. "~-~0");
		sent = sent + 1;
	end
	if (sent > 0) then
		Log("[c3coop] replayed %d trigger events for %s (channel %d)", sent, player:GetName(), channel);
	end
	-- flowgraph outputs the server mirrored before this player joined (fgmirror.cpp)
	System.ExecuteCommand("c3coop fgmirror replay " .. channel);
	-- the level start graph blacks the screen out on Game:Start (every machine) and only its intro fades it back in: a
	-- player joining after the start stayed black. The outputs that fade in, a few seconds after the replay
	local level = C3CoopLevel();
	local fade = level and C3COOP_START_FADEIN and C3COOP_START_FADEIN[level];
	if (fade and not (rules and rules.c3coopStartedAt and System.GetCurrTime() - rules.c3coopStartedAt < 20)) then
		Script.SetTimer(6000, function()
			System.ExecuteCommand("c3coop send " .. channel .. " fgmirror a " .. fade);
			Log("[c3coop] start fade-in sent to channel %d", channel);
		end);
	end
end

-- Lua side of the host's level change (coop.cpp runs it when it tells the clients to rejoin): the next level's start
-- gates wait for this many players
function C3CoopLevelChange()
	local n = #(System.GetEntitiesByClass("Player") or {});
	if (n > 1) then
		C3COOP_EXPECTED = n;
		C3COOP_EXPECTED_FROM_CHANGE = true;
	else
		C3COOP_EXPECTED = nil;
	end
	Log("[c3coop] level change: %d players, the next level start waits for %s", n, tostring(C3COOP_EXPECTED));
end

----------------------------------------------------------------------------------------------------
-- zone tokens (server). A group (C3COOP_ZONES.<level>) is a set of non-once triggers of which only the last one entered
-- counts, e.g. the 12 PlayerPosition_* triggers writing Level.Global.PlayerPosition. Their own Enter/Leave is ignored on
-- every machine (trigger mode "zone"); the server keeps the last zone each player entered and fires the trigger of the
-- REARMOST one (lowest rank; equal ranks: entered last) for everybody, so the layers a player stands in are not unloaded
-- behind him. commits: values written by cutscenes that count as reached by everybody. gates: values nobody crosses
-- alone (quorum, the rest is brought along).
function C3CoopZoneGroups()
	local level = C3CoopLevel();
	return (level and C3COOP_ZONES and C3COOP_ZONES[level]) or {};
end

function C3CoopZoneOf(trigName)
	local level = C3CoopLevel();
	if (not level or not C3COOP_ZONES or not C3COOP_ZONES[level]) then
		return nil;
	end
	C3COOP_ZONE_INDEX = C3COOP_ZONE_INDEX or {};
	local index = C3COOP_ZONE_INDEX[level];
	if (not index) then
		index = {};
		for _, g in ipairs(C3COOP_ZONES[level]) do
			for trig, zone in pairs(g.zones) do
				index[trig] = {group = g, zone = zone, trig = trig};
			end
		end
		C3COOP_ZONE_INDEX[level] = index;
	end
	return index[trigName];
end

function C3CoopZoneState(g)
	local rules = g_gameRules;
	if (not rules) then
		return {last = {}, floor = -1000000, seq = 0, gatesDone = {}};
	end
	rules.c3coopZones = rules.c3coopZones or {};
	local st = rules.c3coopZones[g.name];
	if (not st) then
		st = {last = {}, floor = -1000000, seq = 0, gatesDone = {}};
		rules.c3coopZones[g.name] = st;
	end
	return st;
end

-- server: a player entered one of the zone triggers (solo: the trigger fires as in the original, only record it)
function C3CoopZoneEnter(trig, entityId, solo)
	local z = C3CoopZoneOf(trig:GetName());
	if (not z or not trig.enabled or not C3CoopIsPlayerId(entityId)) then
		return;
	end
	local st = C3CoopZoneState(z.group);
	st.seq = st.seq + 1;
	st.last[entityId] = {trig = z.trig, rank = z.zone.rank, seq = st.seq};
	if (C3COOP_TRIGGER_LOG ~= 0) then
		Log("[c3coop] zone %s: %s entered %s (%s)%s", z.group.name, C3CoopEntName(entityId), z.trig, tostring(z.zone.value),
			solo and ", alone" or "");
	end
	if (solo) then
		st.published = z.trig;
		return;
	end
	C3CoopZoneUpdate(z.group);
end

-- effective rank of a player in a group (nil: not counted)
function C3CoopZoneRank(g, st, p)
	local rec = st.last[p.id];
	local rank, trig, seq;
	if (rec) then
		rank, trig, seq = rec.rank, rec.trig, rec.seq;
	elseif (g.nilZone and g.zones[g.nilZone]) then
		rank, trig, seq = g.zones[g.nilZone].rank, g.nilZone, -1;
	else
		return nil;
	end
	if (rank < st.floor) then
		rank, trig = st.floor, nil; -- behind a point everybody has passed (cutscene): counts as that point
	end
	return rank, trig, seq;
end

-- boundary gates of a group: when somebody is past one and somebody behind, wait for the quorum, then bring the rest
function C3CoopZoneGates(g, st, alive)
	for value, gate in pairs(g.gates or {}) do
		if (not st.gatesDone[value]) then
			local ahead, behind = {}, {};
			for _, p in ipairs(alive) do
				local rank = C3CoopZoneRank(g, st, p);
				if (rank and rank >= gate.rank) then
					ahead[#ahead + 1] = p;
				else
					behind[#behind + 1] = p;
				end
			end
			if (#ahead > 0 and #behind == 0) then
				st.gatesDone[value] = true;
			elseif (#ahead > 0) then
				local need = C3CoopQuorum(#alive);
				local gst = st.gateHud or {};
				st.gateHud = gst;
				if (#ahead >= need) then
					st.gatesDone[value] = true;
					local center, yaw = C3CoopGatePos(gate);
					Log("[c3coop] zone %s: boundary %s passed by %d/%d, bringing %d", g.name, value, #ahead, need, #behind);
					for i, p in ipairs(behind) do
						C3CoopTeleport(p, C3CoopRing(center, i, #behind, gate.ring), yaw, "zone boundary " .. value);
						st.seq = st.seq + 1;
						st.last[p.id] = {trig = gate.trig, rank = gate.rank, seq = st.seq};
					end
				else
					gst.since = gst.since or System.GetCurrTime();
					C3CoopWaitHud(gst, "zone boundary " .. value, ahead, alive, need);
				end
			end
		end
	end
end

-- server: fire the rearmost zone when it changed
function C3CoopZoneUpdate(g)
	local st = C3CoopZoneState(g);
	local alive = C3CoopAlivePlayers();
	C3CoopZoneGates(g, st, alive);
	local best;
	for _, p in ipairs(alive) do
		local rank, trig, seq = C3CoopZoneRank(g, st, p);
		if (rank and ((not best) or rank < best.rank or (rank == best.rank and seq > best.seq))) then
			best = {rank = rank, trig = trig, seq = seq, p = p};
		end
	end
	if (not best or not best.trig or best.trig == st.published) then
		return;
	end
	local ent = System.GetEntityByName(best.trig);
	if (not ent) then
		return;
	end
	local zone = g.zones[best.trig];
	if (st.published == nil and best.trig == g.nilZone) then
		return; -- nobody has reached the group yet: the level's own state stands
	end
	if (not ent.enabled) then
		return; -- a disabled zone trigger writes nothing: try again later
	end
	Log("[c3coop] zone %s: rearmost is %s in %s (%s), was %s", g.name, best.p:GetName(), best.trig, tostring(zone.value),
		tostring(st.published));
	-- a jump forward (the rearmost player was teleported): the zones in between fire first, in rank order, as if walked
	-- through; layer switches listen to single values (Fields: Enable_AB3 on "ab2_shed_b" only)
	local prev = st.published and g.zones[st.published];
	if (prev and zone.rank > prev.rank + 1) then
		local between = {};
		for trig, z in pairs(g.zones) do
			if (z.rank > prev.rank and z.rank < zone.rank) then
				between[#between + 1] = {trig = trig, rank = z.rank};
			end
		end
		table.sort(between, function(a, b) return a.rank < b.rank end);
		for _, b in ipairs(between) do
			local e = System.GetEntityByName(b.trig);
			if (e and e.enabled) then
				Log("[c3coop] zone %s: passing %s", g.name, b.trig);
				e.c3coopZoneFire = true;
				C3CoopCall("zone " .. b.trig, function() e:Event_Enter(best.p.id); end);
				e.c3coopZoneFire = nil;
			end
		end
	end
	st.published = best.trig;
	ent.c3coopZoneFire = true;
	C3CoopCall("zone " .. best.trig, function() ent:Event_Enter(best.p.id); end);
	ent.c3coopZoneFire = nil;
end

-- server, every 0.5 s: deaths, disconnects
function C3CoopZoneTick()
	if (C3CoopSolo()) then
		return;
	end
	for _, g in ipairs(C3CoopZoneGroups()) do
		C3CoopCall("zone tick " .. g.name, C3CoopZoneUpdate, g);
	end
end

-- server, token tick: a commit value written by a cutscene raises the floor of its group
function C3CoopZoneTokens()
	for _, g in ipairs(C3CoopZoneGroups()) do
		if (g.token and g.commits) then
			local value = tostring(GameToken.GetToken(g.token));
			local rank = g.commits[value];
			local st = C3CoopZoneState(g);
			if (rank and rank > st.floor) then
				st.floor = rank;
				st.published = "@" .. value;
				Log("[c3coop] zone %s: %s reached (cutscene), everybody counts as there", g.name, value);
			end
		end
	end
end

-- server: a revived player takes over the zones of the teammate he was revived next to
function C3CoopZoneInherit(p, mate)
	for _, g in ipairs(C3CoopZoneGroups()) do
		local st = C3CoopZoneState(g);
		local rec = mate and st.last[mate.id];
		if (rec) then
			st.last[p.id] = {trig = rec.trig, rank = rec.rank, seq = rec.seq};
		end
	end
end

function C3CoopZoneStatus()
	for _, g in ipairs(C3CoopZoneGroups()) do
		local st = C3CoopZoneState(g);
		Log("[c3coop] zone %s: published %s, floor %s, token %s", g.name, tostring(st.published), tostring(st.floor),
			g.token and tostring(GameToken.GetToken(g.token)) or "-");
		for _, p in ipairs(C3CoopAlivePlayers()) do
			local rank, trig = C3CoopZoneRank(g, st, p);
			Log("[c3coop]   %s: %s (rank %s)", p:GetName(), tostring(trig), tostring(rank));
		end
	end
end

----------------------------------------------------------------------------------------------------
-- InteractiveObjectEx (kick doors): outputs of an interaction played on another machine
function C3CoopIoExOutputs(ent, code, userId)
	ent.c3coopMirroring = true;
	if (code == 1) then
		if (userId) then
			ent:ActivateOutput("UsedBy", userId);
		end
		ent:ActivateOutput("Started", true);
		BroadcastEvent(ent, "Used");
	elseif (code == 2) then
		ent:ActivateOutput("Finished", true);
	elseif (code == 3) then
		ent:ActivateOutput("Aborted", true);
	end
	ent.c3coopMirroring = nil;
end

-- server: outputs on the server's copy (unless it is the host's own interaction) and on every client but the user's
function C3CoopIoExMirror(ent, code, userId)
	if (userId ~= g_localActorId) then
		C3CoopIoExOutputs(ent, code, userId);
	end
	Log("[c3coop] %s: interaction output %d by %s mirrored", ent:GetName(), code or 0, C3CoopEntName(userId));
	C3CoopTrigSend("X", ent, userId, nil, code);
end

-- this machine's player started/finished/aborted an interaction
function C3CoopIoExReport(ent, code)
	if (CryAction.IsServer()) then
		C3CoopIoExMirror(ent, code, g_localActorId);
	else
		C3CoopToServer("ioex " .. tostring(code) .. " " .. C3CoopEntKey(ent));
	end
end

----------------------------------------------------------------------------------------------------
-- Actions the C++ runs only on the acting player's machine (there is no network "use entity"): hacking/scanning
-- callbacks, ziplines, ledges, ammo crates, alien batteries. The acting machine reports it ("act <name>", or the host
-- directly), the server replays it on its copy and sends event "O" to every other client, which replays it too.
--   method:   the callback the C++ calls on the acting machine (wrapped)
--   user:     "table" / "id": its first argument is the user; only the local player's own action is reported
--   fg:       flowgraph inputs that call the same callback on every machine anyway (not reported)
--   replay:   what the other machines run: the callback's outputs, not the per-player C++ effect (a ride, ammo, energy)
--   keep:     the state stays (a hacked panel or minefield): replayed to players who join later
C3COOP_ACTIONS = {
	DoorPanel = {method = "OnCustomActionSucceed", fg = {"Event_ScanSuccess"}, keep = true,
		replay = function(ent) ent:Event_ScanSuccess(); end},
	MineField = {method = "OnCustomActionSucceed", fg = {"Event_SetHacked"}, keep = true,
		replay = function(ent) ent:Event_SetHacked(); end},
	SmartMine = {method = "OnCustomActionSucceed", keep = true,
		replay = function(ent) ent:OnCustomActionSucceed(); end},
	ZipLine = {method = "OnUsed",
		replay = function(ent) ent:ActivateOutput("OnUsed", true); end},
	LedgeObject = {method = "StartUse", user = "id",
		replay = function(ent, senderId) ent:ActivateOutput("StartUse", senderId or NULL_ENTITY); end},
	AmmoCrate = {method = "OnUsed", user = "table",
		replay = function(ent) ent:ActivateOutput("Used", true); end},
	AlienBattery = {method = "OnUsed", user = "table",
		replay = function(ent) BroadcastEvent(ent, "Used"); end},
};

function C3CoopPatchAction(cls, key)
	local spec = C3COOP_ACTIONS[key];
	if (not cls or not spec or cls.c3coopActionKey or not cls[spec.method]) then
		return;
	end
	cls.c3coopActionKey = key;
	local orig = cls[spec.method];
	cls[spec.method] = function(self, user, ...)
		local result = orig(self, user, ...);
		self.c3coopActionAt = System.GetCurrTime();
		if (C3CoopNet() and not self.c3coopMirroring) then
			local userId = user;
			if (type(user) == "table") then
				userId = user.id;
			end
			if (not spec.user or userId == g_localActorId) then
				C3CoopCall(self:GetName() .. " action", C3CoopActionReport, self);
			end
		end
		return result;
	end
	local origs = {};
	for _, name in ipairs(spec.fg or {}) do
		local fgOrig = cls[name];
		if (fgOrig) then
			origs[name] = fgOrig;
			cls[name] = function(self, ...)
				local was = self.c3coopMirroring;
				self.c3coopMirroring = true;
				local result = fgOrig(self, ...);
				self.c3coopMirroring = was;
				return result;
			end
		end
	end
	-- FlowEvents.Inputs hold the original functions
	C3CoopRefreshFlowEvents(cls, origs);
end

for key in pairs(C3COOP_ACTIONS) do
	_G["C3CoopPatchAction" .. key] = function(cls) C3CoopPatchAction(cls, key); end
end

-- another player's action on this machine (server: a client's report; client: event "O")
function C3CoopActionReplay(ent, senderId)
	local spec = C3COOP_ACTIONS[ent.c3coopActionKey or ""];
	if (not spec) then
		return false;
	end
	-- the same action fired here already (the C++ also ran it here for that player): no second set of outputs
	if (ent.c3coopActionAt and System.GetCurrTime() - ent.c3coopActionAt < 2) then
		return true;
	end
	ent.c3coopMirroring = true;
	local ok = C3CoopCall(ent:GetName() .. " action replay", spec.replay, ent, senderId);
	ent.c3coopMirroring = nil;
	ent.c3coopActionAt = System.GetCurrTime();
	return ok;
end

-- server: run it here (unless it is the host's own) and on every client but the actor's
function C3CoopActionMirror(ent, senderId)
	if (senderId ~= g_localActorId) then
		C3CoopActionReplay(ent, senderId);
	end
	Log("[c3coop] %s: action by %s mirrored", ent:GetName(), C3CoopEntName(senderId));
	C3CoopTrigSend("O", ent, senderId, nil, 1);
	local spec = C3COOP_ACTIONS[ent.c3coopActionKey or ""];
	local rules = g_gameRules;
	if (spec and spec.keep and rules) then
		rules.c3coopKeptActions = rules.c3coopKeptActions or {};
		rules.c3coopKeptActions[C3CoopEntKey(ent)] = true;
	end
end

-- this machine's player did it
function C3CoopActionReport(ent)
	if (CryAction.IsServer()) then
		C3CoopActionMirror(ent, g_localActorId);
	else
		C3CoopToServer("act " .. C3CoopEntKey(ent));
	end
end

----------------------------------------------------------------------------------------------------
-- uplink tokens (TOKEN_POLICY uplink): a client's own result that the server's story waits for
function C3CoopUplinkTick()
	if (CryAction.IsServer()) then
		return;
	end
	local level = C3CoopLevel();
	local list = level and C3COOP_TOKEN_UPLINK and C3COOP_TOKEN_UPLINK[level];
	if (not list) then
		return;
	end
	local rules = g_gameRules;
	if (not rules) then
		return;
	end
	rules.c3coopUplinkSent = rules.c3coopUplinkSent or {};
	for name, value in pairs(list) do
		if (tostring(GameToken.GetToken(name)) == value and not rules.c3coopUplinkSent[name]) then
			rules.c3coopUplinkSent[name] = true;
			Log("[c3coop] uplink: %s = %s reached here, telling the server", name, value);
			C3CoopToServer("tok " .. name .. " " .. value);
		end
	end
end

function C3CoopUplinkReceive(player, rest)
	local name, value = string.match(rest or "", "^(%S+) (.+)$");
	local level = C3CoopLevel();
	local list = level and C3COOP_TOKEN_UPLINK and C3COOP_TOKEN_UPLINK[level];
	if (not name or not list or list[name] ~= value) then
		Log("[c3coop] uplink %s from %s refused", tostring(rest), player:GetName());
		return;
	end
	local current = GameToken.GetToken(name);
	if (tostring(current) == value) then
		return;
	end
	local typed = value;
	if (type(current) == "boolean") then
		typed = (value == "true");
	elseif (type(current) == "number") then
		typed = tonumber(value);
	end
	Log("[c3coop] uplink from %s: %s %s -> %s", player:GetName(), name, tostring(current), value);
	GameToken.SetToken(name, typed);
	C3CoopTokenApplied(name, value);
end

-- a token value that reached this machine from the network: play the sequence its flowgraph only plays where the
-- local input happened (C3COOP_TOKEN_SEQUENCES)
function C3CoopTokenApplied(name, value)
	local level = C3CoopLevel();
	local list = level and C3COOP_TOKEN_SEQUENCES and C3COOP_TOKEN_SEQUENCES[level];
	local rule = list and list[name];
	if (rule and rule.value == value) then
		Log("[c3coop] %s = %s from the network: playing %s here", name, value, rule.seq);
		Movie.PlaySequence(rule.seq);
	end
end

----------------------------------------------------------------------------------------------------
-- level specials (C3COOP_SPECIAL.<level>): EndGame runs the finale on every machine; a machine whose player lost the
-- hack (single player reloads the level) makes the host restart the map for everybody, unless somebody already won
function C3CoopSpecialTick()
	local level = C3CoopLevel();
	local sp = level and C3COOP_SPECIAL and C3COOP_SPECIAL[level];
	if (not sp) then
		return;
	end
	local rules = g_gameRules;
	if (not rules) then
		return;
	end
	rules.c3coopSpecialSent = rules.c3coopSpecialSent or {};
	for _, what in ipairs({"won", "restart"}) do
		local rule = sp[what];
		if (rule and tostring(GameToken.GetToken(rule.token)) == rule.value and not rules.c3coopSpecialSent[what]) then
			rules.c3coopSpecialSent[what] = true;
			if (CryAction.IsServer()) then
				C3CoopSpecialReceive(g_localActor, what);
			else
				C3CoopToServer("special " .. what);
			end
		end
	end
end

function C3CoopSpecialReceive(player, what)
	local level = C3CoopLevel();
	local sp = level and C3COOP_SPECIAL and C3COOP_SPECIAL[level];
	local rules = g_gameRules;
	if (not sp or not rules or not player) then
		return;
	end
	if (what == "won") then
		rules.c3coopWon = true;
		Log("[c3coop] %s: %s won", level, player:GetName());
	elseif (what == "restart" and sp.restart) then
		if (rules.c3coopWon) then
			Log("[c3coop] %s: %s lost after somebody won: no restart", level, player:GetName());
		elseif (not rules.c3coopRestart) then
			rules.c3coopRestart = true;
			Log("[c3coop] %s: %s lost, the host restarts %s in %d s", level, player:GetName(), sp.restart.map, sp.restart.delay);
			Script.SetTimer(sp.restart.delay * 1000, function()
				System.ExecuteCommand("map " .. sp.restart.map);
			end);
		end
	end
end

----------------------------------------------------------------------------------------------------
-- rooms: a room's AI waves stay enabled while a player is inside (every machine runs the level's AIWave events)
function C3CoopRoomFor(waveName)
	local level = C3CoopLevel();
	for _, room in ipairs((level and C3COOP_ROOMS and C3COOP_ROOMS[level]) or {}) do
		if (room.waves[waveName]) then
			return room;
		end
	end
	return nil;
end

function C3CoopRoomOccupied(room)
	for _, p in ipairs(C3CoopAlivePlayers()) do
		if (C3CoopInZone(room.zone, p:GetWorldPos())) then
			return true;
		end
	end
	return false;
end

function C3CoopRoomDefer(wave)
	if (not C3CoopNet() or not g_gameRules) then
		return false;
	end
	local room = C3CoopRoomFor(wave:GetName());
	if (not room or not C3CoopRoomOccupied(room)) then
		return false;
	end
	g_gameRules.c3coopDeferredWaves = g_gameRules.c3coopDeferredWaves or {};
	g_gameRules.c3coopDeferredWaves[wave:GetName()] = room;
	Log("[c3coop] %s is occupied: AI wave %s stays enabled", room.name, wave:GetName());
	return true;
end

-- every machine, every 0.5 s: disable deferred waves once their room is empty (if the level still wants them off)
function C3CoopRoomTick()
	local deferred = g_gameRules and g_gameRules.c3coopDeferredWaves;
	if (not deferred) then
		return;
	end
	for name, room in pairs(deferred) do
		if (not C3CoopRoomOccupied(room)) then
			deferred[name] = nil;
			local wave = System.GetEntityByName(name);
			if (wave and tostring(GameToken.GetToken(room.token)) == "false") then
				Log("[c3coop] %s is empty: disabling AI wave %s now", room.name, name);
				C3CoopCall("AIWave Disable " .. name, wave.c3coopOrig.Event_Disable, wave);
			else
				Log("[c3coop] %s is empty, AI wave %s stays enabled (%s = %s)", room.name, name, room.token,
					tostring(GameToken.GetToken(room.token)));
			end
		end
	end
end

----------------------------------------------------------------------------------------------------
-- class patches (called at the end of each patched entity script)
function C3CoopRefreshFlowEvents(cls, orig)
	local inputs = cls.FlowEvents and cls.FlowEvents.Inputs;
	if (not inputs) then
		return;
	end
	local map = {};
	for name, f in pairs(orig) do
		if (type(f) == "function" and cls[name] and cls[name] ~= f) then
			map[f] = cls[name];
		end
	end
	for _, def in pairs(inputs) do
		if (type(def) == "table" and map[def[1]]) then
			def[1] = map[def[1]];
		end
	end
end

-- ProximityTrigger and AreaTrigger
function C3CoopPatchTrigger(cls)
	if (not cls or cls.c3coopOrig) then
		return;
	end
	local orig = {CanTrigger = cls.CanTrigger, Event_Enter = cls.Event_Enter, Event_Leave = cls.Event_Leave,
		Event_Enable = cls.Event_Enable, OnUsed = cls.OnUsed};
	cls.c3coopOrig = orig;

	cls.CanTrigger = function(self, entity, ...)
		local ok = orig.CanTrigger(self, entity, ...);
		local mode = C3CoopTriggerMode(self);
		if (not ok and mode and mode ~= "cosmetic" and self.c3coopAnyPlayer and CryAction.IsServer()) then
			-- "my player" / "local player" meant the player in single player: any player on the server
			local p = self.Properties;
			local my, localOnly = p.bOnlyMyPlayer, p.bOnlyLocalPlayer;
			p.bOnlyMyPlayer, p.bOnlyLocalPlayer = (my and 0), (localOnly and 0);
			local done, result = pcall(orig.CanTrigger, self, entity, ...);
			p.bOnlyMyPlayer, p.bOnlyLocalPlayer = my, localOnly;
			ok = done and result;
		end
		if (not ok) then
			return ok;
		end
		if (mode == "cosmetic") then
			local e = entity;
			if (type(e) ~= "table") then
				e = System.GetEntity(e);
			end
			if (e and e.actor and e.actor:IsPlayer() and e.id ~= g_localActorId) then
				return false; -- cosmetic: this machine's own player only
			end
		elseif (mode and not CryAction.IsServer()) then
			return false; -- gameplay: the server's events drive this trigger
		end
		return ok;
	end

	cls.Event_Enter = function(self, entityId, ...)
		local mode = C3CoopTriggerMode(self);
		if (mode == nil or mode == "cosmetic") then
			return orig.Event_Enter(self, entityId, ...);
		end
		if (not CryAction.IsServer()) then
			return;
		end
		if (mode == "zone" or mode == "kill") and C3CoopSolo() and not self.c3coopZoneFire then
			-- alone: original behaviour (the zone engine only records where the player is)
			if (mode == "zone") then
				C3CoopCall("zone enter " .. self:GetName(), C3CoopZoneEnter, self, entityId, true);
			end
			local fires = self.enabled and not (self.triggerOnce and self.triggeredOnce);
			orig.Event_Enter(self, entityId, ...);
			if (fires) then
				C3CoopTrigSend("E", self, entityId);
			end
			return;
		end
		if (mode == "zone") then
			if (self.c3coopZoneFire) then
				-- the zone engine fires the rearmost player's zone for everybody
				local fires = self.enabled;
				orig.Event_Enter(self, entityId, ...);
				if (fires) then
					C3CoopTrigSend("E", self, entityId);
				end
			else
				C3CoopCall("zone enter " .. self:GetName(), C3CoopZoneEnter, self, entityId);
			end
			return;
		end
		if (mode == "kill") then
			if (self.enabled) then
				C3CoopKillVolume(self, entityId);
			end
			return; -- never latches: the volume stays armed
		end
		-- player-only triggers fire Enter when they fill up and Leave when they empty, as with one player
		if (not self.c3coopEnabling and self.triggered and (self.insideCount or 0) > 1 and C3CoopPlayerOnly(self)) then
			return;
		end
		if (C3CoopGateHold("trigger", self, entityId)) then
			return;
		end
		local fires = self.enabled and not (self.triggerOnce and self.triggeredOnce);
		orig.Event_Enter(self, entityId, ...);
		if (fires) then
			C3CoopTrigSend("E", self, entityId);
		end
	end

	cls.Event_Leave = function(self, entityId, ...)
		local mode = C3CoopTriggerMode(self);
		if (mode == nil or mode == "cosmetic") then
			return orig.Event_Leave(self, entityId, ...);
		end
		if (not CryAction.IsServer()) then
			return;
		end
		if ((mode == "kill" or mode == "zone") and not C3CoopSolo()) then
			return; -- zone triggers: only the last zone entered counts, Leave means nothing; kill volumes never latch
		end
		if ((self.insideCount or 0) > 0 and C3CoopPlayerOnly(self) and C3CoopIsPlayerId(entityId)) then
			return; -- other players are still inside
		end
		local fires = self.enabled and self.triggered;
		orig.Event_Leave(self, entityId, ...);
		if (fires) then
			C3CoopTrigSend("L", self, entityId);
		end
	end

	cls.Event_Enable = function(self, ...)
		self.c3coopEnabling = true;
		C3CoopCall(self:GetName() .. " Enable", orig.Event_Enable, self, ...);
		self.c3coopEnabling = nil;
	end

	if (orig.OnUsed) then
		cls.OnUsed = function(self, user, ...)
			local mode = C3CoopTriggerMode(self);
			if (mode and mode ~= "cosmetic" and not CryAction.IsServer()) then
				if (user and user.id == g_localActorId) then
					C3CoopToServer("use " .. C3CoopEntKey(self));
				end
				return;
			end
			return orig.OnUsed(self, user, ...);
		end
	end
	C3CoopRefreshFlowEvents(cls, orig);
end

-- CinematicTrigger: visibility of each machine's own player; the server collects it from every player
function C3CoopPatchCinematic(cls)
	if (not cls or cls.c3coopOrig) then
		return;
	end
	local orig = {Event_Visible = cls.Event_Visible, Event_Invisible = cls.Event_Invisible, Event_Trigger = cls.Event_Trigger,
		ClientOnTimer = cls.Client.OnTimer};
	cls.c3coopOrig = orig;
	local function wrap(name, kind, fires)
		cls[name] = function(self, entityId, ...)
			local mode = C3CoopTriggerMode(self);
			if (mode == nil or mode == "cosmetic") then
				return orig[name](self, entityId, ...);
			end
			if (not CryAction.IsServer()) then
				return;
			end
			local f = fires(self);
			orig[name](self, entityId, ...);
			if (f) then
				C3CoopTrigSend(kind, self, entityId);
			end
		end
	end
	wrap("Event_Visible", "V", function(self) return self.enabled and not (self.triggerOnce and self.triggeredOnce); end);
	wrap("Event_Invisible", "I", function(self) return self.enabled and self.triggered; end);
	wrap("Event_Trigger", "T", function(self) return true; end);

	cls.Client.OnTimer = function(self, timerId, msec)
		local mode = C3CoopTriggerMode(self);
		if (mode == nil or self.isServer) then
			return orig.ClientOnTimer(self, timerId, msec);
		end
		if (timerId ~= self.VISIBILITY_TIMER_ID) then
			if (mode == "cosmetic") then
				return orig.ClientOnTimer(self, timerId, msec);
			end
			return;
		end
		-- the original asks the server through the trigger entity, which is not bound to the network
		if (self:IsInRange() and self:IsVisible() and self:CheckZoom()) then
			if (not self.isvisible) then
				if (not self.last_visible_time) then
					self.last_visible_time = _time;
				end
				if (_time - self.last_visible_time > self.Properties.fMinVisibleTime) then
					if (mode == "cosmetic") then
						self:Visible(g_localActorId);
					else
						C3CoopToServer("cv 1 " .. C3CoopEntKey(self));
					end
					self.isvisible = true;
				end
			end
		else
			if (self.last_visible_time and self.isvisible) then
				if (mode == "cosmetic") then
					self:Invisible(g_localActorId);
				else
					C3CoopToServer("cv 0 " .. C3CoopEntKey(self));
				end
				self.isvisible = nil;
			end
			self.last_visible_time = nil;
		end
		self:SetTimer(self.VISIBILITY_TIMER_ID, self.Properties.fCheckTimer * 1000);
	end
	C3CoopRefreshFlowEvents(cls, orig);
end

-- InteractiveEntity (switches): usable in co-op; the server's use is replayed on every client
function C3CoopPatchUsable(cls)
	if (not cls or cls.c3coopOrig) then
		return;
	end
	local orig = {IsUsable = cls.IsUsable, OnUsed = cls.OnUsed, Use = cls.Use};
	cls.c3coopOrig = orig;

	cls.IsUsable = function(self, user)
		if (not C3CoopNet()) then
			return orig.IsUsable(self, user);
		end
		-- the original refuses every use in a network game; single player rules
		if (self.bUsable == 1 and self:GetState() ~= "Destroyed") then
			return 2;
		end
		if (self.Properties.bPickable == 1) then
			if ((not EmptyString(self.Properties.fileModelDestroyed)) or (not EmptyString(self.Properties.DestroyedSubObject))) then
				return 1;
			end
		end
		return 0;
	end

	cls.OnUsed = function(self, user, idx)
		if (C3CoopNet() and not CryAction.IsServer()) then
			if (user and user.id == g_localActorId) then
				Log("[c3coop] use %s: asking the server", self:GetName());
				C3CoopToServer("use " .. C3CoopEntKey(self));
			end
			return;
		end
		if (C3CoopGateHold("use", self, user and user.id)) then
			return;
		end
		return orig.OnUsed(self, user, idx);
	end

	-- InteractiveEntity.Use also runs from Script.SetTimerForFunction("InteractiveEntity.Use", self)
	cls.Use = function(self, ...)
		if (C3CoopNet() and CryAction.IsServer()) then
			C3CoopTrigSend("U", self, nil);
			-- once-only uses (vent grates) are replayed to players who join later
			if (g_gameRules and tonumber(self.Properties and self.Properties.bUsableOnlyOnce or 0) == 1) then
				g_gameRules.c3coopKeptUses = g_gameRules.c3coopKeptUses or {};
				g_gameRules.c3coopKeptUses[C3CoopEntKey(self)] = true;
			end
		end
		return orig.Use(self, ...);
	end
	C3CoopRefreshFlowEvents(cls, orig);
end

-- Switch (Net.Exposed: OnUsed -> server SvRequestUse -> allClients OnUsed_Internal): a gated switch waits on the server
function C3CoopPatchSwitch(cls)
	if (not cls or cls.c3coopOrig or not cls.Server or not cls.Server.SvRequestUse) then
		return;
	end
	local orig = {SvRequestUse = cls.Server.SvRequestUse};
	cls.c3coopOrig = orig;
	cls.Server.SvRequestUse = function(self, userId, ...)
		if (C3CoopGateHold("switch", self, userId)) then
			return;
		end
		return orig.SvRequestUse(self, userId, ...);
	end
end

-- InteractiveObjectEx (kick doors, C++ interaction on the user's machine only): mirror its outputs everywhere
function C3CoopPatchIoEx(cls)
	if (not cls or cls.c3coopOrig) then
		return;
	end
	local orig = {OnUsed = cls.OnUsed, StopUse = cls.StopUse, AbortUse = cls.AbortUse};
	cls.c3coopOrig = orig;
	local function wrap(name, code)
		if (not orig[name]) then
			return;
		end
		cls[name] = function(self, user, ...)
			local result = orig[name](self, user, ...);
			if (C3CoopNet() and not self.c3coopMirroring) then
				local userId = user;
				if (type(user) == "table") then
					userId = user.id;
				end
				local mine;
				if (code == 1) then
					mine = (userId ~= nil and userId == g_localActorId);
					self.c3coopLocalUse = mine;
				else
					-- Finished/Aborted only of a use this machine's player started with the use key (flowgraph-driven
					-- uses run on every machine anyway)
					mine = self.c3coopLocalUse;
					self.c3coopLocalUse = nil;
				end
				if (mine) then
					C3CoopCall(self:GetName() .. " " .. name, C3CoopIoExReport, self, code);
				end
			end
			return result;
		end
	end
	wrap("OnUsed", 1);
	wrap("StopUse", 2);
	wrap("AbortUse", 3);
end

-- AIWave: Disable of a wave whose room is occupied waits until the room is empty (C3CoopRoomTick)
function C3CoopPatchWave(cls)
	if (not cls or cls.c3coopOrig) then
		return;
	end
	local orig = {Event_Disable = cls.Event_Disable, Event_DisableAndClear = cls.Event_DisableAndClear};
	cls.c3coopOrig = orig;
	cls.Event_Disable = function(self, ...)
		if (not self.c3coopClearing and C3CoopRoomDefer(self)) then
			return;
		end
		return orig.Event_Disable(self, ...);
	end
	if (orig.Event_DisableAndClear) then
		cls.Event_DisableAndClear = function(self, ...)
			self.c3coopClearing = true;
			C3CoopCall(self:GetName() .. " DisableAndClear", orig.Event_DisableAndClear, self, ...);
			self.c3coopClearing = nil;
		end
	end
	C3CoopRefreshFlowEvents(cls, orig);
end
'''

# appended to each patched entity script: load Lockstep.lua once, then patch the class
SCRIPT_PATCH_LUA = '''
-- c3coop: co-op lockstep (Scripts/c3coop/Lockstep.lua)
if (not C3COOP_LOCKSTEP) then
	Script.ReloadScript("Scripts/c3coop/Lockstep.lua");
end
if (%(patch)s) then
	%(patch)s(%(cls)s);
end
'''
SCRIPT_PATCHES = {
    'scripts/entities/triggers/proximitytrigger.lua': ('C3CoopPatchTrigger', 'ProximityTrigger'),
    'scripts/entities/triggers/areatrigger.lua': ('C3CoopPatchTrigger', 'AreaTrigger'),
    'scripts/entities/triggers/cinematictrigger.lua': ('C3CoopPatchCinematic', 'CinematicTrigger'),
    'scripts/entities/others/interactiveentity.lua': ('C3CoopPatchUsable', 'InteractiveEntity'),
    'scripts/entities/ai/aiwave.lua': ('C3CoopPatchWave', 'AIWave'),
    'scripts/entities/others/switch.lua': ('C3CoopPatchSwitch', 'Switch'),
    'scripts/entities/playerinteractive/interactiveobjectex.lua': ('C3CoopPatchIoEx', 'InteractiveObjectEx'),
    # C3COOP_ACTIONS (Lockstep.lua): actions run only on the acting player's machine
    'scripts/entities/environment/doorpanel.lua': ('C3CoopPatchActionDoorPanel', 'DoorPanel'),
    'scripts/entities/environment/minefield.lua': ('C3CoopPatchActionMineField', 'MineField'),
    'scripts/entities/environment/smartmine.lua': ('C3CoopPatchActionSmartMine', 'SmartMine'),
    'scripts/entities/environment/zipline.lua': ('C3CoopPatchActionZipLine', 'ZipLine'),
    'scripts/entities/contextualnavigation/ledgeobject.lua': ('C3CoopPatchActionLedgeObject', 'LedgeObject'),
    'scripts/entities/items/ammocrate.lua': ('C3CoopPatchActionAmmoCrate', 'AmmoCrate'),
    'scripts/entities/environment/alienbattery.lua': ('C3CoopPatchActionAlienBattery', 'AlienBattery'),
}


def patch_entity_script(src, patch, cls):
    text = src.decode('latin-1')
    if not re.search(r'^(\xef\xbb\xbf)?\s*%s\s*=' % re.escape(cls), text, flags=re.M):  # (latin-1 decoded BOM)
        raise SystemExit(f'{cls}: class table not found')
    nl = '\r\n' if '\r\n' in text else '\n'
    return (text + (SCRIPT_PATCH_LUA % {'patch': patch, 'cls': cls}).replace('\n', nl)).encode('latin-1')


def lockstep_lua(gate_levels):
    files = '\n'.join('Script.ReloadScript("Scripts/c3coop/Gates_%s.lua");' % level for level in gate_levels)
    text = (LOCKSTEP_LUA.replace('@@GATE_FILES@@', files)
            .replace('@@TEXT_WAIT@@', lua_utf8('Ждём игроков:'))
            .replace('@@TEXT_WAITED@@', lua_utf8('Вас ждут:')))
    return text.encode('ascii')


def level_triggers_lua(classes):
    lines = ['-- generated by scripts/build_pak.py: co-op handling of each level\'s local triggers (see classify_triggers)',
             '-- cosmetic: local on every machine, local player only; kill: kill volume (the server kills who enters);',
             '-- original: left as in the game; every other trigger is server-authoritative (Scripts/c3coop/Lockstep.lua);',
             '-- anyplayer: server-authoritative triggers with bOnlyMyPlayer/bOnlyLocalPlayer, which react to any player.',
             'C3COOP_LEVEL_TRIGGERS = {']
    for level, result in classes.items():
        lines.append('\t%s = {' % level)
        for mode in ('cosmetic', 'kill', 'original', 'anyplayer'):
            names = sorted(result[mode])
            lines.append('\t\t%s = {%s},' % (mode, ', '.join('[%s] = 1' % _lua_value(n) for n in names)))
        lines.append('\t},')
    lines.append('};')
    return ('\n'.join(lines) + '\n').encode('utf-8')


def game_dir():
    """C3_GAME_DIR, else the Steam library that has the game (steamapps/libraryfolders.vdf)."""
    if os.environ.get('C3_GAME_DIR'):
        return os.environ['C3_GAME_DIR']
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
            steam = os.path.normpath(winreg.QueryValueEx(key, 'SteamPath')[0])
    except OSError:
        steam = r'C:\Program Files (x86)\Steam'
    libraries = [steam]
    try:
        with open(os.path.join(steam, 'steamapps', 'libraryfolders.vdf'), encoding='utf-8') as f:
            libraries += [p.replace('\\\\', '\\') for p in re.findall(r'"path"\s+"([^"]+)"', f.read())]
    except OSError:
        pass
    for library in libraries:
        path = os.path.join(library, 'steamapps', 'common', 'Crysis3Remastered')
        if os.path.isdir(path):
            return path
    return os.path.join(steam, 'steamapps', 'common', 'Crysis3Remastered')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=game_dir())
    args = ap.parse_args()
    files = GameFiles(args.game)
    out = {}

    # 1. GAME="SP"/"MP" filtered data (HUD, weapons, ammo, nanosuit, material effects, perks...)
    spified = failed = 0
    for name in files.names():
        if not name.endswith('.xml') or not name.startswith(('scripts/', 'libs/')):
            continue
        data = files.read(name)
        if b'GAME="' not in data and b"GAME='" not in data:
            continue
        try:
            new, removed = spify(data)
        except ET.ParseError as e:
            failed += 1
            print(f'  skip (xml parse error) {name}: {e}')
            continue
        out[files.original_name(name)] = new
        spified += 1
    print(f'spified {spified} xml files ({failed} skipped)')

    # 1b. CellFeline.xml keeps the default third person model feline_tp.cdf although feline_cell_tp.cdf (material
    #     feline_tp_cell) ships; teammates' and dropped weapons are drawn third person -> default skin
    tp_fixes = {'scripts/entities/items/crysis2/weapons/humans/cellfeline.xml':
                ('Objects/weapons/us/feline/feline_tp.cdf', 'Objects/weapons/us/feline/feline_cell_tp.cdf')}
    for name, (old, new) in tp_fixes.items():
        key = files.original_name(name)
        text = (out.get(key) or files.read(name)).decode('utf-8-sig')
        pat = f'<thirdperson name="{old}"'
        if text.count(pat) != 1 or new.lower() not in files.index:
            raise SystemExit(f'{name}: third person model {old!r} not found exactly once, or {new!r} not shipped')
        out[key] = text.replace(pat, f'<thirdperson name="{new}"').encode('utf-8')
        print(f'patched {key}: third person model {new}')

    # 2. MP-only files the network code paths ask for -> SP equivalents
    copies = {
        'Libs/UI/config/navigation_multiplayer_ingame.xml': 'Libs/UI/config/navigation_singleplayer_ingame.xml',
        'Libs/BodyDamage/BodyDamage_MP.xml': 'Libs/BodyDamage/BodyDamage_Default.xml',
        'Libs/HitDeathReactionsData/HitDeathReactions_MP.xml': 'Libs/HitDeathReactionsData/HitDeathReactions_PlayerSP.xml',
    }
    # MP HUD panels (objects/hud/cw2_*.cgf, picked by C++ in network games) render allinone_hud_3d_mp.gfx
    copies['Libs/UI/allinone_hud_3d_mp.gfx'] = 'Libs/UI/AllInOne_HUD_3D.gfx'
    # MP nanosuit / team materials -> SP nanosuit material (the C++ also asks for these directly)
    for suit in ('army', 'ops'):
        for variant in ('', '_friendly', '_hostile', '_friendly_dead', '_hostile_dead'):
            copies[f'{MP_SUIT_DIR}nanosuit_mp_{suit}{variant}.mtl'] = SP_SUIT_MTL
    for dst, src in copies.items():
        data = out.get(files.original_name(src)) or files.read(src)
        out[dst] = data
        print(f'copy {src} -> {dst}')

    # 3. no MP team materials (nanosuit_mp_*_friendly/hostile are not shipped -> white characters)
    out['Scripts/PlayerTeamVisualization.xml'] = b'<Root>\n\t<PlayerTeamVisualization>\n\t</PlayerTeamVisualization>\n</Root>\n'

    # 4. player.lua: SetIsMultiplayer becomes a no-op
    out[files.original_name('scripts/entities/actor/player.lua')] = patch_player_lua(
        files.read('scripts/entities/actor/player.lua'))

    # 5. SinglePlayer gamerules get a spectator module: in a network game a dead player uses CDeathCameraMode
    #    (MP), which asks the gamerules for a spectator module and crashes on NULL. The spectator picks its targets
    #    from the player stats module (NULL in SinglePlayer -> crash on death). No automatic spectating: the
    #    co-op respawn (step 7) revives the player first, and a client still auto-requesting follow mode after
    #    its revive loops forever ("requested spectator mode 3 but the request was denied").
    sp_rules_name = files.original_name('scripts/gamerules/singleplayer.xml')
    sp_rules = files.read(sp_rules_name).decode('utf-8-sig')
    modules = ("\t<PlayerStats class='StandardPlayerStats' />\n"
               "\t<Spectator class='MPSpectator' eatsActorActions='1' enableFree='0' enableFollow='0' "
               "enableKiller='0' timeFromDeathTillAutoSpectate='3600000' />\n")
    if 'Spectator' not in sp_rules:
        sp_rules = sp_rules.replace('</Mode>', modules + '</Mode>')
    out[sp_rules_name] = sp_rules.encode('utf-8')

    # 6. network compression policy for campaign-sized levels and AI stances (see POLICY_OVERRIDES)
    policy_name = files.original_name('scripts/network/compressionpolicy.xml')
    out[policy_name] = patch_compression_policy(files.read(policy_name))
    print(f'patched {policy_name}: {", ".join(POLICY_OVERRIDES)}')

    # 7. co-op respawn next to a living teammate instead of endless spectating
    sp_lua_name = files.original_name('scripts/gamerules/singleplayer.lua')
    out[sp_lua_name] = patch_singleplayer_lua(files.read(sp_lua_name))
    mo_lua_name = files.original_name('scripts/entities/others/missionobjective.lua')
    out[mo_lua_name] = patch_mission_objective_lua(files.read(mo_lua_name))
    ba_lua_name = files.original_name('scripts/entities/actor/basicactor.lua')
    out[ba_lua_name] = patch_basic_actor_lua(files.read(ba_lua_name))
    nu_lua_name = files.original_name('scripts/entities/others/nanosuitupgrade.lua')
    out[nu_lua_name] = patch_nanosuit_upgrade_lua(files.read(nu_lua_name))
    for kind, path in (('destroyableobject', 'scripts/entities/physics/destroyableobject.lua'),
                       ('huskreplenisher', 'scripts/entities/others/huskreplenisher.lua'),
                       ('pressurizedobject', 'scripts/entities/physics/pressurizedobject.lua')):
        name = files.original_name(path)
        out[name] = patch_destroyable_lua(files.read(name), kind)
    for path in ('scripts/entities/physics/basicentity.lua', 'scripts/entities/physics/rigidbodyex.lua'):
        name = files.original_name(path)
        out[name] = patch_net_prop_lua(files.read(name), path)
    el_lua_name = files.original_name('scripts/entities/elevators/elevator.lua')
    out[el_lua_name] = patch_elevator_lua(files.read(el_lua_name))
    st_name = files.original_name('scripts/ai/behaviortrees/stalkertree.xml')
    out[st_name] = patch_stalker_tree(files.read(st_name))

    # 8. levels: trigger classes (lockstep / cosmetic / kill volume) from each level's flowgraphs, co-op gates, and the
    #    shared game tokens (story state) the server syncs to the clients
    all_tokens, synced, old_shared, classes, gate_levels, uplinks, fades = {}, {}, {}, {}, [], {}, {}
    levels = dict(level_paks(args.game))
    for name in set(GATES) | set(ROOMS) | set(ZONES) | set(KILL_SAFE) | set(LEVEL_SPECIAL) | set(TOKEN_POLICY) | set(
            TRIGGER_OVERRIDES):
        if name not in levels:
            raise SystemExit(f'level {name!r} (co-op tables) not found in the game')
    for level, pak in levels.items():
        all_tokens[level] = read_level_token_libs(pak)
        token_names = {t for t, _, _ in all_tokens[level]}
        old_shared[level] = [t for t, lib, lo in all_tokens[level] if lib == 'Level' and not lo]
        model = LevelModel(pak)
        fades[level] = start_fade_records(pak)
        classes[level], local_tokens, stats = classify_triggers(model)
        apply_trigger_overrides(level, model, classes[level])
        for mode in ('kill', 'cosmetic', 'original'):
            if classes[level][mode]:
                stats[mode] = len(classes[level][mode])
        refs = set()
        for _, _, inputs in model.nodes.values():
            for k, v in inputs.items():
                if k.startswith('gametoken_') or k == 'Token':
                    refs.add(v)
        synced[level], tstats = select_level_tokens(level, all_tokens[level], refs, local_tokens)
        for token, (value, seq) in TOKEN_SEQUENCES.get(level, {}).items():
            _check_token(level, token_names, token)
            if not any(inputs.get('seq_Sequence_File') == seq for _, _, inputs in model.nodes.values()):
                raise SystemExit(f'TOKEN_SEQUENCES[{level}]: no flowgraph plays sequence {seq!r}')
        up = TOKEN_POLICY.get(level, {}).get('uplink')
        if up:
            uplinks[level] = up
        print(f'level {level}: triggers {dict(stats)}; tokens: {len(synced[level])} synced of {len(token_names)} '
              f'({", ".join("%s %d" % kv for kv in sorted(tstats.items()))})')
        if level in GATES or level in ROOMS or level in ZONES or level in KILL_SAFE or level in LEVEL_SPECIAL:
            out[f'Scripts/c3coop/Gates_{level}.lua'] = gates_lua(level, model, token_names)
            gate_levels.append(level)
            print(f'  {len(GATES.get(level, []))} gates, {len(ROOMS.get(level, []))} rooms, '
                  f'{len(ZONES.get(level, []))} zone groups')
    signatures = level_signatures(all_tokens, old_shared)
    for level in levels:
        if level not in signatures:
            raise SystemExit(f'level {level}: no unique token to detect it')
    out['Scripts/c3coop/LevelTokens.lua'] = level_tokens_lua(synced, signatures, uplinks) + start_fades_lua(fades)
    print('start fade-ins for late joiners: ' + ', '.join(f'{lv} {len(r)}' for lv, r in fades.items() if r))
    out['Scripts/c3coop/LevelTriggers.lua'] = level_triggers_lua(classes)
    out['Scripts/c3coop/Lockstep.lua'] = lockstep_lua(gate_levels)
    for name, (patch, cls) in SCRIPT_PATCHES.items():
        out[files.original_name(name)] = patch_entity_script(files.read(name), patch, cls)
        print(f'patched {files.original_name(name)}: {patch}')

    # 9. marker: `exec c3coop.cfg` in the console proves the pak is mounted
    out['c3coop.cfg'] = b'echo c3coop.pak is mounted\n'

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with zipfile.ZipFile(OUT, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in sorted(out):
            z.writestr(name, out[name])
    print(f'wrote {OUT}: {len(out)} files, {os.path.getsize(OUT) / 1024:.0f} KB')


if __name__ == '__main__':
    sys.exit(main())
