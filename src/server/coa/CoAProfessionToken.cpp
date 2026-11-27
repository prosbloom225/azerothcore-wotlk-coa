/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "CoAProfessionToken.h"
#include "CoAProfessionRescale.h"
#include "AscensionItemScaling.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemScript.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "WorldSession.h"
#include <array>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>

namespace ProfessionToken
{
namespace
{
constexpr std::array<Rune, 4> Runes = {{
    { 7800161, 65 },
    { 7800168, 68 },
    { 7800172, 72 },
    { 7800175, 75 }
}};

std::mutex LevelsMutex;
std::unordered_map<std::uint32_t, std::uint32_t> Levels;

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
    std::uint32_t const stored = StoredLevel(item->GetGUID().GetCounter());
    if (stored)
        return stored;
    ItemTemplate const* proto = item->GetTemplate();
    return proto ? proto->ItemLevel : 0;
}

std::uint32_t StoredLevel(std::uint32_t itemGuid)
{
    std::lock_guard lock(LevelsMutex);
    auto const found = Levels.find(itemGuid);
    return found == Levels.end() ? 0 : found->second;
}

void LoadLevels()
{
    std::unordered_map<std::uint32_t, std::uint32_t> loaded;
    if (QueryResult result = CharacterDatabase.Query(
        "SELECT `item_guid`, `scaling_level` FROM `character_item_scaling`"))
    {
        do
        {
            Field const* fields = result->Fetch();
            loaded[fields[0].Get<std::uint32_t>()] = fields[1].Get<std::uint32_t>();
        } while (result->NextRow());
    }

    std::lock_guard lock(LevelsMutex);
    Levels = std::move(loaded);
}

void SetLevel(Item* item, std::uint32_t level)
{
    if (!item)
        return;

    std::lock_guard lock(LevelsMutex);
    Levels[item->GetGUID().GetCounter()] = level;
}

std::mutex TemplatesMutex;
std::unordered_map<std::uint64_t, std::unique_ptr<ItemTemplate>> Templates;

ItemTemplate const* Derived(Item const* item)
{
    if (!item)
        return nullptr;
    ItemTemplate const* proto = item->GetTemplate();
    std::uint32_t const level = CurrentLevel(item);
    if (!proto || level <= proto->ItemLevel)
        return nullptr;

    std::uint64_t const key = (std::uint64_t(proto->ItemId) << 8) | level;
    std::lock_guard lock(TemplatesMutex);
    auto [entry, inserted] = Templates.try_emplace(key);
    if (inserted)
        entry->second = ItemScaling::DeriveTemplate(proto->ItemId, *proto, level);
    return entry->second.get();
}

ItemTemplate const* DerivedForQuery(std::uint32_t entry, std::uint32_t level)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    if (!proto || level <= proto->ItemLevel)
        return nullptr;

    std::uint64_t const key = (std::uint64_t(entry) << 8) | level;
    std::lock_guard lock(TemplatesMutex);
    auto [derived, inserted] = Templates.try_emplace(key);
    if (inserted)
        derived->second = ItemScaling::DeriveTemplate(entry, *proto, level);
    return derived->second.get();
}

bool Apply(Player* player, Item* rune, Item* target)
{
    if (!player || !rune || !target || rune == target)
        return false;

    Rune const* selected = FindRune(rune->GetEntry());
    if (!selected || !IsEligibleCraft(target) || target->GetOwner() != player)
        return false;

    std::uint32_t const current = CurrentLevel(target);
    if (current >= selected->TargetLevel)
        return false;

    CharacterDatabase.Execute(
        "REPLACE INTO `character_item_scaling` (`item_guid`, `item_entry`, `scaling_level`) VALUES ({}, {}, {})",
        target->GetGUID().GetCounter(), target->GetEntry(), selected->TargetLevel);
    SetLevel(target, selected->TargetLevel);
    target->SetState(ITEM_CHANGED, player);
    player->DestroyItemCount(rune, 1, true);
    if (WorldSession* session = player->GetSession())
        session->SendItemQuerySingleResponse(target->GetEntry());
    Message(player, "The rune reshapes the item.");
    return true;
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
    profession_token_levels() : WorldScript("profession_token_levels", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        ProfessionToken::LoadLevels();
    }
};

void AddSC_CoAProfessionToken()
{
    new item_coa_profession_rune();
    new profession_token_levels();
}
