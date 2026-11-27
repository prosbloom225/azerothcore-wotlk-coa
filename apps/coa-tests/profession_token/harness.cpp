#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using uint32 = std::uint32_t;
enum ItemClass : uint32
{
    ITEM_CLASS_WEAPON = 2,
    ITEM_CLASS_ARMOR = 4
};
enum InventoryType : uint32
{
    INVTYPE_NON_EQUIP = 0,
    INVTYPE_BODY = 4,
    INVTYPE_TABARD = 19,
    INVTYPE_BAG = 18
};
constexpr int ITEM_CHANGED = 1;

struct ItemTemplate
{
    uint32 ItemId = 0;
    uint32 Class = 0;
    uint32 InventoryType = 0;
    uint32 ItemLevel = 0;
    uint32 Quality = 0;
};

struct Player;
struct Item
{
    ItemTemplate proto{};
    Player* owner = nullptr;
    uint32 GetEntry() const { return proto.ItemId; }
    ItemTemplate const* GetTemplate() const { return &proto; }
    Player* GetOwner() const { return owner; }
    uint32 GetGUID() const { return 42; }
    void SetState(int, Player*) { }
};

struct Player
{
    Item* target = nullptr;
    Item* GetItemByGuid(uint32) const { return target; }
    void DestroyItemCount(Item*, uint32, bool) { }
};

struct WorldSession
{
    void SendItemQuerySingleResponse(uint32) { }
};

struct ObjectMgr
{
    std::unordered_map<uint32, ItemTemplate> items;
    ItemTemplate const* GetItemTemplate(uint32 id) const
    {
        auto found = items.find(id);
        return found == items.end() ? nullptr : &found->second;
    }
};
ObjectMgr objects;
ObjectMgr* sObjectMgr = &objects;

namespace ProfessionRescale
{
bool owned = true;
bool OwnsItem(uint32) { return owned; }
}

namespace ItemScaling
{
std::unique_ptr<ItemTemplate> DeriveTemplate(uint32, ItemTemplate const&, uint32 level)
{
    auto result = std::make_unique<ItemTemplate>();
    result->ItemLevel = level;
    return result;
}
}

struct Field
{
    uint32 value = 0;
    uint32 Get(uint32) const { return value; }
};
struct QueryResult
{
    bool NextRow() { return false; }
    Field const* Fetch() const { return nullptr; }
};
struct CharacterDatabaseStub
{
    QueryResult* Query(std::string const&) const { return nullptr; }
    void Execute(std::string const&) const { }
} CharacterDatabase;

#define ChatHandler(...) ChatHandlerStub{}
struct ChatHandlerStub
{
    void SendSysMessage(std::string const&) { }
};

// ACTUAL_PROFESSION_TOKEN

int main()
{
    ProfessionToken::LoadLevels();
    assert(ProfessionToken::FindRune(7800161)->TargetLevel == 65);
    assert(ProfessionToken::FindRune(7800168)->TargetLevel == 68);
    assert(ProfessionToken::FindRune(7800172)->TargetLevel == 72);
    assert(ProfessionToken::FindRune(7800175)->TargetLevel == 75);
    assert(!ProfessionToken::FindRune(1));

    ItemTemplate craft{ 28483, ITEM_CLASS_ARMOR, 5, 68, 4 };
    objects.items[craft.ItemId] = craft;
    Item target;
    target.proto = craft;
    assert(ProfessionToken::IsEligibleCraft(&target));
    ProfessionRescale::owned = false;
    assert(!ProfessionToken::IsEligibleCraft(&target));
    ProfessionRescale::owned = true;

    Item nonGear;
    nonGear.proto = craft;
    nonGear.proto.InventoryType = INVTYPE_BAG;
    assert(!ProfessionToken::IsEligibleCraft(&nonGear));

    assert(ProfessionToken::CurrentLevel(&target) == 68);
    ProfessionToken::SetLevel(&target, 72);
    assert(ProfessionToken::CurrentLevel(&target) == 72);

    Player player;
    target.owner = &player;
    Item rune;
    rune.proto.ItemId = 7800168;
    assert(!ProfessionToken::Apply(&player, &rune, &target));
}
