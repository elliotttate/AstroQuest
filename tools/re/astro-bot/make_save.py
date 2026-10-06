# Builds edited ASTRO BOT (CUSA12392) save folders from build/pc-bench/save.
# usage: python tools/re/astro-bot/make_save.py  -> writes build/pc-bench/save-w2 and build/pc-bench/save-all
# (needs build/pc-bench/save, which tools/pc-bench.sh makes on its first run; use one with
# SAVE=build/pc-bench/save-w2 tools/pc-bench.sh ...)
# Save format and semantics: see save-format.md in this folder.
import os, re, shutil

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "build", "pc-bench"))
SRC = os.path.join(ROOT, "save", "CUSA12392")
XML_OFF = 0x400          # game's save-memory block allocator starts at 0x400 (0xc5e5c4)
BLOCK_MAX = 0x8000       # PlayRoom block size (0x15ff0: ecx=0x8000)

# L number (1-based) -> (name, kind). kind: N normal, B boss, F final boss, K collection, C challenge, R credits
W = {
    1: ["ROOFTOPS", "CONSTRUCTION", "CAVE & CANNONS", "ROCK CLIMBING", "GORILLA"],
    2: ["BEANSTALK", "UNDERWATER", "GIANT 1", "MINE CART", "FIRE OCTOPUS"],
    3: ["CANYONS", "INDY CAVE 1", "INTO THE WHALE", "FOREST CASTLE", "BIRD BOSS"],
    4: ["GIANT 2", "FUNFAIR", "FIRE TRAPS", "INDY CAVE 2", "SPIDER"],
    5: ["HEAVEN & STORM", "GRAVEYARD", "NINJA", "VOLCANO", "SHARK BOSS"],
}
CHALLENGES = [  # (name, RequiredForUnlock) in Challenge-planet order -> L28..L53
    ("BUMPERS!", "ROOFTOPS"), ("DODGEBALL!", "CONSTRUCTION"), ("CANNON MANIA", "CAVE & CANNONS"),
    ("ROPE MIX", "ROCK CLIMBING"), ("GORILLA CHALLENGE", "GORILLA"), ("WATER SHOOTING", "BEANSTALK"),
    ("UNDERWATER MAZE", "UNDERWATER"), ("CATERPILLAR MANIA", "GIANT 1"), ("EXTREME PLATFORMS", "MINE CART"),
    ("FIRE OCTOPUS CHALLENGE", "FIRE OCTOPUS"), ("WATERFALL", "CANYONS"), ("LASERS", "INDY CAVE 1"),
    ("WAVES", "INTO THE WHALE"), ("VERTICAL TREE", "FOREST CASTLE"), ("BIRD BOSS CHALLENGE", "BIRD BOSS"),
    ("BUMPERS! (HARD)", "GIANT 2"), ("MACHINE GUN", "FUNFAIR"), ("FIRE ROAD", "FIRE TRAPS"),
    ("TATAMI", "INDY CAVE 2"), ("SPIDER BOSS CHALLENGE", "SPIDER"), ("STORM CITY", "HEAVEN & STORM"),
    ("LIGHT BLOCKS", "GRAVEYARD"), ("SHURIKEN RIVER", "NINJA"), ("LAVA RUN", "VOLCANO"),
    ("SHARK BOSS CHALLENGE", "SHARK BOSS"), ("FINAL BOSS CHALLENGE", "ALIEN BOSS"),
]
LNUM = {}
for w, names in W.items():
    for i, n in enumerate(names):
        LNUM[n] = (w - 1) * 5 + i + 1
LNUM["ALIEN BOSS"] = 26
LNUM["COLLECTION"] = 27
for i, (n, _) in enumerate(CHALLENGES):
    LNUM[n] = 28 + i
LNUM["CREDITS ROLL"] = 78


def cleared_normal():  # what the game's own "clear level" path leaves (0x9937f0/0x8d8290)
    return dict(A=1, B=0xFF, C=1, D=1)   # 8 bots, chameleon found, cleared once


def cleared_boss():
    return dict(A=1, B=0, C=0, D=1)


def unlocked():
    return dict(A=1, B=0, C=0, D=0)


def plan(worlds_cleared, next_unlocked):
    """worlds_cleared: list of world numbers fully cleared (4 levels 100% + boss).
    next_unlocked: list of level names to unlock (A=1, not cleared)."""
    s = {}
    cleared = set()
    for w in worlds_cleared:
        for i, n in enumerate(W[w]):
            s[LNUM[n]] = cleared_boss() if i == 4 else cleared_normal()
            cleared.add(n)
    for n in next_unlocked:
        s.setdefault(LNUM[n], unlocked())
    if "GORILLA" in cleared:
        s.setdefault(LNUM["COLLECTION"], unlocked())     # RequiredForUnlock GORILLA
    for n, req in CHALLENGES:                             # chameleon found / boss beaten -> challenge open
        if req in cleared:
            s.setdefault(LNUM[n], unlocked())
    return s


def edit_text(text, levels):
    for ln, vals in levels.items():
        m = re.search(r"(\t<L%d>\r\n)(.*?)(\t</L%d>\r\n)" % (ln, ln), text, re.S)
        assert m, ln
        body = m.group(2)
        for k, v in vals.items():
            body, n = re.subn(r"(\t\t<%s>)-?\d+(</%s>)" % (k, k), r"\g<1>%d\g<2>" % v, body)
            assert n == 1, (ln, k)
        text = text[:m.start(2)] + body + text[m.end(2):]
    return text


def build(dst_name, levels):
    dst = os.path.join(ROOT, dst_name, "CUSA12392")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(SRC, dst)
    for rel in ("sce_sdmemory/memory.dat", "sce_sdmemory/sce_backup/memory.dat"):
        p = os.path.join(dst, rel)
        raw = open(p, "rb").read()
        assert raw[:XML_OFF] == bytes(XML_OFF)
        end = raw.index(b"\0", XML_OFF)
        old = raw[XML_OFF:end].decode("ascii")
        root_end = old.rindex("</PlayRoom>") + len("</PlayRoom>")
        body, trailer = old[:root_end], old[root_end:]      # trailer: stale byte(s) from an older, longer write
        new = edit_text(body, levels) + trailer
        assert len(new) < BLOCK_MAX
        out = bytearray(len(raw))
        out[XML_OFF:XML_OFF + len(new)] = new.encode("ascii")
        assert len(out) == 1 << 20
        open(p, "wb").write(bytes(out))
    bots = sum(bin(v.get("B", 0)).count("1") for v in levels.values())
    print("%s: %d L entries edited, total bots %d" % (dst_name, len(levels), bots))
    for ln in sorted(levels):
        name = next(k for k, v in LNUM.items() if v == ln)
        print("  L%-2d %-24s %s" % (ln, name, levels[ln]))


if __name__ == "__main__":
    build("save-w2", plan([1, 2], ["CANYONS"]))
    build("save-all", plan([1, 2, 3, 4, 5], ["ALIEN BOSS"]))
