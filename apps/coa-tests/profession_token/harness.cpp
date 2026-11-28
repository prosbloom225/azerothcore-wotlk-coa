#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using uint32 = std::uint32_t;
using int32 = std::int32_t;
using uint8 = std::uint8_t;

enum ItemClass : uint32 { ITEM_CLASS_WEAPON = 2, ITEM_CLASS_ARMOR = 4 };
enum InventoryType : uint32
{
    INVTYPE_NON_EQUIP = 0,
    INVTYPE_BODY = 4,
    INVTYPE_WEAPON = 13,
    INVTYPE_BAG = 18,
    INVTYPE_TABARD = 19,
    INVTYPE_ROBE = 20
};
constexpr int ITEM_CHANGED = 1;
constexpr uint32 ITEM_FIELD_RANDOM_PROPERTIES_ID = 12;
constexpr uint32 EQUIP_ERR_INVENTORY_FULL = 1;

struct ObjectGuid
{
    uint32 value = 0;
    uint32 GetCounter() const { return value; }
    std::uint64_t GetRawValue() const { return value; }
    bool operator==(ObjectGuid const& other) const { return value == other.value; }
};

struct Creature;
struct GroupReference;
struct Group;
struct Item;
struct Player;
struct WorldSession;

struct ItemTemplate
{
    uint32 ItemId = 0;
    uint32 Class = 0;
    uint32 InventoryType = 0;
    uint32 ItemLevel = 0;
    uint32 Quality = 0;
    int32 RandomProperty = 0;
    int32 RandomSuffix = 0;
};

struct ObjectMgr
{
    std::unordered_map<uint32, ItemTemplate> items;
    ItemTemplate const* GetItemTemplate(uint32 id) const
    {
        auto entry = items.find(id);
        return entry == items.end() ? nullptr : &entry->second;
    }
};
ObjectMgr objects;
ObjectMgr* sObjectMgr = &objects;

struct Creature
{
    uint32 entry = 0;
    uint32 GetEntry() const { return entry; }
};

struct WorldSession
{
    std::vector<uint32> queries;
    bool bot = false;
    bool IsBot() const { return bot; }
    void SendItemQuerySingleResponse(uint32 entry) { queries.push_back(entry); }
};

struct Item
{
    uint32 guid = 0;
    ItemTemplate proto{};
    Player* owner = nullptr;
    bool equipped = false;
    uint8 slot = 0;
    int32 randomProperty = 0;
    uint32 suffixFactor = 0;
    uint32 GetEntry() const { return proto.ItemId; }
    ItemTemplate const* GetTemplate() const { return &proto; }
    Player* GetOwner() const { return owner; }
    ObjectGuid GetGUID() const { return ObjectGuid{ guid }; }
    int32 GetItemRandomPropertyId() const { return randomProperty; }
    void SetInt32Value(uint32 field, int32 value)
    {
        assert(field == ITEM_FIELD_RANDOM_PROPERTIES_ID);
        randomProperty = value;
    }
    void UpdateItemSuffixFactor() { suffixFactor = 700 + proto.ItemLevel; }
    void SetState(int, Player*) { }
    bool IsEquipped() const { return equipped; }
    uint8 GetSlot() const { return slot; }
};

struct GroupReference
{
    GroupReference* next() { return nullptr; }
    Player* GetSource() const { return nullptr; }
};

struct Group
{
    GroupReference* GetFirstMember() { return nullptr; }
};

struct Player
{
    std::vector<std::pair<Item*, uint32>> destroyed;
    std::vector<uint32> added;
    WorldSession* session = nullptr;
    uint32 applied = 0;
    Item* GetItemByGuid(uint32) const { return nullptr; }
    template <typename T>
    void DestroyItemCount(T* item, uint32 count, bool)
    {
        destroyed.push_back({ reinterpret_cast<Item*>(item), count });
    }
    ObjectGuid GetGUID() const { return ObjectGuid{ 1 }; }
    bool AddItem(uint32 entry, uint32)
    {
        added.push_back(entry);
        return true;
    }
    void SendEquipError(uint32, Item*, Item*) { }
    WorldSession* GetSession() const { return session; }
    bool IsInMap(Creature const*) const { return true; }
    bool IsAtLootRewardDistance(Creature const*) const { return true; }
    Group* GetGroup() const { return nullptr; }
    void _ApplyItemMods(Item* item, uint8, bool)
    {
        assert(item);
        ++applied;
    }
};

namespace ProfessionRescale
{
bool owned = true;
bool OwnsItem(uint32) { return owned; }
}

namespace ItemScaling
{
std::unique_ptr<ItemTemplate> DeriveTemplate(uint32 entry, ItemTemplate const& base, uint32 level)
{
    auto result = std::make_unique<ItemTemplate>(base);
    result->ItemId = entry;
    result->ItemLevel = level;
    if (base.RandomSuffix)
        result->RandomSuffix = base.RandomSuffix + int32(level - base.ItemLevel);
    return result;
}
}

struct Field
{
    uint32 value = 0;
    template <typename T>
    T Get() const { return T(value); }
    template <typename T>
    T Get(uint32) const { return T(value); }
};
struct ResultSet
{
    bool NextRow() { return false; }
    Field* Fetch() { return nullptr; }
};
using QueryResult = std::shared_ptr<ResultSet>;
struct CharacterDatabaseStub
{
    std::vector<std::string> executed;
    QueryResult Query(std::string const&) const { return nullptr; }
    template <typename... Args>
    void Execute(std::string const& sql, Args&&...)
    {
        executed.push_back(sql);
    }
};
CharacterDatabaseStub CharacterDatabase;

struct ChatHandler
{
    explicit ChatHandler(WorldSession*) { }
    void SendSysMessage(std::string const&) { }
};

namespace ProfessionToken
{
struct Rune
{
    std::uint32_t Entry;
    std::uint32_t TargetLevel;
};
}

// ACTUAL_PROFESSION_TOKEN

namespace
{
ItemTemplate Craft(uint32 entry, uint32 level, uint32 inventory = INVTYPE_ROBE, uint32 itemClass = ITEM_CLASS_ARMOR)
{
    ItemTemplate proto;
    proto.ItemId = entry;
    proto.Class = itemClass;
    proto.InventoryType = inventory;
    proto.ItemLevel = level;
    proto.Quality = 4;
    proto.RandomProperty = 6250036;
    objects.items[entry] = proto;
    return proto;
}

uint32 MakeRune(uint32 entry)
{
    objects.items[entry] = ItemTemplate{ entry, 12, 0, 0, 1, 0, 0 };
    return entry;
}

Item RuneItem(uint32 guid, uint32 entry)
{
    Item rune;
    rune.guid = guid;
    rune.proto = objects.items.at(entry);
    return rune;
}
}

int main()
{
    ProfessionToken::LoadLevels();

    assert(ProfessionToken::FindRune(7800165)->TargetLevel == 65);
    assert(ProfessionToken::FindRune(7800168)->TargetLevel == 68);
    assert(ProfessionToken::FindRune(7800172)->TargetLevel == 72);
    assert(ProfessionToken::FindRune(7800175)->TargetLevel == 75);
    assert(!ProfessionToken::FindRune(7800161) && !ProfessionToken::FindRune(42));

    WorldSession session;
    Player player;
    player.session = &session;

    ItemTemplate chest = Craft(28483, 65);
    Item target;
    target.guid = 42;
    target.proto = chest;
    target.owner = &player;
    target.randomProperty = 6250036;

    assert(ProfessionToken::IsEligibleCraft(&target));
    ProfessionRescale::owned = false;
    assert(!ProfessionToken::IsEligibleCraft(&target));
    ProfessionRescale::owned = true;

    ItemTemplate bag = Craft(28484, 65, INVTYPE_BAG);
    Item bagItem;
    bagItem.proto = bag;
    assert(!ProfessionToken::IsEligibleCraft(&bagItem));
    ItemTemplate tabard = Craft(28485, 65, INVTYPE_TABARD);
    Item tabardItem;
    tabardItem.proto = tabard;
    assert(!ProfessionToken::IsEligibleCraft(&tabardItem));
    ItemTemplate axe = Craft(23497, 60, INVTYPE_WEAPON, ITEM_CLASS_WEAPON);
    Item axeItem;
    axeItem.proto = axe;
    assert(ProfessionToken::IsEligibleCraft(&axeItem));
    assert(!ProfessionToken::IsEligibleCraft(nullptr));

    assert(ProfessionToken::CurrentLevel(&target) == 65);
    assert(ProfessionToken::CurrentLevel(nullptr) == 0);

    Item normal = RuneItem(10, MakeRune(7800165));
    Item heroic = RuneItem(11, MakeRune(7800168));
    Item mythic = RuneItem(12, MakeRune(7800172));
    Item ascended = RuneItem(13, MakeRune(7800175));

    assert(!ProfessionToken::Apply(&player, &normal, &target));
    assert(player.destroyed.empty());
    assert(!ProfessionToken::Apply(&player, nullptr, &target));
    assert(!ProfessionToken::Apply(&player, &heroic, nullptr));
    assert(!ProfessionToken::Apply(&player, &heroic, &heroic));

    Item other;
    other.guid = 43;
    other.proto = chest;
    other.owner = &player;
    other.randomProperty = 6250036;

    assert(ProfessionToken::Apply(&player, &heroic, &target));
    assert(ProfessionToken::CurrentLevel(&target) == 68);
    assert(ProfessionToken::CurrentLevel(&other) == 65);
    assert(!ProfessionToken::Derived(&other));
    assert(player.destroyed.size() == 1 && player.destroyed[0].second == 1);
    assert(session.queries == std::vector<uint32>{ 28483 });

    ItemTemplate const* derived = ProfessionToken::Derived(&target);
    assert(derived && derived->ItemLevel == 68 && derived->ItemId == 28483);
    assert(derived->RandomProperty == 6250036 && derived->RandomSuffix == 0);
    assert(target.suffixFactor == 765);
    assert(target.randomProperty == 6250036);

    size_t destroyed = player.destroyed.size();
    assert(!ProfessionToken::Apply(&player, &normal, &target));
    assert(!ProfessionToken::Apply(&player, &heroic, &target));
    assert(ProfessionToken::CurrentLevel(&target) == 68);
    assert(player.destroyed.size() == destroyed);

    assert(ProfessionToken::Apply(&player, &mythic, &target));
    assert(ProfessionToken::CurrentLevel(&target) == 72);
    assert(ProfessionToken::Derived(&target)->ItemLevel == 72);
    assert(!ProfessionToken::Apply(&player, &heroic, &target));
    assert(ProfessionToken::Apply(&player, &ascended, &target));
    assert(ProfessionToken::CurrentLevel(&target) == 75);
    assert(!ProfessionToken::Apply(&player, &mythic, &target));
    assert(ProfessionToken::Derived(&target)->ItemLevel == 75);

    assert(ProfessionToken::Apply(&player, &heroic, &other));
    assert(ProfessionToken::CurrentLevel(&other) == 68);
    assert(ProfessionToken::CurrentLevel(&target) == 75);

    assert(std::any_of(CharacterDatabase.executed.begin(), CharacterDatabase.executed.end(),
        [](std::string const& sql) { return sql.find("`character_item_scaling`") != std::string::npos; }));
    assert(std::none_of(CharacterDatabase.executed.begin(), CharacterDatabase.executed.end(),
        [](std::string const& sql) { return sql.find("item_template") != std::string::npos; }));

    ItemTemplate suffixCraft = Craft(292040, 60, INVTYPE_WEAPON, ITEM_CLASS_WEAPON);
    suffixCraft.RandomSuffix = -128;
    objects.items[suffixCraft.ItemId] = suffixCraft;
    Item suffixed;
    suffixed.guid = 77;
    suffixed.proto = suffixCraft;
    suffixed.owner = &player;
    suffixed.randomProperty = -128;
    Item suffixRune = RuneItem(14, MakeRune(7800168));
    assert(ProfessionToken::Apply(&player, &suffixRune, &suffixed));
    assert(suffixed.randomProperty == -120);
    assert(target.suffixFactor == 765);
    assert(suffixed.suffixFactor == 760);
    assert(ProfessionToken::Derived(&suffixed)->RandomSuffix == -120);

    target.equipped = true;
    target.slot = 4;
    Item ascended2 = RuneItem(15, MakeRune(7800175));
    assert(!ProfessionToken::Apply(&player, &ascended2, &target));
    target.slot = 5;

    for (uint32 boss : { 11502u, 11982u, 11988u, 12056u, 12057u, 12098u, 12118u, 12259u, 12264u })
    {
        assert(ProfessionToken::IsRaidBoss(boss));
        assert(ProfessionToken::IsRaidBoss(boss + 100000));
        assert(ProfessionToken::IsRaidBoss(boss + 200000));
        assert(ProfessionToken::IsRaidBoss(boss + 300000));
        assert(!ProfessionToken::IsRaidBoss(boss + 400000));
        assert(ProfessionToken::RuneForBoss(boss) == 7800165);
        assert(ProfessionToken::RuneForBoss(boss + 100000) == 7800168);
        assert(ProfessionToken::RuneForBoss(boss + 200000) == 7800172);
        assert(ProfessionToken::RuneForBoss(boss + 300000) == 7800175);
    }
    assert(!ProfessionToken::IsRaidBoss(11673));
    assert(!ProfessionToken::IsRaidBoss(0));
    assert(ProfessionToken::RuneForBoss(1) == 0);

    Creature boss;
    boss.entry = 11502;
    std::vector<Player*> members = ProfessionToken::RaidMembers(&player, &boss);
    assert(members.size() == 1 && members[0] == &player);
}
