---
name: coa-profession-token
description: >-
  Plan or implement raid- and difficulty-specific tokens that upgrade profession-crafted equipment to
  approved item-level budgets, including item-instance stat rescaling, loot, persistence and verification.
---

# CoA profession token

Follow the checkout's AGENTS.md and [CoA development](../coa-development/SKILL.md). Use this skill for
raid-earned tokens that upgrade profession-crafted equipment. This does not convert raid gear or define
the raid's broader release, attunement, encounter, or loot progression.

For raid reward placement or raid-wide progression changes, also use
[CoA raid rescaling](../coa-raid-rescaling/SKILL.md). For crafted item budgets and runtime ownership, read
[CoA profession rescale](../coa-profession-rescale/SKILL.md) and
[fixed item rescaling](../../docs/systems/fixed-item-rescaling.md). The profession-rescale skill covers
baseline budgets and provenance; do not apply its template-wide conversion policy to this item-instance
upgrade feature.

## Resolve inputs

Settled token rules:

- Every boss in a raid difficulty drops one rune at 100% chance. All bosses in that raid/difficulty share
  the same rune identity and target item level; the rune is not boss-specific. For example, all Normal Molten
  Core bosses drop `Rune of Molten Core`, while all Heroic bosses drop `Rune of Molten Core (Heroic)`.
- A rune can upgrade eligible profession-crafted equipment from any lower item level to the rune's item
  level. It does not apply to an item already at or above the rune's level. Normal uses the existing
  level-65 Molten Core row; Heroic, Mythic and Ascended use 68, 72 and 75.
- Runes apply only when they raise the item's level. A lower-level rune on an already upgraded item is
  refused, and an item cannot be upgraded to a tier at or below its current level. A higher-level rune can
  upgrade a previously upgraded item. The rune is consumed on success; failed or invalid uses do not consume
  it. Upgrades cannot be reversed.

Before implementation, establish:

- The target raid. Scope includes all available difficulties for that raid: Normal, Heroic, Mythic and
  Ascendant. Inventory the difficulties that actually exist in the target realm; do not create unsupported
  modes merely because they are listed in this default.
- The target item level for each included difficulty and the bosses present in each difficulty. Read targets
  from installed realm data or obtain them from the owner; never infer or invent missing targets.
- All profession-crafted equipment is eligible. Discover and verify the exact item IDs and crafted provenance
  from the effective realm data; do not rely on item names, current item level, or equipped location alone.

Show the proposed token-to-tier mapping, qualifying gear policy and unresolved choices before implementation
when any of them materially changes progression or item ownership. Skill use and planning do not authorize
deployment or live database changes.

## Audit the existing item path

Trace the current profession budget ownership, item-stat generation and delivery paths before adding another
scaler. Identify authoritative baseline stats and budgets, including quality, slot, role, armor, weapon DPS,
ratings, sockets, socket bonuses and PvE Power where applicable. Check for overlap with raid, dungeon, zone,
native-item and profession owners. Reuse the established item-query delivery and client stat protocol where
possible; do not assume changing an item instance's item level alone recalculates its effective stats.

Reuse the realm's existing item-level scaling rules. `ItemScaling::BuildTemplate` already derives a new
template from a base item at a target item level, preserving item identity, bindings, slots, random-property
families and effects while recalculating primary stats, resistances, armor, block, weapon damage, sell/buy price,
required level and disenchant bracket. Use effective `RandPropPoints` and the installed item curves; do not
invent a second stat-scaling formula. The existing registry keys scaled copies by a *lift*, so the rune needs
an equivalent explicit target-level path (not a stepped lift) before implementation.

Random affixes need both halves of the change. The derived template's `RandomProperty`/random-suffix pool must
come from the target item level, and the consumed item instance's affix must remain a member of that same
pool family. Recalculate the instance's suffix factor for the derived entry and target level using
`GenerateEnchSuffixFactor`/`UpdateItemSuffixFactor`; do not keep the source item's suffix factor, or the
displayed and equipped random-stat magnitudes will stay at the old tier. A fixed `ItemRandomProperties` row
uses its own enchantment values and has no suffix factor. Verify both random-property and random-suffix items,
including the client's item-query reply.

Do not require a matching raid drop for a crafted item. Derive every tier from the crafted base itself with
the shared scaling rules; the raid supplies the target item levels, not a donor item. A crafted slot with no
raid equivalent, such as a fist weapon or non-robe cloth chest, is still eligible. Existing raid rows are
comparisons and must not be used as crafted upgrade targets or as a gate on eligibility.

Materialize the derived template through the established scaled item-template path and explicit mapping, not
by editing a shared crafted template in response to rune use. Give each crafted base and difficulty a stable
mapping to its derived template. Prefer unused item-ID space grouped near the raid tier, after verifying the IDs
are free and checking that the existing item-query/template-delivery path can show the row; obtain explicit
approval before relying on a downloadable client patch.

A rune changes only the consumed item instance to the derived template and consumes itself after that change is
saved. Do not reuse a raid-drop row. If the existing scaling path cannot preserve a crafted item's
non-scalable properties, report that item and target instead of altering them.

## Implement

- For the target raid, define a data-driven mapping from each available difficulty (Normal, Heroic, Mythic
  and Ascendant) to one shared rune identity and target item level, plus the complete boss set in that mode.
  The Molten Core target levels are 65 Normal, 68 Heroic, 72 Mythic and 75 Ascended.
  Give each distinct difficulty target its own item identity; a shared template cannot represent
  different rune item levels. Name each rune `Rune of <Raid>` and append the difficulty in parentheses, such
  as `(Heroic)` and `(Ascended)`, for non-Normal variants. Ascendant is the realm's Ascended difficulty;
  `(Ascended)` is the approved rune suffix.
- Route rune use through the established item-script mechanism. Validate the caster, target ownership,
  target item category/provenance, and the item's current and target levels before modifying anything. Refuse
  a rune whose level is not above the item's current level. A previously applied lower-tier rune does not
  disqualify the item from a higher-tier rune. Fail without consuming the rune or partially changing the item.
  Persist the item upgrade and consume the rune only after success.
- Resolve the crafted base's derived template at the rune's exact target item level and point only the
  consumed instance at it through the established scaled-template path. Keep equipped values, inventory and
  item-query previews, relog/load, persistence, enchantments and gems consistent. Recalculate the
  random-affix suffix factor for the derived entry so random stats scale with the new item level. Do not alter
  unrelated copies, the crafted base template or raid-drop templates. An item at or above the rune's target
  level is refused; applying the same tier again is also refused.
- Add the same difficulty-specific rune to every boss in that raid difficulty at exactly 100% chance and
  one item per boss. Trace shared loot entries, references and difficulty routing so a rune does not leak into
  other difficulties or raids and every intended boss has exactly one drop.
- Use repeatable pending migrations and keep SQL changes within repository authorization boundaries. Do not
  edit immutable historical or merged SQL.

## Verify

Add focused coverage for:

- One shared rune identity per raid/difficulty, target item level mapping, and exactly one guaranteed rune
  from every boss in that difficulty, with no missing bosses or unapproved loot paths.
- Complete discovery and eligibility coverage for all profession-crafted equipment item IDs; reject
  non-crafted items, unsupported categories, wrong owners, lower-or-equal-level runes, and invalid upgrades.
  Verify low-level crafted gear can use successive higher-tier runes, while lower-tier and same-tier runes are
  refused without consuming them.
- Exact target item level and scalable stat budgets from the shared item-scaling rules (`RandPropPoints` and
  the installed curves), including armor and weapon damage where applicable; verify the derived template is
  delivered to the client and bindings, sockets/gems, enchantments, random properties, procs and on-use effects
  match the crafted base. Verify random affixes use the target-level pool and recalculated suffix factor, on
  both the client reply and equipped stats. Verify every eligible base item can derive a template at each
  approved target.
- Success-only token consumption, failure atomicity, inventory/equipment behavior, relog persistence and
  enforcement that upgrades cannot be repeated or reversed.
- Item-query/preview values and client cache delivery; distinguish server-side proof from actual client
  acceptance.
- Migration idempotence and unchanged behavior for unupgraded copies and unrelated item owners.

Run selected checks through `tools/verify_all.py` as directed by repository verification guidance. If C++
changes require a build, follow the repository build preflight and confirmation requirements. Report automated
evidence separately from manual gameplay and client checks. Deployment, installation, and live SQL remain
separate scopes requiring explicit authorization.
