import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE = ROOT / 'src/server/coa/CoAProfessionToken.cpp'

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using uint32 = std::uint32_t;
enum { ITEM_CLASS_WEAPON = 2, ITEM_CLASS_ARMOR = 4, INVTYPE_NON_EQUIP = 0, INVTYPE_BAG = 18, INVTYPE_BODY = 4,
       INVTYPE_TABARD = 19, ITEM_CHANGED = 1 };
struct ItemTemplate { uint32 ItemId, Class, InventoryType, ItemLevel, Quality; };
struct Item
{
    ItemTemplate proto{};
    std::string owner = "player";
    uint32 ItemLevel() const { return proto.ItemLevel; }
    uint32 GetEntry() const { return proto.ItemId; }
    ItemTemplate const* GetTemplate() const { return &proto; }
    std::string const& GetOwner() const { return owner; }
    uint32 GetGUID() const { return 7; }
    void SetState(int, void*) { }
};
struct ObjectMgr { std::unordered_map<uint32, ItemTemplate> items; ItemTemplate const* GetItemTemplate(uint32 id) const { auto i = items.find(id); return i == items.end() ? nullptr : &i->second; } } objects;
auto* sObjectMgr = &objects;
namespace ProfessionRescale { bool owned = true; bool OwnsItem(uint32) { return owned; } }
namespace ItemScaling { std::unique_ptr<ItemTemplate> DeriveTemplate(uint32, ItemTemplate const&, uint32 level) { return std::make_unique<ItemTemplate>(ItemTemplate{0, 0, 0, level, 0}); } }
struct Field { uint32 value; uint32 Get(uint32) const { return value; } };
struct Player { Item* target = nullptr; std::string name = "player"; std::string const& GetName() const { return name; } Item* GetItemByGuid(uint32) const { return target; } void DestroyItemCount(Item*, uint32, bool) { } };
struct WorldSession { };
namespace Chatter { void SendSysMessage(std::string const&) { } }
namespace ProfessionToken { }
'''


def main():
    source = SOURCE.read_text(encoding='utf-8')
    start = source.index('namespace ProfessionToken\n{')
    end = source.index('\nclass item_coa_profession_rune', start)
    block = source[start:end]
    harness = (HERE / 'harness.cpp').read_text(encoding='utf-8')
    harness = harness.replace('// ACTUAL_PROFESSION_TOKEN', block)
    compiler = shutil.which(os.environ.get('CXX', 'c++'))
    assert compiler, 'Enable a C++20 compiler.'
    with tempfile.TemporaryDirectory(prefix='coa-profession-token-') as directory:
        out = Path(directory)
        cpp = out / 'harness.cpp'
        cpp.write_text(harness, encoding='utf-8')
        exe = out / 'regression'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src/server/coa'),
                        str(cpp), '-o', str(exe)], check=True, timeout=60)
        subprocess.run([str(exe)], check=True, timeout=15)
    print('PASS: profession rune lookup, eligibility and monotonic level rules')


if __name__ == '__main__':
    main()
