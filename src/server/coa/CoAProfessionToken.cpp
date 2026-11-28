/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "CoAProfessionToken.h"
#include "AscensionItemScaling.h"
#include "Chat.h"
#include "CoAProfessionRescale.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Item.h"
#include "ItemScript.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "WorldSession.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ProfessionToken
{
namespace
{
constexpr std::array<Rune, 4> Runes = {{
    { 7800165, 65 },
    { 7800168, 68 },
    { 7800172, 72 },
    { 7800175, 75 }
}};

constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 36> RaidBosses = {{
    { 11502, 7800165 }, { 111502, 7800168 }, { 211502, 7800172 }, { 311502, 7800175 },
    { 11982, 7800165 }, { 111982, 7800168 }, { 211982, 7800172 }, { 311982, 7800175 },
    { 11988, 7800165 }, { 111988, 7800168 }, { 211988, 7800172 }, { 311988, 7800175 },
    { 12056, 7800165 }, { 112056, 7800168 }, { 212056, 7800172 }, { 312056, 7800175 },
    { 12057, 7800165 }, { 112057, 7800168 }, { 212057, 7800172 }, { 312057, 7800175 },
    { 12098, 7800165 }, { 112098, 7800168 }, { 212098, 7800172 }, { 312098, 7800175 },
    { 12118, 7800165 }, { 112118, 7800168 }, { 212118, 7800172 }, { 312118, 7800175 },
    { 12259, 7800165 }, { 112259, 7800168 }, { 212259, 7800172 }, { 312259, 7800175 },
    { 12264, 7800165 }, { 112264, 7800168 }, { 212264, 7800172 }, { 312264, 7800175 }
}};

std::mutex LevelsMutex;
std::unordered_map<std::uint32_t, std::uint32_t> Levels;

std::mutex TemplatesMutex;
std::unordered_map<std::uint64_t, std::unique_ptr<ItemTemplate>> Templates;

std::mutex GuardMutex;
std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t> Guards;

bool Gear(ItemTemplate const& item)
{
    return (item.Class == ITEM_CLASS_ARMOR || item.Class == ITEM_CLASS_WEAPON)
        && item.InventoryType != INVTYPE_NON_EQUIP && item.InventoryType != INVTYPE_BAG
        && item.InventoryType != INVTYPE_BODY && item.InventoryType != INVTYPE_TABARD;
}

void Message(Player* player, char const* text)
{
    if (player && player->GetSession())
        ChatHandler(player->GetSession()).SendSysMessage(text);
}

std::uint32_t LevelOf(std::uint32_t itemGuid)
{
    std::lock_guard lock(LevelsMutex);
    auto const found = Levels.find(itemGuid);
    return found == Levels.end() ? 0 : found->second;
}

void Persist(std::uint32_t guid, std::uint32_t entry, std::uint32_t level)
{
    CharacterDatabase.Execute(
        "REPLACE INTO `character_item_scaling` (`item_guid`, `item_entry`, `scaling_level`) "
        "VALUES ({}, {}, {})",
        guid, entry, level);
}

void AnswerQuery(Player* player, ItemTemplate const& proto)
{
    if (WorldSession* session = player ? player->GetSession() : nullptr)
        session->SendItemQuerySingleResponse(proto.ItemId);
}

void RefreshRandomAffix(Player* player, Item* item, ItemTemplate const* proto)
{
    int32 const randomProperty = item->GetItemRandomPropertyId();
    if (!randomProperty || !proto)
        return;

    bool const suffix = randomProperty < 0;
    std::uint32_t const pool = suffix ? std::uint32_t(-randomProperty) : std::uint32_t(randomProperty);
    bool const derived = suffix ? proto->RandomSuffix == int32(pool) : proto->RandomProperty == int32(pool);
    if (!derived)
    {
        int32 const replacement = suffix ? proto->RandomSuffix : proto->RandomProperty;
        if (!replacement)
            return;

        item->SetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID, replacement);
    }

    item->UpdateItemSuffixFactor();
    item->SetState(ITEM_CHANGED, player);
}

std::uint32_t RaidBossRune(std::uint32_t entry)
{
    for (auto const& [boss, rune] : RaidBosses)
        if (boss == entry)
            return rune;
    return 0;
}

bool Grant(Player* player, std::uint32_t entry)
{
    return player->AddItem(entry, 1);
}
}

Rune const* FindRune(std::uint32_t entry)
{
    for (Rune const& rune : Runes)
        if (rune.Entry == entry)
            return &rune;
    return nullptr;
}

bool IsEligibleCraft(Item const* item)
{
    if (!item)
        return false;
    ItemTemplate const* proto = item->GetTemplate();
    return proto && ProfessionRescale::OwnsItem(proto->ItemId) && Gear(*proto);
}

std::uint32_t CurrentLevel(Item const* item)
{
    if (!item)
        return 0;
    std::uint32_t const stored = LevelOf(item->GetGUID().GetCounter());
    if (stored)
        return stored;
    ItemTemplate const* proto = item->GetTemplate();
    return proto ? proto->ItemLevel : 0;
}

void SetLevel(Item* item, std::uint32_t level)
{
    if (!item)
        return;

    Persist(item->GetGUID().GetCounter(), item->GetEntry(), level);
    std::lock_guard lock(LevelsMutex);
    Levels[item->GetGUID().GetCounter()] = level;
}

void LoadLevels()
{
    std::unordered_map<std::uint32_t, std::uint32_t> loaded;
    if (QueryResult result = CharacterDatabase.Query(
        "SELECT s.`item_guid`, s.`scaling_level` FROM `character_item_scaling` s "
        "JOIN `coa_profession_item_budget` b ON b.`entry` = s.`item_entry`"))
    {
        do
        {
            Field const* fields = result->Fetch();
            loaded[fields[0].Get<std::uint32_t>()] = fields[1].Get<std::uint32_t>();
        } while (result->NextRow());
    }

    std::unordered_map<std::uint64_t, std::unique_ptr<ItemTemplate>> templates;
    for (auto const& [guid, level] : loaded)
        templates[(std::uint64_t(guid) << 8) | level] = nullptr;

    std::lock_guard levels(LevelsMutex);
    std::lock_guard cache(TemplatesMutex);
    Levels = std::move(loaded);
    Templates = std::move(templates);
}

ItemTemplate const* Derived(Item const* item)
{
    if (!item)
        return nullptr;
    ItemTemplate const* proto = item->GetTemplate();
    std::uint32_t const level = CurrentLevel(item);
    if (!proto || level <= proto->ItemLevel)
        return nullptr;

    std::uint64_t const key = (std::uint64_t(item->GetGUID().GetCounter()) << 8) | level;
    std::lock_guard lock(TemplatesMutex);
    auto [derived, inserted] = Templates.try_emplace(key);
    if (inserted)
        derived->second = ItemScaling::DeriveTemplate(proto->ItemId, *proto, level);
    return derived->second ? derived->second.get() : nullptr;
}

ItemTemplate const* DerivedForQuery(std::uint32_t entry, std::uint32_t level)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    if (!proto || level <= proto->ItemLevel)
        return nullptr;

    std::uint64_t const key = (std::uint64_t(1) << 32) | (std::uint64_t(entry) << 8) | level;
    std::lock_guard lock(TemplatesMutex);
    auto [derived, inserted] = Templates.try_emplace(key);
    if (inserted)
        derived->second = ItemScaling::DeriveTemplate(entry, *proto, level);
    return derived->second ? derived->second.get() : nullptr;
}

bool Apply(Player* player, Item* rune, Item* target)
{
    if (!player || !rune || !target || rune == target)
        return false;

    Rune const* selected = FindRune(rune->GetEntry());
    if (!selected || !IsEligibleCraft(target) || target->GetOwner() != player)
        return false;

    if (CurrentLevel(target) >= selected->TargetLevel)
    {
        Message(player, "The rune is spent only when it raises an item's level.");
        return false;
    }

    ItemTemplate const* proto = target->GetTemplate();
    std::uint64_t const key = (std::uint64_t(target->GetGUID().GetCounter()) << 8) | selected->TargetLevel;
    {
        std::lock_guard lock(TemplatesMutex);
        auto [derived, inserted] = Templates.try_emplace(key);
        if (inserted)
            derived->second = ItemScaling::DeriveTemplate(proto->ItemId, *proto, selected->TargetLevel);
        if (!derived->second)
        {
            Templates.erase(key);
            Message(player, "The rune cannot reshape this item.");
            return false;
        }
    }

    SetLevel(target, selected->TargetLevel);
    RefreshRandomAffix(player, target, Derived(target));

    if (target->IsEquipped())
    {
        player->_ApplyItemMods(target, target->GetSlot(), false);
        player->_ApplyItemMods(target, target->GetSlot(), true);
    }

    uint32 count = 1;
    player->DestroyItemCount(rune, count, true);
    AnswerQuery(player, *proto);
    Message(player, "The rune reshapes the item.");
    return true;
}

bool IsRaidBoss(std::uint32_t entry)
{
    return RaidBossRune(entry) != 0;
}

std::uint32_t RuneForBoss(std::uint32_t bossEntry)
{
    return RaidBossRune(bossEntry);
}

std::vector<Player*> RaidMembers(Player* owner, Creature const* boss)
{
    std::vector<Player*> members;
    if (!owner || !boss)
        return members;

    auto eligible = [boss](Player* member)
    {
        return member && member->GetSession() && !member->GetSession()->IsBot() && member->IsInMap(boss)
            && member->IsAtLootRewardDistance(boss);
    };

    if (Group* group = owner->GetGroup())
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
            if (Player* member = reference->GetSource(); eligible(member))
                members.push_back(member);

    if (members.empty() && eligible(owner))
        members.push_back(owner);
    return members;
}

bool AwardRune(Player* player, std::uint32_t bossEntry)
{
    std::uint32_t const rune = RaidBossRune(bossEntry);
    if (!rune || !player || !player->GetSession())
        return false;

    std::pair<std::uint32_t, std::uint64_t> const key = { bossEntry, player->GetGUID().GetRawValue() };
    {
        std::lock_guard lock(GuardMutex);
        if (Guards.contains(key))
            return false;
    }

    if (!Grant(player, rune))
    {
        player->SendEquipError(EQUIP_ERR_INVENTORY_FULL, nullptr, nullptr);
        return false;
    }

    {
        std::lock_guard lock(GuardMutex);
        Guards[key] = rune;
    }
    CharacterDatabase.Execute("REPLACE INTO `coa_profession_token_award` "
        "(`boss_entry`, `player_guid`, `rune_entry`) VALUES ({}, {}, {})",
        bossEntry, player->GetGUID().GetCounter(), rune);
    return true;
}

void LoadGuards()
{
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t> guards;
    if (QueryResult result = CharacterDatabase.Query(
        "SELECT `boss_entry`, `player_guid`, `rune_entry` FROM `coa_profession_token_award`"))
    {
        do
        {
            Field const* fields = result->Fetch();
            guards[{ fields[0].Get<std::uint32_t>(), fields[1].Get<std::uint64_t>() }] =
                fields[2].Get<std::uint32_t>();
        } while (result->NextRow());
    }

    std::lock_guard lock(GuardMutex);
    Guards = std::move(guards);
}
}

class item_coa_profession_rune : public ItemScript
{
public:
    item_coa_profession_rune() : ItemScript("item_coa_profession_rune") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const& targets) override
    {
        if (Item* target = targets.GetItemTarget())
            return ProfessionToken::Apply(player, item, target);

        if (Item* target = player->GetItemByGuid(targets.GetItemTargetGUID()))
            return ProfessionToken::Apply(player, item, target);

        return false;
    }
};

class profession_token_levels : public WorldScript
{
public:
    profession_token_levels() : WorldScript("profession_token_levels",
        { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnStartup() override
    {
        ProfessionToken::LoadLevels();
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (!reload)
            ProfessionToken::LoadGuards();
    }
};

class profession_token_raid_runes : public MiscScript
{
public:
    profession_token_raid_runes() : MiscScript("profession_token_raid_runes",
        { MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS }) { }

    void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const*, LootStore const& store, Player* owner,
        bool personal, bool, uint16) override
    {
        if (!loot || !owner || personal || &store != &LootTemplates_Creature || !owner->FindMap())
            return;

        Creature* boss = owner->FindMap()->GetCreature(loot->sourceWorldObjectGUID);
        if (!boss || !ProfessionToken::IsRaidBoss(boss->GetEntry()))
            return;

        for (Player* member : ProfessionToken::RaidMembers(owner, boss))
            ProfessionToken::AwardRune(member, boss->GetEntry());
    }
};

void AddSC_CoAProfessionToken()
{
    new item_coa_profession_rune();
    new profession_token_levels();
    new profession_token_raid_runes();
}
