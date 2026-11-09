# Patches to NVIDIA's HSB IP

The folder containts the patches for the IP. `make hsb-src` copies the IP to `build/hsb/nv_hsb_ip` and applies the patches in name order.

| Patch | What it changes |
|---|---|
| `0001-dp_pkt-imm_disable-plain-write.patch` | bit 17 of the host UDP port word (`imm_disable`): with `md_disable`, the last packet of a frame is a plain UC WRITE instead of WRITE_WITH_IMM |

To add one, change the IP in a git checkout of the vendored copy, commit, and export the commit:

```bash
cp -R third_party/hsb/nv_hsb_ip /tmp/nv_hsb_ip && cd /tmp/nv_hsb_ip
git init -q && git add -A && git commit -qm vendored
for p in <repo>/patches/*.patch; do git am -q "$p"; done
# edit, then
git commit -am "<area>: <what it does>"
git format-patch -1 -o <repo>/patches/ --start-number <next number>
```
