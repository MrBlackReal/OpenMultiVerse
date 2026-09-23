# Asset download links

Link lists for the real-data tier: planet and moon textures, small-body shape
models and spacecraft models. The lists come from the scripts here; rerun a
script to refresh its list. `check_links.py FILE.tsv` HEAD-checks a list and
adds `status` and `bytes` columns.

| List | Script | What |
|---|---|---|
| `prograde_assets.tsv` | `prograde_links.py` | prograde-data *Solar System*: 311 files, 1.37 GB. Includes diffuse, normal, specular, night, cloud and ring maps for 38 bodies, PLY shapes for 12 small moons, asteroids and 67P, and OBJ, glTF and .blend spacecraft. |
| `prograde_links.tsv` | `prograde_links.py` | The same folder in full (946 files), including 627 orbital-parameter tables |
| `sbn_links.tsv` | `sbn_shape_links.py` | PDS Small Bodies Node shape-model index: 63 asteroids, comets and moons. Archive originals (TAB, WRL, ICQ, DSK, OBJ) plus SBN's derived OBJ files and previews. |
| `damit_links.tsv` | written by hand | DAMIT: the complete lightcurve-inversion model archive (~1.3 GB, refreshed monthly) and its CSV tables |

Download everything in a list:

    aria2c -x4 -j4 -i prograde_asset_urls.txt -d prograde   # or: wget -i ...
    aria2c -x4 -j4 -i prograde_urls.txt -d prograde         # + orbital tables
    aria2c -x4 -j4 -i sbn_urls.txt -d sbn

Keeping the folder layout for prograde:

    tail -n +2 prograde_assets.tsv | while IFS=$'\t' read kind body path url rest; do
        mkdir -p "prograde/$(dirname "$path")"
        curl -sSL -o "prograde/$path" "$url"; done

Licences: each prograde asset has a `LICENSE-*` file next to it (listed as kind
`license`). PDS and DAMIT data are public, but cite the model papers.

Not scraped: **3d-asteroids.space** sits behind a Cloudflare bot challenge,
so scripts get HTTP 403. Most of its lightcurve models come from DAMIT, and
its radar and spacecraft models from the PDS SBN, both covered above.
