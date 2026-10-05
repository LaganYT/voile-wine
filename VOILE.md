# voile-wine

The Wine that [Voile](https://github.com/Zagrios/voile), the runtime of [Obelisk](https://github.com/Zagrios/obelisk), runs Windows games with: CrossOver's open-source Wine, plus Obelisk's changes. Like Wine, it is licensed under the LGPL-2.1-or-later (see `COPYING.LIB`).

## Branches

- `crossover` holds CrossOver's Wine sources exactly as CodeWeavers publishes them, one commit per release, tagged `crossover-<version>`. It never carries Obelisk's changes.
- `main` is `crossover` plus Obelisk's changes, one commit each. Releases are tagged `v<crossover version>-<revision>`, such as `v26.3.0-1`. Voile's build pins one of these commits.

Several of Obelisk's changes come from Proton's Wine (ValveSoftware/wine). Their commit messages name the original commits.

## Updating CrossOver

```sh
voile/check-crossover.sh                  # exits 10 when a newer release exists
voile/import-crossover.sh 26.4.0          # commits it on crossover, tags crossover-26.4.0
git rebase crossover-26.4.0 main          # replays Obelisk's changes, stopping at conflicts
```

Then tag the new `main` `v26.4.0-1`. In [Voile](https://github.com/Zagrios/voile), point the `wine` submodule at it, update the CrossOver archive in `sources.json` with the hash the import prints, and push a tag `v26.4.0-1` there to publish the runtime.
