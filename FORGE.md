# Forge's Wine

The Wine that [Forge](https://github.com/Zagrios/forge) runs Windows games with: CrossOver's open-source Wine, plus Forge's changes. Like Wine, it is licensed under the LGPL-2.1-or-later (see `COPYING.LIB`).

## Branches

- `crossover` holds CrossOver's Wine sources exactly as CodeWeavers publishes them, one commit per release, tagged `crossover-<version>`. It never carries Forge's changes.
- `main` is `crossover` plus Forge's changes, one commit each. Releases are tagged `v<crossover version>-<revision>`, such as `v26.3.0-1`. Forge's build pins one of these commits.

Several of Forge's changes come from Proton's Wine (ValveSoftware/wine). Their commit messages name the original commits.

## Updating CrossOver

```sh
forge/check-crossover.sh                  # exits 10 when a newer release exists
forge/import-crossover.sh 26.4.0          # commits it on crossover, tags crossover-26.4.0
git rebase crossover-26.4.0 main          # replays Forge's changes, stopping at conflicts
```

Then build the runtime from the new `main`, tag it `v26.4.0-1`, and update the pinned commit and the CrossOver archive hash in Forge's `wine-runtime/sources.json`. The import prints that hash.
