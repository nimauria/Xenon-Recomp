# Game modules from the shared repository

Game modules live in [Xenon-Modules](https://github.com/nimauria/Xenon-Modules)
under games/<contributor>/<game>/. For example:

~~~text
games/nimauria/project-gracemeria/
games/nimauria/gears-of-war/
games/nimauria/gears-of-war-2/
games/another-contributor/their-game/
~~~

Only Gracemeria is included in this migration; other names illustrate the layout.
Game folders contain installable module DLLs, details, artwork, licences and
required runtime data. Developer source, tests and build tools are excluded.
Hooks compiled into prebuilt DLLs need no separate source files. Gracemeria
currently retains exported preparation inputs until a tested DLL is available.

The launcher reads version-2 catalog.json, each entry.json and generated
package.json from Xenon-Modules main. It fetches the selected game's indexed
files, verifies every size and SHA-256 and assembles a local staging archive.
No GitHub Release, tag, uploaded ZIP or separate game repository is needed.
File changes expose updates even without a version bump. IDs stay stable.

Contributors declare installable paths in package-files.json, run
node scripts/sync-packages.mjs and node scripts/validate-catalog.mjs, then submit
a PR to Xenon-Modules. Failed updates preserve installations; previous versions
and their file digests remain available for rollback. Commercial content and
game-derived native output stay local to the user.

Launcher self-updates still use Xenon-Recomp releases. This guide retains its
old filename for existing links. Ship the updated launcher before switching the
public catalogue to version 2.
