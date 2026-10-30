# Launcher languages

The English master and the French, German and Spanish translations live in
[contracts/localization.json](contracts/localization.json). Settings → General →
Language selects the display language or the system default. The choice is stored
as `general/language` in native launcher settings and survives renderer reloads and
restarts. Browser preview has its own settings.

The catalogue currently has 777 UI entries translated into all four languages.
It also inventories 654 native strings; 52 have translations. This inventory is
not proof that every native message has been converted. Native notification
headings and matching error/status labels are connected; many native message
bodies, composed diagnostics, host errors and the pre-React native recovery
document still use English. Third-party module text, release notes and user
content retain their supplied language. Translator review is still needed.

## One source file

`translations.en` is the English master. Other language records use exactly the
same stable keys. `languages` supplies the language picker labels and text
direction. `sources` preserves the original program label and inventory locations;
keep its `original` text unchanged when editing displayed English.

For example, change `translations.en["language.choose"]` to change the English
language-picker label. Translate that same key under `fr`, `de` and `es`. Messages
with parameters such as `{name}`, `{page}` or `{count}` must retain those names,
but their position can change. Counted messages use `.one`/`.other` keys and
`Intl.PluralRules`. Messages are plain React text, never HTML.

Page IDs, commands, settings keys, saved enum values, paths, protocol fields,
profile/game names and package identifiers are not translated. Notification
titles explicitly distinguish program text from user-supplied text. Notification
bodies remain original text until their parameters are represented explicitly.

## Development and packaged copies

`npm run dev`, `npm run build` and `npm test` automatically run
`scripts/sync-localization.mjs`. It copies the master to
`public/localization.json` and generates a compact English startup fallback under
ignored `web/generated/`. Edit the master, not those generated files.

Vite includes the runtime copy in `dist/localization.json`; the desktop package
ships it as `web/localization.json` beside its other web assets. The launcher
loads that file at startup without a network request to an external service.
Editing the packaged copy and reloading the window changes displayed copy
without rebuilding C++ or installing Node. A subsequent install/update replaces
that copy, so maintain permanent translations in the source master.

A missing translation uses English for that key. A missing, invalid, oversized
or incompatible runtime catalogue uses bundled English so the interface remains
usable. Placeholders and language records are validated before accepting the
file. A saved regional locale resolves to its base language when available;
unsupported languages fall back to English. Adding a language record and its
translation dictionary makes it available in the picker after reload.

## Validation

`npm test` checks catalogue shape, complete UI dictionaries for the four launch
languages, matching placeholders, fallback behavior, plural selection, referenced
keys and stable technical JSX attributes. Native tests exercise accepted/rejected
locale values and program/user notification tagging.

With an isolated desktop window exposing the usual local debugging port:

```powershell
npm run test:localization -- build/native/desktop/web
```

The smoke check exercises all four languages, persistence after reload, translated
command navigation, unchanged filter/sort values and profile names, editable
English, per-key fallback, invalid JSON recovery and a 640-pixel layout. It changes
and restores only the supplied ignored build copy. Use fresh test data; it creates
a profile named `Library`. No production user data is required.

Native bodies and remaining diagnostic/recovery copy are an open localisation
gate, separate from the remaining legacy feature-parity gates in
[MIGRATION_STATUS.md](MIGRATION_STATUS.md).
