/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#ifndef COA_PROFESSION_TOKEN_H
#define COA_PROFESSION_TOKEN_H

#include <cstdint>

class Item;
class Player;
struct ItemTemplate;

namespace ProfessionToken
{
struct Rune
{
    std::uint32_t Entry;
    std::uint32_t TargetLevel;
};

Rune const* FindRune(std::uint32_t entry);
bool IsEligibleCraft(Item const* item);
std::uint32_t CurrentLevel(Item const* item);
bool Apply(Player* player, Item* rune, Item* target);
void LoadLevels();
std::uint32_t StoredLevel(std::uint32_t itemGuid);
void SetLevel(Item* item, std::uint32_t level);
ItemTemplate const* Derived(Item const* item);
ItemTemplate const* DerivedForQuery(std::uint32_t entry, std::uint32_t level);
}

#endif
