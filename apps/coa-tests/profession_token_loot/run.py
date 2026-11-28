import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RUNES = ROOT / 'data/sql/updates/pending_db_world/rev_20261128_00_coa_profession_runes.sql'
AWARDS = ROOT / 'data/sql/updates/pending_db_characters/rev_20261128_00_coa_profession_token_award.sql'
SOURCE = ROOT / 'src/server/coa/CoAProfessionToken.cpp'
MC_LOOT = ROOT / 'data/sql/updates/pending_db_world/rev_20261001_12_molten_core_token_loot.sql'

EXPECTED = {
    'Rune of Molten Core': 65,
    'Rune of Molten Core (Heroic)': 68,
    'Rune of Molten Core (Mythic)': 72,
    'Rune of Molten Core (Ascended)': 75,
}


def main():
    rune_sql = RUNES.read_text(encoding='utf-8')
    header, rows = rune_sql[rune_sql.index('INSERT INTO'):].split('VALUES', 1)
    columns = re.search(r'\((.*)\)', header).group(1)
    columns = [name.strip(' `') for name in columns.split(',')]
    inserted = {}
    for row in re.findall(r'^\((.*?)\),?;?$', rows.strip(), re.MULTILINE | re.DOTALL):
        cells = [cell.strip() for cell in row.split(',')]
        assert len(cells) == len(columns), (len(cells), len(columns))
        record = dict(zip(columns, cells))
        inserted[record['name'].strip("'")] = record

    assert list(inserted) == list(EXPECTED), inserted
    for name, level in EXPECTED.items():
        record = inserted[name]
        assert record['ItemLevel'] == str(level), record
        assert record['class'] == '12' and record['Flags'] == '4', record
        assert record['bonding'] == '1' and record['stackable'] == '1', record
        assert record['ScriptName'] == "'item_coa_profession_rune'", record
        assert record['RandomProperty'] == '0' and record['RandomSuffix'] == '0', record
    assert len({record['entry'] for record in inserted.values()}) == 4

    source = SOURCE.read_text(encoding='utf-8')
    bosses = dict(re.findall(r'\{\s*(\d+),\s*(7800\d{3})\s*\}', source))
    assert len(bosses) == 36, bosses
    for base in (11502, 11982, 11988, 12056, 12057, 12098, 12118, 12259, 12264):
        assert [bosses[str(base + 100000 * tier)] for tier in range(4)] == [
            '7800165', '7800168', '7800172', '7800175']
    assert set(bosses.values()) <= {record['entry'] for record in inserted.values()}

    awards = AWARDS.read_text(encoding='utf-8')
    assert 'CREATE TABLE IF NOT EXISTS `coa_profession_token_award`' in awards
    assert 'PRIMARY KEY (`boss_entry`, `player_guid`)' in awards
    assert awards.count('CREATE TABLE') == 1

    mc_bosses = set(re.findall(r'^\((\d+),', MC_LOOT.read_text(encoding='utf-8'), re.MULTILINE))
    assert set(bosses) <= mc_bosses, set(bosses) - mc_bosses

    print(f'PASS: {len(inserted)} runes, {len(bosses)} boss/difficulty entries, award migration')


if __name__ == '__main__':
    main()
